#include "ui/viewport.h"

#include <windows.h>
#include <GL/gl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "core/config.h"
#include "imgui.h"
#include "sim/agent.h"
#include "sim/simulation.h"
#include "sim/world.h"

namespace gen {

namespace {

inline uint32_t rgba(int r, int g, int b, int a = 255) {
    auto c = [](int v) { return static_cast<uint32_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); };
    return c(r) | (c(g) << 8) | (c(b) << 16) | (c(a) << 24);
}

inline uint32_t lerpColour(uint32_t a, uint32_t b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const int ar = static_cast<int>(a & 0xFF), ag = static_cast<int>((a >> 8) & 0xFF);
    const int ab = static_cast<int>((a >> 16) & 0xFF);
    const int br = static_cast<int>(b & 0xFF), bg = static_cast<int>((b >> 8) & 0xFF);
    const int bb = static_cast<int>((b >> 16) & 0xFF);
    return rgba(static_cast<int>(ar + (br - ar) * t),
                static_cast<int>(ag + (bg - ag) * t),
                static_cast<int>(ab + (bb - ab) * t));
}

// A perceptually reasonable diverging ramp for signed quantities and a
// sequential one for magnitudes. Both are hand-picked rather than generated so
// that the mid-tones stay distinguishable on a dark background.
uint32_t rampViridis(float t) {
    static const uint32_t kStops[6] = {
        rgba(68, 1, 84), rgba(72, 40, 120), rgba(62, 74, 137),
        rgba(49, 104, 142), rgba(53, 183, 121), rgba(253, 231, 37)
    };
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const float s = t * 5.0f;
    const int i = std::min(4, static_cast<int>(s));
    return lerpColour(kStops[i], kStops[i + 1], s - static_cast<float>(i));
}

uint32_t rampThermal(float t) {
    static const uint32_t kStops[6] = {
        rgba(20, 40, 120), rgba(40, 130, 200), rgba(160, 210, 230),
        rgba(240, 225, 150), rgba(230, 130, 50), rgba(150, 20, 20)
    };
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const float s = t * 5.0f;
    const int i = std::min(4, static_cast<int>(s));
    return lerpColour(kStops[i], kStops[i + 1], s - static_cast<float>(i));
}

uint32_t rampRain(float t) {
    static const uint32_t kStops[5] = {
        rgba(120, 90, 40), rgba(200, 180, 110), rgba(120, 190, 150),
        rgba(40, 130, 180), rgba(20, 40, 120)
    };
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const float s = t * 4.0f;
    const int i = std::min(3, static_cast<int>(s));
    return lerpColour(kStops[i], kStops[i + 1], s - static_cast<float>(i));
}

uint32_t biomeColour(Biome b) {
    switch (b) {
        case Biome::DeepOcean:          return rgba(12, 28, 62);
        case Biome::Ocean:              return rgba(22, 56, 108);
        case Biome::Lake:               return rgba(44, 96, 152);
        case Biome::River:              return rgba(58, 122, 178);
        case Biome::Beach:              return rgba(196, 182, 138);
        case Biome::Ice:                return rgba(226, 234, 240);
        case Biome::Tundra:             return rgba(140, 148, 128);
        case Biome::BorealForest:       return rgba(46, 82, 60);
        case Biome::TemperateForest:    return rgba(52, 104, 54);
        case Biome::TemperateGrassland: return rgba(126, 148, 76);
        case Biome::Shrubland:          return rgba(146, 138, 88);
        case Biome::Savanna:            return rgba(168, 156, 78);
        case Biome::TropicalForest:     return rgba(30, 96, 44);
        case Biome::Desert:             return rgba(206, 178, 118);
        case Biome::Alpine:             return rgba(150, 146, 142);
        case Biome::Wetland:            return rgba(72, 110, 92);
        default:                        return rgba(255, 0, 255);
    }
}

uint32_t rockColour(RockType r) {
    switch (r) {
        case RockType::Granite:   return rgba(198, 176, 168);
        case RockType::Basalt:    return rgba(64, 66, 72);
        case RockType::Limestone: return rgba(214, 210, 186);
        case RockType::Sandstone: return rgba(206, 168, 118);
        case RockType::Shale:     return rgba(96, 100, 104);
        case RockType::Gneiss:    return rgba(158, 146, 158);
        case RockType::Marble:    return rgba(238, 236, 230);
        case RockType::Coal:      return rgba(28, 26, 26);
        case RockType::Alluvium:  return rgba(168, 146, 106);
        case RockType::Ice:       return rgba(226, 240, 250);
        default:                  return rgba(255, 0, 255);
    }
}

uint32_t oreColour(OreType o) {
    switch (o) {
        case OreType::None:         return rgba(28, 30, 34);
        case OreType::Hematite:     return rgba(150, 52, 40);
        case OreType::Magnetite:    return rgba(70, 62, 70);
        case OreType::Malachite:    return rgba(40, 168, 120);
        case OreType::Chalcopyrite: return rgba(198, 168, 52);
        case OreType::Cassiterite:  return rgba(112, 92, 72);
        case OreType::Galena:       return rgba(126, 130, 140);
        case OreType::Sphalerite:   return rgba(160, 118, 70);
        case OreType::Bauxite:      return rgba(190, 118, 80);
        case OreType::NativeGold:   return rgba(240, 200, 60);
        case OreType::NativeCopper: return rgba(200, 110, 60);
        case OreType::Pyrolusite:   return rgba(88, 80, 96);
        case OreType::Chromite:     return rgba(58, 66, 62);
        case OreType::Wolframite:   return rgba(72, 56, 60);
        case OreType::Uraninite:    return rgba(110, 190, 60);
        case OreType::Cinnabar:     return rgba(190, 40, 46);
        case OreType::Rocksalt:     return rgba(228, 228, 236);
        case OreType::Sulfur:       return rgba(226, 214, 60);
        case OreType::Niter:        return rgba(212, 220, 190);
        case OreType::Clay:         return rgba(170, 128, 108);
        default:                    return rgba(255, 0, 255);
    }
}

}  // namespace

// ---------------------------------------------------------------------------

const char* overlayName(Overlay o) {
    switch (o) {
        case Overlay::Terrain:           return "Terrain";
        case Overlay::Elevation:         return "Elevation";
        case Overlay::Temperature:       return "Temperature";
        case Overlay::Rainfall:          return "Rainfall";
        case Overlay::SoilFertility:     return "Soil nutrients";
        case Overlay::SoilMoisture:      return "Soil moisture";
        case Overlay::WaterTable:        return "Water table";
        case Overlay::Biome:             return "Biome";
        case Overlay::SurfaceRock:       return "Surface rock";
        case Overlay::Ore:               return "Ore deposits";
        case Overlay::Biomass:           return "Plant biomass";
        case Overlay::Drainage:          return "Drainage";
        case Overlay::Plates:            return "Tectonic plates";
        case Overlay::PopulationDensity: return "Population density";
        case Overlay::AlleleFrequency:   return "Allele frequency";
        case Overlay::DiseasePrevalence: return "Disease prevalence";
        case Overlay::Territory:         return "Territory";
        case Overlay::Culture:           return "Culture";
        case Overlay::Pollution:         return "Pollution";
        case Overlay::Count:             break;
    }
    return "?";
}

