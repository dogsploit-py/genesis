// sim/world.h — the tile grid and the physical environment.
//
// Structure-of-Arrays throughout: every field is its own contiguous vector,
// indexed by tileIndex = y * width + x. Arrays are sized once at creation and
// never reallocated, so no tick ever allocates. See ARCHITECTURE.md §2.1 for
// the per-tile memory budget (~64 bytes/tile => 67 MB at the 1024x1024 default).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/jobs.h"
#include "core/profiler.h"
#include "core/rng.h"
#include "sim/time.h"

namespace gen {

class BinaryWriter;
class BinaryReader;

constexpr int kMaxStrata = 6;

// Real rock types, because the geology drives the chemistry in M6: limestone
// calcines to lime, sandstone is the silica source for glass, shale hosts the
// hydrocarbons, and the ore each rock can host differs.
enum class RockType : uint8_t {
    Granite = 0,   // felsic intrusive; hosts hydrothermal Sn/W veins
    Basalt,        // mafic extrusive; hosts Cu, Ni
    Limestone,     // marine sedimentary; CaCO3, flux and lime source
    Sandstone,     // clastic sedimentary; SiO2 source
    Shale,         // fine clastic; hydrocarbon source rock
    Gneiss,        // metamorphic basement
    Marble,        // metamorphosed limestone
    Coal,          // ancient swamp strata
    Alluvium,      // unconsolidated river deposit; hosts placers
    Ice,
    Count
};

enum class OreType : uint8_t {
    None = 0,
    Hematite,      // Fe2O3, sedimentary banded iron
    Magnetite,     // Fe3O4
    Malachite,     // Cu2CO3(OH)2, oxidised copper, the easy first smelt
    Chalcopyrite,  // CuFeS2, sulfidic copper
    Cassiterite,   // SnO2, tin -- the bronze bottleneck
    Galena,        // PbS, lead + silver
    Sphalerite,    // ZnS
    Bauxite,       // Al oxide, useless without electricity
    NativeGold,
    NativeCopper,
    Pyrolusite,    // MnO2
    Chromite,      // FeCr2O4
    Wolframite,    // W
    Uraninite,     // UO2
    Cinnabar,      // HgS
    Rocksalt,      // NaCl, the chlor-alkali feedstock
    Sulfur,        // native S, gunpowder and acids
    Niter,         // KNO3, gunpowder
    Clay,          // pottery and refractories
    Count
};

enum class Biome : uint8_t {
    Ocean = 0, DeepOcean, Lake, River, Beach, Ice, Tundra, BorealForest,
    TemperateForest, TemperateGrassland, Shrubland, Savanna, TropicalForest,
    Desert, Alpine, Wetland, Count
};

const char* rockName(RockType r);
const char* oreName(OreType o);
const char* biomeName(Biome b);
// Elemental payload of an ore, for the M6 chemistry engine to reduce.
const char* oreFormula(OreType o);

struct WorldParams {
    int      width = 1024;
    int      height = 1024;
    float    tileMetres = 50.0f;
    float    seaLevel = 0.0f;
    float    latitudeSpan = 120.0f;
    uint64_t seed = 0;
};

// Aggregate readouts recomputed on the telemetry cadence, cheap to copy into
// the render snapshot.
struct WorldStats {
    double meanTemperature = 0.0;
    double meanRainfall = 0.0;
    double totalBiomass = 0.0;
    double landFraction = 0.0;
    double iceFraction = 0.0;
    double meanSoilFertility = 0.0;
    double totalWaterVolume = 0.0;
};

class World {
public:
    World() = default;

    // Allocates the tile arrays and runs procedural generation. `progress` is
    // called with 0..1 and a stage name so the UI can show a generation bar.
    void generate(const WorldParams& params, RngBank& rng, JobSystem& jobs,
                  void (*progress)(float, const char*, void*) = nullptr,
                  void* progressUser = nullptr);

    // One environment step for tick `tick`. Most sub-steps do nothing on most
    // ticks -- see the schedule in ARCHITECTURE.md §4. `lastStage` receives the
    // name of the most expensive stage that actually ran, for the bottleneck
    // readout in the UI.
    void step(uint64_t tick, RngBank& rng, JobSystem& jobs, Profiler& prof);

    void recomputeStats(JobSystem& jobs);

