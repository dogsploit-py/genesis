// sim/worldgen.cpp — procedural world generation.
//
// The generation order follows the real causal chain, because that is what
// makes the result internally consistent rather than merely plausible-looking:
//
//   plates -> topography -> erosion -> drainage -> climate -> strata -> ore -> biomes
//
// Ore is emplaced LAST among the physical layers and by process, not by
// sprinkling: hydrothermal veins follow plate-boundary faults, banded iron sits
// in shallow marine sediments, placers accumulate in alluvium downstream of the
// granite that sheds them, evaporites need an arid endorheic basin. That is
// what makes prospecting in M6 a real skill rather than a lottery.
#include <algorithm>
#include <cmath>
#include <vector>

#include "core/config.h"
#include "core/noise.h"
#include "sim/world.h"

namespace gen {

namespace {
constexpr float kPi = 3.14159265358979f;

struct ProgressReporter {
    void (*fn)(float, const char*, void*) = nullptr;
    void* user = nullptr;
    void operator()(float t, const char* stage) const { if (fn) fn(t, stage, user); }
};
ProgressReporter g_progress;
}  // namespace

// ---------------------------------------------------------------------------

void World::generate(const WorldParams& params, RngBank& rng, JobSystem& jobs,
                     void (*progress)(float, const char*, void*), void* progressUser) {
    g_progress.fn = progress;
    g_progress.user = progressUser;

    m_params = params;
    g_progress(0.00f, "Allocating tile arrays");
    allocate(params.width, params.height);

    g_progress(0.05f, "Seeding tectonic plates");
    genTectonics(rng, jobs);

    g_progress(0.20f, "Raising terrain");
    genBaseTerrain(rng, jobs);

    g_progress(0.40f, "Eroding");
    genErosion(rng, jobs);

    g_progress(0.55f, "Routing drainage");
    genHydrology(jobs);

    g_progress(0.65f, "Computing climate");
    genClimate(jobs, 0);

    g_progress(0.78f, "Laying down strata");
    genStrata(rng, jobs);

    g_progress(0.88f, "Emplacing ore deposits");
    genOre(rng, jobs);

    g_progress(0.94f, "Classifying biomes");
    genBiomes(jobs);

    g_progress(0.97f, "Seeding vegetation");
    genInitialBiomass(rng, jobs);

    recomputeStats(jobs);
    g_progress(1.00f, "Done");
    g_progress.fn = nullptr;
}

// ---------------------------------------------------------------------------
// Tectonics: Voronoi plates with velocities. Convergent boundaries build
// mountains, divergent ones open rifts, and the boundary distance field is
// reused later to place hydrothermal ore.
// ---------------------------------------------------------------------------

void World::genTectonics(RngBank& rng, JobSystem& jobs) {
    Rng& r = rng[Stream::WorldGen];
    const int plateCount = static_cast<int>(cfg().getInt("worldgen.plates", 12));
    const int W = m_params.width, H = m_params.height;

    m_plates.clear();
    m_plates.reserve(static_cast<size_t>(plateCount));
    for (int i = 0; i < plateCount; ++i) {
        Plate p;
        p.cx = r.rangef(0.0f, static_cast<float>(W));
        p.cy = r.rangef(0.0f, static_cast<float>(H));
        const float ang = r.rangef(0.0f, 2.0f * kPi);
        const float spd = r.rangef(0.3f, 1.0f);
        p.vx = std::cos(ang) * spd;
        p.vy = std::sin(ang) * spd;
        p.oceanic = (r.nextFloat() < cfg().getF("worldgen.oceanic_plate_fraction", 0.55f)) ? 1u : 0u;
        m_plates.push_back(p);
    }

    const float oceanicBase = cfg().getF("worldgen.oceanic_baseline", -3800.0f);
    const float continentalBase = cfg().getF("worldgen.continental_baseline", 700.0f);

    // For each tile: nearest plate, second-nearest plate, and the distance to
    // the boundary between them (the difference of the two distances is a good
    // cheap approximation to the true boundary distance for Voronoi cells).
    // m_scratchA <- signed boundary influence, m_scratchB <- plate baseline.
    const std::vector<Plate>& plates = m_plates;
    jobs.parallelFor(m_tileCount, [&](size_t b, size_t e, unsigned) {
        for (size_t i = b; i < e; ++i) {
            const float x = static_cast<float>(i % static_cast<size_t>(W));
            const float y = static_cast<float>(i / static_cast<size_t>(W));
            int best = 0, second = 0;
            float bestD = 1e30f, secondD = 1e30f;
            for (size_t p = 0; p < plates.size(); ++p) {
                const float dx = x - plates[p].cx;
                const float dy = y - plates[p].cy;
                const float d = dx * dx + dy * dy;
                if (d < bestD) { secondD = bestD; second = best; bestD = d; best = static_cast<int>(p); }
                else if (d < secondD) { secondD = d; second = static_cast<int>(p); }
            }
            const float d1 = std::sqrt(bestD), d2 = std::sqrt(secondD);
            const float boundaryDist = (d2 - d1) * 0.5f;

            // Convergence: are the two plates moving towards each other along
            // the line joining them? Positive = collision = uplift.
            const Plate& A = plates[static_cast<size_t>(best)];
            const Plate& B = plates[static_cast<size_t>(second)];
            float jx = B.cx - A.cx, jy = B.cy - A.cy;
            const float jl = std::sqrt(jx * jx + jy * jy);
            if (jl > 0.0001f) { jx /= jl; jy /= jl; }
            const float relative = (A.vx - B.vx) * jx + (A.vy - B.vy) * jy;

            m_scratchA[i] = boundaryDist;
            m_scratchB[i] = relative;
            // Store the plate baseline in elevation for now: oceanic plates sit
            // low, continental crust sits high, because continental crust is
            // less dense and floats higher on the mantle.
            elevation[i] = A.oceanic ? oceanicBase : continentalBase;
        }
    });
}

void World::genBaseTerrain(RngBank& rng, JobSystem& jobs) {
    Rng& r = rng[Stream::WorldGen];
    Noise base, detail, warp;
    base.seed(r);
    detail.seed(r);
    warp.seed(r);

    const int octaves = static_cast<int>(cfg().getInt("worldgen.noise_octaves", 8));
    const float lac   = cfg().getF("worldgen.noise_lacunarity", 2.0f);
    const float gain  = cfg().getF("worldgen.noise_gain", 0.5f);
    const float scale = cfg().getF("worldgen.noise_scale", 0.0025f);
    const float uplift = cfg().getF("worldgen.uplift_strength", 1800.0f);
    const float falloff = cfg().getF("worldgen.mountain_falloff", 28.0f);
    const int W = m_params.width;

    jobs.parallelFor(m_tileCount, [&](size_t b, size_t e, unsigned) {
        for (size_t i = b; i < e; ++i) {
            const float x = static_cast<float>(i % static_cast<size_t>(W)) * scale;
            const float y = static_cast<float>(i / static_cast<size_t>(W)) * scale;

            // Broad, domain-warped fBm gives the continental outline its
            // irregular, non-blobby coastline.
            const float continental = base.warpedFbm(x, y, octaves, lac, gain, 1.6f);
            // Ridged noise at higher frequency supplies the fine relief.
            const float ridges = detail.ridged(x * 3.1f, y * 3.1f, 5, lac, gain);

            const float boundaryDist = m_scratchA[i];
            const float convergence = m_scratchB[i];

            // Orogeny: uplift decays with distance from the boundary. Positive
            // convergence builds mountains, negative opens a rift valley.
            const float prox = std::exp(-boundaryDist / falloff);
            const float tectonic = uplift * convergence * prox
                                 * (0.55f + 0.45f * ridges);

            elevation[i] += continental * 900.0f + tectonic;
        }
    });

    // Match the requested land fraction by finding the elevation quantile that
    // splits it, then SHIFTING the whole field so that quantile lands exactly
    // on the configured sea level. Doing it this way rather than by tuning the
    // noise means the target is hit exactly regardless of the noise parameters,
    // and it leaves `elevation` meaning literally "metres relative to sea
    // level" -- which is what the field is documented to hold, and what the
    // lapse rate, the biome thresholds and the tile inspector all assume.
    const float wantLand = cfg().getF("worldgen.continent_fraction", 0.34f);
    {
        // 4096-bucket histogram over the observed elevation range: O(n) and
        // deterministic, where a full sort of 16M floats would not be cheap.
        float lo = 1e30f, hi = -1e30f;
        for (size_t i = 0; i < m_tileCount; ++i) {
            lo = std::min(lo, elevation[i]);
            hi = std::max(hi, elevation[i]);
        }
        if (hi <= lo) hi = lo + 1.0f;
        constexpr int kBuckets = 4096;
        std::vector<uint32_t> hist(kBuckets, 0);
        const float inv = static_cast<float>(kBuckets - 1) / (hi - lo);
        for (size_t i = 0; i < m_tileCount; ++i) {
            int bkt = static_cast<int>((elevation[i] - lo) * inv);
            if (bkt < 0) bkt = 0;
            if (bkt >= kBuckets) bkt = kBuckets - 1;
            ++hist[static_cast<size_t>(bkt)];
        }
        const uint64_t wantBelow =
            static_cast<uint64_t>(static_cast<double>(m_tileCount) * (1.0 - wantLand));
        uint64_t running = 0;
        int cut = kBuckets - 1;
        for (int k = 0; k < kBuckets; ++k) {
            running += hist[static_cast<size_t>(k)];
            if (running >= wantBelow) { cut = k; break; }
        }
        const float quantile = lo + static_cast<float>(cut) / inv;
        // Shift so `quantile` becomes m_params.seaLevel (0 by default). The
        // configured sea level is preserved rather than overwritten, so raising
        // it later floods the world instead of silently redefining the datum.
        const float shift = quantile - m_params.seaLevel;
        jobs.parallelFor(m_tileCount, [&](size_t b, size_t e, unsigned) {
            for (size_t i = b; i < e; ++i) elevation[i] -= shift;
        });
    }

    // Fill the oceans and deepen the abyssal plains a little for contrast.
    const float sea = m_params.seaLevel;
    jobs.parallelFor(m_tileCount, [&](size_t b, size_t e, unsigned) {
        for (size_t i = b; i < e; ++i) {
            waterDepth[i] = (elevation[i] < sea) ? (sea - elevation[i]) : 0.0f;
        }
    });
}

void World::genErosion(RngBank& rng, JobSystem& jobs) {
    (void)rng;
    const int passes = static_cast<int>(cfg().getInt("worldgen.erosion_passes", 60));
    if (passes <= 0) return;
    const float rate = cfg().getF("worldgen.erosion_rate", 0.22f);
    const float sea = m_params.seaLevel;
    const int W = m_params.width, H = m_params.height;

    for (int pass = 0; pass < passes; ++pass) {
        if ((pass & 7) == 0)
            g_progress(0.40f + 0.15f * static_cast<float>(pass) / static_cast<float>(passes),
                       "Eroding");

        // Thermal erosion: material above the angle of repose slumps downhill.
        // Read elevation, write scratch, so no chunk sees another's writes.
        jobs.parallelFor(m_tileCount, [&](size_t b, size_t e, unsigned) {
            for (size_t i = b; i < e; ++i) {
                const float h = elevation[i];
                if (h <= sea) { m_scratchA[i] = h; continue; }
                const int x = static_cast<int>(i % static_cast<size_t>(W));
                const int y = static_cast<int>(i / static_cast<size_t>(W));

                float sum = 0.0f;
                int count = 0;
                float lowest = h;
                auto sample = [&](size_t j) {
                    sum += elevation[j];
                    ++count;
                    lowest = std::min(lowest, elevation[j]);
                };
                if (x > 0)     sample(i - 1);
                if (x < W - 1) sample(i + 1);
                if (y > 0)     sample(i - static_cast<size_t>(W));
                if (y < H - 1) sample(i + static_cast<size_t>(W));
                if (count == 0) { m_scratchA[i] = h; continue; }

                const float avg = sum / static_cast<float>(count);
                const float talus = 24.0f;  // metres of relief tolerated per tile
                float nh = h;
                if (h - lowest > talus) nh = h + (avg - h) * rate;
                m_scratchA[i] = nh;
            }
        });
        elevation.swap(m_scratchA);
    }
}

// ---------------------------------------------------------------------------
// Drainage: D8 steepest descent, then flow accumulation processed in order of
// descending elevation so every tile's upstream contribution is complete
// before it is passed downstream.
// ---------------------------------------------------------------------------

void World::genHydrology(JobSystem& jobs) {
    const int W = m_params.width, H = m_params.height;
    const float sea = m_params.seaLevel;

    // D8: point each tile at its steepest downhill neighbour. Diagonals are
    // distance-corrected by 1/sqrt(2) so the choice is a true gradient.
    jobs.parallelFor(m_tileCount, [&](size_t b, size_t e, unsigned) {
        for (size_t i = b; i < e; ++i) {
            const int x = static_cast<int>(i % static_cast<size_t>(W));
            const int y = static_cast<int>(i / static_cast<size_t>(W));
            const float h = elevation[i];
            int bestIdx = -1;
            float bestSlope = 0.0f;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                    const size_t j = static_cast<size_t>(ny) * W + static_cast<size_t>(nx);
                    const float dist = (dx != 0 && dy != 0) ? 1.41421356f : 1.0f;
                    const float slope = (h - elevation[j]) / dist;
                    if (slope > bestSlope) { bestSlope = slope; bestIdx = static_cast<int>(j); }
                }
            }
            flowDir[i] = bestIdx;
            flowAccum[i] = 1.0f;
        }
    });

    // Order tiles by descending elevation using a bucket sort: O(n) and
    // deterministic (ties resolve by ascending tile index, because tiles are
    // appended to their bucket in index order).
    float lo = 1e30f, hi = -1e30f;
    for (size_t i = 0; i < m_tileCount; ++i) {
        lo = std::min(lo, elevation[i]);
        hi = std::max(hi, elevation[i]);
    }
    if (hi <= lo) hi = lo + 1.0f;
    constexpr int kBuckets = 8192;
    std::vector<uint32_t> counts(static_cast<size_t>(kBuckets) + 1, 0);
    const float inv = static_cast<float>(kBuckets - 1) / (hi - lo);
    std::vector<uint16_t> bucketOf(m_tileCount);
    for (size_t i = 0; i < m_tileCount; ++i) {
        int bkt = static_cast<int>((hi - elevation[i]) * inv);  // descending
        if (bkt < 0) bkt = 0;
        if (bkt >= kBuckets) bkt = kBuckets - 1;
        bucketOf[i] = static_cast<uint16_t>(bkt);
        ++counts[static_cast<size_t>(bkt) + 1];
    }
    for (int k = 0; k < kBuckets; ++k) counts[static_cast<size_t>(k) + 1] += counts[static_cast<size_t>(k)];
    std::vector<uint32_t> cursor(counts.begin(), counts.end() - 1);
    m_sortedByElevation.resize(m_tileCount);
    for (size_t i = 0; i < m_tileCount; ++i)
        m_sortedByElevation[cursor[bucketOf[i]]++] = static_cast<uint32_t>(i);

    // Accumulate downstream. Serial by necessity -- this is a dependency chain,
    // and forcing it parallel would be both wrong and non-deterministic.
    for (size_t k = 0; k < m_tileCount; ++k) {
        const uint32_t i = m_sortedByElevation[k];
        const int32_t d = flowDir[i];
        if (d >= 0) flowAccum[static_cast<size_t>(d)] += flowAccum[i];
    }

    // Rivers and lakes. A land tile carrying enough discharge becomes a
    // channel; an interior sink that collects discharge becomes a lake.
    const float riverThreshold = cfg().getF("worldgen.river_threshold", 220.0f);
    jobs.parallelFor(m_tileCount, [&](size_t b, size_t e, unsigned) {
        for (size_t i = b; i < e; ++i) {
            if (elevation[i] <= sea) { waterDepth[i] = sea - elevation[i]; continue; }
            if (flowDir[i] < 0 && flowAccum[i] > riverThreshold * 0.5f) {
                // Endorheic sink: pond depth grows with the discharge it traps.
                waterDepth[i] = std::min(40.0f, std::sqrt(flowAccum[i]) * 0.08f);
            } else if (flowAccum[i] > riverThreshold) {
                waterDepth[i] = std::min(8.0f, std::sqrt(flowAccum[i] / riverThreshold) * 0.5f);
            } else {
                waterDepth[i] = 0.0f;
            }
        }
    });
}