const char* overlayDescription(Overlay o) {
    switch (o) {
        case Overlay::Terrain:
            return "Shaded relief coloured by biome. Hillshading is computed from the\n"
                   "elevation gradient against a light from the north-west.";
        case Overlay::Elevation:
            return "Metres relative to sea level. Sea level itself is chosen at generation\n"
                   "as the elevation quantile that hits the requested land fraction.";
        case Overlay::Temperature:
            return "Current tile temperature in degrees C. Driven by latitude, solar\n"
                   "declination, time of day, the 6.5 C/km lapse rate, and the higher heat\n"
                   "capacity of water. Updated every env.thermal_period_ticks ticks.";
        case Overlay::Rainfall:
            return "Annual precipitation in mm. Latitude bands supply the general\n"
                   "circulation (wet equator, dry subtropics, wet mid-latitudes); a\n"
                   "moisture-advection sweep along the prevailing wind adds rain shadows.";
        case Overlay::SoilFertility:
            return "Mean of the N, P and K stocks. Set at generation by parent rock and\n"
                   "climate (leaching in the wet tropics, sparse in deserts), then drawn\n"
                   "down by plant growth and returned by decay.";
        case Overlay::SoilMoisture:
            return "Saturation from 0 to 1. Rainfall wets, temperature-driven\n"
                   "evapotranspiration dries. This is one of the four Liebig limiters on\n"
                   "plant growth.";
        case Overlay::WaterTable:
            return "Depth to groundwater in metres. Shallow in valleys with large upstream\n"
                   "contribution, deep on dry uplands.";
        case Overlay::Biome:
            return "Whittaker classification from mean temperature and annual rainfall,\n"
                   "with water, ice and alpine handled separately.";
        case Overlay::SurfaceRock:
            return "The topmost of the six stored strata. Determines what can be quarried,\n"
                   "what weathers to fertile soil, and what ore the tile can host.";
        case Overlay::Ore:
            return "Ore deposits, coloured by mineral and shaded by grade. Emplaced by\n"
                   "process: hydrothermal veins on plate-boundary faults, banded iron in\n"
                   "marine sediments, placers in alluvium, evaporites in arid sinks.";
        case Overlay::Biomass:
            return "Standing plant biomass in kg per tile. Grows logistically against the\n"
                   "tile carrying capacity, limited by whichever of light, water, nutrients\n"
                   "and temperature is scarcest.";
        case Overlay::Drainage:
            return "Flow accumulation: how many upstream tiles drain through each tile.\n"
                   "Computed by D8 steepest descent processed in descending elevation order.";
        case Overlay::Plates:
            return "Tectonic plate domains and their boundaries. Convergent boundaries are\n"
                   "where the mountain belts and the hydrothermal ore both come from.";
        case Overlay::PopulationDensity:
            return "Living agents per tile, on a square-root ramp. Where the population\n"
                   "actually is, which is rarely where the map looks most habitable.";
        case Overlay::AlleleFrequency:
            return "Mean expressed value of one gene, per tile, as a deviation from the\n"
                   "population mean. A NEUTRAL locus by default: a coding locus maps\n"
                   "selection, and a neutral one maps drift and gene flow -- which is what\n"
                   "shows a barrier to breeding before speciation finishes.";
        case Overlay::DiseasePrevalence:
            return "Not implemented. There is no disease model in this build, so there is\n"
                   "nothing to show and no placeholder is drawn.";
        case Overlay::Territory:
            return "Which detected lineage has the most members on each tile, in the same\n"
                   "colours the phylogeny uses. Nothing claims territory -- this is where\n"
                   "each lineage happens to be.";
        case Overlay::Culture:
            return "Mean position on the technique ladder of the agents on each tile.\n"
                   "Bright where fire, kilns and furnaces are known; dark where they are\n"
                   "not, whatever the genetics there look like.";
        case Overlay::Pollution:
            return "Not implemented. Gaseous reaction products are vented to an atmosphere\n"
                   "that is not tracked per tile, so there is no accumulation to map.";
        case Overlay::Count: break;
    }
    return "";
}

const char* overlayRequires(Overlay o) {
    switch (o) {
        // Two things in this list were never built, and saying so is more useful
        // than a greyed-out swatch with a milestone number that has already
        // shipped.
        case Overlay::DiseasePrevalence:
            return "nothing -- there is no disease model in this build at all, so there is "
                   "no prevalence to map. It is the one part of the society milestone that "
                   "was not built.";
        case Overlay::Pollution:
            return "nothing -- reactions vent their gaseous products to the air and the air "
                   "is not tracked as a per-tile field, so there is no accumulated pollution "
                   "to map.";
        default:
            return nullptr;
    }
}

void Camera::clampTo(int worldW, int worldH, double minPPT, double maxPPT) {
    if (pixelsPerTile < minPPT) pixelsPerTile = minPPT;
    if (pixelsPerTile > maxPPT) pixelsPerTile = maxPPT;
    if (centreX < 0.0) centreX = 0.0;
    if (centreY < 0.0) centreY = 0.0;
    if (centreX > worldW) centreX = worldW;
    if (centreY > worldH) centreY = worldH;
}

// ---------------------------------------------------------------------------

Viewport::Viewport() = default;

Viewport::~Viewport() { shutdown(); }

