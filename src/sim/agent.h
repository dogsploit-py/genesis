// sim/agent.h — the population: agents, their life cycle, and the tick stages
// that make them behave.
//
// Structure-of-Arrays with a free list of slots and stable {slot, generation}
// handles, so a stale reference is DETECTABLE rather than a use-after-free.
// Genomes and brains live in their own arenas indexed by the same slot, so an
// agent's genetics, brain and body state are three parallel lookups with no
// pointer chasing.
//
// Nothing here allocates per tick.
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/jobs.h"
#include "core/rng.h"
#include "core/spatial_hash.h"
#include "sim/attraction.h"
#include "sim/brain.h"
#include "sim/chemistry_agent.h"
#include "sim/genetics.h"
#include "sim/knowledge.h"
#include "sim/species.h"

namespace gen {

class World;
class BinaryWriter;
class BinaryReader;

// ---------------------------------------------------------------------------

enum class LifeStage : uint8_t {
    Embryo = 0,    // gestating or in the egg; not on the map
    Juvenile,      // dependent on parents, very high plasticity
    Adolescent,    // independent, not yet fertile
    Adult,
    Senescent,     // fertility falling, mortality hazard climbing
    Dead,
    Count
};
const char* lifeStageName(LifeStage s);

enum class DeathCause : uint8_t {
    None = 0, Starvation, Dehydration, Exposure, Predation, Violence,
    Disease, Childbirth, Accident, OldAge, LethalGenes, Divine, Count
};
const char* deathCauseName(DeathCause c);

enum class Action : uint8_t {
    Idle = 0, Move, Eat, Drink, Rest, Flee, Attack, Groom, Court,
    Mate, Vocalise, Teach, Share, Build, Nurse, Count
};
const char* actionName(Action a);

// A stable handle. The generation counter is bumped every time a slot is
// recycled, so a handle to a dead agent compares unequal to whatever now
// occupies its slot.
struct AgentId {
    uint32_t slot = 0xFFFFFFFFu;
    uint32_t generation = 0;

    bool valid() const { return slot != 0xFFFFFFFFu; }
    bool operator==(const AgentId& o) const { return slot == o.slot && generation == o.generation; }
    bool operator!=(const AgentId& o) const { return !(*this == o); }
};
constexpr AgentId kNoAgent = AgentId();

// ---------------------------------------------------------------------------
// Pedigree: outlives the agents themselves, so lineage survives death.
// ---------------------------------------------------------------------------

struct PedigreeRecord {
    uint64_t uid = 0;
    uint64_t motherUid = 0;
    uint64_t fatherUid = 0;
    uint64_t birthTick = 0;
    uint64_t deathTick = 0;      // 0 while alive
    uint8_t  deathCause = 0;
    uint8_t  chromosomalSex = 0;
    float    sexExpression = 0.5f;
    uint16_t offspringCount = 0;
    std::string name;
};

class Pedigree {
public:
    void configure(size_t capacity);
    // The record's uid must already be set. Records are appended in birth
    // order, which is what makes the kinship recursion terminate.
    void insert(const PedigreeRecord& rec);
    void recordDeath(uint64_t uid, uint64_t tick, DeathCause cause);

    const PedigreeRecord* find(uint64_t uid) const;
    PedigreeRecord* findMutable(uint64_t uid);

    // Wright's coefficient of relatedness, r = 2 * kinship. Computed
    // recursively over the pedigree with a depth bound, because an unbounded
    // walk over a deep pedigree would be unaffordable at 10k agents.
    float relatedness(uint64_t a, uint64_t b, int maxDepth = 6) const;

    size_t size() const { return m_records.size(); }
    const std::deque<PedigreeRecord>& records() const { return m_records; }

    void serialize(BinaryWriter& w) const;
    void deserialize(BinaryReader& r);
    void clear();

private:
    float kinship(uint64_t a, uint64_t b, int depth) const;