// ---------------------------------------------------------------------------
// Climate: latitude bands for the general circulation, plus a moisture-advection
// sweep along the prevailing wind that produces genuine rain shadows.
// ---------------------------------------------------------------------------

void World::genClimate(JobSystem& jobs, uint64_t tick) {
    const int W = m_params.width, H = m_params.height;
    const float sea = m_params.seaLevel;
    const float baseRain = cfg().getF("climate.base_rainfall", 900.0f);
    const float oro = cfg().getF("climate.orographic_strength", 1.4f);
    const float windFromDeg = cfg().getF("climate.prevailing_wind_deg", 270.0f);

    // Temperature first: the moisture model needs it, because warm air holds
    // more water (Clausius-Clapeyron).
    const char* dummy = nullptr;
    (void)dummy;
    stepThermal(tick, jobs);

    // Latitudinal precipitation: wet at the equator (ITCZ), dry at ~30 degrees
    // (subtropical high), wet at ~60 (polar front), dry at the poles.
    jobs.parallelFor(m_tileCount, [&](size_t b, size_t e, unsigned) {
        for (size_t i = b; i < e; ++i) {
            const int y = static_cast<int>(i / static_cast<size_t>(W));
            const float lat = std::fabs(latitudeOfRow(y));
            float band;
            if (lat < 15.0f)      band = 1.6f - 0.4f * (lat / 15.0f);
            else if (lat < 32.0f) band = 1.2f - 0.9f * ((lat - 15.0f) / 17.0f);
            else if (lat < 58.0f) band = 0.3f + 0.8f * ((lat - 32.0f) / 26.0f);
            else                  band = 1.1f - 0.8f * std::min(1.0f, (lat - 58.0f) / 32.0f);
            rainfall[i] = baseRain * std::max(0.05f, band);
        }
    });

    // Moisture advection. The wind is quantised to its dominant axis and the
    // sweep marches along it, picking up moisture over water and dropping it
    // when forced to rise. This is a real advection-condensation model, just
    // with an axis-aligned trajectory; the cost is that rain shadows form
    // perpendicular to the dominant axis rather than exactly along the wind.
    const float travelRad = (windFromDeg + 180.0f) * kPi / 180.0f;
    const float vx = std::sin(travelRad);
    const float vy = -std::cos(travelRad);
    const bool alongX = std::fabs(vx) >= std::fabs(vy);
    const int stepSign = alongX ? (vx >= 0.0f ? 1 : -1) : (vy >= 0.0f ? 1 : -1);

    const int outer = alongX ? H : W;
    const int inner = alongX ? W : H;

    // Each scan line is independent, so this parallelises cleanly over lines.
    jobs.parallelFor(static_cast<size_t>(outer), [&](size_t b, size_t e, unsigned) {
        for (size_t line = b; line < e; ++line) {
            float moisture = 1.0f;
            float prevElev = sea;
            for (int s = 0; s < inner; ++s) {
                const int t = (stepSign > 0) ? s : (inner - 1 - s);
                const int x = alongX ? t : static_cast<int>(line);
                const int y = alongX ? static_cast<int>(line) : t;
                const size_t i = static_cast<size_t>(y) * W + static_cast<size_t>(x);

                const float h = elevation[i];
                if (h <= sea) {
                    // Evaporation over water recharges the air mass, faster
                    // when it is warm.
                    const float warmth = std::max(0.0f, (temperature[i] + 5.0f) / 30.0f);
                    moisture = std::min(1.6f, moisture + 0.06f * warmth);
                    prevElev = sea;
                } else {
                    const float rise = std::max(0.0f, h - prevElev);
                    // Orographic lift wrings out moisture in proportion to the
                    // rate of ascent; descent on the lee side gives up nothing.
                    const float wrung = std::min(moisture, oro * rise * 0.0016f);
                    moisture -= wrung;
                    // Baseline rainout over land, and slow recharge from
                    // evapotranspiration.
                    const float baseDrop = moisture * 0.012f;
                    moisture -= baseDrop;
                    moisture = std::min(1.6f, moisture + 0.004f);
                    prevElev = h;
                    // Multiply the latitudinal band by what actually fell here.
                    const float delivered = (wrung + baseDrop) * 1400.0f + moisture * 260.0f;
                    rainfall[i] = rainfall[i] * 0.35f + delivered * 0.65f;
                }
                if (moisture < 0.0f) moisture = 0.0f;
            }
        }
    });

    jobs.parallelFor(m_tileCount, [&](size_t b, size_t e, unsigned) {
        for (size_t i = b; i < e; ++i) {
            if (rainfall[i] < 0.0f) rainfall[i] = 0.0f;
            if (rainfall[i] > 8000.0f) rainfall[i] = 8000.0f;
            // Prime soil moisture so the first ecology step is not starting
            // from a dead planet.
            soilMoisture[i] = std::min(1.0f, rainfall[i] / 1600.0f);
        }
    });
}