void Viewport::init() {
    // A small dedicated pool. The rasteriser must not share the simulation's
    // job system: the sim thread may be inside a parallelFor at any moment.
    unsigned n = std::thread::hardware_concurrency();
    n = (n >= 8) ? 3u : (n >= 4 ? 2u : 1u);
    m_rasterJobs.start(n);

    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Viewport::shutdown() {
    m_rasterJobs.shutdown();
    if (m_texture) {
        GLuint t = m_texture;
        glDeleteTextures(1, &t);
        m_texture = 0;
    }
}

void Viewport::resetCamera(int worldW, int worldH) {
    m_camera.centreX = worldW * 0.5;
    m_camera.centreY = worldH * 0.5;
    m_camera.pixelsPerTile = 1.0;
    m_dirty = true;
}

void Viewport::focusOn(int x, int y) {
    m_camera.centreX = x + 0.5;
    m_camera.centreY = y + 0.5;
    if (m_camera.pixelsPerTile < 4.0) m_camera.pixelsPerTile = 8.0;
    m_dirty = true;
}

uint32_t Viewport::sampleColour(const World& w, size_t i) const {
    const float sea = w.params().seaLevel;
    const bool land = w.elevation[i] > sea;

    switch (m_overlay) {
        case Overlay::Terrain: {
            uint32_t base = biomeColour(static_cast<Biome>(w.biome[i]));
            if (!land) {
                // Depth shading so the shelf reads differently from the abyss.
                const float depth = std::min(1.0f, (sea - w.elevation[i]) / 3000.0f);
                base = lerpColour(rgba(46, 98, 158), rgba(8, 18, 48), depth);
            } else {
                // Hillshade: dot the surface normal with a light from the
                // north-west. Cheap, and it is what makes relief legible.
                const int W = w.width(), H = w.height();
                const int x = static_cast<int>(i % static_cast<size_t>(W));
                const int y = static_cast<int>(i / static_cast<size_t>(W));
                const float hl = w.elevation[(x > 0) ? i - 1 : i];
                const float hr = w.elevation[(x < W - 1) ? i + 1 : i];
                const float hu = w.elevation[(y > 0) ? i - static_cast<size_t>(W) : i];
                const float hd = w.elevation[(y < H - 1) ? i + static_cast<size_t>(W) : i];
                const float dx = (hr - hl) * 0.02f;
                const float dy = (hd - hu) * 0.02f;
                float shade = 0.62f + 0.38f * (-dx - dy) / std::sqrt(dx * dx + dy * dy + 1.0f);
                shade = std::min(1.45f, std::max(0.35f, shade));
                const int r = static_cast<int>(static_cast<float>(base & 0xFF) * shade);
                const int g = static_cast<int>(static_cast<float>((base >> 8) & 0xFF) * shade);
                const int b = static_cast<int>(static_cast<float>((base >> 16) & 0xFF) * shade);
                base = rgba(r, g, b);
                // Rivers drawn on top so the drainage network is visible in the
                // default view without switching overlays.
                if (w.waterDepth[i] > 0.01f) base = lerpColour(base, rgba(70, 130, 190), 0.75f);
            }
            return base;
        }

        case Overlay::Elevation: {
            if (!land) {
                const float t = std::min(1.0f, (sea - w.elevation[i]) / 4000.0f);
                return lerpColour(rgba(70, 130, 190), rgba(4, 10, 40), t);
            }
            const float t = std::min(1.0f, (w.elevation[i] - sea) / 4000.0f);
            static const uint32_t kStops[5] = {
                rgba(60, 120, 70), rgba(150, 160, 80), rgba(150, 110, 70),
                rgba(140, 130, 125), rgba(250, 250, 255)
            };
            const float s = t * 4.0f;
            const int k = std::min(3, static_cast<int>(s));
            return lerpColour(kStops[k], kStops[k + 1], s - static_cast<float>(k));
        }

        case Overlay::Temperature:
            return rampThermal((w.temperature[i] + 40.0f) / 90.0f);

        case Overlay::Rainfall:
            return rampRain(std::min(1.0f, w.rainfall[i] / 3000.0f));

        case Overlay::SoilFertility: {
            if (!land) return rgba(18, 22, 30);
            const float f = (static_cast<float>(w.soilN[i]) + w.soilP[i] + w.soilK[i]) / (3.0f * 255.0f);
            return rampViridis(f);
        }

        case Overlay::SoilMoisture:
            return land ? rampRain(w.soilMoisture[i]) : rgba(18, 22, 30);

        case Overlay::WaterTable:
            return land ? rampViridis(1.0f - std::min(1.0f, w.waterTable[i] / 30.0f))
                        : rgba(18, 22, 30);

        case Overlay::Biome:
            return biomeColour(static_cast<Biome>(w.biome[i]));

        case Overlay::SurfaceRock:
            return land ? rockColour(static_cast<RockType>(w.strataRock[0][i]))
                        : rgba(18, 22, 30);

        case Overlay::Ore: {
            const OreType o = static_cast<OreType>(w.oreType[i]);
            if (o == OreType::None) {
                // Keep a dim terrain context so deposits can be located.
                return land ? rgba(38, 40, 44) : rgba(14, 18, 26);
            }
            const float grade = static_cast<float>(w.oreGrade[i]) / 255.0f;
            return lerpColour(rgba(40, 42, 46), oreColour(o), 0.30f + 0.70f * grade);
        }

        case Overlay::Biomass: {
            if (!land) return rgba(18, 22, 30);
            const float maxBio = cfg().getF("ecology.plant_max_biomass", 400.0f);
            return rampViridis(std::min(1.0f, w.biomass[i] / maxBio));
        }

        case Overlay::PopulationDensity: {
            if (!m_agentFieldValid) return rgba(18, 22, 30);
            const float v = (i < m_agentField.size()) ? m_agentField[i] : 0.0f;
            if (v <= 0.0f) return land ? rgba(24, 28, 34) : rgba(12, 16, 24);
            // Square-rooted, because density spans orders of magnitude between an
            // empty steppe and a crowded river bank and a linear ramp shows only
            // the crowd.
            return rampThermal(std::min(1.0f, std::sqrt(v / m_agentFieldMax)));
        }

        case Overlay::AlleleFrequency: {
            if (!m_agentFieldValid) return rgba(18, 22, 30);
            const float v = (i < m_agentField.size()) ? m_agentField[i] : -1.0f;
            if (v < 0.0f) return land ? rgba(24, 28, 34) : rgba(12, 16, 24);
            // Diverging, centred on the population mean: this overlay is about
            // where a lineage differs from the whole, so the interesting thing is
            // the sign of the deviation, not its magnitude.
            return rampThermal(v);
        }

        case Overlay::Culture: {
            if (!m_agentFieldValid) return rgba(18, 22, 30);
            const float v = (i < m_agentField.size()) ? m_agentField[i] : -1.0f;
            if (v < 0.0f) return land ? rgba(24, 28, 34) : rgba(12, 16, 24);
            return rampViridis(std::min(1.0f, v));
        }

        case Overlay::Territory: {
            if (!m_agentFieldValid) return rgba(18, 22, 30);
            const uint32_t sid = (i < m_tileSpecies.size()) ? m_tileSpecies[i] : 0u;
            if (sid == 0) return land ? rgba(24, 28, 34) : rgba(12, 16, 24);
            // The same id hash the Phylogeny panel and the agent sprites use, so
            // a lineage is one colour everywhere in the program.
            uint32_t h = sid * 2654435761u;
            h ^= h >> 15;
            float r, g, b;
            ImGui::ColorConvertHSVtoRGB(static_cast<float>(h % 360u) / 360.0f,
                                        0.55f, 0.85f, r, g, b);
            return rgba(static_cast<int>(r * 255.0f), static_cast<int>(g * 255.0f),
                        static_cast<int>(b * 255.0f));
        }

        case Overlay::Drainage: {
            if (!land) return rgba(14, 24, 44);
            // Log scale: flow accumulation spans five orders of magnitude, so a
            // linear ramp would show only the trunk streams.
            const float t = std::log10(1.0f + w.flowAccum[i]) / 5.0f;
            return rampViridis(std::min(1.0f, t));
        }

        case Overlay::Plates: {
            // Hash the tile's nearest-plate assignment for a stable colour.
            // Recomputing the Voronoi here would be wasteful, so the boundary
            // proximity stored during generation is approximated from slope.
            const uint32_t h = static_cast<uint32_t>(w.biome[i]) * 2654435761u;
            uint32_t c = rgba(static_cast<int>(60 + (h & 0x7F)),
                              static_cast<int>(60 + ((h >> 8) & 0x7F)),
                              static_cast<int>(60 + ((h >> 16) & 0x7F)));
            if (!land) c = lerpColour(c, rgba(10, 20, 40), 0.6f);
            return c;
        }

        default:
            // Overlays awaiting a later milestone fall back to dimmed terrain
            // rather than drawing nothing at all.
            return land ? rgba(40, 44, 48) : rgba(16, 20, 28);
    }
}

// Reduces the population to a per-tile field, for the overlays whose data comes
// from agents rather than from the world.
//
// This exists because the rasteriser is handed the World and NOT the Agents, on
// purpose: it runs on its own thread pool and must not be able to reach into
// agent state while the simulation is mutating it. So anything agent-derived is
// reduced to a plain tile field here first, inside the read lock, and the
// rasteriser only ever sees the reduction.
//
// Built only for the overlay that needs it. Four of these overlays used to be
// greyed-out labels waiting on a milestone that had already shipped.
void Viewport::buildAgentField(Simulation& sim) {
    m_agentFieldValid = false;

    const bool wantsDensity = m_overlay == Overlay::PopulationDensity;
    const bool wantsAllele  = m_overlay == Overlay::AlleleFrequency;
    const bool wantsCulture = m_overlay == Overlay::Culture;
    const bool wantsTerritory = m_overlay == Overlay::Territory;
    if (!wantsDensity && !wantsAllele && !wantsCulture && !wantsTerritory) return;

    int worldW = 0, worldH = 0;
    sim.readWorld([&](const World& w) { worldW = w.width(); worldH = w.height(); });
    if (worldW <= 0 || worldH <= 0) return;

    const size_t tiles = static_cast<size_t>(worldW) * static_cast<size_t>(worldH);
    m_agentField.assign(tiles, wantsDensity ? 0.0f : -1.0f);
    if (wantsTerritory) m_tileSpecies.assign(tiles, 0u);
    m_agentFieldMax = 1.0f;

    // Accumulators. Separate from the output field because a mean needs a count,
    // and the output has to encode "nobody here" distinctly from "here and zero".
    static std::vector<float> sum;
    static std::vector<uint32_t> count;
    if (!wantsDensity) {
        sum.assign(tiles, 0.0f);
        count.assign(tiles, 0u);
    }
    // Territory needs a per-tile winner, which needs per-tile tallies. A full
    // histogram per tile would be enormous, so this keeps the leader and its
    // count -- the Boyer-Moore majority idea, which is exact when there is a
    // majority and picks a plurality candidate otherwise. Good enough for a map.
    static std::vector<uint32_t> leadId;
    static std::vector<uint32_t> leadCount;
    if (wantsTerritory) {
        leadId.assign(tiles, 0u);
        leadCount.assign(tiles, 0u);
    }

    uint16_t geneId = 0;
    double popMean = 0.0;
    double popSpread = 1.0;

    sim.readAgents([&](const Agents& a) {
        if (wantsAllele) {
            // Default to the first neutral locus in the first genome, because
            // that is the one whose spatial pattern means something: a coding
            // locus maps selection, and a neutral one maps drift and gene flow.
            geneId = static_cast<uint16_t>(alleleGeneId);
            if (geneId == 0 && !a.liveSlots().empty()) {
                ConstGenomeView g = a.genetics().genome(a.liveSlots().front());
                for (uint16_t k = 0; k < g.count; ++k) {
                    if (static_cast<LocusType>(g[k].type) != LocusType::Junk) continue;
                    geneId = g[k].id;
                    break;
                }
            }
            // The population mean and spread, so the map can show a DEVIATION.
            // An absolute value would be a flat wash: what matters is where one
            // region differs from the whole.
            double n = 0.0, s1 = 0.0, s2 = 0.0;
            for (uint32_t slot : a.liveSlots()) {
                ConstGenomeView g = a.genetics().genome(slot);
                for (uint16_t k = 0; k < g.count; ++k) {
                    if (g[k].id != geneId) continue;
                    const double v = 0.5 * (g[k].alleleA + g[k].alleleB);
                    n += 1.0; s1 += v; s2 += v * v;
                    break;
                }
            }
            if (n >= 2.0) {
                popMean = s1 / n;
                const double var = std::max(0.0, s2 / n - popMean * popMean);
                popSpread = std::max(1e-4, 2.0 * std::sqrt(var));
            }
        }

        for (uint32_t slot : a.liveSlots()) {
            if (a.m_stage[slot] == static_cast<uint8_t>(LifeStage::Embryo)) continue;
            const int tx = static_cast<int>(a.m_x[slot]);
            const int ty = static_cast<int>(a.m_y[slot]);
            if (tx < 0 || ty < 0 || tx >= worldW || ty >= worldH) continue;
            const size_t i = static_cast<size_t>(ty) * static_cast<size_t>(worldW) +
                             static_cast<size_t>(tx);

            if (wantsDensity) {
                m_agentField[i] += 1.0f;
                if (m_agentField[i] > m_agentFieldMax) m_agentFieldMax = m_agentField[i];
                continue;
            }

            if (wantsCulture) {
                int highest = -1;
                for (int t = 0; t < static_cast<int>(Technique::Count); ++t)
                    if (a.knowledge().hasTechnique(slot, static_cast<Technique>(t))) highest = t;
                sum[i] += static_cast<float>(highest + 1) /
                          static_cast<float>(static_cast<int>(Technique::Count));
                ++count[i];
            } else if (wantsAllele) {
                ConstGenomeView g = a.genetics().genome(slot);
                for (uint16_t k = 0; k < g.count; ++k) {
                    if (g[k].id != geneId) continue;
                    const double v = 0.5 * (g[k].alleleA + g[k].alleleB);
                    // Mapped to 0..1 around the population mean, so mid-tone is
                    // "average" and the extremes are genuine outliers.
                    sum[i] += static_cast<float>(
                        std::min(1.0, std::max(0.0, 0.5 + (v - popMean) / (2.0 * popSpread))));
                    ++count[i];
                    break;
                }
            } else if (wantsTerritory) {
                const uint32_t sid = a.speciation().speciesOf(slot);
                if (sid == 0) continue;
                if (leadCount[i] == 0) { leadId[i] = sid; leadCount[i] = 1; }
                else if (leadId[i] == sid) ++leadCount[i];
                else --leadCount[i];
                ++count[i];
            }
        }
    });

    if (wantsCulture || wantsAllele) {
        for (size_t i = 0; i < tiles; ++i)
            if (count[i] > 0) m_agentField[i] = sum[i] / static_cast<float>(count[i]);
    } else if (wantsTerritory) {
        for (size_t i = 0; i < tiles; ++i)
            if (count[i] > 0) m_tileSpecies[i] = leadId[i];
    }

    if (wantsAllele) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "gene %u (neutral)", static_cast<unsigned>(geneId));
        m_alleleGeneLabel = buf;
    }
    m_agentFieldValid = true;
}

