#include "god/god.h"

#include "econ/economy.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

#include "core/config.h"
#include "core/noise.h"
#include "core/serialize.h"
#include "sim/time.h"

namespace gen {

namespace {
constexpr float kPi = 3.14159265358979f;

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list args;
    va_start(args, f);
    std::vsnprintf(buf, sizeof(buf), f, args);
    va_end(args);
    return std::string(buf);
}
}  // namespace

const char* godActionName(GodActionKind k) {
    switch (k) {
        case GodActionKind::None:              return "none";
        case GodActionKind::BrushElevation:    return "Raise / lower terrain";
        case GodActionKind::BrushRock:         return "Change surface rock";
        case GodActionKind::BrushSoil:         return "Enrich / strip soil";
        case GodActionKind::BrushWater:        return "Add / remove water";
        case GodActionKind::BrushPlants:       return "Plant / clear vegetation";
        case GodActionKind::BrushOre:          return "Place / remove ore";
        case GodActionKind::BrushTemperature:  return "Warm / cool region";
        case GodActionKind::SpawnAgents:       return "Spawn agents";
        case GodActionKind::SpawnDesigned:     return "Spawn designed individual";
        case GodActionKind::MassKill:          return "Mass kill";
        case GodActionKind::MassEdit:          return "Mass edit trait";
        case GodActionKind::Bottleneck:        return "Forced bottleneck";
        case GodActionKind::Migrate:           return "Forced migration";
        case GodActionKind::Teleport:          return "Teleport";
        case GodActionKind::Sterilise:         return "Mass sterilise";
        case GodActionKind::Fertilise:         return "Mass fertilise";
        case GodActionKind::SelectionPressure: return "Selection pressure";
        case GodActionKind::Drought:           return "Drought";
        case GodActionKind::Flood:             return "Flood";
        case GodActionKind::Storm:             return "Storm";
        case GodActionKind::Wildfire:          return "Wildfire";
        case GodActionKind::Earthquake:        return "Earthquake";
        case GodActionKind::Volcano:           return "Volcanic eruption";
        case GodActionKind::Meteor:            return "Meteor impact";
        case GodActionKind::IceAge:            return "Ice age";
        case GodActionKind::Plague:            return "Plague";
        case GodActionKind::EnableBarter:      return "Enable barter";
        case GodActionKind::IntroduceCurrency: return "INTRODUCE CURRENCY";
        case GodActionKind::AbolishEconomy:    return "Abolish the economy";
        case GodActionKind::SetClimate:        return "Set climate";
        case GodActionKind::SetConfigValue:    return "Set rule";
        case GodActionKind::Count:             break;
    }
    return "?";
}