    std::deque<PedigreeRecord> m_records;
    // uid -> ABSOLUTE record number. Old records are dropped from the front
    // when the cap is hit, so the deque index is (absolute - m_baseIndex).
    // Storing absolute numbers means dropping a record does not invalidate
    // every other entry in the map.
    std::unordered_map<uint64_t, size_t> m_index;
    size_t m_baseIndex = 0;
    size_t m_capacity = 400000;
};

// ---------------------------------------------------------------------------
// Social memory: what A remembers about B.
// ---------------------------------------------------------------------------

struct Relationship {
    uint64_t otherUid = 0;
    float    familiarity = 0.0f;
    float    affinity = 0.0f;      // -1 hostile .. +1 affiliative
    uint16_t interactions = 0;
    uint16_t rejections = 0;
    uint8_t  flags = 0;
};

enum RelationshipFlags : uint8_t {
    Rel_None = 0,
    Rel_Bonded   = 1 << 0,   // pair bond
    Rel_Mated    = 1 << 1,
    Rel_Offspring= 1 << 2,
    Rel_Parent   = 1 << 3,
    Rel_Rival    = 1 << 4,
    Rel_Ally     = 1 << 5,
};

// ---------------------------------------------------------------------------

struct PopulationStats {
    uint32_t population = 0;
    uint32_t byStage[static_cast<int>(LifeStage::Count)] = {0};
    uint32_t births = 0;           // cumulative
    uint32_t deaths = 0;
    uint32_t deathsByCause[static_cast<int>(DeathCause::Count)] = {0};
    double   meanAgeYears = 0.0;
    double   meanLifespanAtDeath = 0.0;
    double   meanEnergy = 0.0;
    double   meanHealth = 0.0;
    double   meanBrainNodes = 0.0;
    double   meanBrainConns = 0.0;
    double   meanGenomeLength = 0.0;
    double   meanSexExpression = 0.0;
    double   intersexFraction = 0.0;
    double   meanOrnament = 0.0;
    double   meanPrefOrnament = 0.0;
    double   meanInbreedingF = 0.0;
    double   meanRewardRepro = 0.0;
    double   bondedFraction = 0.0;
    double   meanReproDrive = 0.0;
    uint32_t pairBonds = 0;
    uint32_t pregnancies = 0;
    // Culture, tracked separately from genetics throughout.
    uint32_t discoveries = 0;        // distinct reactions ever discovered
    uint32_t knowledgeUnits = 0;     // units held across the living population
    double   meanKnowledge = 0.0;    // units per living agent
    uint32_t agentsWithFire = 0;
    uint32_t agentsWithKiln = 0;
    uint32_t agentsWithBellows = 0;
    double   technologyIndex = 0.0;  // 0..1, mean position on the technique ladder
    // Speciation.
    uint32_t extantSpecies = 0;
    uint32_t speciesEverNamed = 0;
    uint32_t hybridConceptions = 0;   // crosses that paid a fertility penalty
    uint32_t hybridBlocked = 0;       // crosses the penalty prevented
    // The courtship funnel, cumulative. Where a population fails to replace
    // itself, this says at which step it fails: never meeting, refusing each
    // other, being the same gamete type, or failing to conceive.
    uint32_t courtshipsAttempted = 0;
    uint32_t courtshipsMutual = 0;
    uint32_t matingsSameType = 0;
    uint32_t conceptions = 0;
};

// ---------------------------------------------------------------------------

class Agents {
public:
    void configure(size_t maxAgents, Rng& rngForSchema);
    void clear();

    size_t capacity() const { return m_capacity; }
    uint32_t population() const { return m_population; }
    const std::vector<uint32_t>& liveSlots() const { return m_liveSlots; }

    Genetics& genetics() { return m_genetics; }
    const Genetics& genetics() const { return m_genetics; }
    Brains& brains() { return m_brains; }
    const Brains& brains() const { return m_brains; }
    Pedigree& pedigree() { return m_pedigree; }
    const Pedigree& pedigree() const { return m_pedigree; }
    Inventory& inventory() { return m_inventory; }
    const Inventory& inventory() const { return m_inventory; }
    KnowledgeBase& knowledge() { return m_knowledge; }
    const KnowledgeBase& knowledge() const { return m_knowledge; }
    Speciation& speciation() { return m_speciation; }
    const Speciation& speciation() const { return m_speciation; }

    // -- creation and destruction -------------------------------------------
    AgentId spawnFounder(float x, float y, RngBank& rng, uint64_t tick,
                         bool heterogameticSex);
    AgentId spawnChild(uint32_t motherSlot, uint32_t fatherSlot, RngBank& rng, uint64_t tick);
    void    kill(uint32_t slot, DeathCause cause, uint64_t tick, World* world);

