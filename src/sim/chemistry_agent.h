// sim/chemistry_agent.h — the bridge between agents and chemistry.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "chem/reactions.h"
#include "core/rng.h"
#include "sim/knowledge.h"
#include "sim/world.h"

namespace gen {

class BinaryWriter;
class BinaryReader;

// What each ore and rock actually is, so the mineralogy and the chemistry
// cannot drift apart. Returns nullptr where there is no matching substance.
const char* oreSubstanceFormula(OreType o);
const char* rockSubstanceFormula(RockType r);

// A small fixed inventory of substances per agent. Fixed because nothing in the
// tick may allocate, and small because an agent carrying forty reagents is not
// a forager.
class Inventory {
public:
    void configure(size_t maxAgents, int slots);
    void clearAgent(uint32_t slot);

    bool  add(uint32_t slot, uint16_t substanceId, float amount);
    bool  consume(uint32_t slot, uint16_t substanceId, float amount);
    float amountOf(uint32_t slot, uint16_t substanceId) const;
    void  list(uint32_t slot, std::vector<uint16_t>& out) const;
    int   slots() const { return m_slots; }
    uint16_t substanceAt(uint32_t slot, int i) const {
        return m_substance[slot * static_cast<size_t>(m_slots) + i];
    }
    float amountAt(uint32_t slot, int i) const {
        return m_amount[slot * static_cast<size_t>(m_slots) + i];
    }

    void serializeAgent(BinaryWriter& w, uint32_t slot) const;
    void deserializeAgent(BinaryReader& r, uint32_t slot);

private:
    std::vector<uint16_t> m_substance;
    std::vector<float>    m_amount;
    int m_slots = 8;
};

// Picks up whatever the tile offers: ore, then bedrock, then vegetation, then
// water. Returns true and names what was taken.
bool gatherFromTile(Inventory& inv, uint32_t slot, const World& world, int tx, int ty,
                    std::string* whatOut);

// THE discovery mechanic. Brings the agent's held substances together at the
// hottest temperature its techniques can reach and asks the reaction engine
// what happens. Nothing is looked up.
ExperimentOutcome runExperiment(Inventory& inv, KnowledgeBase& kb, uint32_t slot,
                                uint64_t agentUid, const std::string& agentName,
                                uint64_t tick, Rng& rng, float curiosityWeight);

// Passes the teacher's most valued knowledge to the student, degraded by the
// telling. Returns true if the student learned something new.
bool teachKnowledge(KnowledgeBase& kb, uint32_t teacher, uint32_t student,
                    uint64_t tick, Rng& rng, std::string* whatOut);

// A chance to work out the next technique up the ladder, given what the agent
// already has. Returns true and reports what was acquired.
bool tryAcquireTechnique(KnowledgeBase& kb, uint32_t slot, Rng& rng, Technique& acquiredOut);

}  // namespace gen