bool godActionIsBrush(GodActionKind k) {
    switch (k) {
        case GodActionKind::BrushElevation:
        case GodActionKind::BrushRock:
        case GodActionKind::BrushSoil:
        case GodActionKind::BrushWater:
        case GodActionKind::BrushPlants:
        case GodActionKind::BrushOre:
        case GodActionKind::BrushTemperature:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Population filter
// ---------------------------------------------------------------------------

bool PopulationFilter::matches(const Agents& a, uint32_t slot, uint64_t tick) const {
    if (!a.slotAlive(slot)) return false;
    if (excludeImmortal && a.immortal(slot)) return false;
    if (taggedOnly && !a.tagged(slot)) return false;
    if (stage >= 0 && a.m_stage[slot] != stage) return false;

    if (useRegion) {
        const float dx = a.m_x[slot] - x, dy = a.m_y[slot] - y;
        if (dx * dx + dy * dy > radius * radius) return false;
    }

    const uint64_t born = a.m_birthTick[slot];
    const float age = static_cast<float>((tick > born ? tick - born : 0) + a.m_ageOffset[slot]) /
                      static_cast<float>(kHoursPerYear);
    if (age < minAge || age > maxAge) return false;

    if (traitIndex >= 0 && traitIndex < kTraitCount) {
        const float v = a.m_phenotype[slot].traits[traitIndex];
        if (v < traitMin || v > traitMax) return false;
    }

    if (sexMode != 0) {
        const float e = a.m_phenotype[slot].get(Trait::SexExpression);
        if (sexMode == 1 && e >= 0.5f) return false;
        if (sexMode == 2 && e < 0.5f) return false;
        if (sexMode == 3 && !isIntersex(e)) return false;
    }
    return true;
}

std::string PopulationFilter::describe() const {
    std::string s;
    if (useRegion) s += fmt("within %.0f tiles of (%.0f, %.0f), ", radius, x, y);
    if (stage >= 0) s += std::string(lifeStageName(static_cast<LifeStage>(stage))) + "s, ";
    if (minAge > 0.0f || maxAge < 1.0e8f) s += fmt("age %.1f-%.1f yr, ", minAge, maxAge);
    if (traitIndex >= 0 && traitIndex < kTraitCount)
        s += fmt("%s in [%.3g, %.3g], ", traitSpec(static_cast<Trait>(traitIndex)).name,
                 traitMin, traitMax);
    if (sexMode == 1) s += "female-expressed, ";
    else if (sexMode == 2) s += "male-expressed, ";
    else if (sexMode == 3) s += "intersex, ";
    if (taggedOnly) s += "tagged only, ";
    if (s.empty()) return "everyone";
    s.resize(s.size() - 2);
    return s;
}

// ---------------------------------------------------------------------------
// Tile undo
// ---------------------------------------------------------------------------

TileUndo TileUndo::capture(const World& w, uint32_t i) {
    TileUndo t;
    t.index = i;
    t.elevation = w.elevation[i];
    t.waterDepth = w.waterDepth[i];
    t.temperature = w.temperature[i];
    t.rainfall = w.rainfall[i];
    t.biomass = w.biomass[i];
    t.soilMoisture = w.soilMoisture[i];
    t.soilN = w.soilN[i];
    t.soilP = w.soilP[i];
    t.soilK = w.soilK[i];
    t.biome = w.biome[i];
    t.oreType = w.oreType[i];
    t.oreGrade = w.oreGrade[i];
    t.strataRock0 = w.strataRock[0][i];
    t.strataThick0 = w.strataThick[0][i];
    return t;
}

void TileUndo::restore(World& w) const {
    const size_t i = index;
    w.elevation[i] = elevation;
    w.waterDepth[i] = waterDepth;
    w.temperature[i] = temperature;
    w.rainfall[i] = rainfall;
    w.biomass[i] = biomass;
    w.soilMoisture[i] = soilMoisture;
    w.soilN[i] = soilN;
    w.soilP[i] = soilP;
    w.soilK[i] = soilK;
    w.biome[i] = biome;
    w.oreType[i] = oreType;
    w.oreGrade[i] = oreGrade;
    w.strataRock[0][i] = strataRock0;
    w.strataThick[0][i] = strataThick0;
}

size_t UndoRecord::memoryBytes() const {
    return sizeof(UndoRecord)
         + (tilesBefore.size() + tilesAfter.size()) * sizeof(TileUndo)
         + agentsBefore.size() + agentsAfter.size()
         + (createdUids.size() + removedUids.size()) * sizeof(uint64_t)
         + action.text.size() + action.description.size();
}

// ---------------------------------------------------------------------------

void GodMode::configure() {
    m_maxRecords = static_cast<size_t>(cfg().getInt("god.undo_depth", 64));
    m_maxBytes = static_cast<size_t>(cfg().getFloat("god.undo_memory_mb", 512.0)) * 1024ull * 1024ull;
}

size_t GodMode::undoMemoryBytes() const {
    size_t total = 0;
    for (const UndoRecord& r : m_undo) total += r.memoryBytes();
    for (const UndoRecord& r : m_redo) total += r.memoryBytes();
    return total;
}

void GodMode::clearHistory() {
    m_undo.clear();
    m_redo.clear();
}

void GodMode::pushUndo(UndoRecord&& rec) {
    m_undo.push_back(std::move(rec));
    // A new action invalidates the redo branch, as in any editor.
    m_redo.clear();
    trimUndo();
}

void GodMode::trimUndo() {
    while (m_undo.size() > m_maxRecords) m_undo.erase(m_undo.begin());
    // A mass kill of a thousand agents stores a thousand whole genomes and
    // brains. The budget is honest about that: the oldest records are dropped
    // rather than letting undo history quietly consume the machine.
    while (m_undo.size() > 1 && undoMemoryBytes() > m_maxBytes) m_undo.erase(m_undo.begin());
}

// ---------------------------------------------------------------------------
// Brushes
// ---------------------------------------------------------------------------

void GodMode::applyBrush(const GodAction& a, World& w, Agents& ag, RngBank& rng,
                         UndoRecord& rec) {
    (void)ag;
    const int W = w.width(), H = w.height();
    const int x0 = std::max(0, static_cast<int>(a.x - a.radius));
    const int x1 = std::min(W - 1, static_cast<int>(a.x + a.radius));
    const int y0 = std::max(0, static_cast<int>(a.y - a.radius));
    const int y1 = std::min(H - 1, static_cast<int>(a.y + a.radius));
    const float r2 = a.radius * a.radius;

    for (int ty = y0; ty <= y1; ++ty) {
        for (int tx = x0; tx <= x1; ++tx) {
            const float dx = static_cast<float>(tx) + 0.5f - a.x;
            const float dy = static_cast<float>(ty) + 0.5f - a.y;
            const float d2 = dx * dx + dy * dy;
            if (d2 > r2) continue;
            // Soft-edged falloff, so a brush blends rather than stamping a disc.
            const float falloff = 1.0f - std::sqrt(d2) / std::max(0.001f, a.radius);
            const float strength = a.intensity * falloff * falloff;
            const uint32_t i = static_cast<uint32_t>(w.index(tx, ty));

            rec.tilesBefore.push_back(TileUndo::capture(w, i));

            switch (a.kind) {
                case GodActionKind::BrushElevation:
                    w.elevation[i] += a.f0 * strength;
                    // Water follows the land: raising a seabed above sea level
                    // drains it, lowering land below floods it.
                    w.waterDepth[i] = (w.elevation[i] < w.params().seaLevel)
                        ? (w.params().seaLevel - w.elevation[i]) : w.waterDepth[i];
                    if (w.elevation[i] > w.params().seaLevel && w.waterDepth[i] > 0.0f &&
                        a.f0 > 0.0f)
                        w.waterDepth[i] = std::max(0.0f, w.waterDepth[i] - a.f0 * strength);
                    break;

                case GodActionKind::BrushRock:
                    if (strength > 0.35f) {
                        w.strataRock[0][i] = static_cast<uint8_t>(a.i0);
                        if (w.strataThick[0][i] < 200) w.strataThick[0][i] = 200;
                    }
                    break;

                case GodActionKind::BrushSoil: {
                    auto bump = [&](uint8_t& v) {
                        const float nv = static_cast<float>(v) + a.f0 * strength;
                        v = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, nv)));
                    };
                    bump(w.soilN[i]);
                    bump(w.soilP[i]);
                    bump(w.soilK[i]);
                    break;
                }

                case GodActionKind::BrushWater:
                    w.waterDepth[i] = std::max(0.0f, w.waterDepth[i] + a.f0 * strength);
                    if (w.waterDepth[i] > 0.02f) w.soilMoisture[i] = 1.0f;
                    break;

                case GodActionKind::BrushPlants: {
                    const float maxBio = cfg().getF("ecology.plant_max_biomass", 800.0f);
                    w.biomass[i] = std::min(maxBio,
                                            std::max(0.0f, w.biomass[i] + a.f0 * strength));
                    break;
                }

                case GodActionKind::BrushOre:
                    if (a.i0 == static_cast<int>(OreType::None)) {
                        w.oreType[i] = static_cast<uint8_t>(OreType::None);
                        w.oreGrade[i] = 0;
                    } else if (strength > 0.25f) {
                        // A vein is patchy, not a uniform disc: the draw makes
                        // a placed deposit look prospected rather than painted.
                        if (rng[Stream::God].nextFloat() < 0.4f + 0.6f * strength) {
                            w.oreType[i] = static_cast<uint8_t>(a.i0);
                            const float grade = a.f0 * (0.5f + 0.5f * strength);
                            w.oreGrade[i] = static_cast<uint8_t>(
                                std::min(255.0f, std::max(0.0f, grade * 255.0f)));
                        }
                    }
                    break;

                case GodActionKind::BrushTemperature:
                    w.temperature[i] += a.f0 * strength;
                    break;

                default:
                    break;
            }
            rec.tilesAfter.push_back(TileUndo::capture(w, i));
        }
    }
}