// ---------------------------------------------------------------------------
// Strata: a plausible stack, surface-first. Which rocks are present is set by
// the tectonic setting and the depositional environment, because the M6
// chemistry cares a great deal about whether there is limestone (flux, lime),
// sandstone (silica for glass) or coal (the reductant that gets you past bronze).
// ---------------------------------------------------------------------------

void World::genStrata(RngBank& rng, JobSystem& jobs) {
    Rng& r = rng[Stream::Geology];
    Noise facies;
    facies.seed(r);

    const int layers = std::min(kMaxStrata, static_cast<int>(cfg().getInt("worldgen.strata_layers", 6)));
    const int W = m_params.width;
    const float sea = m_params.seaLevel;

    // Per-tile draws must not come from a shared Rng inside a parallel loop --
    // that would be both a data race and non-deterministic. Instead the noise
    // field supplies the spatial variation and it is sampled purely.
    jobs.parallelFor(m_tileCount, [&](size_t b, size_t e, unsigned) {
        for (size_t i = b; i < e; ++i) {
            const float x = static_cast<float>(i % static_cast<size_t>(W));
            const float y = static_cast<float>(i / static_cast<size_t>(W));
            const float h = elevation[i];
            const float depthBelowSea = sea - h;
            const float f = facies.fbm(x * 0.01f, y * 0.01f, 4, 2.0f, 0.5f);
            const float f2 = facies.fbm(x * 0.03f + 100.0f, y * 0.03f - 40.0f, 3, 2.0f, 0.5f);
            const bool oceanic = h < sea;
            const bool shallowMarine = oceanic && depthBelowSea < 260.0f;
            const bool floodplain = !oceanic && flowAccum[i] > 400.0f && h - sea < 220.0f;

            RockType stack[kMaxStrata];
            uint16_t thick[kMaxStrata];

            if (oceanic) {
                // Oceanic crust: thin sediment over basalt.
                stack[0] = shallowMarine ? RockType::Limestone : RockType::Shale;
                thick[0] = static_cast<uint16_t>(200 + 600 * (f * 0.5f + 0.5f));
                stack[1] = RockType::Sandstone;
                thick[1] = static_cast<uint16_t>(300 + 900 * (f2 * 0.5f + 0.5f));
                stack[2] = RockType::Basalt;
                thick[2] = 20000;
                stack[3] = RockType::Basalt; thick[3] = 30000;
                stack[4] = RockType::Gneiss; thick[4] = 30000;
                stack[5] = RockType::Gneiss; thick[5] = 30000;
            } else if (floodplain) {
                // River valley: alluvium over the sedimentary pile.
                stack[0] = RockType::Alluvium;
                thick[0] = static_cast<uint16_t>(80 + 320 * (f * 0.5f + 0.5f));
                stack[1] = RockType::Sandstone;
                thick[1] = static_cast<uint16_t>(400 + 1200 * (f2 * 0.5f + 0.5f));
                stack[2] = RockType::Shale;
                thick[2] = static_cast<uint16_t>(600 + 1600 * (f * 0.5f + 0.5f));
                // Coal needs an ancient swamp: low, wet, and buried. Using the
                // present-day wetness as a proxy for the palaeo-environment.
                stack[3] = (f2 > 0.25f && rainfall[i] > 900.0f) ? RockType::Coal : RockType::Shale;
                thick[3] = static_cast<uint16_t>(60 + 340 * (f2 * 0.5f + 0.5f));
                stack[4] = RockType::Limestone;
                thick[4] = static_cast<uint16_t>(1000 + 3000 * (f * 0.5f + 0.5f));
                stack[5] = RockType::Granite;
                thick[5] = 40000;
            } else if (h - sea > 1400.0f) {
                // Uplifted core: metamorphic and intrusive rock at the surface,
                // because the sedimentary cover has been stripped off.
                stack[0] = (f > 0.0f) ? RockType::Gneiss : RockType::Granite;
                thick[0] = static_cast<uint16_t>(2000 + 4000 * (f * 0.5f + 0.5f));
                stack[1] = RockType::Granite;  thick[1] = 20000;
                stack[2] = (f2 > 0.35f) ? RockType::Marble : RockType::Gneiss;
                thick[2] = static_cast<uint16_t>(800 + 2400 * (f2 * 0.5f + 0.5f));
                stack[3] = RockType::Gneiss;   thick[3] = 30000;
                stack[4] = RockType::Granite;  thick[4] = 40000;
                stack[5] = RockType::Granite;  thick[5] = 40000;
            } else {
                // Ordinary continental interior: sedimentary cover on basement.
                stack[0] = (f > 0.15f) ? RockType::Sandstone : RockType::Shale;
                thick[0] = static_cast<uint16_t>(300 + 1100 * (f * 0.5f + 0.5f));
                stack[1] = (f2 > 0.0f) ? RockType::Limestone : RockType::Sandstone;
                thick[1] = static_cast<uint16_t>(600 + 2200 * (f2 * 0.5f + 0.5f));
                stack[2] = RockType::Shale;
                thick[2] = static_cast<uint16_t>(500 + 2000 * (f * 0.5f + 0.5f));
                stack[3] = (f2 < -0.3f && rainfall[i] > 800.0f) ? RockType::Coal : RockType::Sandstone;
                thick[3] = static_cast<uint16_t>(100 + 500 * (f2 * 0.5f + 0.5f));
                stack[4] = RockType::Granite;  thick[4] = 40000;
                stack[5] = RockType::Gneiss;   thick[5] = 40000;
            }

            for (int L = 0; L < kMaxStrata; ++L) {
                if (L < layers) {
                    strataRock[L][i] = static_cast<uint8_t>(stack[L]);
                    strataThick[L][i] = thick[L];
                } else {
                    strataRock[L][i] = static_cast<uint8_t>(RockType::Granite);
                    strataThick[L][i] = 0;
                }
            }

            // Soil nutrients follow the parent material and the climate: high
            // where there is biomass turnover and a fertile parent rock, leached
            // where rainfall is extreme, sparse where it is arid or frozen.
            const float leaching = std::max(0.0f, (rainfall[i] - 2200.0f) / 3000.0f);
            const float aridity = std::max(0.0f, (500.0f - rainfall[i]) / 500.0f);
            float fert = 0.55f;
            const RockType surf = stack[0];
            if (surf == RockType::Alluvium) fert += 0.30f;
            if (surf == RockType::Limestone) fert += 0.12f;
            if (surf == RockType::Basalt) fert += 0.18f;   // basalt weathers rich
            if (surf == RockType::Granite || surf == RockType::Gneiss) fert -= 0.18f;
            fert *= (1.0f - 0.6f * leaching) * (1.0f - 0.55f * aridity);
            if (oceanic) fert = 0.0f;
            fert = std::min(1.0f, std::max(0.0f, fert));

            // The three macronutrients are not perfectly correlated: N comes
            // from biological fixation, P and K from mineral weathering.
            const float nOff = facies.perlin(x * 0.02f + 7.0f, y * 0.02f + 3.0f) * 0.18f;
            const float pOff = facies.perlin(x * 0.02f - 5.0f, y * 0.02f + 11.0f) * 0.18f;
            const float kOff = facies.perlin(x * 0.02f + 2.0f, y * 0.02f - 9.0f) * 0.18f;
            auto clamp255 = [](float v) {
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                return static_cast<uint8_t>(v * 255.0f);
            };
            soilN[i] = clamp255(fert + nOff);
            soilP[i] = clamp255(fert + pOff);
            soilK[i] = clamp255(fert + kOff);
        }
    });
}