    bool     slotAlive(uint32_t slot) const {
        return slot < m_capacity && m_alive[slot] != 0;
    }
    bool     idValid(AgentId id) const {
        return id.valid() && id.slot < m_capacity && m_alive[id.slot] &&
               m_generation[id.slot] == id.generation;
    }
    AgentId  idOf(uint32_t slot) const {
        AgentId a;
        a.slot = slot;
        a.generation = m_generation[slot];
        return a;
    }
    int32_t  slotOfUid(uint64_t uid) const;

    // -- the tick stages ----------------------------------------------------
    // These are called in order from Simulation::tickOnce and correspond one
    // for one to steps 3-11 of the tick order in ARCHITECTURE.md.
    void buildSpatialIndex(const World& world);
    void sense(const World& world, uint64_t tick, JobSystem& jobs);
    void think(JobSystem& jobs);
    void act(World& world, RngBank& rng, uint64_t tick, JobSystem& jobs);
    void physics(const World& world, JobSystem& jobs);
    void metabolism(const World& world, RngBank& rng, uint64_t tick, JobSystem& jobs);
    void reproduction(World& world, RngBank& rng, uint64_t tick);
    void reapDead(World& world, uint64_t tick);
    // Tick stage: gathering, experiment, teaching. Serial, because it mutates
    // the world and other agents' knowledge.
    // Tick stage 4.5: the close-range neighbour index used by act(). Parallel.
    void buildInteractionNeighbours(JobSystem& jobs, float radius);
    void chemistry(World& world, RngBank& rng, uint64_t tick);
    // Periodic. A measurement of the population, not a process acting on it,
    // so it runs on its own cadence and consumes no randomness.
    void detectSpecies(uint64_t tick);

    void recomputeStats(uint64_t tick);
    const PopulationStats& stats() const { return m_stats; }

    // Population genetics, on the telemetry cadence rather than every tick.
    void recomputePopulationGenetics();
    const Genetics::PopulationGenetics& populationGenetics() const { return m_popGenetics; }
    double fst() const { return m_fst; }

    // -- queries used by the UI ---------------------------------------------
    // Nearest living agent to a world position within `radius` tiles, or -1.
    int32_t pickNearest(float x, float y, float radius) const;
    void    agentsInRect(float x0, float y0, float x1, float y1,
                         std::vector<uint32_t>& out) const;

    // Relational attraction between two live agents, fully broken down.
    //
    // `relatednessOverride` of -1 means "look the relatedness up in the
    // pedigree", which is correct but costs a bounded recursion with a hash
    // lookup at every node. The sensory path passes the CHEAP perceived
    // kinship instead (shared parentage plus MHC similarity), because that is
    // both what an animal actually has access to and what keeps this off the
    // critical path at 10k agents.
    float attractionBetween(uint32_t observerSlot, uint32_t targetSlot,
                            AttractionBreakdown* out,
                            float relatednessOverride = -1.0f) const;

    // The relationship record A holds about B, creating it if absent.
    Relationship* relationship(uint32_t slot, uint64_t otherUid, bool createIfMissing);
    const std::vector<Relationship>& relationships(uint32_t slot) const {
        return m_relationships[slot];
    }

    // Re-express a genome into a phenotype and rebuild the brain phenotype.
    // Called at birth, and again whenever god-mode edits an allele.
    void redevelop(uint32_t slot, RngBank& rng);

    // -- god-mode hooks -----------------------------------------------------
    void setImmortal(uint32_t slot, bool v) {
        if (v) m_flags[slot] |= Flag_Immortal; else m_flags[slot] &= static_cast<uint8_t>(~Flag_Immortal);
    }
    bool immortal(uint32_t slot) const { return (m_flags[slot] & Flag_Immortal) != 0; }
    void setTagged(uint32_t slot, bool v) {
        if (v) m_flags[slot] |= Flag_Tagged; else m_flags[slot] &= static_cast<uint8_t>(~Flag_Tagged);
    }
    bool tagged(uint32_t slot) const { return (m_flags[slot] & Flag_Tagged) != 0; }
    void forceBond(uint32_t a, uint32_t b, bool bonded);
    void setAttractionOverride(uint32_t observer, uint64_t targetUid, float value, bool enabled);
    void setImprint(uint32_t slot, uint32_t templateSlot);

