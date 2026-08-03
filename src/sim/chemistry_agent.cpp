// sim/chemistry_agent.cpp — the bridge between agents and chemistry.
//
// This is where discovery actually happens. An agent gathers substances from
// the ground, and when it uses them it brings them together at whatever
// temperature its techniques can reach. The reaction engine is then asked what
// happens -- not a recipe table. If a reduction is downhill and fast enough at
// that temperature it proceeds, and if it is not, nothing happens however much
// the agent wanted it to.
#include "sim/chemistry_agent.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/config.h"
#include "core/serialize.h"
#include "sim/agent.h"
#include "sim/time.h"
#include "sim/world.h"

namespace gen {

// What each ore actually is. The mineralogy and the chemistry are tied together
// here rather than being allowed to drift apart.
const char* oreSubstanceFormula(OreType o) {
    switch (o) {
        case OreType::Hematite:     return "Fe2O3";
        case OreType::Magnetite:    return "Fe3O4";
        case OreType::Malachite:    return "Cu2CO3(OH)2";
        case OreType::Chalcopyrite: return "CuFeS2";
        case OreType::Cassiterite:  return "SnO2";
        case OreType::Galena:       return "PbS";
        case OreType::Sphalerite:   return "ZnS";
        case OreType::Bauxite:      return "Al2O3";
        case OreType::NativeGold:   return "Au";
        case OreType::NativeCopper: return "Cu";
        case OreType::Pyrolusite:   return "MnO2";
        case OreType::Wolframite:   return "WO3";
        case OreType::Uraninite:    return "UO2";
        case OreType::Cinnabar:     return "HgS";
        case OreType::Rocksalt:     return "NaCl";
        case OreType::Sulfur:       return "S";
        case OreType::Niter:        return "KNO3";
        case OreType::Clay:         return "Al2Si2O5(OH)4";
        default:                    return nullptr;
    }
}

// And what the bedrock is, for the things that are quarried rather than mined.
const char* rockSubstanceFormula(RockType r) {
    switch (r) {
        case RockType::Limestone: return "CaCO3";
        case RockType::Marble:    return "CaCO3";
        case RockType::Sandstone: return "SiO2";
        case RockType::Coal:      return "C";
        default:                  return nullptr;
    }
}

// ---------------------------------------------------------------------------

void Inventory::configure(size_t maxAgents, int slots) {
    m_slots = std::max(1, slots);
    m_substance.assign(maxAgents * static_cast<size_t>(m_slots), 0);
    m_amount.assign(maxAgents * static_cast<size_t>(m_slots), 0.0f);
}

void Inventory::clearAgent(uint32_t slot) {
    for (int i = 0; i < m_slots; ++i) {
        m_substance[slot * m_slots + i] = 0;
        m_amount[slot * m_slots + i] = 0.0f;
    }
}

bool Inventory::add(uint32_t slot, uint16_t substanceId, float amount) {
    if (substanceId == 0 || amount <= 0.0f) return false;
    const size_t base = slot * static_cast<size_t>(m_slots);
    for (int i = 0; i < m_slots; ++i) {
        if (m_substance[base + i] == substanceId) {
            m_amount[base + i] += amount;
            return true;
        }
    }
    for (int i = 0; i < m_slots; ++i) {
        if (m_substance[base + i] == 0) {
            m_substance[base + i] = substanceId;
            m_amount[base + i] = amount;
            return true;
        }
    }
    // Full. Drop whatever there is least of, which is the closest thing to a
    // sensible choice without asking the brain.
    int worst = 0;
    for (int i = 1; i < m_slots; ++i)
        if (m_amount[base + i] < m_amount[base + worst]) worst = i;
    if (m_amount[base + worst] >= amount) return false;
    m_substance[base + worst] = substanceId;
    m_amount[base + worst] = amount;
    return true;
}

float Inventory::amountOf(uint32_t slot, uint16_t substanceId) const {
    const size_t base = slot * static_cast<size_t>(m_slots);
    for (int i = 0; i < m_slots; ++i)
        if (m_substance[base + i] == substanceId) return m_amount[base + i];
    return 0.0f;
}

bool Inventory::consume(uint32_t slot, uint16_t substanceId, float amount) {
    const size_t base = slot * static_cast<size_t>(m_slots);
    for (int i = 0; i < m_slots; ++i) {
        if (m_substance[base + i] != substanceId) continue;
        if (m_amount[base + i] < amount) return false;
        m_amount[base + i] -= amount;
        if (m_amount[base + i] < 1e-4f) {
            m_substance[base + i] = 0;
            m_amount[base + i] = 0.0f;
        }
        return true;
    }
    return false;
}

void Inventory::list(uint32_t slot, std::vector<uint16_t>& out) const {
    out.clear();
    const size_t base = slot * static_cast<size_t>(m_slots);
    for (int i = 0; i < m_slots; ++i)
        if (m_substance[base + i] != 0 && m_amount[base + i] > 1e-4f)
            out.push_back(m_substance[base + i]);
}

void Inventory::serializeAgent(BinaryWriter& w, uint32_t slot) const {
    const uint16_t n = static_cast<uint16_t>(m_slots);
    w.pod(n);
    const size_t base = slot * static_cast<size_t>(m_slots);
    for (int i = 0; i < m_slots; ++i) {
        w.pod(m_substance[base + i]);
        w.pod(m_amount[base + i]);
    }
}

void Inventory::deserializeAgent(BinaryReader& r, uint32_t slot) {
    uint16_t n = 0;
    r.pod(n);
    const size_t base = slot * static_cast<size_t>(m_slots);
    for (uint16_t i = 0; i < n; ++i) {
        uint16_t s = 0;
        float a = 0.0f;
        r.pod(s);
        r.pod(a);
        if (i < static_cast<uint16_t>(m_slots)) {
            m_substance[base + i] = s;
            m_amount[base + i] = a;
        }
    }
}

// ---------------------------------------------------------------------------
// Gathering
// ---------------------------------------------------------------------------

bool gatherFromTile(Inventory& inv, uint32_t slot, const World& world, int tx, int ty,
                    std::string* whatOut) {
    if (!world.inBounds(tx, ty)) return false;
    const size_t i = world.index(tx, ty);
    const float take = cfg().getF("chem.gather_amount", 1.0f);

    // Ore first: it is what an agent is most likely to have gone looking for.
    const OreType ore = static_cast<OreType>(world.oreType[i]);
    if (ore != OreType::None && world.oreGrade[i] > 20) {
        if (const char* f = oreSubstanceFormula(ore)) {
            if (const Substance* s = chem().substanceByFormula(f, Phase::Solid)) {
                if (inv.add(slot, s->id, take * (world.oreGrade[i] / 255.0f))) {
                    if (whatOut) *whatOut = s->name;
                    return true;
                }
            }
        }
    }
    // Then the bedrock.
    if (const char* f = rockSubstanceFormula(static_cast<RockType>(world.strataRock[0][i]))) {
        if (const Substance* s = chem().substanceByFormula(f, Phase::Solid)) {
            if (inv.add(slot, s->id, take)) {
                if (whatOut) *whatOut = s->name;
                return true;
            }
        }
    }
    // Then vegetation, which is where fire and charcoal begin.
    if (world.biomass[i] > 20.0f) {
        if (const Substance* s = chem().substanceByFormula("C6H10O5", Phase::Solid)) {
            if (inv.add(slot, s->id, take)) {
                if (whatOut) *whatOut = s->name;
                return true;
            }
        }
    }
    // And water.
    if (world.waterDepth[i] > 0.02f || world.soilMoisture[i] > 0.8f) {
        if (const Substance* s = chem().substanceByFormula("H2O", Phase::Liquid)) {
            if (inv.add(slot, s->id, take)) {
                if (whatOut) *whatOut = s->name;
                return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// The experiment. This is the discovery mechanic.
// ---------------------------------------------------------------------------

ExperimentOutcome runExperiment(Inventory& inv, KnowledgeBase& kb, uint32_t slot,
                                uint64_t agentUid, const std::string& agentName,
                                uint64_t tick, Rng& rng, float curiosityWeight) {
    ExperimentOutcome out;

    static std::vector<uint16_t> held;
    inv.list(slot, held);
    if (held.empty()) return out;

    // The agent works at the hottest it knows how to get. Air is always present
    // -- an agent does not have to "have" oxygen to burn something in it.
    Technique best = Technique::Mixing;
    for (int i = 0; i < static_cast<int>(Technique::Count); ++i) {
        const Technique t = static_cast<Technique>(i);
        if (!kb.hasTechnique(slot, t)) continue;
        if (techniqueTemperature(t) >= techniqueTemperature(best)) best = t;
    }

    ReactionConditions cond;
    cond.temperature = techniqueTemperature(best);
    cond.pressure = kb.bestPressure(slot);
    cond.electricity = kb.hasTechnique(slot, Technique::Electricity);
    cond.ignition = cond.temperature >= 700.0;
    // Grinding does not change the chemistry, only the surface area -- and
    // therefore the rate, which can decide whether anything happens at all.
    cond.concentration = kb.hasTechnique(slot, Technique::Grinding) ? 3.0 : 1.0;

    // The air. Resolved once: these lookups are formula-string scans, and
    // this function is on the tick path.
    static uint16_t oxygenId = 0;
    static uint16_t carbonMonoxideId = 0;
    static bool airResolved = false;
    if (!airResolved) {
        airResolved = true;
        if (const Substance* o2 = chem().substanceByFormula("O2", Phase::Gas))
            oxygenId = o2->id;
        if (const Substance* co = chem().substanceByFormula("CO", Phase::Gas))
            carbonMonoxideId = co->id;
    }

    // Scratch buffers, reused across calls. This stage is serial by contract
    // (ARCHITECTURE.md section 3), and the tick must not allocate.
    static std::vector<uint16_t> available;
    static std::vector<uint16_t> applicable;
    available.clear();
    available.insert(available.end(), held.begin(), held.end());
    if (oxygenId != 0) available.push_back(oxygenId);
    if (cond.temperature >= 900.0 && carbonMonoxideId != 0) {
        // A fire hot enough to make carbon monoxide has it in the atmosphere,
        // whether or not the agent understands that it is there.
        available.push_back(carbonMonoxideId);
    }

    chem().findApplicable(available, cond, applicable);
    if (applicable.empty()) return out;

    // Which of the possible reactions actually gets noticed is a draw. That is
    // what makes discovery stochastic without making it arbitrary: the set it
    // draws from was determined entirely by physics.
    const uint16_t chosen = applicable[rng.nextBounded(static_cast<uint32_t>(applicable.size()))];
    const Reaction* r = chem().reaction(chosen);
    if (!r) return out;

    // Consume the reactants the agent actually holds, and keep the products.
    for (const ReactionTerm& t : r->reactants) {
        const Substance* s = chem().substance(t.substance);
        if (!s || s->phase == Phase::Gas) continue;   // gases come from the air
        inv.consume(slot, t.substance, static_cast<float>(t.coefficient) * 0.25f);
    }
    for (const ReactionTerm& t : r->products) {
        const Substance* s = chem().substance(t.substance);
        if (!s || s->phase == Phase::Gas) continue;   // gases are lost to the air
        inv.add(slot, t.substance, static_cast<float>(t.coefficient) * 0.25f);
    }

    out.happened = true;
    out.reactionId = chosen;
    out.technique = best;
    out.temperature = cond.temperature;

    // How much the agent thinks it is worth. A reaction that makes a metal is
    // valued highly; one that makes a gas it cannot hold, less so.
    float value = 0.2f;
    for (const ReactionTerm& t : r->products) {
        const Substance* s = chem().substance(t.substance);
        if (!s) continue;
        if (s->phase != Phase::Solid && s->phase != Phase::Liquid) continue;
        for (const FormulaTerm& ft : s->formula.terms) {
            const Element* e = elements().byZ(ft.z);
            if (!e) continue;
            if (e->category.find("metal") != std::string::npos) value += 0.6f;
            if (e->hardness > 3.0) value += 0.2f;
        }
    }
    value = std::min(3.0f, value * (0.5f + curiosityWeight * 0.5f));

    out.newToAgent = kb.grant(slot, chosen, best, value, 1.0f, tick, agentUid);
    if (out.newToAgent) out.firstEver = kb.recordDiscovery(chosen, tick, agentUid, agentName);

    char buf[320];
    std::snprintf(buf, sizeof(buf), "%s at %.0f K (%s)", r->name.c_str(), cond.temperature,
                  techniqueName(best));
    out.description = buf;
    return out;
}

// ---------------------------------------------------------------------------
// Teaching. This is where culture diverges from genetics.
// ---------------------------------------------------------------------------

bool teachKnowledge(KnowledgeBase& kb, uint32_t teacher, uint32_t student,
                    uint64_t tick, Rng& rng, std::string* whatOut) {
    const std::vector<KnowledgeUnit>& mine = kb.known(teacher);
    if (mine.empty()) return false;

    // A teacher passes on what it values most, not a random fact.
    size_t bestIdx = 0;
    for (size_t i = 1; i < mine.size(); ++i)
        if (mine[i].valuation > mine[bestIdx].valuation) bestIdx = i;
    const KnowledgeUnit& unit = mine[bestIdx];
    if (!unit.usable()) return false;
    if (kb.knows(student, unit.reactionId)) return false;

    // Transmission is lossy. Each retelling degrades fidelity a little, and
    // below a threshold the knowledge stops working -- which is how a technique
    // can be half-remembered and useless rather than simply present or absent.
    const float loss = cfg().getF("knowledge.teach_fidelity_loss", 0.08f);
    const float noise = rng.rangef(0.0f, loss);
    const float fidelity = std::max(0.0f, unit.fidelity - noise);

    const bool isNew = kb.grant(student, unit.reactionId, static_cast<Technique>(unit.technique),
                                unit.valuation * 0.8f, fidelity, tick, unit.discovererUid);
    if (isNew && whatOut) {
        const Reaction* r = chem().reaction(unit.reactionId);
        *whatOut = r ? r->name : "a technique";
    }
    // Teaching a technique carries the technique itself with it, which is why
    // fire spreads before anything that depends on fire can.
    const float transmission = cfg().getF("knowledge.technique_transmission", 0.35f);
    for (int i = 0; i < static_cast<int>(Technique::Count); ++i) {
        const Technique t = static_cast<Technique>(i);
        if (kb.hasTechnique(teacher, t) && rng.nextFloat() < transmission)
            kb.grantTechnique(student, t);
    }
    return isNew;
}

// Techniques an agent can work out for itself, given what it has already got.
// Each step is a real capability built on the last: you cannot bank a fire you
// cannot light, and you cannot run a bellows without a kiln to put it on.
bool tryAcquireTechnique(KnowledgeBase& kb, uint32_t slot, Rng& rng, Technique& acquiredOut) {
    struct Step { Technique t; Technique requires_; double chance; };
    static const Step kLadder[] = {
        {Technique::Mixing,      Technique::Mixing,     1.00},
        {Technique::Wetting,     Technique::Mixing,     0.90},
        {Technique::Grinding,    Technique::Mixing,     0.60},
        {Technique::OpenFire,    Technique::Grinding,   0.030},
        {Technique::BankedFire,  Technique::OpenFire,   0.012},
        {Technique::Kiln,        Technique::BankedFire, 0.006},
        {Technique::Bellows,     Technique::Kiln,       0.003},
        {Technique::Containment, Technique::Kiln,       0.001},
    };
    const double scale = cfg().getFloat("knowledge.technique_discovery_rate", 1.0);
    for (const Step& s : kLadder) {
        if (kb.hasTechnique(slot, s.t)) continue;
        if (s.t != s.requires_ && !kb.hasTechnique(slot, s.requires_)) continue;
        if (rng.nextDouble() >= s.chance * scale) continue;
        kb.grantTechnique(slot, s.t);
        acquiredOut = s.t;
        return true;
    }
    return false;
}

}  // namespace gen