// ---------------------------------------------------------------------------
// Ore emplacement by process.
// ---------------------------------------------------------------------------

void World::genOre(RngBank& rng, JobSystem& jobs) {
    Rng& r = rng[Stream::Geology];
    Noise veinNoise, patchNoise;
    veinNoise.seed(r);
    patchNoise.seed(r);

    const float richness = cfg().getF("worldgen.ore_richness", 1.0f);
    const int W = m_params.width;
    const float sea = m_params.seaLevel;

    // m_scratchA still holds the plate-boundary distance from genTectonics; it
    // is regenerated here because erosion passes overwrote it.
    const std::vector<Plate>& plates = m_plates;
    jobs.parallelFor(m_tileCount, [&](size_t b, size_t e, unsigned) {
        for (size_t i = b; i < e; ++i) {
            const float x = static_cast<float>(i % static_cast<size_t>(W));
            const float y = static_cast<float>(i / static_cast<size_t>(W));
            float bestD = 1e30f, secondD = 1e30f;
            for (size_t p = 0; p < plates.size(); ++p) {
                const float dx = x - plates[p].cx;
                const float dy = y - plates[p].cy;
                const float d = dx * dx + dy * dy;
                if (d < bestD) { secondD = bestD; bestD = d; }
                else if (d < secondD) { secondD = d; }
            }
            m_scratchA[i] = (std::sqrt(secondD) - std::sqrt(bestD)) * 0.5f;
        }
    });

    jobs.parallelFor(m_tileCount, [&](size_t b, size_t e, unsigned) {
        for (size_t i = b; i < e; ++i) {
            const float x = static_cast<float>(i % static_cast<size_t>(W));
            const float y = static_cast<float>(i / static_cast<size_t>(W));
            const float h = elevation[i];
            const RockType surf = static_cast<RockType>(strataRock[0][i]);
            const float boundary = m_scratchA[i];
            const float relief = h - sea;

            OreType type = OreType::None;
            float grade = 0.0f;

            // Vein noise is ridged so deposits form linear trends, the way real
            // vein systems follow fracture sets rather than scattering evenly.
            const float vein = veinNoise.ridged(x * 0.045f, y * 0.045f, 4, 2.1f, 0.55f);
            const float patch = patchNoise.fbm(x * 0.012f, y * 0.012f, 4, 2.0f, 0.5f);

            // 1. Hydrothermal veins: hot fluids rising along plate-boundary
            //    faults through felsic intrusives. Tin and tungsten are
            //    granite-hosted; copper sulfides prefer the mafic side.
            const bool nearFault = boundary < 18.0f;
            if (nearFault && vein > 0.72f) {
                if (surf == RockType::Granite || surf == RockType::Gneiss) {
                    type = (patch > 0.25f) ? OreType::Cassiterite : OreType::Wolframite;
                    grade = (vein - 0.72f) * 3.0f;
                } else if (surf == RockType::Basalt) {
                    type = OreType::Chalcopyrite;
                    grade = (vein - 0.72f) * 3.2f;
                } else {
                    type = (patch > 0.1f) ? OreType::Galena : OreType::Sphalerite;
                    grade = (vein - 0.72f) * 2.6f;
                }
            }

            // 2. Oxidised copper caps. Malachite forms where a sulfide body has
            //    weathered in place -- shallow, above the water table, in a
            //    climate wet enough to oxidise it. This is the ore that gets
            //    smelted first in every real technological sequence, because it
            //    reduces at a temperature a wood fire can reach.
            if (type == OreType::None && relief > 30.0f && relief < 900.0f &&
                vein > 0.62f && patch > 0.0f && rainfall[i] > 400.0f) {
                type = (vein > 0.80f) ? OreType::NativeCopper : OreType::Malachite;
                grade = (vein - 0.62f) * 2.2f;
            }

            // 3. Banded iron: chemical sediment in shallow marine basins, later
            //    uplifted. Abundant, but useless until you can hit 1200 C.
            if (type == OreType::None &&
                (surf == RockType::Shale || surf == RockType::Sandstone ||
                 surf == RockType::Limestone) &&
                patch > 0.42f) {
                type = (patch > 0.62f) ? OreType::Magnetite : OreType::Hematite;
                grade = (patch - 0.42f) * 2.4f;
            }

            // 4. Placers: dense grains eroded out of upstream veins and dropped
            //    where the current slackens. Requires alluvium and real
            //    discharge, which is why they sit in river beds.
            if (surf == RockType::Alluvium && flowAccum[i] > 600.0f) {
                const float placer = patchNoise.perlin(x * 0.08f + 31.0f, y * 0.08f - 17.0f);
                if (placer > 0.55f) {
                    type = OreType::NativeGold;
                    grade = (placer - 0.55f) * 1.1f;
                } else if (type == OreType::None && placer > 0.30f) {
                    type = OreType::Cassiterite;  // stream tin, the easy tin
                    grade = (placer - 0.30f) * 1.4f;
                }
            }

            // 5. Evaporites: an arid basin with no outlet concentrates brine
            //    until halite and niter precipitate.
            if (type == OreType::None && relief > 0.0f && rainfall[i] < 380.0f &&
                flowDir[i] < 0 && patch > -0.2f) {
                type = (patch > 0.3f) ? OreType::Niter : OreType::Rocksalt;
                grade = 0.5f + patch * 0.4f;
            }

            // 6. Volcanic sulfur near active boundaries in basaltic terrain.
            if (type == OreType::None && nearFault && surf == RockType::Basalt &&
                relief > 0.0f && vein > 0.55f) {
                type = OreType::Sulfur;
                grade = (vein - 0.55f) * 1.6f;
            }

            // 7. Bauxite: intense tropical leaching strips everything but
            //    aluminium oxide. Common, and worthless for millennia.
            if (type == OreType::None && rainfall[i] > 2000.0f && temperature[i] > 20.0f &&
                relief > 20.0f && patch > 0.25f) {
                type = OreType::Bauxite;
                grade = (patch - 0.25f) * 1.5f;
            }

            // 8. Clay in floodplains and lake margins: the pottery and
            //    refractory feedstock, and therefore the gate to kilns.
            if (type == OreType::None && relief > 0.0f && relief < 400.0f &&
                (surf == RockType::Alluvium || surf == RockType::Shale) &&
                soilMoisture[i] > 0.35f) {
                type = OreType::Clay;
                grade = 0.4f + 0.5f * (patch * 0.5f + 0.5f);
            }

            if (h < sea) { type = OreType::None; grade = 0.0f; }

            grade *= richness;
            if (grade < 0.0f) grade = 0.0f;
            if (grade > 1.0f) grade = 1.0f;
            if (type == OreType::None) grade = 0.0f;

            oreType[i] = static_cast<uint8_t>(type);
            oreGrade[i] = static_cast<uint8_t>(grade * 255.0f);
        }
    });
}