// ---------------------------------------------------------------------------
// Population tools
// ---------------------------------------------------------------------------

void GodMode::applyPopulation(const GodAction& a, World& w, Agents& ag, RngBank& rng,
                              uint64_t tick, UndoRecord& rec, std::string& what) {
    // The selection is resolved ONCE up front, in ascending slot order, so the
    // set operated on is a pure function of the filter and does not shift as
    // the operation mutates the store.
    std::vector<uint32_t> selected;
    for (uint32_t slot : ag.liveSlots())
        if (a.filter.matches(ag, slot, tick)) selected.push_back(slot);

    switch (a.kind) {
        case GodActionKind::SpawnAgents: {
            const int count = std::max(0, a.i0);
            int placed = 0;
            for (int k = 0; k < count; ++k) {
                float fx = a.x, fy = a.y;
                for (int attempt = 0; attempt < 48; ++attempt) {
                    const float ang = rng[Stream::God].rangef(0.0f, 2.0f * kPi);
                    const float rad = a.radius * std::sqrt(rng[Stream::God].nextFloat());
                    float cx = a.x + std::cos(ang) * rad;
                    float cy = a.y + std::sin(ang) * rad;
                    cx = std::min(std::max(cx, 0.0f), static_cast<float>(w.width()) - 1.0f);
                    cy = std::min(std::max(cy, 0.0f), static_cast<float>(w.height()) - 1.0f);
                    const size_t ti = w.index(static_cast<int>(cx), static_cast<int>(cy));
                    if (w.elevation[ti] <= w.params().seaLevel) continue;
                    if (w.waterDepth[ti] > 1.0f) continue;
                    fx = cx;
                    fy = cy;
                    break;
                }
                const AgentId id = ag.spawnFounder(fx, fy, rng, tick, (k % 2) == 1);
                if (!id.valid()) break;
                rec.createdUids.push_back(ag.m_uid[id.slot]);
                ag.appendAgentBlob(rec.agentsAfter, id.slot);
                ++rec.agentsAfterCount;
                ++placed;
            }
            what = fmt("Created %d agents at (%.0f, %.0f)", placed, a.x, a.y);
            break;
        }

        case GodActionKind::MassKill: {
            for (uint32_t slot : selected) {
                ag.appendAgentBlob(rec.agentsBefore, slot);
                ++rec.agentsBeforeCount;
                rec.removedUids.push_back(ag.m_uid[slot]);
                ag.kill(slot, DeathCause::Divine, tick, &w);
            }
            what = fmt("Struck down %zu individuals (%s)", selected.size(),
                       a.filter.describe().c_str());
            break;
        }

        case GodActionKind::Bottleneck: {
            // Keep the first i0 of the selection and remove the rest. Selection
            // order is by slot, which is arbitrary but fixed -- a bottleneck
            // should be indifferent to fitness, that is what makes it drift.
            const size_t keep = static_cast<size_t>(std::max(0, a.i0));
            for (size_t k = keep; k < selected.size(); ++k) {
                ag.appendAgentBlob(rec.agentsBefore, selected[k]);
                ++rec.agentsBeforeCount;
                rec.removedUids.push_back(ag.m_uid[selected[k]]);
                ag.kill(selected[k], DeathCause::Divine, tick, &w);
            }
            what = fmt("Bottleneck: %zu of %zu survived", std::min(keep, selected.size()),
                       selected.size());
            break;
        }

        case GodActionKind::MassEdit: {
            const int t = a.i0;
            if (t < 0 || t >= kTraitCount) { what = "Mass edit: no trait selected"; break; }
            const TraitSpec& spec = traitSpec(static_cast<Trait>(t));
            for (uint32_t slot : selected) {
                ag.appendAgentBlob(rec.agentsBefore, slot);
                ++rec.agentsBeforeCount;
                float& v = ag.m_phenotype[slot].traits[t];
                if (a.i1 == 1) v = a.f0;
                else if (a.i1 == 2) v *= a.f0;
                else v += a.f0;
                v = std::min(spec.maxValue, std::max(spec.minValue, v));
                buildPreferenceVector(ag.m_phenotype[slot], ag.m_prefs[slot]);
                ag.appendAgentBlob(rec.agentsAfter, slot);
                ++rec.agentsAfterCount;
            }
            const char* op = (a.i1 == 1) ? "set to" : (a.i1 == 2) ? "scaled by" : "shifted by";
            what = fmt("%zu individuals: %s %s %.4g (%s)", selected.size(), spec.name, op,
                       a.f0, a.filter.describe().c_str());
            break;
        }

        case GodActionKind::SelectionPressure: {
            // A fitness function rather than an edit: individuals far from the
            // target value die with a probability set by the strength. This is
            // selection, not engineering -- the survivors are a biased sample
            // of what was already there, and the trait shifts across
            // generations rather than instantly.
            const int t = a.i0;
            if (t < 0 || t >= kTraitCount) { what = "Selection pressure: no trait"; break; }
            const TraitSpec& spec = traitSpec(static_cast<Trait>(t));
            const float range = std::max(1e-6f, spec.maxValue - spec.minValue);
            size_t culled = 0;
            for (uint32_t slot : selected) {
                const float v = ag.m_phenotype[slot].traits[t];
                const float miss = std::fabs(v - a.f0) / range;
                const float pDeath = std::min(0.98f, miss * a.f1);
                if (rng[Stream::God].nextFloat() >= pDeath) continue;
                ag.appendAgentBlob(rec.agentsBefore, slot);
                ++rec.agentsBeforeCount;
                rec.removedUids.push_back(ag.m_uid[slot]);
                ag.kill(slot, DeathCause::Divine, tick, &w);
                ++culled;
            }
            what = fmt("Selection for %s near %.3g: %zu of %zu removed", spec.name, a.f0,
                       culled, selected.size());
            break;
        }

        case GodActionKind::Migrate:
        case GodActionKind::Teleport: {
            for (uint32_t slot : selected) {
                ag.appendAgentBlob(rec.agentsBefore, slot);
                ++rec.agentsBeforeCount;
                const float ang = rng[Stream::God].rangef(0.0f, 2.0f * kPi);
                const float rad = a.f2 * std::sqrt(rng[Stream::God].nextFloat());
                ag.m_x[slot] = std::min(std::max(a.f0 + std::cos(ang) * rad, 0.0f),
                                        static_cast<float>(w.width()) - 1.0f);
                ag.m_y[slot] = std::min(std::max(a.f1 + std::sin(ang) * rad, 0.0f),
                                        static_cast<float>(w.height()) - 1.0f);
                // Spatial memories point at places that are now far away.
                ag.m_memFoodX[slot] = ag.m_memFoodY[slot] = -1.0f;
                ag.m_memWaterX[slot] = ag.m_memWaterY[slot] = -1.0f;
                ag.appendAgentBlob(rec.agentsAfter, slot);
                ++rec.agentsAfterCount;
            }
            what = fmt("Moved %zu individuals to (%.0f, %.0f)", selected.size(), a.f0, a.f1);
            break;
        }

        case GodActionKind::Sterilise:
        case GodActionKind::Fertilise: {
            const bool sterile = (a.kind == GodActionKind::Sterilise);
            for (uint32_t slot : selected) {
                ag.appendAgentBlob(rec.agentsBefore, slot);
                ++rec.agentsBeforeCount;
                if (sterile) ag.m_flags[slot] |= Agents::Flag_Sterile;
                else ag.m_flags[slot] &= static_cast<uint8_t>(~Agents::Flag_Sterile);
                ag.appendAgentBlob(rec.agentsAfter, slot);
                ++rec.agentsAfterCount;
            }
            what = fmt("%s %zu individuals", sterile ? "Sterilised" : "Restored fertility to",
                       selected.size());
            break;
        }

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Disasters
// ---------------------------------------------------------------------------

void GodMode::applyDisaster(const GodAction& a, World& w, Agents& ag, RngBank& rng,
                            uint64_t tick, UndoRecord& rec, std::string& what) {
    const int W = w.width(), H = w.height();
    const bool global = (a.radius <= 0.0f);
    const int x0 = global ? 0 : std::max(0, static_cast<int>(a.x - a.radius));
    const int x1 = global ? W - 1 : std::min(W - 1, static_cast<int>(a.x + a.radius));
    const int y0 = global ? 0 : std::max(0, static_cast<int>(a.y - a.radius));
    const int y1 = global ? H - 1 : std::min(H - 1, static_cast<int>(a.y + a.radius));
    const float r2 = a.radius * a.radius;

    auto forEachTile = [&](void (*fn)(World&, uint32_t, float, const GodAction&),
                           bool captureUndo) {
        for (int ty = y0; ty <= y1; ++ty) {
            for (int tx = x0; tx <= x1; ++tx) {
                float strength = a.intensity;
                if (!global) {
                    const float dx = static_cast<float>(tx) + 0.5f - a.x;
                    const float dy = static_cast<float>(ty) + 0.5f - a.y;
                    const float d2 = dx * dx + dy * dy;
                    if (d2 > r2) continue;
                    strength = a.intensity * (1.0f - std::sqrt(d2) / std::max(0.001f, a.radius));
                }
                const uint32_t i = static_cast<uint32_t>(w.index(tx, ty));
                if (captureUndo) rec.tilesBefore.push_back(TileUndo::capture(w, i));
                fn(w, i, strength, a);
                if (captureUndo) rec.tilesAfter.push_back(TileUndo::capture(w, i));
            }
        }
    };

    // Agents caught in the blast, by radius.
    auto harmAgents = [&](float lethality, DeathCause cause, const char* verb) -> size_t {
        size_t killed = 0;
        std::vector<uint32_t> victims;
        for (uint32_t slot : ag.liveSlots()) {
            if (ag.immortal(slot)) continue;
            if (!global) {
                const float dx = ag.m_x[slot] - a.x, dy = ag.m_y[slot] - a.y;
                if (dx * dx + dy * dy > r2) continue;
            }
            victims.push_back(slot);
        }
        for (uint32_t slot : victims) {
            ag.appendAgentBlob(rec.agentsBefore, slot);
            ++rec.agentsBeforeCount;
            const float p = lethality / std::max(0.2f, ag.m_phenotype[slot].get(Trait::Hardiness));
            if (rng[Stream::God].nextFloat() < p) {
                rec.removedUids.push_back(ag.m_uid[slot]);
                ag.kill(slot, cause, tick, &w);
                ++killed;
            } else {
                ag.m_health[slot] = std::max(0.05f, ag.m_health[slot] - p * 0.5f);
                ag.m_stress[slot] = std::min(1.0f, ag.m_stress[slot] + 0.5f);
                ag.appendAgentBlob(rec.agentsAfter, slot);
                ++rec.agentsAfterCount;
            }
        }
        (void)verb;
        return killed;
    };

    switch (a.kind) {
        case GodActionKind::Drought: {
            forEachTile([](World& w2, uint32_t i, float s, const GodAction&) {
                w2.soilMoisture[i] = std::max(0.0f, w2.soilMoisture[i] - s * 0.8f);
                w2.rainfall[i] *= std::max(0.05f, 1.0f - s * 0.7f);
            }, true);
            ActiveDisaster d;
            d.kind = a.kind;
            d.x = a.x; d.y = a.y; d.radius = a.radius; d.intensity = a.intensity;
            d.ticksRemaining = static_cast<uint64_t>(a.f0 * kHoursPerYear);
            d.startTick = tick;
            d.label = "Drought";
            m_disasters.push_back(d);
            what = fmt("Drought for %.1f years%s", a.f0, global ? " (global)" : "");
            break;
        }

        case GodActionKind::Flood: {
            forEachTile([](World& w2, uint32_t i, float s, const GodAction& act) {
                w2.waterDepth[i] += act.f0 * s;
                w2.soilMoisture[i] = 1.0f;
                // Standing water drowns standing vegetation.
                w2.biomass[i] *= std::max(0.0f, 1.0f - s * 0.6f);
            }, true);
            const size_t killed = harmAgents(0.25f * a.intensity, DeathCause::Accident, "drowned");
            what = fmt("Flood: %.1f m of water, %zu drowned", a.f0, killed);
            break;
        }

        case GodActionKind::Storm: {
            forEachTile([](World& w2, uint32_t i, float s, const GodAction&) {
                w2.soilMoisture[i] = std::min(1.0f, w2.soilMoisture[i] + s * 0.5f);
                w2.biomass[i] *= std::max(0.0f, 1.0f - s * 0.25f);
            }, true);
            const size_t killed = harmAgents(0.08f * a.intensity, DeathCause::Accident, "killed");
            what = fmt("Storm: %zu killed", killed);
            break;
        }

        case GodActionKind::Wildfire: {
            forEachTile([](World& w2, uint32_t i, float s, const GodAction&) {
                // Fire consumes the standing crop but returns ash: burnt ground
                // is briefly MORE fertile than it was, which is why fire-adapted
                // vegetation recovers fast.
                const float burnt = w2.biomass[i] * std::min(1.0f, s * 1.2f);
                w2.biomass[i] -= burnt;
                auto bump = [&](uint8_t& v) {
                    v = static_cast<uint8_t>(std::min(255.0f,
                        static_cast<float>(v) + burnt * 0.02f));
                };
                bump(w2.soilN[i]);
                bump(w2.soilP[i]);
                bump(w2.soilK[i]);
                w2.temperature[i] += s * 40.0f;
            }, true);
            const size_t killed = harmAgents(0.45f * a.intensity, DeathCause::Accident, "burnt");
            ActiveDisaster d;
            d.kind = a.kind;
            d.x = a.x; d.y = a.y; d.radius = a.radius; d.intensity = a.intensity;
            d.ticksRemaining = 48;   // spreads for two days
            d.startTick = tick;
            d.label = "Wildfire";
            m_disasters.push_back(d);
            what = fmt("Wildfire: %zu killed, still spreading", killed);
            break;
        }

        case GodActionKind::Earthquake: {
            Noise n;
            n.seed(rng[Stream::God]);
            for (int ty = y0; ty <= y1; ++ty) {
                for (int tx = x0; tx <= x1; ++tx) {
                    float strength = a.intensity;
                    if (!global) {
                        const float dx = static_cast<float>(tx) + 0.5f - a.x;
                        const float dy = static_cast<float>(ty) + 0.5f - a.y;
                        const float d2 = dx * dx + dy * dy;
                        if (d2 > r2) continue;
                        strength = a.intensity * (1.0f - std::sqrt(d2) / std::max(0.001f, a.radius));
                    }
                    const uint32_t i = static_cast<uint32_t>(w.index(tx, ty));
                    rec.tilesBefore.push_back(TileUndo::capture(w, i));
                    // Displacement along a fault trace rather than uniform lift.
                    const float f = n.perlin(tx * 0.06f, ty * 0.06f);
                    w.elevation[i] += f * strength * a.f0;
                    rec.tilesAfter.push_back(TileUndo::capture(w, i));
                }
            }
            const size_t killed = harmAgents(0.30f * a.intensity, DeathCause::Accident, "crushed");
            what = fmt("Earthquake: ground displaced by up to %.0f m, %zu killed", a.f0, killed);
            break;
        }

        case GodActionKind::Volcano: {
            forEachTile([](World& w2, uint32_t i, float s, const GodAction& act) {
                w2.elevation[i] += act.f0 * s * s;          // a cone, not a dome
                w2.strataRock[0][i] = static_cast<uint8_t>(RockType::Basalt);
                w2.strataThick[0][i] = static_cast<uint16_t>(
                    std::min(60000.0f, w2.strataThick[0][i] + act.f0 * s * 10.0f));
                w2.biomass[i] = 0.0f;
                w2.temperature[i] += s * 200.0f;
                w2.oreType[i] = static_cast<uint8_t>(OreType::Sulfur);
                w2.oreGrade[i] = static_cast<uint8_t>(std::min(255.0f, s * 180.0f));
            }, true);
            const size_t killed = harmAgents(0.85f * a.intensity, DeathCause::Accident, "killed");
            // Volcanic winter: sulfate aerosols cool the whole planet for years.
            ActiveDisaster d;
            d.kind = a.kind;
            d.x = a.x; d.y = a.y; d.radius = 0.0f;   // global effect
            d.intensity = a.intensity;
            d.ticksRemaining = static_cast<uint64_t>(3.0 * kHoursPerYear);
            d.startTick = tick;
            d.label = "Volcanic winter";
            m_disasters.push_back(d);
            what = fmt("Volcanic eruption: %zu killed, volcanic winter begins", killed);
            break;
        }

        case GodActionKind::Meteor: {
            forEachTile([](World& w2, uint32_t i, float s, const GodAction& act) {
                // A crater: excavated in the middle, with an uplifted rim.
                const float rim = (s < 0.35f) ? 1.0f : -1.0f;
                w2.elevation[i] += rim * act.f0 * s;
                w2.biomass[i] = 0.0f;
                w2.temperature[i] += s * 300.0f;
                w2.strataRock[0][i] = static_cast<uint8_t>(RockType::Basalt);
            }, true);
            const size_t killed = harmAgents(0.97f * a.intensity, DeathCause::Accident, "killed");
            ActiveDisaster d;
            d.kind = a.kind;
            d.radius = 0.0f;
            d.intensity = a.intensity;
            d.ticksRemaining = static_cast<uint64_t>(8.0 * kHoursPerYear);
            d.startTick = tick;
            d.label = "Impact winter";
            m_disasters.push_back(d);
            what = fmt("Meteor impact: %zu killed, impact winter begins", killed);
            break;
        }

        case GodActionKind::IceAge: {
            ActiveDisaster d;
            d.kind = a.kind;
            d.radius = 0.0f;
            d.intensity = a.intensity;
            d.ticksRemaining = static_cast<uint64_t>(a.f0 * kHoursPerYear);
            d.startTick = tick;
            d.label = "Ice age";
            m_disasters.push_back(d);
            what = fmt("Ice age: %.0f C colder for %.0f years", a.intensity * 12.0f, a.f0);
            break;
        }

        case GodActionKind::Plague: {
            // Until pathogens are their own evolving entities, a plague is an
            // imposed mortality event rather than a transmissible agent. It is
            // labelled as such rather than pretending to be an epidemic.
            size_t killed = 0, infected = 0;
            for (uint32_t slot : ag.liveSlots()) {
                if (ag.immortal(slot)) continue;
                if (!global) {
                    const float dx = ag.m_x[slot] - a.x, dy = ag.m_y[slot] - a.y;
                    if (dx * dx + dy * dy > r2) continue;
                }
                ag.appendAgentBlob(rec.agentsBefore, slot);
                ++rec.agentsBeforeCount;
                ++infected;
                // Resistance comes from the immune trait, so a population with
                // diverse MHC and high ImmuneStrength genuinely fares better.
                const float resist = ag.m_phenotype[slot].get(Trait::ImmuneStrength);
                const float p = a.intensity / std::max(0.2f, resist);
                if (rng[Stream::God].nextFloat() < p) {
                    rec.removedUids.push_back(ag.m_uid[slot]);
                    ag.kill(slot, DeathCause::Disease, tick, &w);
                    ++killed;
                } else {
                    ag.m_health[slot] = std::max(0.05f, ag.m_health[slot] - p);
                    ag.appendAgentBlob(rec.agentsAfter, slot);
                    ++rec.agentsAfterCount;
                }
            }
            what = fmt("Plague: %zu of %zu died", killed, infected);
            break;
        }

        default:
            break;
    }
}

// ---------------------------------------------------------------------------

std::string GodMode::apply(GodAction action, World& world, Agents& agents,
                           RngBank& rng, uint64_t tick) {
    UndoRecord rec;
    action.appliedTick = tick;
    std::string what;

    if (godActionIsBrush(action.kind)) {
        applyBrush(action, world, agents, rng, rec);
        what = fmt("%s at (%.0f, %.0f) r=%.0f", godActionName(action.kind),
                   action.x, action.y, action.radius);
    } else if (action.kind >= GodActionKind::Drought && action.kind <= GodActionKind::Plague) {
        applyDisaster(action, world, agents, rng, tick, rec, what);
    } else if (action.kind == GodActionKind::EnableBarter ||
               action.kind == GodActionKind::IntroduceCurrency ||
               action.kind == GodActionKind::AbolishEconomy) {
        // The economy acts. NOT pushed onto the undo stack in any meaningful
        // sense: undo restores tiles and whole agents, and none of these touches
        // either. The inverse act is the way back, and the Economy panel offers it.
        if (!m_economy) {
            what = "There is no economy module in this build.";
        } else if (action.kind == GodActionKind::EnableBarter) {
            m_economy->activateBarter(agents.capacity());
            what = "Barter is now possible. Nobody was given anything: goods can change "
                   "hands, and whether they do is up to the inhabitants.";
        } else if (action.kind == GodActionKind::IntroduceCurrency) {
            m_economy->introduceCurrency(static_cast<uint16_t>(action.i0), action.text,
                                         agents.capacity(), action.f0);
            what = "CURRENCY INTRODUCED BY DECREE: " + m_economy->currencyName() +
                   ". It was not discovered, it was imposed.";
        } else {
            m_economy->deactivate();
            what = "The economy is abolished. Not suspended -- every structure is released "
                   "and the concept is absent again.";
        }
    } else if (action.kind == GodActionKind::SetConfigValue) {
        rec.configKey = action.text;
        rec.configBefore = cfg().getFloat(action.text.c_str());
        rec.configAfter = action.f0;
        cfg().setFloat(action.text.c_str(), action.f0);
        what = fmt("%s = %.6g (was %.6g)", action.text.c_str(),
                   rec.configAfter, rec.configBefore);
    } else {
        applyPopulation(action, world, agents, rng, tick, rec, what);
    }

    // Reap immediately rather than waiting for the tick's cleanup stage, so a
    // script or panel that reads the population straight after a mass kill sees
    // the truth rather than the pre-kill count.
    agents.reapDead(world, tick);

    if (what.empty()) what = godActionName(action.kind);
    action.description = what;
    rec.action = action;
    pushUndo(std::move(rec));
    return what;
}

// ---------------------------------------------------------------------------
// Undo / redo
// ---------------------------------------------------------------------------

namespace {
// Restores every agent blob in a buffer, back to back.
void restoreAll(Agents& ag, const std::vector<uint8_t>& blob, uint32_t count) {
    size_t offset = 0;
    for (uint32_t k = 0; k < count && offset < blob.size(); ++k) {
        size_t consumed = 0;
        if (ag.restoreAgentBlob(blob, offset, consumed) < 0 || consumed == 0) break;
        offset += consumed;
    }
}

void removeByUid(Agents& ag, World& w, const std::vector<uint64_t>& uids, uint64_t tick) {
    for (uint64_t uid : uids) {
        const int32_t slot = ag.slotOfUid(uid);
        if (slot >= 0) ag.kill(static_cast<uint32_t>(slot), DeathCause::Divine, tick, &w);
    }
}
}  // namespace

bool GodMode::undo(World& world, Agents& agents, uint64_t tick, std::string& what) {
    if (m_undo.empty()) return false;
    UndoRecord rec = std::move(m_undo.back());
    m_undo.pop_back();

    for (const TileUndo& t : rec.tilesBefore) t.restore(world);
    // Anything the action created is removed; anything it destroyed or modified
    // is restored from its recorded state.
    removeByUid(agents, world, rec.createdUids, tick);
    agents.reapDead(world, tick);
    restoreAll(agents, rec.agentsBefore, rec.agentsBeforeCount);
    if (!rec.configKey.empty()) cfg().setFloat(rec.configKey.c_str(), rec.configBefore);

    what = "Undid: " + rec.action.description;
    m_redo.push_back(std::move(rec));
    return true;
}

bool GodMode::redo(World& world, Agents& agents, uint64_t tick, std::string& what) {
    if (m_redo.empty()) return false;
    UndoRecord rec = std::move(m_redo.back());
    m_redo.pop_back();

    for (const TileUndo& t : rec.tilesAfter) t.restore(world);
    // Redo restores the RECORDED after-state rather than re-running the action.
    // Re-running would consume randomness and produce a different world, which
    // is not what "redo" means.
    removeByUid(agents, world, rec.removedUids, tick);
    agents.reapDead(world, tick);
    restoreAll(agents, rec.agentsAfter, rec.agentsAfterCount);
    if (!rec.configKey.empty()) cfg().setFloat(rec.configKey.c_str(), rec.configAfter);

    what = "Redid: " + rec.action.description;
    m_undo.push_back(std::move(rec));
    return true;
}

// ---------------------------------------------------------------------------
// Persistent disaster effects
// ---------------------------------------------------------------------------

void GodMode::stepDisasters(World& world, Agents& agents, RngBank& rng, uint64_t tick,
                            std::vector<std::string>& eventsOut) {
    (void)agents;
    m_temperatureOffset = 0.0f;
    m_rainfallMultiplier = 1.0f;
    if (m_disasters.empty()) return;

    for (size_t k = 0; k < m_disasters.size();) {
        ActiveDisaster& d = m_disasters[k];
        if (d.ticksRemaining == 0) {
            eventsOut.push_back(d.label + " has ended");
            m_disasters.erase(m_disasters.begin() + static_cast<long>(k));
            continue;
        }
        --d.ticksRemaining;

        switch (d.kind) {
            case GodActionKind::IceAge:
                m_temperatureOffset -= 12.0f * d.intensity;
                break;
            case GodActionKind::Volcano:
                // Aerosols thin out over the years rather than stopping dead.
                m_temperatureOffset -= 6.0f * d.intensity *
                    static_cast<float>(d.ticksRemaining) /
                    static_cast<float>(3.0 * kHoursPerYear);
                break;
            case GodActionKind::Meteor:
                m_temperatureOffset -= 18.0f * d.intensity *
                    static_cast<float>(d.ticksRemaining) /
                    static_cast<float>(8.0 * kHoursPerYear);
                m_rainfallMultiplier *= 0.6f;
                break;
            case GodActionKind::Drought:
                m_rainfallMultiplier *= std::max(0.05f, 1.0f - 0.7f * d.intensity);
                break;
            case GodActionKind::Wildfire: {
                // Fire spreads outward and burns what it reaches, but only into
                // fuel: it stops at water, bare ground and already-burnt tiles.
                d.radius += 0.5f * d.intensity;
                const int W = world.width(), H = world.height();
                const int x0 = std::max(0, static_cast<int>(d.x - d.radius));
                const int x1 = std::min(W - 1, static_cast<int>(d.x + d.radius));
                const int y0 = std::max(0, static_cast<int>(d.y - d.radius));
                const int y1 = std::min(H - 1, static_cast<int>(d.y + d.radius));
                const float r2 = d.radius * d.radius;
                const float inner = std::max(0.0f, d.radius - 2.0f);
                for (int ty = y0; ty <= y1; ++ty) {
                    for (int tx = x0; tx <= x1; ++tx) {
                        const float dx = tx + 0.5f - d.x, dy = ty + 0.5f - d.y;
                        const float dd = dx * dx + dy * dy;
                        if (dd > r2 || dd < inner * inner) continue;
                        const size_t i = world.index(tx, ty);
                        if (world.waterDepth[i] > 0.05f) continue;
                        if (world.biomass[i] < 5.0f) continue;
                        world.biomass[i] *= 0.15f;
                        world.temperature[i] += 30.0f;
                    }
                }
                break;
            }
            default:
                break;
        }
        ++k;
    }
    (void)rng;
    (void)tick;
}

// ---------------------------------------------------------------------------

void GodMode::serialize(BinaryWriter& w) const {
    const uint32_t nd = static_cast<uint32_t>(m_disasters.size());
    w.pod(nd);
    for (const ActiveDisaster& d : m_disasters) {
        const uint16_t k = static_cast<uint16_t>(d.kind);
        w.pod(k);
        w.pod(d.x); w.pod(d.y); w.pod(d.radius); w.pod(d.intensity);
        w.pod(d.ticksRemaining); w.pod(d.startTick);
        w.str(d.label);
    }
    const uint32_t nm = static_cast<uint32_t>(m_miracles.size());
    w.pod(nm);
    for (const Miracle& m : m_miracles) {
        w.str(m.name);
        w.pod(m.hotkey);
        const uint32_t na = static_cast<uint32_t>(m.actions.size());
        w.pod(na);
        for (const GodAction& a : m.actions) {
            const uint16_t k = static_cast<uint16_t>(a.kind);
            w.pod(k);
            w.pod(a.x); w.pod(a.y); w.pod(a.radius); w.pod(a.intensity);
            w.pod(a.i0); w.pod(a.i1);
            w.pod(a.f0); w.pod(a.f1); w.pod(a.f2);
            w.str(a.text);
        }
    }
    // The undo history is deliberately NOT saved. It holds whole agents from a
    // world state the snapshot no longer contains, and restoring one into a
    // reloaded world would resurrect an individual with a stale genome arena.
}

void GodMode::deserialize(BinaryReader& r) {
    m_disasters.clear();
    m_miracles.clear();
    clearHistory();

    uint32_t nd = 0;
    r.pod(nd);
    if (nd > 4096) return;
    for (uint32_t i = 0; i < nd && r.ok(); ++i) {
        ActiveDisaster d;
        uint16_t k = 0;
        r.pod(k);
        d.kind = static_cast<GodActionKind>(k);
        r.pod(d.x); r.pod(d.y); r.pod(d.radius); r.pod(d.intensity);
        r.pod(d.ticksRemaining); r.pod(d.startTick);
        r.str(d.label);
        m_disasters.push_back(std::move(d));
    }
    uint32_t nm = 0;
    r.pod(nm);
    if (nm > 4096) return;
    for (uint32_t i = 0; i < nm && r.ok(); ++i) {
        Miracle m;
        r.str(m.name);
        r.pod(m.hotkey);
        uint32_t na = 0;
        r.pod(na);
        if (na > 4096) return;
        for (uint32_t j = 0; j < na && r.ok(); ++j) {
            GodAction a;
            uint16_t k = 0;
            r.pod(k);
            a.kind = static_cast<GodActionKind>(k);
            r.pod(a.x); r.pod(a.y); r.pod(a.radius); r.pod(a.intensity);
            r.pod(a.i0); r.pod(a.i1);
            r.pod(a.f0); r.pod(a.f1); r.pod(a.f2);
            r.str(a.text);
            m.actions.push_back(std::move(a));
        }
        m_miracles.push_back(std::move(m));
    }
}

}  // namespace gen