    // -- serialization ------------------------------------------------------
    void serialize(BinaryWriter& w) const;
    void deserialize(BinaryReader& r);

    // One agent, whole, without its slot index. This is the unit god-mode undo
    // stores so a killed individual can be brought back with its uid -- and
    // therefore its relationships and pedigree links -- intact.
    void serializeAgent(BinaryWriter& w, uint32_t slot) const;
    void deserializeAgent(BinaryReader& r, uint32_t slot);
    // Allocates a free slot, restores a blob into it, and re-derives the
    // runtime state. Returns the slot, or -1 if the store is full.
    int32_t restoreAgentBlob(const std::vector<uint8_t>& blob, size_t offset, size_t& consumed);
    // Serialises one agent into a growing byte buffer.
    void appendAgentBlob(std::vector<uint8_t>& blob, uint32_t slot) const;
    // Rebuilds derived state (phenotype-dependent caches, brain CSR) for a slot
    // whose stored fields have just been replaced.
    void rederiveAfterRestore(uint32_t slot);

    enum AgentFlags : uint8_t {
        Flag_None      = 0,
        Flag_Immortal  = 1 << 0,
        Flag_Tagged    = 1 << 1,
        Flag_Pregnant  = 1 << 2,
        Flag_Bonded    = 1 << 3,
        Flag_Sterile   = 1 << 4,
        Flag_LearningFrozen = 1 << 5,
    };

    // -- the store (public: this is a data-oriented store, and wrapping
    //    contiguous arrays in accessors would defeat the point) -------------
    std::vector<uint8_t>  m_alive;
    std::vector<uint32_t> m_generation;
    std::vector<uint64_t> m_uid;
    std::vector<uint8_t>  m_flags;

    std::vector<float>    m_x, m_y;
    std::vector<float>    m_vx, m_vy;
    std::vector<float>    m_heading;

    std::vector<float>    m_energy, m_hydration, m_health, m_bodyTemp;
    std::vector<float>    m_pain, m_stress;
    std::vector<float>    m_damage;        // accumulated cellular damage
    std::vector<float>    m_telomere;      // 1.0 at birth, falls with divisions
    std::vector<uint64_t> m_birthTick;
    // Age already accumulated at spawn, in ticks. Founders are created as
    // established adults; birthTick alone cannot express that, because it
    // would have to be negative at tick 0.
    std::vector<uint64_t> m_ageOffset;
    std::vector<uint8_t>  m_stage;
    std::vector<uint8_t>  m_action;
    std::vector<uint8_t>  m_chromosomalSex;
    std::vector<uint8_t>  m_deathCause;

    std::vector<Phenotype>       m_phenotype;
    std::vector<DriveState>      m_drives;
    std::vector<DisplayVector>   m_display;
    std::vector<PreferenceVector> m_prefs;

    std::vector<uint64_t> m_motherUid, m_fatherUid;
    std::vector<uint64_t> m_bondedUid;
    std::vector<uint64_t> m_pregnantByUid;
    std::vector<uint64_t> m_embryoUid;            // the child already conceived
    std::vector<float>    m_gestationRemaining;   // hours
    // Hours until this agent will attempt courtship again. Without a
    // refractory period a pair re-evaluates every tick, is refused within
    // hours, and the rejection memory then locks them out permanently.
    std::vector<float>    m_courtCooldown;
    // Recent mates and their competitive weight, for sperm competition. Three
    // is enough for paternity to be genuinely contested without storing a list.
    std::vector<uint64_t> m_recentMateUid;        // capacity * 3
    std::vector<float>    m_recentMateWeight;     // capacity * 3
    std::vector<uint16_t> m_offspringCount;
    std::vector<float>    m_status;               // social standing 0..1
    std::vector<float>    m_reputation;

    std::vector<std::string> m_name;
    std::vector<std::vector<Relationship>> m_relationships;

    // Sensory and motor buffers, one row per slot, allocated once.
    std::vector<float>    m_inputs;    // capacity * kBrainInputCount
    std::vector<float>    m_outputs;   // capacity * kBrainOutputCount
    std::vector<float>    m_signals;   // capacity * 4, emitted vocalisations
    std::vector<float>    m_reward;

    // Per-agent memory of where useful things were.
    std::vector<float>    m_memFoodX, m_memFoodY, m_memWaterX, m_memWaterY;