    // Global climate forcing imposed by god mode and by persistent disasters
    // (ice age, volcanic winter, drought). Applied on top of the physical
    // model rather than replacing it, so latitude, season and lapse rate all
    // still do their work underneath.
    void setClimateOverride(float temperatureOffsetC, float rainfallMultiplier) {
        m_temperatureOffset = temperatureOffsetC;
        m_rainfallMultiplier = rainfallMultiplier;
    }
    float temperatureOffset() const { return m_temperatureOffset; }
    float rainfallMultiplier() const { return m_rainfallMultiplier; }

    // -- geometry ------------------------------------------------------------
    int    width()  const { return m_params.width; }
    int    height() const { return m_params.height; }
    size_t tileCount() const { return m_tileCount; }
    const WorldParams& params() const { return m_params; }
    bool   valid() const { return m_tileCount > 0; }

    size_t index(int x, int y) const {
        return static_cast<size_t>(y) * static_cast<size_t>(m_params.width) + static_cast<size_t>(x);
    }
    bool inBounds(int x, int y) const {
        return x >= 0 && y >= 0 && x < m_params.width && y < m_params.height;
    }
    // Latitude of a row in degrees, +ve north. Row 0 is the northern edge.
    float latitudeOfRow(int y) const;

    // -- tile fields (public by design: this is a data-oriented store, and
    //    hiding contiguous arrays behind accessors would defeat the point) ---
    std::vector<float>   elevation;     // m relative to sea level
    std::vector<float>   waterDepth;    // m of standing water
    std::vector<float>   temperature;   // deg C, current
    std::vector<float>   rainfall;      // mm/yr climatological
    std::vector<float>   soilMoisture;  // 0..1
    std::vector<float>   waterTable;    // m below surface
    std::vector<float>   flowAccum;     // upstream contributing tiles
    std::vector<float>   biomass;       // kg plant matter
    std::vector<float>   sediment;      // loose material, erosion scratch
    std::vector<int32_t> flowDir;       // downstream neighbour index, -1 = sink
    std::vector<uint8_t> soilN, soilP, soilK;
    std::vector<uint8_t> biome;
    std::vector<uint8_t> oreType;
    std::vector<uint8_t> oreGrade;      // 0..255 -> mass fraction
    std::vector<uint8_t> strataRock[kMaxStrata];
    std::vector<uint16_t> strataThick[kMaxStrata];  // decimetres

    const WorldStats& stats() const { return m_stats; }

    bool isLand(size_t i) const { return elevation[i] > m_params.seaLevel; }
    bool isWater(size_t i) const { return waterDepth[i] > 0.01f; }

    // Insolation multiplier at a row for the current tick: latitude, axial tilt
    // and time of day combined. 0 at night, peaks at local noon in summer.
    double insolation(int y, uint64_t tick) const;

    void serialize(BinaryWriter& w) const;
    bool deserialize(BinaryReader& r, uint64_t chunkTag);

private:
    void allocate(int w, int h);

    // Generation stages, in order.
    void genTectonics(RngBank& rng, JobSystem& jobs);
    void genBaseTerrain(RngBank& rng, JobSystem& jobs);
    void genErosion(RngBank& rng, JobSystem& jobs);
    void genHydrology(JobSystem& jobs);
    void genClimate(JobSystem& jobs, uint64_t tick);
    void genStrata(RngBank& rng, JobSystem& jobs);
    void genOre(RngBank& rng, JobSystem& jobs);
    void genBiomes(JobSystem& jobs);
    void genInitialBiomass(RngBank& rng, JobSystem& jobs);

    // Per-tick / scheduled environment stages.
    void stepThermal(uint64_t tick, JobSystem& jobs);
    void stepHydrology(JobSystem& jobs);
    void stepEcology(RngBank& rng, JobSystem& jobs);
    void stepWeather(uint64_t tick, JobSystem& jobs);
    void stepGeology(RngBank& rng, JobSystem& jobs);

    WorldParams m_params;
    float       m_temperatureOffset = 0.0f;
    float       m_rainfallMultiplier = 1.0f;
    size_t      m_tileCount = 0;
    WorldStats  m_stats;

    // Plate seed points, kept after generation so the UI can show plate
    // boundaries and so long-timescale tectonics can keep acting on them.
    struct Plate { float cx, cy, vx, vy; uint8_t oceanic; };
    std::vector<Plate> m_plates;

    // Scratch buffers owned here and reused, so no stage allocates per call.
    std::vector<float>    m_scratchA;
    std::vector<float>    m_scratchB;
    std::vector<uint32_t> m_sortedByElevation;  // for flow routing
};

}  // namespace gen
