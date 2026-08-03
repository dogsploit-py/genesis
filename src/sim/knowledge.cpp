#include "sim/knowledge.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/config.h"
#include "core/serialize.h"

namespace gen {

const char* techniqueName(Technique t) {
    switch (t) {
        case Technique::Mixing:      return "Mixing";
        case Technique::Wetting:     return "Wetting";
        case Technique::Grinding:    return "Grinding";
        case Technique::OpenFire:    return "Open fire";
        case Technique::BankedFire:  return "Banked fire";
        case Technique::Kiln:        return "Kiln";
        case Technique::Bellows:     return "Bellows furnace";
        case Technique::Containment: return "Containment";
        case Technique::Electricity: return "Electricity";
        case Technique::Count:       break;
    }
    return "?";
}

const char* techniqueNote(Technique t) {
    switch (t) {
        case Technique::Mixing:
            return "Putting two substances together. Enough for anything that proceeds at "
                   "ambient temperature.";
        case Technique::Wetting:
            return "Adding water. Slaking lime needs nothing more than this.";
        case Technique::Grinding:
            return "Breaking material up. Does not change the chemistry, but multiplies the "
                   "surface area and therefore the rate -- which can be the difference between "
                   "a reaction that happens and one that does not.";
        case Technique::OpenFire:
            return "About 900 K. Cooks food, decomposes malachite, and makes charcoal.";
        case Technique::BankedFire:
            return "About 1200 K with a starved, reducing atmosphere. The Boudouard shift makes "
                   "carbon monoxide here, and copper oxide will reduce.";
        case Technique::Kiln:
            return "About 1400 K, sustained and enclosed. Fires pottery, calcines limestone, "
                   "smelts tin.";
        case Technique::Bellows:
            return "About 1700 K. The threshold for iron, which is why the bloomery is the "
                   "hardest thing anyone builds for several thousand years.";
        case Technique::Containment:
            return "Holding pressure. Shifts every equilibrium that changes the number of gas "
                   "molecules -- without it, Haber-Bosch simply sits at the wrong end.";
        case Technique::Electricity:
            return "Supplying free energy directly. The only way to reach aluminium, chlorine "
                   "and anything else whose oxide is too stable for carbon.";
        case Technique::Count: break;
    }
    return "";
}

double techniqueTemperature(Technique t) {
    switch (t) {
        case Technique::Mixing:      return 293.0;
        case Technique::Wetting:     return 293.0;
        case Technique::Grinding:    return 300.0;
        case Technique::OpenFire:    return 900.0;
        case Technique::BankedFire:  return 1200.0;
        case Technique::Kiln:        return 1400.0;
        case Technique::Bellows:     return 1700.0;
        case Technique::Containment: return 700.0;
        case Technique::Electricity: return 1300.0;
        case Technique::Count:       break;
    }
    return 293.0;
}

double techniquePressure(Technique t) {
    if (t == Technique::Containment) return 20.0e6;
    return kStandardPressure;
}

// ---------------------------------------------------------------------------

void KnowledgeBase::configure(size_t maxAgents) {
    m_known.assign(maxAgents, {});
    m_techniques.assign(maxAgents, 0);
    m_discoveries.clear();
    m_nextId = 0;
}

void KnowledgeBase::clear() {
    for (auto& k : m_known) k.clear();
    std::fill(m_techniques.begin(), m_techniques.end(), 0u);
    m_discoveries.clear();
    m_nextId = 0;
}

bool KnowledgeBase::knows(uint32_t slot, uint16_t reactionId) const {
    if (slot >= m_known.size()) return false;
    for (const KnowledgeUnit& k : m_known[slot])
        if (k.reactionId == reactionId && k.usable()) return true;
    return false;
}

bool KnowledgeBase::grant(uint32_t slot, uint16_t reactionId, Technique via, float valuation,
                          float fidelity, uint64_t tick, uint64_t discovererUid) {
    if (slot >= m_known.size()) return false;
    for (KnowledgeUnit& k : m_known[slot]) {
        if (k.reactionId != reactionId) continue;
        // Already known: hearing it again from a better source repairs it, and
        // seeing it work again raises what the holder thinks it is worth.
        k.fidelity = std::max(k.fidelity, fidelity);
        k.valuation = std::max(k.valuation, valuation);
        return false;
    }
    const size_t cap = static_cast<size_t>(cfg().getInt("knowledge.capacity", 64));
    if (m_known[slot].size() >= cap) {
        // Memory is finite. What goes is what the holder values least, which is
        // why useless knowledge is lost first and useful knowledge persists.
        size_t worst = 0;
        for (size_t i = 1; i < m_known[slot].size(); ++i)
            if (m_known[slot][i].valuation < m_known[slot][worst].valuation) worst = i;
        if (m_known[slot][worst].valuation >= valuation) return false;
        m_known[slot].erase(m_known[slot].begin() + static_cast<long>(worst));
    }
    KnowledgeUnit k;
    k.id = ++m_nextId;
    k.reactionId = reactionId;
    k.technique = static_cast<uint8_t>(via);
    k.valuation = valuation;
    k.fidelity = fidelity;
    k.discoveredTick = tick;
    k.discovererUid = discovererUid;
    m_known[slot].push_back(k);
    return true;
}

void KnowledgeBase::forget(uint32_t slot, uint16_t reactionId) {
    if (slot >= m_known.size()) return;
    auto& v = m_known[slot];
    v.erase(std::remove_if(v.begin(), v.end(),
                           [&](const KnowledgeUnit& k) { return k.reactionId == reactionId; }),
            v.end());
}

bool KnowledgeBase::recordDiscovery(uint16_t reactionId, uint64_t tick, uint64_t uid,
                                    const std::string& name) {
    for (DiscoveryRecord& d : m_discoveries) {
        if (d.reactionId != reactionId) continue;
        if (d.holders == 0) d.everLost = true;   // rediscovered after being lost
        return false;
    }
    DiscoveryRecord d;
    d.reactionId = reactionId;
    d.firstTick = tick;
    d.discovererUid = uid;
    d.discovererName = name;
    d.holders = 1;
    m_discoveries.push_back(std::move(d));
    return true;
}

const DiscoveryRecord* KnowledgeBase::discovery(uint16_t reactionId) const {
    for (const DiscoveryRecord& d : m_discoveries)
        if (d.reactionId == reactionId) return &d;
    return nullptr;
}

void KnowledgeBase::recountHolders(const std::vector<uint32_t>& liveSlots) {
    for (DiscoveryRecord& d : m_discoveries) d.holders = 0;
    for (uint32_t slot : liveSlots) {
        if (slot >= m_known.size()) continue;
        for (const KnowledgeUnit& k : m_known[slot]) {
            if (!k.usable()) continue;
            for (DiscoveryRecord& d : m_discoveries)
                if (d.reactionId == k.reactionId) { ++d.holders; break; }
        }
    }
}

bool KnowledgeBase::hasTechnique(uint32_t slot, Technique t) const {
    if (slot >= m_techniques.size()) return false;
    return (m_techniques[slot] & (1u << static_cast<uint32_t>(t))) != 0;
}

void KnowledgeBase::grantTechnique(uint32_t slot, Technique t) {
    if (slot >= m_techniques.size()) return;
    m_techniques[slot] |= (1u << static_cast<uint32_t>(t));
}

double KnowledgeBase::bestTemperature(uint32_t slot) const {
    double best = 293.0;
    for (int i = 0; i < static_cast<int>(Technique::Count); ++i) {
        const Technique t = static_cast<Technique>(i);
        if (hasTechnique(slot, t)) best = std::max(best, techniqueTemperature(t));
    }
    return best;
}

double KnowledgeBase::bestPressure(uint32_t slot) const {
    return hasTechnique(slot, Technique::Containment) ? 20.0e6 : kStandardPressure;
}

size_t KnowledgeBase::totalKnownUnits() const {
    size_t n = 0;
    for (const auto& v : m_known) n += v.size();
    return n;
}

// ---------------------------------------------------------------------------

void KnowledgeBase::serializeAgent(BinaryWriter& w, uint32_t slot) const {
    const uint32_t mask = (slot < m_techniques.size()) ? m_techniques[slot] : 0u;
    w.pod(mask);
    const uint32_t n = (slot < m_known.size()) ? static_cast<uint32_t>(m_known[slot].size()) : 0u;
    w.pod(n);
    if (slot >= m_known.size()) return;
    for (const KnowledgeUnit& k : m_known[slot]) {
        w.pod(k.id); w.pod(k.reactionId); w.pod(k.technique);
        w.pod(k.valuation); w.pod(k.fidelity);
        w.pod(k.discoveredTick); w.pod(k.discovererUid); w.pod(k.timesTaught);
    }
}

void KnowledgeBase::deserializeAgent(BinaryReader& r, uint32_t slot) {
    uint32_t mask = 0;
    r.pod(mask);
    if (slot < m_techniques.size()) m_techniques[slot] = mask;
    uint32_t n = 0;
    r.pod(n);
    if (slot < m_known.size()) m_known[slot].clear();
    if (n > 100000u) return;
    for (uint32_t i = 0; i < n && r.ok(); ++i) {
        KnowledgeUnit k;
        r.pod(k.id); r.pod(k.reactionId); r.pod(k.technique);
        r.pod(k.valuation); r.pod(k.fidelity);
        r.pod(k.discoveredTick); r.pod(k.discovererUid); r.pod(k.timesTaught);
        if (slot < m_known.size()) m_known[slot].push_back(k);
        if (k.id > m_nextId) m_nextId = k.id;
    }
}

void KnowledgeBase::serializeWorld(BinaryWriter& w) const {
    w.pod(m_nextId);
    const uint32_t n = static_cast<uint32_t>(m_discoveries.size());
    w.pod(n);
    for (const DiscoveryRecord& d : m_discoveries) {
        w.pod(d.reactionId); w.pod(d.firstTick); w.pod(d.discovererUid);
        w.pod(d.holders);
        const uint8_t lost = d.everLost ? 1u : 0u;
        w.pod(lost);
        w.str(d.discovererName);
    }
}

void KnowledgeBase::deserializeWorld(BinaryReader& r) {
    m_discoveries.clear();
    r.pod(m_nextId);
    uint32_t n = 0;
    r.pod(n);
    if (n > 100000u) return;
    for (uint32_t i = 0; i < n && r.ok(); ++i) {
        DiscoveryRecord d;
        r.pod(d.reactionId); r.pod(d.firstTick); r.pod(d.discovererUid);
        r.pod(d.holders);
        uint8_t lost = 0;
        r.pod(lost);
        d.everLost = lost != 0;
        r.str(d.discovererName);
        m_discoveries.push_back(std::move(d));
    }
}

}  // namespace gen
