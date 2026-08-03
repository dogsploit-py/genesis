// god/god.h — divine intervention: brushes, disasters, population tools, rule
// overrides, undo/redo and miracles.
//
// Every god power is a GodAction, a plain value that can be logged, replayed,
// stored in a miracle, and undone. Actions are applied ON THE SIM THREAD at a
// tick boundary with the world lock held exclusively (ARCHITECTURE.md §1), so
// no intervention can ever tear state or race a tick.
//
// UNDO is delta-based, not snapshot-based. Each record stores the affected
// tiles and agents both BEFORE and AFTER, which is what lets undo and redo be
// the same machinery walked in opposite directions. That matters because
// re-applying an action forward would consume randomness and produce a
// different result -- redo has to restore a recorded state, not re-roll it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/rng.h"
#include "sim/agent.h"
#include "sim/world.h"

namespace gen {

class BinaryWriter;
class BinaryReader;

// ---------------------------------------------------------------------------

enum class GodActionKind : uint16_t {
    None = 0,

    // -- terrain and resource brushes --
    BrushElevation,     // f0 = metres added per unit intensity
    BrushRock,          // i0 = RockType for the surface stratum
    BrushSoil,          // f0 = nutrient delta (-255..255)
    BrushWater,         // f0 = water depth delta
    BrushPlants,        // f0 = biomass delta
    BrushOre,           // i0 = OreType, f0 = grade
    BrushTemperature,   // f0 = degrees added

    // -- creation --
    SpawnAgents,        // i0 = count
    SpawnDesigned,      // spawns one agent from a designed genome (text = name)

    // -- population tools --
    MassKill,
    MassEdit,           // i0 = trait index, f0 = delta, i1 = 0 add / 1 set / 2 scale
    Bottleneck,         // i0 = survivors to keep
    Migrate,            // f0,f1 = destination
    Teleport,           // f0,f1 = destination (selection only)
    Sterilise,
    Fertilise,
    SelectionPressure,  // i0 = trait, f0 = target value, f1 = strength

    // -- disasters --
    Drought, Flood, Storm, Wildfire, Earthquake, Volcano, Meteor, IceAge, Plague,

    // -- economy (M8) --
    EnableBarter,       // lets agents exchange goods at all
    IntroduceCurrency,  // i0 = substance id (0 = fiat token), f0 = initial holding
    AbolishEconomy,     // returns the world to having no economy whatsoever

    // -- global --
    SetClimate,         // i0 = which field, f0 = value
    SetConfigValue,     // text = key, f0 = value
    Count
};

const char* godActionName(GodActionKind k);
// Actions that paint over an area and therefore use radius/intensity.
bool godActionIsBrush(GodActionKind k);

// ---------------------------------------------------------------------------
// Population filter: the selector shared by every mass operation.
// ---------------------------------------------------------------------------

struct PopulationFilter {
    bool     useRegion = false;
    float    x = 0.0f, y = 0.0f, radius = 50.0f;
    int      stage = -1;              // LifeStage, -1 = any
    float    minAge = 0.0f, maxAge = 1.0e9f;
    int      traitIndex = -1;         // Trait, -1 = no trait constraint
    float    traitMin = -1.0e30f, traitMax = 1.0e30f;
    int      sexMode = 0;             // 0 any, 1 female-expressed, 2 male-expressed, 3 intersex
    bool     taggedOnly = false;
    bool     excludeImmortal = true;

    bool matches(const Agents& a, uint32_t slot, uint64_t tick) const;
    std::string describe() const;
};

// ---------------------------------------------------------------------------

struct GodAction {
    GodActionKind kind = GodActionKind::None;
    float    x = 0.0f, y = 0.0f;
    float    radius = 8.0f;
    float    intensity = 1.0f;
    int32_t  i0 = 0, i1 = 0;
    float    f0 = 0.0f, f1 = 0.0f, f2 = 0.0f;
    std::string text;
    PopulationFilter filter;

    uint64_t appliedTick = 0;
    std::string description;   // filled in when applied, for the log
};

// ---------------------------------------------------------------------------
// Undo records
// ---------------------------------------------------------------------------

// Every mutable per-tile field, so a brush can be reversed exactly whatever it
// touched. 40 bytes; a radius-20 brush is ~1250 tiles, so ~50 KB per stroke.
struct TileUndo {
    uint32_t index = 0;
    float    elevation = 0.0f, waterDepth = 0.0f, temperature = 0.0f;
    float    rainfall = 0.0f, biomass = 0.0f, soilMoisture = 0.0f;
    uint8_t  soilN = 0, soilP = 0, soilK = 0, biome = 0;
    uint8_t  oreType = 0, oreGrade = 0;
    uint8_t  strataRock0 = 0;
    uint8_t  pad = 0;
    uint16_t strataThick0 = 0;

