#include "sim/world.h"

#include <algorithm>
#include <cmath>

#include "core/config.h"
#include "core/serialize.h"

namespace gen {

// ---------------------------------------------------------------------------
// Names and reference data
// ---------------------------------------------------------------------------

const char* rockName(RockType r) {
    switch (r) {
        case RockType::Granite:   return "Granite";
        case RockType::Basalt:    return "Basalt";
        case RockType::Limestone: return "Limestone";
        case RockType::Sandstone: return "Sandstone";
        case RockType::Shale:     return "Shale";
        case RockType::Gneiss:    return "Gneiss";
        case RockType::Marble:    return "Marble";
        case RockType::Coal:      return "Coal";
        case RockType::Alluvium:  return "Alluvium";
        case RockType::Ice:       return "Ice";
        case RockType::Count:     break;
    }
    return "?";
}

const char* oreName(OreType o) {
    switch (o) {
        case OreType::None:         return "none";
        case OreType::Hematite:     return "Hematite";
        case OreType::Magnetite:    return "Magnetite";
        case OreType::Malachite:    return "Malachite";
        case OreType::Chalcopyrite: return "Chalcopyrite";
        case OreType::Cassiterite:  return "Cassiterite";
        case OreType::Galena:       return "Galena";
        case OreType::Sphalerite:   return "Sphalerite";
        case OreType::Bauxite:      return "Bauxite";
        case OreType::NativeGold:   return "Native gold";
        case OreType::NativeCopper: return "Native copper";
        case OreType::Pyrolusite:   return "Pyrolusite";
        case OreType::Chromite:     return "Chromite";
        case OreType::Wolframite:   return "Wolframite";
        case OreType::Uraninite:    return "Uraninite";
        case OreType::Cinnabar:     return "Cinnabar";
        case OreType::Rocksalt:     return "Rock salt";
        case OreType::Sulfur:       return "Native sulfur";
        case OreType::Niter:        return "Niter";
        case OreType::Clay:         return "Clay";
        case OreType::Count:        break;
    }
    return "?";
}

// The chemistry engine in M6 reduces these formulae; they are stored here so
// the mineralogy and the chemistry cannot drift apart.
const char* oreFormula(OreType o) {
    switch (o) {
        case OreType::None:         return "";
        case OreType::Hematite:     return "Fe2O3";
        case OreType::Magnetite:    return "Fe3O4";
        case OreType::Malachite:    return "Cu2CO3(OH)2";
        case OreType::Chalcopyrite: return "CuFeS2";
        case OreType::Cassiterite:  return "SnO2";
        case OreType::Galena:       return "PbS";
        case OreType::Sphalerite:   return "ZnS";
        case OreType::Bauxite:      return "Al2O3.2H2O";
        case OreType::NativeGold:   return "Au";
        case OreType::NativeCopper: return "Cu";
        case OreType::Pyrolusite:   return "MnO2";
        case OreType::Chromite:     return "FeCr2O4";
        case OreType::Wolframite:   return "(Fe,Mn)WO4";
        case OreType::Uraninite:    return "UO2";
        case OreType::Cinnabar:     return "HgS";
        case OreType::Rocksalt:     return "NaCl";
        case OreType::Sulfur:       return "S8";
        case OreType::Niter:        return "KNO3";
        case OreType::Clay:         return "Al2Si2O5(OH)4";
        case OreType::Count:        break;
    }
    return "";
}

const char* biomeName(Biome b) {
    switch (b) {
        case Biome::Ocean:              return "Ocean";
        case Biome::DeepOcean:          return "Deep ocean";
        case Biome::Lake:               return "Lake";
        case Biome::River:              return "River";
        case Biome::Beach:              return "Beach";
        case Biome::Ice:                return "Ice";
        case Biome::Tundra:             return "Tundra";
        case Biome::BorealForest:       return "Boreal forest";
        case Biome::TemperateForest:    return "Temperate forest";
        case Biome::TemperateGrassland: return "Grassland";
        case Biome::Shrubland:          return "Shrubland";
        case Biome::Savanna:            return "Savanna";
        case Biome::TropicalForest:     return "Tropical forest";
        case Biome::Desert:             return "Desert";
        case Biome::Alpine:             return "Alpine";
        case Biome::Wetland:            return "Wetland";
        case Biome::Count:              break;
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------

void World::allocate(int w, int h) {
    m_params.width = w;
    m_params.height = h;
    m_tileCount = static_cast<size_t>(w) * static_cast<size_t>(h);
    const size_t n = m_tileCount;

    elevation.assign(n, 0.0f);
    waterDepth.assign(n, 0.0f);
    temperature.assign(n, 15.0f);
    rainfall.assign(n, 0.0f);
    soilMoisture.assign(n, 0.0f);
    waterTable.assign(n, 0.0f);
    flowAccum.assign(n, 1.0f);
    biomass.assign(n, 0.0f);
    sediment.assign(n, 0.0f);
    flowDir.assign(n, -1);
    soilN.assign(n, 0);
    soilP.assign(n, 0);
    soilK.assign(n, 0);
    biome.assign(n, static_cast<uint8_t>(Biome::Ocean));
    oreType.assign(n, static_cast<uint8_t>(OreType::None));
    oreGrade.assign(n, 0);
    for (int i = 0; i < kMaxStrata; ++i) {
        strataRock[i].assign(n, static_cast<uint8_t>(RockType::Granite));
        strataThick[i].assign(n, 0);
    }

    m_scratchA.assign(n, 0.0f);
    m_scratchB.assign(n, 0.0f);
    m_sortedByElevation.assign(n, 0);
}

float World::latitudeOfRow(int y) const {
    if (m_params.height <= 1) return 0.0f;
    const float t = static_cast<float>(y) / static_cast<float>(m_params.height - 1);
    // Row 0 is the northern edge, so latitude runs from +span/2 down to -span/2.
    return m_params.latitudeSpan * (0.5f - t);
}

// ---------------------------------------------------------------------------
// Solar geometry
// ---------------------------------------------------------------------------

double World::insolation(int y, uint64_t tick) const {
    const double kDeg2Rad = 3.14159265358979323846 / 180.0;
    const double tilt = cfg().getFloat("climate.axial_tilt", 23.44);
    const double lat = static_cast<double>(latitudeOfRow(y)) * kDeg2Rad;

    // Solar declination: the sub-solar latitude, swinging +/- the axial tilt
    // over the year. The 0.125 phase offset puts the northern summer solstice
    // in the middle of the months the calendar calls Summer, so the season name
    // and the solar geometry agree.
    const double phase = yearPhase(tick);
    const double decl = tilt * kDeg2Rad *
                        std::sin(2.0 * 3.14159265358979323846 * (phase - 0.125));

    // Hour angle: 0 at local solar noon, +/-pi at midnight.
    const double h = (dayPhase(tick) - 0.5) * 2.0 * 3.14159265358979323846;

    // Cosine of the solar zenith angle. Negative means the sun is below the
    // horizon, which is night; clamped to zero rather than folded.
    const double cosZ = std::sin(lat) * std::sin(decl)
                      + std::cos(lat) * std::cos(decl) * std::cos(h);
    return (cosZ > 0.0) ? cosZ : 0.0;
}

// ---------------------------------------------------------------------------
// Scheduled environment step
// ---------------------------------------------------------------------------

void World::step(uint64_t tick, RngBank& rng, JobSystem& jobs, Profiler& prof) {
    if (!valid()) return;

    const uint64_t thermalP = static_cast<uint64_t>(cfg().getInt("env.thermal_period_ticks", 6));
    const uint64_t hydroP   = static_cast<uint64_t>(cfg().getInt("env.hydrology_period_ticks", 24));
    const uint64_t ecoP     = static_cast<uint64_t>(cfg().getInt("env.ecology_period_ticks", 24));
    const uint64_t weathP   = static_cast<uint64_t>(cfg().getInt("env.weather_period_ticks", 720));
    const uint64_t geoP     = static_cast<uint64_t>(cfg().getInt("env.geology_period_ticks", 8640));

    // Each sub-stage is timed separately. They run at their own natural
    // timescales (SS4), so most of them do nothing on most ticks -- which the
    // profiler shows as a small mean over a large peak rather than as a lie.
    if (thermalP && tick % thermalP == 0) {
        ScopedStage g(prof, Stage::Thermal); stepThermal(tick, jobs);
    }
    if (hydroP && tick % hydroP == 0) {
        ScopedStage g(prof, Stage::Hydrology); stepHydrology(jobs);
    }
    if (ecoP && tick % ecoP == 0) {
        ScopedStage g(prof, Stage::Ecology); stepEcology(rng, jobs);
    }
    if (weathP && tick % weathP == 0) {
        ScopedStage g(prof, Stage::Weather); stepWeather(tick, jobs);
    }
    if (geoP && tick % geoP == 0) {
        ScopedStage g(prof, Stage::Geology); stepGeology(rng, jobs);
    }
}

void World::stepThermal(uint64_t tick, JobSystem& jobs) {
    const float base      = cfg().getF("climate.base_temperature", 14.0f);
    const float lapse     = cfg().getF("climate.lapse_rate", 6.5f);
    const float seasonAmp = cfg().getF("climate.seasonal_amplitude", 12.0f);
    const float diurnal   = cfg().getF("climate.diurnal_amplitude", 6.0f);
    const float oceanMod  = cfg().getF("climate.ocean_moderation", 0.55f);
    const float gradient  = cfg().getF("climate.pole_gradient", 40.0f);
    const float sea       = m_params.seaLevel;
    const int   W = m_params.width;
    const int   H = m_params.height;

    // Equilibrium temperature per row, then relaxation towards it plus lateral
    // diffusion. Doing the row term once per row rather than per tile saves a
    // sin/cos pair per tile, which dominates this stage otherwise.
    const double phase = yearPhase(tick);
    const float seasonPhase =
        std::sin(2.0f * 3.14159265f * (static_cast<float>(phase) - 0.125f));

    std::vector<float> rowEquilibrium(static_cast<size_t>(H));
    for (int y = 0; y < H; ++y) {
        const float latRad = latitudeOfRow(y) * 3.14159265f / 180.0f;
        const float sinLat = std::sin(latRad);
        const float sin2 = sinLat * sinLat;

        // Mean annual profile. The area average of sin^2(latitude) over a
        // sphere is exactly 1/3, so writing the profile as
        //     T(lat) = base + gradient * (1/3 - sin^2 lat)
        // makes the global area mean come out at `base` by construction, with
        // `gradient` as the equator-to-pole contrast. At the defaults that is
        // +27 C at the equator and -13 C at the pole, against a 14 C mean.
        const float mean = base + gradient * (0.3333333f - sin2);

        // Seasonal term: zero at the equator, maximal at the poles, and
        // opposite in the two hemispheres because sin(lat) changes sign.
        const float seasonal = seasonAmp * sinLat * seasonPhase;

        // Diurnal term from the instantaneous solar elevation, referenced to
        // the daily-mean insolation so it averages out over a day.
        const double sun = insolation(y, tick);
        const float diurnalTerm = diurnal * (static_cast<float>(sun) - 0.35f);

        rowEquilibrium[static_cast<size_t>(y)] =
            mean + seasonal + diurnalTerm + m_temperatureOffset;
    }

    // Relaxation rate per thermal step. Water has ~4x the volumetric heat
    // capacity of rock, so it is relaxed more slowly -- this is what makes
    // coasts mild and continental interiors extreme.
    const float rockRate = 0.35f;
    const float waterRate = rockRate * (1.0f - oceanMod);

    jobs.parallelFor(m_tileCount, [&](size_t b, size_t e, unsigned) {
        for (size_t i = b; i < e; ++i) {
            const int y = static_cast<int>(i / static_cast<size_t>(W));
            float target = rowEquilibrium[static_cast<size_t>(y)];
            const float elev = elevation[i];
            if (elev > sea) target -= (elev - sea) * (lapse * 0.001f);
            const bool water = waterDepth[i] > 0.01f;
            const float rate = water ? waterRate : rockRate;
            m_scratchA[i] = temperature[i] + (target - temperature[i]) * rate;
        }
    });

    // Lateral conduction, one Jacobi pass. Reads m_scratchA, writes temperature,
    // so no tile ever reads a value another chunk wrote this pass.
    const float diffuse = 0.12f;
    jobs.parallelFor(m_tileCount, [&](size_t b, size_t e, unsigned) {
        for (size_t i = b; i < e; ++i) {
            const int x = static_cast<int>(i % static_cast<size_t>(W));
            const int y = static_cast<int>(i / static_cast<size_t>(W));
            const float c = m_scratchA[i];
            float sum = 0.0f;
            int count = 0;
            if (x > 0)     { sum += m_scratchA[i - 1]; ++count; }
            if (x < W - 1) { sum += m_scratchA[i + 1]; ++count; }
            if (y > 0)     { sum += m_scratchA[i - static_cast<size_t>(W)]; ++count; }
            if (y < H - 1) { sum += m_scratchA[i + static_cast<size_t>(W)]; ++count; }
            const float avg = (count > 0) ? sum / static_cast<float>(count) : c;
            temperature[i] = c + (avg - c) * diffuse;
        }
    });
}

void World::stepHydrology(JobSystem& jobs) {
    const float sea = m_params.seaLevel;

    jobs.parallelFor(m_tileCount, [&](size_t b, size_t e, unsigned) {
        for (size_t i = b; i < e; ++i) {
            if (elevation[i] <= sea) {
                // Ocean tiles are saturated by definition.
                soilMoisture[i] = 1.0f;
                waterTable[i] = 0.0f;
                continue;
            }
            // Daily precipitation share of the annual total, in metres.
            const float dailyRain = rainfall[i] * m_rainfallMultiplier / 365.0f * 0.001f;

            // Potential evapotranspiration rises steeply with temperature; a
            // simple exponential response is enough to separate a hot desert
            // from a cold one at the same rainfall.
            const float t = temperature[i];
            const float pet = 0.0012f * std::exp(0.06f * (t - 10.0f));

            float m = soilMoisture[i];
            m += dailyRain * 4.0f;                 // rain wets the top horizon fast
            m -= pet * (0.3f + 0.7f * m);          // evaporation scales with wetness
            // Standing water on the tile keeps the soil wet regardless.
            if (waterDepth[i] > 0.01f) m = 1.0f;
            if (m < 0.0f) m = 0.0f;
            if (m > 1.0f) m = 1.0f;
            soilMoisture[i] = m;

            // Water table depth: deep where dry and high above the drainage
            // network, shallow in valleys with large upstream contribution.
            const float wetness = m * 0.5f + std::min(1.0f, flowAccum[i] / 500.0f) * 0.5f;
            waterTable[i] = (1.0f - wetness) * 30.0f;
        }
    });
}

void World::stepEcology(RngBank& rng, JobSystem& jobs) {
    (void)rng;  // seed dispersal randomness enters in M2 with mobile seeds
    const float growth   = cfg().getF("ecology.plant_growth_rate", 0.06f);
    const float maxBio   = cfg().getF("ecology.plant_max_biomass", 400.0f);
    const float optT     = cfg().getF("ecology.optimal_temperature", 22.0f);
    const float tolT     = cfg().getF("ecology.temperature_tolerance", 16.0f);
    const float uptake   = cfg().getF("ecology.nutrient_uptake", 0.35f);
    const float decay    = cfg().getF("ecology.decay_return_rate", 0.02f);
    const float sea      = m_params.seaLevel;

    jobs.parallelFor(m_tileCount, [&](size_t b, size_t e, unsigned) {
        for (size_t i = b; i < e; ++i) {
            if (elevation[i] <= sea) { biomass[i] = 0.0f; continue; }

            // Liebig's law of the minimum: growth is set by the scarcest of
            // light, water, nutrients and temperature, not by their product.
            const float dt = (temperature[i] - optT) / tolT;
            const float fT = std::exp(-dt * dt);              // Gaussian response
            const float fW = soilMoisture[i];
            const float nutrient = static_cast<float>(std::min(std::min(soilN[i], soilP[i]), soilK[i]))
                                 / 255.0f;
            const float limiting = std::min(std::min(fT, fW), nutrient);

            float bio = biomass[i];
            // Logistic growth against the tile carrying capacity.
            const float grownKg = growth * limiting * bio * (1.0f - bio / maxBio);
            // Senescence.
            const float diedKg = bio * decay;
            bio += grownKg - diedKg;
            // A seed rain floor, so an emptied tile can recolonise from
            // neighbours rather than staying dead forever.
            if (bio < 0.5f && limiting > 0.15f) bio = 0.5f;
            if (bio < 0.0f) bio = 0.0f;
            biomass[i] = bio;

            // Nutrient cycling. Uptake and return use the SAME coefficient
            // against the same mass units, so a tile at equilibrium (grown ==
            // died) has zero net nutrient change. Getting this wrong -- as an
            // earlier version did, by scaling uptake off the growth RATE rather
            // than the growth MASS -- silently strips the soil in a few months
            // and collapses the whole food web.
            const float drawn = uptake * grownKg / maxBio * 255.0f;
            const float returned = uptake * diedKg / maxBio * 255.0f;
            // Real soils are not closed: mineral weathering adds nutrients and
            // heavy rain leaches them away.
            const float weathering = 0.02f;
            const float leaching = 0.02f * std::min(1.0f, rainfall[i] / 2500.0f);
            const float delta = returned - drawn + weathering;
            auto adjust = [&](uint8_t& v) {
                float nv = static_cast<float>(v) + delta - leaching * static_cast<float>(v) / 255.0f;
                if (nv < 0.0f) nv = 0.0f;
                if (nv > 255.0f) nv = 255.0f;
                v = static_cast<uint8_t>(nv);
            };
            adjust(soilN[i]);
            adjust(soilP[i]);
            adjust(soilK[i]);
        }
    });
}

void World::stepWeather(uint64_t tick, JobSystem& jobs) {
    // Seasonal modulation of the climatological rainfall field. The spatial
    // pattern (orographic, latitudinal) is fixed at generation; what varies
    // here is the seasonal amplitude, which is what drives monsoon-like wet and
    // dry seasons and therefore the boom-bust in plant biomass.
    const double phase = yearPhase(tick);
    const float H = static_cast<float>(m_params.height);

    jobs.parallelFor(m_tileCount, [&](size_t b, size_t e, unsigned) {
        for (size_t i = b; i < e; ++i) {
            const int y = static_cast<int>(i / static_cast<size_t>(m_params.width));
            const float lat = latitudeOfRow(y);
            // Northern and southern hemispheres are half a year out of phase.
            const float hemi = (lat >= 0.0f) ? 0.0f : 0.5f;
            const float s = std::sin(2.0f * 3.14159265f *
                                     (static_cast<float>(phase) - 0.25f + hemi));
            const float seasonal = 1.0f + 0.35f * s;
            m_scratchB[i] = seasonal;
        }
        (void)H;
    });

    // Apply as a multiplier on the stored climatological field. rainfall holds
    // the annual mean; the seasonal factor is folded into soil moisture on the
    // hydrology step, so rainfall itself is left as the climatology.
    jobs.parallelFor(m_tileCount, [&](size_t b, size_t e, unsigned) {
        for (size_t i = b; i < e; ++i) {
            const float target = rainfall[i] * m_scratchB[i];
            // Relax rather than snap, so weather has persistence.
            rainfall[i] += (target - rainfall[i]) * 0.15f;
        }
    });
}

void World::stepGeology(RngBank& rng, JobSystem& jobs) {
    if (!cfg().getBool("env.enable_erosion", true)) return;
    (void)rng;

    const float sea = m_params.seaLevel;
    const int W = m_params.width;
    const int H = m_params.height;

    // Slope-limited diffusive erosion: hillslope creep plus fluvial incision
    // weighted by upstream discharge. One pass per year of simulated time.
    const float rate = cfg().getF("worldgen.erosion_rate", 0.22f) * 0.01f;

    jobs.parallelFor(m_tileCount, [&](size_t b, size_t e, unsigned) {
        for (size_t i = b; i < e; ++i) {
            if (elevation[i] <= sea) { m_scratchA[i] = elevation[i]; continue; }
            const int x = static_cast<int>(i % static_cast<size_t>(W));
            const int y = static_cast<int>(i / static_cast<size_t>(W));
            const float h = elevation[i];
            float lowest = h;
            if (x > 0)     lowest = std::min(lowest, elevation[i - 1]);
            if (x < W - 1) lowest = std::min(lowest, elevation[i + 1]);
            if (y > 0)     lowest = std::min(lowest, elevation[i - static_cast<size_t>(W)]);
            if (y < H - 1) lowest = std::min(lowest, elevation[i + static_cast<size_t>(W)]);

            const float slope = h - lowest;
            // Stream power: incision goes as discharge^m * slope^n, with the
            // usual m~0.5, n~1. Discharge is proxied by flow accumulation.
            const float discharge = std::sqrt(std::max(1.0f, flowAccum[i]));
            const float incision = rate * discharge * slope * 0.01f;
            m_scratchA[i] = h - std::min(incision, slope * 0.5f);
        }
    });

    jobs.parallelFor(m_tileCount, [&](size_t b, size_t e, unsigned) {
        for (size_t i = b; i < e; ++i) {
            const float removed = elevation[i] - m_scratchA[i];
            elevation[i] = m_scratchA[i];
            if (removed > 0.0f) sediment[i] += removed;
        }
    });

    if (cfg().getBool("env.enable_glaciation", true)) {
        jobs.parallelFor(m_tileCount, [&](size_t b, size_t e, unsigned) {
            for (size_t i = b; i < e; ++i) {
                if (temperature[i] < -4.0f && elevation[i] > sea)
                    biome[i] = static_cast<uint8_t>(Biome::Ice);
                else if (biome[i] == static_cast<uint8_t>(Biome::Ice) && temperature[i] > 1.0f)
                    biome[i] = static_cast<uint8_t>(Biome::Tundra);
            }
        });
    }

    genHydrology(jobs);  // re-route drainage over the new topography
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

void World::recomputeStats(JobSystem& jobs) {
    if (!valid()) { m_stats = WorldStats(); return; }
    const float sea = m_params.seaLevel;
    const size_t n = m_tileCount;

    // Every reduction goes through parallelReduce, which sums per-chunk
    // partials in ascending chunk order -- float addition is not associative,
    // so a racing accumulation would give a different total every run.
    const double sumT = jobs.parallelReduce<double>(n, 0.0, [&](size_t b, size_t e, unsigned) {
        double s = 0.0;
        for (size_t i = b; i < e; ++i) s += temperature[i];
        return s;
    });
    const double sumR = jobs.parallelReduce<double>(n, 0.0, [&](size_t b, size_t e, unsigned) {
        double s = 0.0;
        for (size_t i = b; i < e; ++i) s += rainfall[i];
        return s;
    });
    const double sumB = jobs.parallelReduce<double>(n, 0.0, [&](size_t b, size_t e, unsigned) {
        double s = 0.0;
        for (size_t i = b; i < e; ++i) s += biomass[i];
        return s;
    });
    const double land = jobs.parallelReduce<double>(n, 0.0, [&](size_t b, size_t e, unsigned) {
        double s = 0.0;
        for (size_t i = b; i < e; ++i) if (elevation[i] > sea) s += 1.0;
        return s;
    });
    const double ice = jobs.parallelReduce<double>(n, 0.0, [&](size_t b, size_t e, unsigned) {
        double s = 0.0;
        for (size_t i = b; i < e; ++i)
            if (biome[i] == static_cast<uint8_t>(Biome::Ice)) s += 1.0;
        return s;
    });
    const double fert = jobs.parallelReduce<double>(n, 0.0, [&](size_t b, size_t e, unsigned) {
        double s = 0.0;
        for (size_t i = b; i < e; ++i)
            s += (static_cast<double>(soilN[i]) + soilP[i] + soilK[i]) / 3.0;
        return s;
    });
    const double water = jobs.parallelReduce<double>(n, 0.0, [&](size_t b, size_t e, unsigned) {
        double s = 0.0;
        for (size_t i = b; i < e; ++i) s += waterDepth[i];
        return s;
    });

    const double dn = static_cast<double>(n);
    const double tileArea = static_cast<double>(m_params.tileMetres) * m_params.tileMetres;
    m_stats.meanTemperature = sumT / dn;
    m_stats.meanRainfall = sumR / dn;
    m_stats.totalBiomass = sumB;
    m_stats.landFraction = land / dn;
    m_stats.iceFraction = ice / dn;
    m_stats.meanSoilFertility = fert / dn / 255.0;
    m_stats.totalWaterVolume = water * tileArea;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

void World::serialize(BinaryWriter& w) const {
    // Field by field, never a struct blit: see BinaryWriter::writeHeader for
    // why (alignment padding is indeterminate and compiler-specific).
    w.beginChunk(kChunkWorld);
    w.pod(m_params.width);
    w.pod(m_params.height);
    w.pod(m_params.tileMetres);
    w.pod(m_params.seaLevel);
    w.pod(m_params.latitudeSpan);
    w.pod(m_params.seed);

    w.pod(m_stats.meanTemperature);
    w.pod(m_stats.meanRainfall);
    w.pod(m_stats.totalBiomass);
    w.pod(m_stats.landFraction);
    w.pod(m_stats.iceFraction);
    w.pod(m_stats.meanSoilFertility);
    w.pod(m_stats.totalWaterVolume);

    const uint32_t plateCount = static_cast<uint32_t>(m_plates.size());
    w.pod(plateCount);
    for (const Plate& p : m_plates) {
        w.pod(p.cx);
        w.pod(p.cy);
        w.pod(p.vx);
        w.pod(p.vy);
        w.pod(p.oceanic);
    }
    w.endChunk();

    w.beginChunk(kChunkTiles);
    w.array(elevation);
    w.array(waterDepth);
    w.array(temperature);
    w.array(rainfall);
    w.array(soilMoisture);
    w.array(waterTable);
    w.array(flowAccum);
    w.array(biomass);
    w.array(sediment);
    w.array(flowDir);
    w.array(soilN);
    w.array(soilP);
    w.array(soilK);
    w.array(biome);
    w.array(oreType);
    w.array(oreGrade);
    for (int i = 0; i < kMaxStrata; ++i) {
        w.array(strataRock[i]);
        w.array(strataThick[i]);
    }
    w.endChunk();
}

bool World::deserialize(BinaryReader& r, uint64_t chunkTag) {
    if (chunkTag == kChunkWorld) {
        r.pod(m_params.width);
        r.pod(m_params.height);
        r.pod(m_params.tileMetres);
        r.pod(m_params.seaLevel);
        r.pod(m_params.latitudeSpan);
        r.pod(m_params.seed);

        r.pod(m_stats.meanTemperature);
        r.pod(m_stats.meanRainfall);
        r.pod(m_stats.totalBiomass);
        r.pod(m_stats.landFraction);
        r.pod(m_stats.iceFraction);
        r.pod(m_stats.meanSoilFertility);
        r.pod(m_stats.totalWaterVolume);

        uint32_t plateCount = 0;
        r.pod(plateCount);
        if (plateCount > 4096) return false;
        m_plates.resize(plateCount);
        for (uint32_t i = 0; i < plateCount; ++i) {
            r.pod(m_plates[i].cx);
            r.pod(m_plates[i].cy);
            r.pod(m_plates[i].vx);
            r.pod(m_plates[i].vy);
            r.pod(m_plates[i].oceanic);
        }
        // Size the arrays now so the TILE chunk can be read straight in.
        allocate(m_params.width, m_params.height);
        return r.ok();
    }
    if (chunkTag == kChunkTiles) {
        r.array(elevation);
        r.array(waterDepth);
        r.array(temperature);
        r.array(rainfall);
        r.array(soilMoisture);
        r.array(waterTable);
        r.array(flowAccum);
        r.array(biomass);
        r.array(sediment);
        r.array(flowDir);
        r.array(soilN);
        r.array(soilP);
        r.array(soilK);
        r.array(biome);
        r.array(oreType);
        r.array(oreGrade);
        for (int i = 0; i < kMaxStrata; ++i) {
            r.array(strataRock[i]);
            r.array(strataThick[i]);
        }
        m_tileCount = elevation.size();
        m_scratchA.assign(m_tileCount, 0.0f);
        m_scratchB.assign(m_tileCount, 0.0f);
        m_sortedByElevation.assign(m_tileCount, 0);
        return r.ok();
    }
    return false;
}

}  // namespace gen