// ---------------------------------------------------------------------------
// Biomes: Whittaker classification from mean temperature and annual rainfall.
// ---------------------------------------------------------------------------

void World::genBiomes(JobSystem& jobs) {
    const float sea = m_params.seaLevel;
    const float riverThreshold = cfg().getF("worldgen.river_threshold", 220.0f);

    jobs.parallelFor(m_tileCount, [&](size_t b, size_t e, unsigned) {
        for (size_t i = b; i < e; ++i) {
            const float h = elevation[i];
            const float t = temperature[i];
            const float p = rainfall[i];
            Biome bm;

            if (h <= sea) {
                const float depth = sea - h;
                if (t < -2.0f)        bm = Biome::Ice;
                else if (depth > 900.0f) bm = Biome::DeepOcean;
                else                  bm = Biome::Ocean;
            } else if (waterDepth[i] > 4.0f) {
                bm = Biome::Lake;
            } else if (waterDepth[i] > 0.01f && flowAccum[i] > riverThreshold) {
                bm = Biome::River;
            } else if (h - sea < 12.0f) {
                bm = Biome::Beach;
            } else if (t < -6.0f) {
                bm = Biome::Ice;
            } else if (h - sea > 2200.0f) {
                bm = Biome::Alpine;
            } else if (t < 0.0f) {
                bm = Biome::Tundra;
            } else if (soilMoisture[i] > 0.92f && flowAccum[i] > riverThreshold * 0.5f) {
                bm = Biome::Wetland;
            } else if (p < 260.0f) {
                bm = Biome::Desert;
            } else if (t < 6.0f) {
                bm = (p > 420.0f) ? Biome::BorealForest : Biome::Tundra;
            } else if (t < 19.0f) {
                if (p > 1000.0f)     bm = Biome::TemperateForest;
                else if (p > 520.0f) bm = Biome::TemperateGrassland;
                else                 bm = Biome::Shrubland;
            } else {
                if (p > 1800.0f)     bm = Biome::TropicalForest;
                else if (p > 750.0f) bm = Biome::Savanna;
                else                 bm = Biome::Shrubland;
            }
            biome[i] = static_cast<uint8_t>(bm);
        }
    });
}

