// sim/knowledge.h — discovery, knowledge units, and culture.
//
// Discovery is a SEARCH, not a lookup. An agent combines what it is holding
// under whatever conditions it can create -- a fire, a kiln, grinding, wetting
// -- and the reaction engine says what happens. There is no recipe list and no
// tech tree; if a reduction is downhill and fast enough at the temperature
// available, it happens, and if it is not, it does not, however much the agent
// wants it to.
//
// What an agent LEARNS from that is a KnowledgeUnit: inputs, conditions,
// outcome, and a valuation. Knowledge is stored separately from genes and
// travels separately: it is taught, it mutates in the telling, and it is lost
// when the last holder dies without teaching anyone. That is culture, and it is
// tracked distinctly from genetics throughout.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "chem/materials.h"
#include "chem/reactions.h"
#include "core/rng.h"

namespace gen {

class BinaryWriter;
class BinaryReader;

// The conditions an agent can actually bring about, in increasing difficulty.
// Each is a real capability that has to be acquired, not a checkbox.
enum class Technique : uint8_t {
    Mixing = 0,      // put two things together
    Wetting,         // add water
    Grinding,        // increase surface area, which raises the effective rate
    OpenFire,        // ~900 K
    BankedFire,      // ~1200 K, reducing atmosphere
    Kiln,            // ~1400 K, sustained
    Bellows,         // ~1700 K
    Containment,     // pressure
    Electricity,     // much later
    Count
};
const char* techniqueName(Technique t);
const char* techniqueNote(Technique t);
double techniqueTemperature(Technique t);   // K
double techniquePressure(Technique t);      // Pa

// One thing an agent knows. Deliberately concrete: not "metallurgy" but "these
// inputs, heated this hot, give this".
struct KnowledgeUnit {
    uint32_t id = 0;
    uint16_t reactionId = 0;          // what the engine actually did
    uint8_t  technique = 0;           // Technique used
    float    valuation = 0.0f;        // how useful the holder believes it is
    float    fidelity = 1.0f;         // 1 = as discovered, lower = garbled in transmission
    uint64_t discoveredTick = 0;
    uint64_t discovererUid = 0;
    uint32_t timesTaught = 0;

    // A knowledge unit that has been mangled past usefulness stops working.
    bool usable() const { return fidelity > 0.35f; }
};

// The world's record of what has ever been discovered, and by whom first.
struct DiscoveryRecord {
    uint16_t reactionId = 0;
    uint64_t firstTick = 0;
    uint64_t discovererUid = 0;
    std::string discovererName;
    uint32_t holders = 0;      // how many agents currently know it
    bool     everLost = false; // has it been forgotten and rediscovered?
};

class KnowledgeBase {
public:
    void configure(size_t maxAgents);
    void clear();

    // -- per-agent knowledge -------------------------------------------------
    const std::vector<KnowledgeUnit>& known(uint32_t slot) const { return m_known[slot]; }
    std::vector<KnowledgeUnit>& knownMutable(uint32_t slot) { return m_known[slot]; }
    bool knows(uint32_t slot, uint16_t reactionId) const;
    // Adds or reinforces. Returns true if this is new to the agent.
    bool grant(uint32_t slot, uint16_t reactionId, Technique via, float valuation,
               float fidelity, uint64_t tick, uint64_t discovererUid);
    void forget(uint32_t slot, uint16_t reactionId);
    void clearAgent(uint32_t slot) { m_known[slot].clear(); }

    // -- world discovery record ---------------------------------------------
    // Returns true if this is the FIRST time anyone has discovered it.
    bool recordDiscovery(uint16_t reactionId, uint64_t tick, uint64_t uid,
                         const std::string& name);
    const std::vector<DiscoveryRecord>& discoveries() const { return m_discoveries; }
    const DiscoveryRecord* discovery(uint16_t reactionId) const;
    // Counted over the LIVING only. A dead agent's slot keeps its knowledge
    // until the slot is reused, and counting that would mean the world never
    // forgets anything -- which would quietly destroy the whole loss mechanic.
    void recountHolders(const std::vector<uint32_t>& liveSlots);
    const std::vector<std::vector<KnowledgeUnit>>& allKnown() const { return m_known; }

    // -- culture -------------------------------------------------------------
    // Which techniques an agent can perform. Techniques are themselves learned
    // and transmitted, which is why fire has to spread before smelting can.
    bool hasTechnique(uint32_t slot, Technique t) const;
    void grantTechnique(uint32_t slot, Technique t);
    uint32_t techniqueMask(uint32_t slot) const { return m_techniques[slot]; }
    void setTechniqueMask(uint32_t slot, uint32_t mask) { m_techniques[slot] = mask; }

    // The best temperature an agent can reach with what it knows.
    double bestTemperature(uint32_t slot) const;
    double bestPressure(uint32_t slot) const;

    size_t totalKnownUnits() const;
    uint32_t nextUnitId() { return ++m_nextId; }

    void serializeAgent(BinaryWriter& w, uint32_t slot) const;
    void deserializeAgent(BinaryReader& r, uint32_t slot);
    void serializeWorld(BinaryWriter& w) const;
    void deserializeWorld(BinaryReader& r);

private:
    std::vector<std::vector<KnowledgeUnit>> m_known;
    std::vector<uint32_t> m_techniques;   // bitmask of Technique
    std::vector<DiscoveryRecord> m_discoveries;
    uint32_t m_nextId = 0;
};

// The outcome of one experiment, for the event feed and the UI.
struct ExperimentOutcome {
    bool     happened = false;
    bool     firstEver = false;
    bool     newToAgent = false;
    uint16_t reactionId = 0;
    Technique technique = Technique::Mixing;
    double   temperature = 0.0;
    std::string description;
};

}  // namespace gen