void Viewport::rasterise(const World& w, int texW, int texH, int tx0, int ty0, int stride) {
    m_pixels.resize(static_cast<size_t>(texW) * static_cast<size_t>(texH));
    const int worldW = w.width(), worldH = w.height();

    m_rasterJobs.parallelFor(static_cast<size_t>(texH), [&](size_t b, size_t e, unsigned) {
        for (size_t row = b; row < e; ++row) {
            const int ty = ty0 + static_cast<int>(row) * stride;
            uint32_t* out = &m_pixels[row * static_cast<size_t>(texW)];
            if (ty < 0 || ty >= worldH) {
                for (int c = 0; c < texW; ++c) out[c] = rgba(10, 11, 13);
                continue;
            }
            for (int c = 0; c < texW; ++c) {
                const int tx = tx0 + c * stride;
                if (tx < 0 || tx >= worldW) { out[c] = rgba(10, 11, 13); continue; }
                out[c] = sampleColour(w, w.index(tx, ty));
            }
        }
    });
}

// ---------------------------------------------------------------------------

void Viewport::draw(Simulation& sim) {
    ImGui::Begin("World");

    const SimSnapshot snap = sim.snapshot();
    if (!snap.worldReady) {
        ImGui::Dummy(ImVec2(0.0f, 40.0f));
        ImGui::TextUnformatted("Generating world...");
        ImGui::ProgressBar(snap.genProgress, ImVec2(-1.0f, 0.0f), snap.genStage.c_str());
        ImGui::End();
        return;
    }

    if (hideRender) {
        ImGui::Dummy(ImVec2(0.0f, 20.0f));
        ImGui::TextUnformatted("Rendering is disabled (Hide render).");
        ImGui::TextWrapped(
            "The entire frame budget is going to simulation ticks. Charts, the event feed "
            "and every readout keep updating from published snapshots; only the world "
            "raster is suspended.");
        ImGui::End();
        return;
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const int viewW = std::max(16, static_cast<int>(avail.x));
    const int viewH = std::max(16, static_cast<int>(avail.y));

    int worldW = 0, worldH = 0;
    sim.readWorld([&](const World& w) { worldW = w.width(); worldH = w.height(); });
    if (worldW <= 0) { ImGui::End(); return; }

    if (!m_cameraInitialised) {
        resetCamera(worldW, worldH);
        // Fit the whole world into the viewport on first show.
        m_camera.pixelsPerTile = std::min(static_cast<double>(viewW) / worldW,
                                          static_cast<double>(viewH) / worldH);
        m_cameraInitialised = true;
    }

    const double minPPT = cfg().getFloat("render.min_pixels_per_tile", 0.05);
    const double maxPPT = cfg().getFloat("render.max_pixels_per_tile", 64.0);
    m_camera.clampTo(worldW, worldH, minPPT, maxPPT);

    // Visible tile window.
    const double halfW = (viewW * 0.5) / m_camera.pixelsPerTile;
    const double halfH = (viewH * 0.5) / m_camera.pixelsPerTile;
    const int tx0 = static_cast<int>(std::floor(m_camera.centreX - halfW));
    const int ty0 = static_cast<int>(std::floor(m_camera.centreY - halfH));
    const int tilesX = static_cast<int>(std::ceil(viewW / m_camera.pixelsPerTile)) + 2;
    const int tilesY = static_cast<int>(std::ceil(viewH / m_camera.pixelsPerTile)) + 2;

    // Rasterise at no more than one texel per screen pixel: past that the GPU
    // upscales for free, and below it we stride over tiles rather than reading
    // every one of them.
    const int stride = std::max(1, static_cast<int>(std::ceil(static_cast<double>(tilesX) / viewW)));
    const int texW = std::max(1, (tilesX + stride - 1) / stride);
    const int texH = std::max(1, (tilesY + stride - 1) / stride);

    // Redraw the raster only when it would actually differ. The world changes
    // slowly compared to 60 FPS, so a cap of ~12 Hz is invisible and costs a
    // twentieth of what a per-frame raster would.
    m_rasterAgeSeconds += static_cast<double>(ImGui::GetIO().DeltaTime);
    const bool geometryChanged = (tx0 != m_lastTx0) || (ty0 != m_lastTy0) ||
                                 (stride != m_lastStride) ||
                                 (texW != m_texW) || (texH != m_texH);
    const bool worldMoved = (snap.tick != m_lastRasterTick) && (m_rasterAgeSeconds > 0.08);
    if (m_dirty || geometryChanged || worldMoved) {
        // A real clock, not ImGui::GetTime(): that only advances once per
        // frame, so it would report every raster as costing zero.
        const auto t0 = std::chrono::steady_clock::now();
        buildAgentField(sim);
        sim.readWorld([&](const World& w) { rasterise(w, texW, texH, tx0, ty0, stride); });
        m_lastRasterMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

        glBindTexture(GL_TEXTURE_2D, m_texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        if (texW != m_texW || texH != m_texH) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texW, texH, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, m_pixels.data());
            m_texW = texW;
            m_texH = texH;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texW, texH, GL_RGBA,
                            GL_UNSIGNED_BYTE, m_pixels.data());
        }
        glBindTexture(GL_TEXTURE_2D, 0);

        m_dirty = false;
        m_lastTx0 = tx0;
        m_lastTy0 = ty0;
        m_lastStride = stride;
        m_lastRasterTick = snap.tick;
        m_rasterAgeSeconds = 0.0;
    }

    // Place the texture so that texel (0,0) sits exactly on tile (tx0, ty0).
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const double screenX = (tx0 - (m_camera.centreX - halfW)) * m_camera.pixelsPerTile;
    const double screenY = (ty0 - (m_camera.centreY - halfH)) * m_camera.pixelsPerTile;
    const float drawX = cursor.x + static_cast<float>(screenX);
    const float drawY = cursor.y + static_cast<float>(screenY);
    const float drawW = static_cast<float>(texW * stride * m_camera.pixelsPerTile);
    const float drawH = static_cast<float>(texH * stride * m_camera.pixelsPerTile);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(cursor, ImVec2(cursor.x + avail.x, cursor.y + avail.y), true);
    dl->AddRectFilled(cursor, ImVec2(cursor.x + avail.x, cursor.y + avail.y),
                      IM_COL32(10, 11, 13, 255));
    dl->AddImage(static_cast<ImTextureID>(m_texture),
                 ImVec2(drawX, drawY), ImVec2(drawX + drawW, drawY + drawH));

    m_imgX = drawX; m_imgY = drawY; m_imgW = drawW; m_imgH = drawH;
    m_viewTx0 = tx0; m_viewTy0 = ty0; m_viewStride = stride;

    // Tile grid, only when it is legible.
    if (showGrid && m_camera.pixelsPerTile >= 8.0) {
        const ImU32 col = IM_COL32(255, 255, 255, 28);
        for (int c = 0; c <= tilesX; ++c) {
            const float x = drawX + static_cast<float>(c * m_camera.pixelsPerTile);
            dl->AddLine(ImVec2(x, cursor.y), ImVec2(x, cursor.y + avail.y), col);
        }
        for (int rIdx = 0; rIdx <= tilesY; ++rIdx) {
            const float y = drawY + static_cast<float>(rIdx * m_camera.pixelsPerTile);
            dl->AddLine(ImVec2(cursor.x, y), ImVec2(cursor.x + avail.x, y), col);
        }
    }

    // Input. InvisibleButton claims the region so ImGui reports hover/drag for
    // it without stealing clicks from panels docked on top.
    ImGui::InvisibleButton("##worldcanvas", avail,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                           ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    auto screenToTile = [&](const ImVec2& p, int& tx, int& ty) {
        tx = static_cast<int>(std::floor(m_camera.centreX - halfW + (p.x - cursor.x) / m_camera.pixelsPerTile));
        ty = static_cast<int>(std::floor(m_camera.centreY - halfH + (p.y - cursor.y) / m_camera.pixelsPerTile));
    };

    if (hovered) {
        screenToTile(mouse, m_hoverX, m_hoverY);
        if (m_hoverX < 0 || m_hoverY < 0 || m_hoverX >= worldW || m_hoverY >= worldH) {
            m_hoverX = m_hoverY = -1;
        }

        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            // Zoom about the cursor: the tile under the pointer stays put.
            int ax = 0, ay = 0;
            screenToTile(mouse, ax, ay);
            const double anchorX = m_camera.centreX - halfW + (mouse.x - cursor.x) / m_camera.pixelsPerTile;
            const double anchorY = m_camera.centreY - halfH + (mouse.y - cursor.y) / m_camera.pixelsPerTile;
            const double factor = std::pow(cfg().getFloat("render.zoom_speed", 1.14),
                                           static_cast<double>(wheel));
            const double newPPT = std::min(maxPPT, std::max(minPPT, m_camera.pixelsPerTile * factor));
            const double nHalfW = (viewW * 0.5) / newPPT;
            const double nHalfH = (viewH * 0.5) / newPPT;
            m_camera.centreX = anchorX + nHalfW - (mouse.x - cursor.x) / newPPT;
            m_camera.centreY = anchorY + nHalfH - (mouse.y - cursor.y) / newPPT;
            m_camera.pixelsPerTile = newPPT;
            m_dirty = true;
        }

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_hoverX >= 0) {
            m_selX = m_hoverX;
            m_selY = m_hoverY;
        }

        // Painting. A brush claims the left button, so panning falls back to
        // the middle button while a brush is selected -- otherwise every stroke
        // would also drag the camera out from under it.
        if (brushActive && m_hoverX >= 0 &&
            (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
             ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))) {
            m_brushStroke = true;
            m_brushStrokeX = static_cast<float>(m_hoverX) + 0.5f;
            m_brushStrokeY = static_cast<float>(m_hoverY) + 0.5f;
        }
    } else {
        m_hoverX = m_hoverY = -1;
    }

    // Pan with left drag or middle drag -- but not with the left button while
    // a brush is active, because the brush owns it.
    if (ImGui::IsItemActive() &&
        ((!brushActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) ||
         ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        m_camera.centreX -= d.x / m_camera.pixelsPerTile;
        m_camera.centreY -= d.y / m_camera.pixelsPerTile;
        m_dirty = true;
    }

    // ------------------------------------------------------------------
    // Agents.
    //
    // Drawn with the ImGui draw list rather than into the tile raster, because
    // they move every tick while the raster is deliberately cached. Level of
    // detail here is RENDERING ONLY and changes nothing about the simulation:
    // every agent is still simulated in full whether or not it is drawn as a
    // sprite, a dot or a density blob.
    // ------------------------------------------------------------------
    {
        const double ppt = m_camera.pixelsPerTile;
        const float left = static_cast<float>(m_camera.centreX - halfW);
        const float top = static_cast<float>(m_camera.centreY - halfH);
        auto toScreen = [&](float wx, float wy) {
            return ImVec2(cursor.x + static_cast<float>((wx - left) * ppt),
                          cursor.y + static_cast<float>((wy - top) * ppt));
        };

        uint64_t hoverAgentUid = 0;
        float hoverAgentDist2 = 1e30f;
        int   hoverAgentSlot = -1;

        sim.readAgents([&](const Agents& ag) {
            const float x0 = static_cast<float>(tx0) - 2.0f;
            const float y0 = static_cast<float>(ty0) - 2.0f;
            const float x1 = x0 + static_cast<float>(tilesX) + 4.0f;
            const float y1 = y0 + static_cast<float>(tilesY) + 4.0f;

            for (uint32_t slot : ag.liveSlots()) {
                // Embryos are inside their mother and have no independent
                // position to draw.
                if (ag.m_stage[slot] == static_cast<uint8_t>(LifeStage::Embryo)) continue;
                const float wx = ag.m_x[slot], wy = ag.m_y[slot];
                if (wx < x0 || wx > x1 || wy < y0 || wy > y1) continue;

                const ImVec2 sp = toScreen(wx + 0.5f, wy + 0.5f);
                const Phenotype& p = ag.m_phenotype[slot];
                int cr, cg, cb;
                if (colourBySpecies) {
                    const uint32_t sid = ag.speciation().speciesOf(slot);
                    if (sid == 0) {
                        // Genuinely unassigned -- an outlier the detector has
                        // not placed in any lineage. Grey, not a made-up colour.
                        cr = cg = cb = 130;
                    } else {
                        // The same id hash the Phylogeny panel uses, so a
                        // lineage is the same colour in both places.
                        uint32_t h = sid * 2654435761u;
                        h ^= h >> 15;
                        float r, g, b;
                        ImGui::ColorConvertHSVtoRGB(
                            static_cast<float>(h % 360u) / 360.0f, 0.62f, 0.92f, r, g, b);
                        cr = static_cast<int>(r * 255.0f);
                        cg = static_cast<int>(g * 255.0f);
                        cb = static_cast<int>(b * 255.0f);
                    }
                } else {
                    cr = static_cast<int>(p.get(Trait::ColourR) * 255.0f);
                    cg = static_cast<int>(p.get(Trait::ColourG) * 255.0f);
                    cb = static_cast<int>(p.get(Trait::ColourB) * 255.0f);
                }

                if (ppt < 2.0) {
                    // Far out: a single pixel of the right colour. Density
                    // reads correctly because overlapping dots accumulate.
                    dl->AddRectFilled(sp, ImVec2(sp.x + 1.5f, sp.y + 1.5f),
                                      IM_COL32(cr, cg, cb, 220));
                    continue;
                }

                const float sizeT = p.get(Trait::Size);
                // Juveniles are drawn smaller than their adult size, so age
                // structure is visible at a glance.
                const float stageScale =
                    (ag.m_stage[slot] == static_cast<uint8_t>(LifeStage::Juvenile)) ? 0.55f :
                    (ag.m_stage[slot] == static_cast<uint8_t>(LifeStage::Adolescent)) ? 0.8f : 1.0f;
                const float radius = std::max(2.0f,
                    static_cast<float>(ppt) * 0.32f * sizeT * stageScale);

                // Ornaments as radiating spines: costly display made visible.
                const float ornament = 0.5f * (p.get(Trait::Ornament1) + p.get(Trait::Ornament2));
                if (ppt >= 6.0 && ornament > 0.15f) {
                    const int spikes = 5 + static_cast<int>(ornament * 5.0f);
                    for (int k = 0; k < spikes; ++k) {
                        const float a2 = 6.2831853f * static_cast<float>(k) / static_cast<float>(spikes);
                        const float len = radius + radius * ornament * 0.9f;
                        dl->AddLine(sp, ImVec2(sp.x + std::cos(a2) * len, sp.y + std::sin(a2) * len),
                                    IM_COL32(cr, cg, cb, 170), 1.0f);
                    }
                }

                dl->AddCircleFilled(sp, radius, IM_COL32(cr, cg, cb, 255), 12);

                if (ppt >= 5.0) {
                    // Sex expression as the outline colour: a continuous ramp
                    // from one pole to the other, not two discrete icons.
                    const float e = p.get(Trait::SexExpression);
                    const int orr = static_cast<int>(210 * (1.0f - e) + 90 * e);
                    const int org = static_cast<int>(110 * (1.0f - e) + 150 * e);
                    const int orb = static_cast<int>(160 * (1.0f - e) + 230 * e);
                    dl->AddCircle(sp, radius + 1.5f, IM_COL32(orr, org, orb, 235), 12, 1.6f);

                    // Health arc: a red wedge missing from the ring.
                    const float health = std::min(1.0f, std::max(0.0f, ag.m_health[slot]));
                    if (health < 0.999f) {
                        dl->PathArcTo(sp, radius + 3.5f, -1.5707963f,
                                      -1.5707963f + 6.2831853f * (1.0f - health), 16);
                        dl->PathStroke(IM_COL32(230, 70, 60, 220), 0, 2.0f);
                    }
                    // Heading tick, so it is obvious which way an agent faces.
                    const float h = ag.m_heading[slot];
                    dl->AddLine(sp, ImVec2(sp.x + std::cos(h) * (radius + 4.0f),
                                           sp.y + std::sin(h) * (radius + 4.0f)),
                                IM_COL32(255, 255, 255, 150), 1.2f);
                }

                if (ag.tagged(slot))
                    dl->AddCircle(sp, radius + 6.0f, IM_COL32(255, 220, 60, 230), 14, 1.8f);
                if (ag.m_pregnantByUid[slot] != 0)
                    dl->AddCircleFilled(ImVec2(sp.x, sp.y + radius + 4.0f), 2.0f,
                                        IM_COL32(200, 160, 255, 240), 6);
                if (ag.m_uid[slot] == m_selectedAgentUid)
                    dl->AddCircle(sp, radius + 8.0f, IM_COL32(255, 255, 255, 245), 18, 2.2f);

                // Pair bonds drawn as a faint tether, which makes the social
                // structure legible without any extra overlay.
                if (ppt >= 3.0 && ag.m_bondedUid[slot] != 0 && ag.m_uid[slot] < ag.m_bondedUid[slot]) {
                    const int32_t partner = ag.slotOfUid(ag.m_bondedUid[slot]);
                    if (partner >= 0) {
                        const uint32_t ps = static_cast<uint32_t>(partner);
                        dl->AddLine(sp, toScreen(ag.m_x[ps] + 0.5f, ag.m_y[ps] + 0.5f),
                                    IM_COL32(255, 180, 220, 90), 1.0f);
                    }
                }

                if (hovered) {
                    const float dx = mouse.x - sp.x, dy = mouse.y - sp.y;
                    const float d2 = dx * dx + dy * dy;
                    const float grab = std::max(6.0f, radius + 4.0f);
                    if (d2 < grab * grab && d2 < hoverAgentDist2) {
                        hoverAgentDist2 = d2;
                        hoverAgentUid = ag.m_uid[slot];
                        hoverAgentSlot = static_cast<int>(slot);
                    }
                }
            }

            // Hover readout and click-to-select. An agent under the cursor wins
            // over the tile beneath it, which is what a user expects.
            if (hoverAgentSlot >= 0) {
                const uint32_t slot = static_cast<uint32_t>(hoverAgentSlot);
                const Phenotype& p = ag.m_phenotype[slot];
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(ag.m_name[slot].c_str());
                ImGui::Separator();
                ImGui::Text("%s, %s", lifeStageName(static_cast<LifeStage>(ag.m_stage[slot])),
                            actionName(static_cast<Action>(ag.m_action[slot])));
                ImGui::Text("sex expression %.2f (%s)",
                            static_cast<double>(p.get(Trait::SexExpression)),
                            sexExpressionLabel(p.get(Trait::SexExpression)));
                ImGui::Text("health %.2f   energy %.0f", static_cast<double>(ag.m_health[slot]),
                            static_cast<double>(ag.m_energy[slot]));
                ImGui::Text("repro drive %.2f",
                            static_cast<double>(ag.m_drives[slot][Drive::Reproduction]));
                if (ag.m_bondedUid[slot] != 0) ImGui::TextUnformatted("pair-bonded");
                if (ag.m_pregnantByUid[slot] != 0) ImGui::TextUnformatted("gestating");
                ImGui::Separator();
                ImGui::TextDisabled("Click to open the Individual Card");
                ImGui::EndTooltip();
            }
        });

        if (hovered && hoverAgentUid != 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            m_selectedAgentUid = hoverAgentUid;
            m_activatedAgentUid = hoverAgentUid;
        }
    }

    // Selection and hover markers.
    auto tileRect = [&](int tx, int ty, ImVec2& a, ImVec2& b) {
        a.x = cursor.x + static_cast<float>((tx - (m_camera.centreX - halfW)) * m_camera.pixelsPerTile);
        a.y = cursor.y + static_cast<float>((ty - (m_camera.centreY - halfH)) * m_camera.pixelsPerTile);
        b.x = a.x + static_cast<float>(m_camera.pixelsPerTile);
        b.y = a.y + static_cast<float>(m_camera.pixelsPerTile);
    };
    if (m_hoverX >= 0) {
        ImVec2 a, b;
        tileRect(m_hoverX, m_hoverY, a, b);
        const float pad = (m_camera.pixelsPerTile < 4.0) ? 3.0f : 0.0f;
        dl->AddRect(ImVec2(a.x - pad, a.y - pad), ImVec2(b.x + pad, b.y + pad),
                    IM_COL32(255, 255, 255, 120), 0.0f, 0, 1.0f);
    }
    if (hasSelection()) {
        ImVec2 a, b;
        tileRect(m_selX, m_selY, a, b);
        const float pad = (m_camera.pixelsPerTile < 6.0) ? 5.0f : 1.0f;
        dl->AddRect(ImVec2(a.x - pad, a.y - pad), ImVec2(b.x + pad, b.y + pad),
                    IM_COL32(255, 210, 60, 230), 0.0f, 0, 2.0f);
    }

    // Brush preview ring, drawn at true world scale so the radius shown is the
    // radius applied.
    if (brushActive && m_hoverX >= 0) {
        const ImVec2 centre(
            cursor.x + static_cast<float>((m_hoverX + 0.5 - (m_camera.centreX - halfW)) *
                                          m_camera.pixelsPerTile),
            cursor.y + static_cast<float>((m_hoverY + 0.5 - (m_camera.centreY - halfH)) *
                                          m_camera.pixelsPerTile));
        const float rpx = static_cast<float>(brushRadius * m_camera.pixelsPerTile);
        dl->AddCircle(centre, rpx, IM_COL32(255, 220, 90, 200), 48, 1.6f);
        dl->AddCircle(centre, rpx * 0.5f, IM_COL32(255, 220, 90, 90), 48, 1.0f);
        dl->AddCircleFilled(centre, 2.5f, IM_COL32(255, 220, 90, 220), 8);
    }

    dl->PopClipRect();

    // Scale bar and status, drawn over the world.
    {
        char buf[256];
        const double metresPerPixel = cfg().getFloat("world.tile_metres", 50.0) /
                                      m_camera.pixelsPerTile;
        const double barMetres = metresPerPixel * 120.0;
        std::snprintf(buf, sizeof(buf), "%.0f px/tile  |  120 px = %.1f km  |  %s",
                      m_camera.pixelsPerTile, barMetres / 1000.0, overlayName(m_overlay));
        const ImVec2 p(cursor.x + 8.0f, cursor.y + avail.y - 22.0f);
        dl->AddRectFilled(ImVec2(p.x - 4.0f, p.y - 3.0f),
                          ImVec2(p.x + 420.0f, p.y + 18.0f), IM_COL32(0, 0, 0, 150), 3.0f);
        dl->AddText(p, IM_COL32(220, 220, 225, 255), buf);
        dl->AddLine(ImVec2(cursor.x + 8.0f, cursor.y + avail.y - 28.0f),
                    ImVec2(cursor.x + 128.0f, cursor.y + avail.y - 28.0f),
                    IM_COL32(255, 255, 255, 200), 2.0f);
    }

    // Hover tooltip: the essentials without needing to open the inspector.
    if (hovered && m_hoverX >= 0) {
        sim.readWorld([&](const World& w) {
            const size_t i = w.index(m_hoverX, m_hoverY);
            ImGui::BeginTooltip();
            ImGui::Text("Tile %d, %d", m_hoverX, m_hoverY);
            ImGui::Separator();
            ImGui::Text("Elevation   %8.1f m", static_cast<double>(w.elevation[i]));
            ImGui::Text("Temperature %8.1f C", static_cast<double>(w.temperature[i]));
            ImGui::Text("Rainfall    %8.0f mm/yr", static_cast<double>(w.rainfall[i]));
            ImGui::Text("Biome       %s", biomeName(static_cast<Biome>(w.biome[i])));
            ImGui::Text("Surface     %s", rockName(static_cast<RockType>(w.strataRock[0][i])));
            const OreType o = static_cast<OreType>(w.oreType[i]);
            if (o != OreType::None)
                ImGui::Text("Ore         %s (%.0f%% grade)", oreName(o),
                            static_cast<double>(w.oreGrade[i]) / 2.55);
            ImGui::EndTooltip();
        });
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------

bool Viewport::takeBrushStroke(float& x, float& y) {
    if (!m_brushStroke) return false;
    x = m_brushStrokeX;
    y = m_brushStrokeY;
    m_brushStroke = false;
    return true;
}

void Viewport::drawTileInspector(Simulation& sim, bool* open) {
    if (!ImGui::Begin("Tile inspector", open)) { ImGui::End(); return; }

    if (!hasSelection()) {
        ImGui::TextWrapped("Click a tile in the World view to inspect it. "
                           "Every stored field of that tile is shown here.");
        ImGui::End();
        return;
    }

    const int sx = m_selX, sy = m_selY;
    sim.readWorld([&](const World& w) {
        if (!w.inBounds(sx, sy)) return;
        const size_t i = w.index(sx, sy);
        const float sea = w.params().seaLevel;

        ImGui::Text("Tile (%d, %d)   index %llu", sx, sy,
                    static_cast<unsigned long long>(i));
        ImGui::Text("Latitude %.2f deg", static_cast<double>(w.latitudeOfRow(sy)));
        ImGui::Separator();

        if (ImGui::CollapsingHeader("Physical", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Elevation        %10.2f m %s", static_cast<double>(w.elevation[i]),
                        w.elevation[i] > sea ? "(land)" : "(below sea level)");
            ImGui::Text("Water depth      %10.2f m", static_cast<double>(w.waterDepth[i]));
            ImGui::Text("Temperature      %10.2f C", static_cast<double>(w.temperature[i]));
            ImGui::Text("Rainfall         %10.0f mm/yr", static_cast<double>(w.rainfall[i]));
            ImGui::Text("Soil moisture    %10.3f", static_cast<double>(w.soilMoisture[i]));
            ImGui::Text("Water table      %10.2f m below surface",
                        static_cast<double>(w.waterTable[i]));
            ImGui::Text("Flow accumulation%10.0f upstream tiles",
                        static_cast<double>(w.flowAccum[i]));
            if (w.flowDir[i] >= 0) {
                const size_t d = static_cast<size_t>(w.flowDir[i]);
                ImGui::Text("Drains to        (%d, %d)",
                            static_cast<int>(d % static_cast<size_t>(w.width())),
                            static_cast<int>(d / static_cast<size_t>(w.width())));
            } else {
                ImGui::TextUnformatted("Drains to        nowhere (endorheic sink)");
            }
            ImGui::Text("Loose sediment   %10.2f m", static_cast<double>(w.sediment[i]));
        }

        if (ImGui::CollapsingHeader("Soil and ecology", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Biome            %s", biomeName(static_cast<Biome>(w.biome[i])));
            ImGui::Text("Nitrogen   (N)   %3u / 255", static_cast<unsigned>(w.soilN[i]));
            ImGui::Text("Phosphorus (P)   %3u / 255", static_cast<unsigned>(w.soilP[i]));
            ImGui::Text("Potassium  (K)   %3u / 255", static_cast<unsigned>(w.soilK[i]));
            ImGui::Text("Plant biomass    %10.2f kg", static_cast<double>(w.biomass[i]));
        }

        if (ImGui::CollapsingHeader("Geology", ImGuiTreeNodeFlags_DefaultOpen)) {
            const OreType o = static_cast<OreType>(w.oreType[i]);
            if (o == OreType::None) {
                ImGui::TextUnformatted("No ore deposit.");
            } else {
                ImGui::Text("Ore              %s  (%s)", oreName(o), oreFormula(o));
                ImGui::Text("Grade            %.1f%%", static_cast<double>(w.oreGrade[i]) / 2.55);
            }
            ImGui::Spacing();
            ImGui::TextUnformatted("Strata, surface first:");
            if (ImGui::BeginTable("strata", 3,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Layer");
                ImGui::TableSetupColumn("Rock");
                ImGui::TableSetupColumn("Thickness");
                ImGui::TableHeadersRow();
                for (int L = 0; L < kMaxStrata; ++L) {
                    const uint16_t th = w.strataThick[L][i];
                    if (th == 0) continue;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%d", L);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(rockName(static_cast<RockType>(w.strataRock[L][i])));
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%.1f m", th * 0.1);
                }
                ImGui::EndTable();
            }
        }
    });

    ImGui::Separator();
    if (ImGui::Button("Centre camera here")) focusOn(sx, sy);
    ImGui::SameLine();
    if (ImGui::Button("Clear selection")) clearSelection();

    ImGui::End();
}

}  // namespace gen