    SexSystem sexSystem() const { return m_sexSystem; }
    void setSexSystem(SexSystem s) { m_sexSystem = s; }

    // Events produced during a tick, drained by Simulation into the event feed.
    struct PendingEvent {
        uint8_t  kind;      // maps to EventKind
        int32_t  x, y;
        uint64_t subject;
        std::string text;
    };
    std::vector<PendingEvent>& pendingEvents() { return m_pendingEvents; }

    // Neighbour iteration for stages that live outside this class. The spatial
    // index is rebuilt at the top of every tick, so a caller in a later stage is
    // reading positions that are current for this tick.
    template <typename Fn>
    void forEachNeighbour(float x, float y, float radius, Fn&& fn) const {
        m_spatial.query(x, y, radius, [&](uint32_t idx) { fn(m_liveSlots[idx]); });
    }

private:
    uint32_t allocateSlot();
    void     releaseSlot(uint32_t slot);
    void     initialiseNewborn(uint32_t slot, uint64_t tick, RngBank& rng);
    void     updateLifeStage(uint32_t slot, uint64_t tick);
    float    ageYears(uint32_t slot, uint64_t tick) const;
    void     emitEvent(uint8_t kind, int32_t x, int32_t y, uint64_t subject, std::string text);
    float    computeReward(uint32_t slot, const DriveState& before);
    void     attemptCourtship(uint32_t a, uint32_t b, RngBank& rng, uint64_t tick);

    Genetics m_genetics;
    Brains   m_brains;
    Pedigree m_pedigree;
    Inventory m_inventory;
    KnowledgeBase m_knowledge;
    Speciation m_speciation;
    std::vector<uint32_t> m_speciesCountable;   // scratch, reused
    // Insolation per world row for the current tick. Depends only on row and
    // tick, so computing it per agent was pure duplication.
    std::vector<float>    m_insolationRow;

    // Close-range neighbours, in CSR form: m_nbList[m_nbStart[k] .. m_nbStart[k+1])
    // are the slots near live agent k. Built once per tick in parallel so the
    // SERIAL act() stage does not have to run three spatial queries per agent
    // over the same radius. Positions do not move during act -- movement is
    // integrated in physics, the stage after it -- so one list is valid for the
    // whole stage.
    std::vector<uint32_t> m_nbStart;
    std::vector<uint32_t> m_nbList;
    float                 m_nbRadius = 0.0f;

    // Precomputed in act's parallel pass so its SERIAL pass never has to touch
    // the Phenotype struct. Phenotype is several cache lines wide; these are one
    // float each, and at 12,000 agents a serial loop reading three compact
    // arrays instead of one fat struct is the difference between a few cache
    // misses per agent and a dozen.
    std::vector<float>    m_eatMaxTake;      // eatRate * Out_Eat * Size
    std::vector<float>    m_eatAppetite;     // remaining storage, in kg of forage
    std::vector<float>    m_eatEnergyPerKg;  // energyPerKg * DigestiveEfficiency
    std::vector<float>    m_attackPower;     // attackDamage * Strength * Out_Attack
    SpatialHash m_spatial;
    SexSystem m_sexSystem = SexSystem::XY;

    std::vector<uint32_t> m_freeSlots;
    std::vector<uint32_t> m_liveSlots;
    std::vector<uint32_t> m_pendingDeath;
    std::unordered_map<uint64_t, uint32_t> m_uidToSlot;

    // God-mode attraction overrides: (observerSlot, targetUid) -> forced value.
    std::unordered_map<uint64_t, float> m_attractionOverride;

    std::vector<PendingEvent> m_pendingEvents;
    // Scratch owned here rather than function-local statics, so nothing in the
    // tick allocates and nothing is shared between instances.
    std::vector<float>        m_spatialX, m_spatialY;
    std::vector<std::vector<uint32_t>> m_chunkDeaths;
    std::vector<uint32_t>     m_geneticsSample;

    size_t   m_capacity = 0;
    uint32_t m_population = 0;
    uint64_t m_nextUid = 1;
    PopulationStats m_stats;
    Genetics::PopulationGenetics m_popGenetics;
    double   m_fst = 0.0;
    uint64_t m_totalBirths = 0, m_totalDeaths = 0;
    double   m_lifespanSum = 0.0;
    uint64_t m_lifespanCount = 0;
};

}  // namespace gen