void World::genInitialBiomass(RngBank& rng, JobSystem& jobs) {
    (void)rng;
    const float maxBio = cfg().getF("ecology.plant_max_biomass", 400.0f);
    const float sea = m_params.seaLevel;

    jobs.parallelFor(m_tileCount, [&](size_t b, size_t e, unsigned) {
        for (size_t i = b; i < e; ++i) {
            if (elevation[i] <= sea) { biomass[i] = 0.0f; continue; }
            // Start each tile at the standing crop its biome can plausibly hold,
            // so the first simulated year is not a global recolonisation event.
            float frac;
            switch (static_cast<Biome>(biome[i])) {
                case Biome::TropicalForest:     frac = 0.95f; break;
                case Biome::TemperateForest:    frac = 0.80f; break;
                case Biome::BorealForest:       frac = 0.55f; break;
                case Biome::Wetland:            frac = 0.70f; break;
                case Biome::Savanna:            frac = 0.45f; break;
                case Biome::TemperateGrassland: frac = 0.40f; break;
                case Biome::Shrubland:          frac = 0.25f; break;
                case Biome::Tundra:             frac = 0.12f; break;
                case Biome::Alpine:             frac = 0.08f; break;
                case Biome::Beach:              frac = 0.06f; break;
                case Biome::Desert:             frac = 0.03f; break;
                default:                        frac = 0.0f;  break;
            }
            biomass[i] = maxBio * frac;
        }
    });
}

}  // namespace gen