    static TileUndo capture(const World& w, uint32_t index);
    void restore(World& w) const;
};

struct UndoRecord {
    GodAction action;
    std::vector<TileUndo> tilesBefore, tilesAfter;
    // Serialized whole agents. Restoring one recreates it with its uid intact,
    // so relationships and pedigree links resolve again.
    std::vector<uint8_t>  agentsBefore, agentsAfter;
    uint32_t agentsBeforeCount = 0, agentsAfterCount = 0;
    // Agents that did not exist before the action; undo deletes them.
    std::vector<uint64_t> createdUids;
    // Agents removed by the action; redo deletes them again.
    std::vector<uint64_t> removedUids;
    std::string configKey;
    double configBefore = 0.0, configAfter = 0.0;

    size_t memoryBytes() const;
};

// ---------------------------------------------------------------------------
// Disasters that persist for a while rather than resolving instantly.
// ---------------------------------------------------------------------------

struct ActiveDisaster {
    GodActionKind kind = GodActionKind::None;
    float    x = 0.0f, y = 0.0f, radius = 0.0f, intensity = 1.0f;
    uint64_t ticksRemaining = 0;
    uint64_t startTick = 0;
    std::string label;
};

// ---------------------------------------------------------------------------

struct Miracle {
    std::string name;
    std::vector<GodAction> actions;
    int hotkey = -1;   // 0..9, bound to the number keys with Ctrl held
};

// ---------------------------------------------------------------------------

class Economy;

class GodMode {
public:
    // The economy, if the module is present. Nullable ON PURPOSE: God mode
    // must not depend on the optional module existing, so deleting econ/ means
    // deleting this pointer, three enum values and three arms of one switch.
    // Without it the three economy acts report that there is no economy module
    // rather than silently doing nothing.
    void setEconomy(Economy* e) { m_economy = e; }
    void configure();

    // Applies the action and pushes an undo record. Returns a human-readable
    // description of what happened, which also goes into the intervention log.
    std::string apply(GodAction action, World& world, Agents& agents,
                      RngBank& rng, uint64_t tick);

    bool undo(World& world, Agents& agents, uint64_t tick, std::string& what);
    bool redo(World& world, Agents& agents, uint64_t tick, std::string& what);

    size_t undoDepth() const { return m_undo.size(); }
    size_t redoDepth() const { return m_redo.size(); }
    size_t undoMemoryBytes() const;
    void   clearHistory();
    const std::vector<UndoRecord>& undoStack() const { return m_undo; }

    // Disasters that are still running.
    std::vector<ActiveDisaster>& disasters() { return m_disasters; }
    const std::vector<ActiveDisaster>& disasters() const { return m_disasters; }
    // Applied once per tick by the simulation, before the world step.
    void stepDisasters(World& world, Agents& agents, RngBank& rng, uint64_t tick,
                       std::vector<std::string>& eventsOut);

    // The global temperature offset and rainfall multiplier currently imposed
    // by active disasters (ice age, volcanic winter, drought).
    float globalTemperatureOffset() const { return m_temperatureOffset; }
    float globalRainfallMultiplier() const { return m_rainfallMultiplier; }

    std::vector<Miracle>& miracles() { return m_miracles; }
    const std::vector<Miracle>& miracles() const { return m_miracles; }

    void serialize(BinaryWriter& w) const;
    void deserialize(BinaryReader& r);

private:
    void pushUndo(UndoRecord&& rec);
    void trimUndo();

    // Individual appliers. Each fills the undo record as it goes.
    void applyBrush(const GodAction& a, World& w, Agents& ag, RngBank& rng, UndoRecord& rec);
    void applyPopulation(const GodAction& a, World& w, Agents& ag, RngBank& rng,
                         uint64_t tick, UndoRecord& rec, std::string& what);
    void applyDisaster(const GodAction& a, World& w, Agents& ag, RngBank& rng,
                       uint64_t tick, UndoRecord& rec, std::string& what);

    // Nullable on purpose -- see setEconomy above.
    Economy* m_economy = nullptr;

    std::vector<UndoRecord>     m_undo, m_redo;
    std::vector<ActiveDisaster> m_disasters;
    std::vector<Miracle>        m_miracles;
    float m_temperatureOffset = 0.0f;
    float m_rainfallMultiplier = 1.0f;
    size_t m_maxRecords = 64;
    size_t m_maxBytes = 512ull * 1024ull * 1024ull;
};

}  // namespace gen
