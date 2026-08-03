#include "sim/agent.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/config.h"
#include "core/serialize.h"
#include "sim/time.h"
#include "sim/world.h"

namespace gen {

namespace {
constexpr float kPi = 3.14159265358979f;
constexpr int   kVisionSectors = 6;
constexpr int   kRecentMates = 3;

// Constants read on the per-agent path. Resolved once; the value is still read
// live on every call, so a god-mode rule change takes effect immediately.
// The radius at which agents can groom, share and fight. One constant because
// all three used the same 1.5 tiles, which is what lets a single neighbour list
// serve all of them.
constexpr float kInteractionRadius = 1.5f;

const CfgRef kKinDepth("agents.kin_depth");
const CfgRef kRelCapacity("agents.relationship_capacity");

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
}  // namespace

const char* lifeStageName(LifeStage s) {
    switch (s) {
        case LifeStage::Embryo:     return "Embryo";
        case LifeStage::Juvenile:   return "Juvenile";
        case LifeStage::Adolescent: return "Adolescent";
        case LifeStage::Adult:      return "Adult";
        case LifeStage::Senescent:  return "Senescent";
        case LifeStage::Dead:       return "Dead";
        case LifeStage::Count:      break;
    }
    return "?";
}

const char* deathCauseName(DeathCause c) {
    switch (c) {
        case DeathCause::None:        return "-";
        case DeathCause::Starvation:  return "Starvation";
        case DeathCause::Dehydration: return "Dehydration";
        case DeathCause::Exposure:    return "Exposure";
        case DeathCause::Predation:   return "Predation";
        case DeathCause::Violence:    return "Violence";
        case DeathCause::Disease:     return "Disease";
        case DeathCause::Childbirth:  return "Childbirth";
        case DeathCause::Accident:    return "Accident";
        case DeathCause::OldAge:      return "Old age";
        case DeathCause::LethalGenes: return "Lethal genes";
        case DeathCause::Divine:      return "Divine intervention";
        case DeathCause::Count:       break;
    }
    return "?";
}

const char* actionName(Action a) {
    switch (a) {
        case Action::Idle:     return "idle";
        case Action::Move:     return "moving";
        case Action::Eat:      return "eating";
        case Action::Drink:    return "drinking";
        case Action::Rest:     return "resting";
        case Action::Flee:     return "fleeing";
        case Action::Attack:   return "attacking";
        case Action::Groom:    return "grooming";
        case Action::Court:    return "courting";
        case Action::Mate:     return "mating";
        case Action::Vocalise: return "vocalising";
        case Action::Teach:    return "teaching";
        case Action::Share:    return "sharing";
        case Action::Build:    return "building";
        case Action::Nurse:    return "nursing";
        case Action::Count:    break;
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Pedigree
// ---------------------------------------------------------------------------

void Pedigree::configure(size_t capacity) {
    m_capacity = capacity;
    clear();
}

void Pedigree::clear() {
    m_records.clear();
    m_index.clear();
    m_baseIndex = 0;
}

void Pedigree::insert(const PedigreeRecord& rec) {
    m_index[rec.uid] = m_baseIndex + m_records.size();
    m_records.push_back(rec);
    // Oldest records are dropped when the cap is hit. Losing deep ancestry only
    // degrades relatedness estimates for very old lineages; it never corrupts a
    // living agent, and the cap is configurable.
    while (m_records.size() > m_capacity) {
        m_index.erase(m_records.front().uid);
        m_records.pop_front();
        ++m_baseIndex;
    }
}

void Pedigree::recordDeath(uint64_t uid, uint64_t tick, DeathCause cause) {
    PedigreeRecord* r = findMutable(uid);
    if (!r) return;
    r->deathTick = tick;
    r->deathCause = static_cast<uint8_t>(cause);
}

const PedigreeRecord* Pedigree::find(uint64_t uid) const {
    auto it = m_index.find(uid);
    if (it == m_index.end() || it->second < m_baseIndex) return nullptr;
    const size_t i = it->second - m_baseIndex;
    return (i < m_records.size()) ? &m_records[i] : nullptr;
}

PedigreeRecord* Pedigree::findMutable(uint64_t uid) {
    auto it = m_index.find(uid);
    if (it == m_index.end() || it->second < m_baseIndex) return nullptr;
    const size_t i = it->second - m_baseIndex;
    return (i < m_records.size()) ? &m_records[i] : nullptr;
}

float Pedigree::kinship(uint64_t a, uint64_t b, int depth) const {
    if (a == 0 || b == 0) return 0.0f;
    if (a == b) return 0.5f;              // ignoring the individual's own F
    if (depth <= 0) return 0.0f;

    const PedigreeRecord* ra = find(a);
    const PedigreeRecord* rb = find(b);
    if (!ra && !rb) return 0.0f;

    // Recurse on whichever individual is YOUNGER, which is what keeps the
    // recursion moving strictly up the pedigree and terminating.
    const bool aYounger = (ra && rb) ? (ra->birthTick >= rb->birthTick) : (ra != nullptr);
    if (aYounger && ra)
        return 0.5f * (kinship(ra->motherUid, b, depth - 1) + kinship(ra->fatherUid, b, depth - 1));
    if (rb)
        return 0.5f * (kinship(a, rb->motherUid, depth - 1) + kinship(a, rb->fatherUid, depth - 1));
    return 0.0f;
}

float Pedigree::relatedness(uint64_t a, uint64_t b, int maxDepth) const {
    if (a == b) return 1.0f;
    return clampf(2.0f * kinship(a, b, maxDepth), 0.0f, 1.0f);
}

void Pedigree::serialize(BinaryWriter& w) const {
    const uint32_t n = static_cast<uint32_t>(m_records.size());
    w.pod(n);
    for (const PedigreeRecord& r : m_records) {
        w.pod(r.uid); w.pod(r.motherUid); w.pod(r.fatherUid);
        w.pod(r.birthTick); w.pod(r.deathTick);
        w.pod(r.deathCause); w.pod(r.chromosomalSex);
        w.pod(r.sexExpression); w.pod(r.offspringCount);
        w.str(r.name);
    }
}

void Pedigree::deserialize(BinaryReader& r) {
    clear();
    uint32_t n = 0;
    r.pod(n);
    if (n > 20000000u) return;
    for (uint32_t i = 0; i < n && r.ok(); ++i) {
        PedigreeRecord rec;
        r.pod(rec.uid); r.pod(rec.motherUid); r.pod(rec.fatherUid);
        r.pod(rec.birthTick); r.pod(rec.deathTick);
        r.pod(rec.deathCause); r.pod(rec.chromosomalSex);
        r.pod(rec.sexExpression); r.pod(rec.offspringCount);
        r.str(rec.name);
        insert(rec);
    }
}

// ---------------------------------------------------------------------------
// Agents: setup
// ---------------------------------------------------------------------------

void Agents::configure(size_t maxAgents, Rng& rngForSchema) {
    m_capacity = maxAgents;

    m_alive.assign(maxAgents, 0);
    m_generation.assign(maxAgents, 0);
    m_uid.assign(maxAgents, 0);
    m_flags.assign(maxAgents, 0);
    m_x.assign(maxAgents, 0.0f);
    m_y.assign(maxAgents, 0.0f);
    m_vx.assign(maxAgents, 0.0f);
    m_vy.assign(maxAgents, 0.0f);
    m_heading.assign(maxAgents, 0.0f);
    m_energy.assign(maxAgents, 0.0f);
    m_hydration.assign(maxAgents, 0.0f);
    m_health.assign(maxAgents, 1.0f);
    m_bodyTemp.assign(maxAgents, 37.0f);
    m_pain.assign(maxAgents, 0.0f);
    m_stress.assign(maxAgents, 0.0f);
    m_damage.assign(maxAgents, 0.0f);
    m_telomere.assign(maxAgents, 1.0f);
    m_birthTick.assign(maxAgents, 0);
    m_ageOffset.assign(maxAgents, 0);
    m_stage.assign(maxAgents, static_cast<uint8_t>(LifeStage::Dead));
    m_action.assign(maxAgents, static_cast<uint8_t>(Action::Idle));
    m_chromosomalSex.assign(maxAgents, 0);
    m_deathCause.assign(maxAgents, 0);
    m_phenotype.assign(maxAgents, Phenotype());
    m_drives.assign(maxAgents, DriveState());
    m_display.assign(maxAgents, DisplayVector());
    m_prefs.assign(maxAgents, PreferenceVector());
    m_motherUid.assign(maxAgents, 0);
    m_fatherUid.assign(maxAgents, 0);
    m_bondedUid.assign(maxAgents, 0);
    m_pregnantByUid.assign(maxAgents, 0);
    m_embryoUid.assign(maxAgents, 0);
    m_gestationRemaining.assign(maxAgents, 0.0f);
    m_courtCooldown.assign(maxAgents, 0.0f);
    m_recentMateUid.assign(maxAgents * kRecentMates, 0);
    m_recentMateWeight.assign(maxAgents * kRecentMates, 0.0f);
    m_offspringCount.assign(maxAgents, 0);
    m_status.assign(maxAgents, 0.5f);
    m_reputation.assign(maxAgents, 0.0f);
    m_name.assign(maxAgents, std::string());
    m_relationships.assign(maxAgents, {});
    // Reserved to capacity up front. Social memory is bounded, so the vector
    // never has to grow -- and growing it would mean a heap allocation inside
    // the tick, which the architecture forbids for exactly this reason.
    {
        const size_t relCap =
            static_cast<size_t>(cfg().getInt("agents.relationship_capacity", 24));
        for (auto& v : m_relationships) v.reserve(relCap);
    }
    m_eatMaxTake.assign(maxAgents, 0.0f);
    m_eatAppetite.assign(maxAgents, 0.0f);
    m_eatEnergyPerKg.assign(maxAgents, 0.0f);
    m_attackPower.assign(maxAgents, 0.0f);
    m_inputs.assign(maxAgents * kBrainInputCount, 0.0f);
    m_outputs.assign(maxAgents * kBrainOutputCount, 0.0f);
    m_signals.assign(maxAgents * 4, 0.0f);
    m_reward.assign(maxAgents, 0.0f);
    m_memFoodX.assign(maxAgents, -1.0f);
    m_memFoodY.assign(maxAgents, -1.0f);
    m_memWaterX.assign(maxAgents, -1.0f);
    m_memWaterY.assign(maxAgents, -1.0f);

    m_freeSlots.clear();
    m_freeSlots.reserve(maxAgents);
    // Descending, so pop_back hands out slot 0 first and a fresh world fills
    // slots in ascending order -- which keeps conflict resolution by slot index
    // stable and reproducible.
    for (size_t i = maxAgents; i-- > 0;) m_freeSlots.push_back(static_cast<uint32_t>(i));
    m_liveSlots.clear();
    m_liveSlots.reserve(maxAgents);
    m_pendingDeath.clear();
    m_uidToSlot.clear();
    m_attractionOverride.clear();
    m_population = 0;
    m_nextUid = 1;
    m_totalBirths = m_totalDeaths = 0;
    m_lifespanSum = 0.0;
    m_lifespanCount = 0;
    m_stats = PopulationStats();

    m_genetics.configure(maxAgents,
                         static_cast<uint16_t>(cfg().getInt("genetics.gene_capacity", 320)),
                         rngForSchema);
    m_brains.configure(maxAgents,
                       static_cast<uint16_t>(cfg().getInt("brain.node_capacity", 128)),
                       static_cast<uint16_t>(cfg().getInt("brain.connection_capacity", 224)));
    m_pedigree.configure(static_cast<size_t>(cfg().getInt("sim.pedigree_capacity", 400000)));
    m_inventory.configure(maxAgents, static_cast<int>(cfg().getInt("chem.inventory_slots", 8)));
    m_knowledge.configure(maxAgents);
    m_speciation.configure(maxAgents);

    const int sys = static_cast<int>(cfg().getInt("sex.system", 0));
    m_sexSystem = static_cast<SexSystem>(clampf(static_cast<float>(sys), 0.0f, 2.0f));
}

void Agents::clear() {
    for (uint32_t slot : m_liveSlots) { m_alive[slot] = 0; m_relationships[slot].clear(); }
    m_liveSlots.clear();
    m_freeSlots.clear();
    for (size_t i = m_capacity; i-- > 0;) m_freeSlots.push_back(static_cast<uint32_t>(i));
    m_uidToSlot.clear();
    m_attractionOverride.clear();
    m_pedigree.clear();
    m_population = 0;
    m_nextUid = 1;
    m_stats = PopulationStats();
}

uint32_t Agents::allocateSlot() {
    if (m_freeSlots.empty()) return 0xFFFFFFFFu;
    const uint32_t slot = m_freeSlots.back();
    m_freeSlots.pop_back();
    m_alive[slot] = 1;
    ++m_generation[slot];
    m_liveSlots.push_back(slot);
    ++m_population;
    return slot;
}

void Agents::releaseSlot(uint32_t slot) {
    m_alive[slot] = 0;
    m_relationships[slot].clear();
    // Knowledge dies with its holder unless it was taught to someone first.
    // That is the whole reason a technique can be LOST here, so it has to be
    // literally true in memory and not merely true in the reporting.
    m_inventory.clearAgent(slot);
    m_knowledge.clearAgent(slot);
    m_speciation.setSpeciesOf(slot, 0);
    m_uidToSlot.erase(m_uid[slot]);
    m_freeSlots.push_back(slot);
    --m_population;
}

int32_t Agents::slotOfUid(uint64_t uid) const {
    auto it = m_uidToSlot.find(uid);
    return (it == m_uidToSlot.end()) ? -1 : static_cast<int32_t>(it->second);
}

float Agents::ageYears(uint32_t slot, uint64_t tick) const {
    const uint64_t lived = (tick > m_birthTick[slot]) ? (tick - m_birthTick[slot]) : 0;
    return static_cast<float>(lived + m_ageOffset[slot]) / static_cast<float>(kHoursPerYear);
}

void Agents::emitEvent(uint8_t kind, int32_t x, int32_t y, uint64_t subject, std::string text) {
    if (m_pendingEvents.size() > 4096) return;   // never let a burst unbound memory
    PendingEvent e;
    e.kind = kind;
    e.x = x;
    e.y = y;
    e.subject = subject;
    e.text = std::move(text);
    m_pendingEvents.push_back(std::move(e));
}

// ---------------------------------------------------------------------------
// Creation
// ---------------------------------------------------------------------------

void Agents::redevelop(uint32_t slot, RngBank& rng) {
    const float noise = cfg().getF("genetics.developmental_noise", 0.35f);
    m_genetics.express(slot, rng[Stream::Development], noise, m_phenotype[slot]);
    m_brains.develop(slot, m_phenotype[slot]);

    const Phenotype& p = m_phenotype[slot];
    buildPreferenceVector(p, m_prefs[slot]);
    const float energyMax = 100.0f * p.get(Trait::Size);
    buildDisplayVector(p, m_health[slot],
                       (energyMax > 0.0f) ? m_energy[slot] / energyMax : 0.0f,
                       0.0f, m_status[slot], m_display[slot]);
}

void Agents::initialiseNewborn(uint32_t slot, uint64_t tick, RngBank& rng) {
    m_flags[slot] = 0;
    m_vx[slot] = m_vy[slot] = 0.0f;
    m_heading[slot] = rng[Stream::Development].rangef(0.0f, 2.0f * kPi);
    m_health[slot] = 1.0f;
    m_pain[slot] = m_stress[slot] = 0.0f;
    m_damage[slot] = 0.0f;
    m_telomere[slot] = 1.0f;
    m_birthTick[slot] = tick;
    m_ageOffset[slot] = 0;
    m_action[slot] = static_cast<uint8_t>(Action::Idle);
    m_deathCause[slot] = static_cast<uint8_t>(DeathCause::None);
    m_bondedUid[slot] = 0;
    m_pregnantByUid[slot] = 0;
    m_embryoUid[slot] = 0;
    m_gestationRemaining[slot] = 0.0f;
    m_courtCooldown[slot] = 0.0f;
    m_offspringCount[slot] = 0;
    m_status[slot] = 0.5f;
    m_reputation[slot] = 0.0f;
    m_relationships[slot].clear();
    m_inventory.clearAgent(slot);
    m_knowledge.clearAgent(slot);
    for (int i = 0; i < kRecentMates; ++i) {
        m_recentMateUid[slot * kRecentMates + i] = 0;
        m_recentMateWeight[slot * kRecentMates + i] = 0.0f;
    }
    for (int i = 0; i < 4; ++i) m_signals[slot * 4 + i] = 0.0f;
    m_reward[slot] = 0.0f;
    m_memFoodX[slot] = m_memFoodY[slot] = -1.0f;
    m_memWaterX[slot] = m_memWaterY[slot] = -1.0f;

    redevelop(slot, rng);
    const Phenotype& p = m_phenotype[slot];
    m_energy[slot] = 100.0f * p.get(Trait::Size) * 0.7f;
    m_hydration[slot] = 100.0f * p.get(Trait::Size) * 0.7f;
    m_bodyTemp[slot] = p.get(Trait::ThermalOptimum) + 15.0f;

    DriveState& d = m_drives[slot];
    for (int i = 0; i < kDriveCount; ++i) { d.level[i] = 0.0f; d.lastSatisfaction[i] = 0.0f; }
}

AgentId Agents::spawnFounder(float x, float y, RngBank& rng, uint64_t tick,
                             bool heterogameticSex) {
    const uint32_t slot = allocateSlot();
    if (slot == 0xFFFFFFFFu) return kNoAgent;

    m_uid[slot] = m_nextUid++;
    m_uidToSlot[m_uid[slot]] = slot;
    m_motherUid[slot] = 0;
    m_fatherUid[slot] = 0;
    m_x[slot] = x;
    m_y[slot] = y;

    m_genetics.makeFounder(slot, rng[Stream::Genetics], heterogameticSex);
    m_brains.makeFounder(slot, rng[Stream::Brain]);
    initialiseNewborn(slot, tick, rng);

    m_chromosomalSex[slot] = static_cast<uint8_t>(determineChromosomalSex(
        m_genetics.genome(slot), m_sexSystem, 25.0f,
        cfg().getF("sex.pivot_temperature", 29.0f)));
    // Founders arrive as established adults. Without this they spawn at age
    // zero and the run opens with a decade of waiting before anyone can breed.
    m_ageOffset[slot] = static_cast<uint64_t>(
        cfg().getF("sim.founder_age_years", 14.0f) * static_cast<float>(kHoursPerYear));
    m_stage[slot] = static_cast<uint8_t>(LifeStage::Adult);
    m_birthTick[slot] = tick;

    char nameBuf[64];
    std::snprintf(nameBuf, sizeof(nameBuf), "Founder-%llu",
                  static_cast<unsigned long long>(m_uid[slot]));
    m_name[slot] = nameBuf;

    PedigreeRecord rec;
    rec.uid = m_uid[slot];
    rec.birthTick = m_birthTick[slot];
    rec.chromosomalSex = m_chromosomalSex[slot];
    rec.sexExpression = m_phenotype[slot].get(Trait::SexExpression);
    rec.name = m_name[slot];
    // Insert directly so uid and index stay in step.
    m_pedigree.insert(rec);

    ++m_totalBirths;
    return idOf(slot);
}

AgentId Agents::spawnChild(uint32_t motherSlot, uint32_t fatherSlot, RngBank& rng, uint64_t tick) {
    const uint32_t slot = allocateSlot();
    if (slot == 0xFFFFFFFFu) return kNoAgent;

    m_uid[slot] = m_nextUid++;
    m_uidToSlot[m_uid[slot]] = slot;
    m_motherUid[slot] = m_uid[motherSlot];
    m_fatherUid[slot] = m_uid[fatherSlot];
    m_x[slot] = m_x[motherSlot];
    m_y[slot] = m_y[motherSlot];

    // The mutation rate applied to this child is set by its PARENTS' mutator
    // alleles. That is what makes evolvability heritable and selectable.
    const float mutMult = 0.5f * (m_phenotype[motherSlot].get(Trait::MutationRateModifier) +
                                  m_phenotype[fatherSlot].get(Trait::MutationRateModifier));

    m_genetics.recombine(slot, motherSlot, fatherSlot, rng[Stream::Genetics], mutMult);
    m_brains.inherit(slot, motherSlot, fatherSlot, rng[Stream::Brain], mutMult);
    initialiseNewborn(slot, tick, rng);

    m_chromosomalSex[slot] = static_cast<uint8_t>(determineChromosomalSex(
        m_genetics.genome(slot), m_sexSystem,
        m_bodyTemp[motherSlot], cfg().getF("sex.pivot_temperature", 29.0f)));
    m_stage[slot] = static_cast<uint8_t>(LifeStage::Embryo);

    // Sexual imprinting: the template an individual carries into adult mate
    // choice is learned in early life from whoever raised it, not inherited.
    if (rng[Stream::Development].nextFloat() < cfg().getF("attraction.imprint_chance", 0.7f)) {
        const uint32_t model = (rng[Stream::Development].nextFloat() < 0.5f) ? motherSlot : fatherSlot;
        m_prefs[slot].imprint = m_display[model];
        m_prefs[slot].hasImprint = true;
    }

    char nameBuf[64];
    std::snprintf(nameBuf, sizeof(nameBuf), "%llu",
                  static_cast<unsigned long long>(m_uid[slot]));
    m_name[slot] = nameBuf;

    PedigreeRecord rec;
    rec.uid = m_uid[slot];
    rec.motherUid = m_motherUid[slot];
    rec.fatherUid = m_fatherUid[slot];
    rec.birthTick = tick;
    rec.chromosomalSex = m_chromosomalSex[slot];
    rec.sexExpression = m_phenotype[slot].get(Trait::SexExpression);
    rec.name = m_name[slot];
    m_pedigree.insert(rec);

    if (PedigreeRecord* mr = m_pedigree.findMutable(m_uid[motherSlot])) ++mr->offspringCount;
    if (PedigreeRecord* fr = m_pedigree.findMutable(m_uid[fatherSlot])) ++fr->offspringCount;
    ++m_offspringCount[motherSlot];
    ++m_offspringCount[fatherSlot];
    ++m_totalBirths;
    return idOf(slot);
}

void Agents::kill(uint32_t slot, DeathCause cause, uint64_t tick, World* world) {
    if (!slotAlive(slot)) return;
    if (immortal(slot) && cause != DeathCause::Divine) return;

    m_stage[slot] = static_cast<uint8_t>(LifeStage::Dead);
    m_deathCause[slot] = static_cast<uint8_t>(cause);
    m_pedigree.recordDeath(m_uid[slot], tick, cause);

    // Corpses return their matter to the soil, which is what closes the
    // nutrient cycle rather than leaking biomass out of the world.
    if (world && world->valid()) {
        const int tx = static_cast<int>(m_x[slot]);
        const int ty = static_cast<int>(m_y[slot]);
        if (world->inBounds(tx, ty)) {
            const size_t i = world->index(tx, ty);
            const float mass = m_phenotype[slot].get(Trait::Size) * 40.0f;
            const float ret = cfg().getF("ecology.corpse_nutrient_return", 0.6f);
            auto bump = [&](uint8_t& v) {
                v = static_cast<uint8_t>(std::min(255.0f, static_cast<float>(v) + mass * ret * 0.05f));
            };
            bump(world->soilN[i]);
            bump(world->soilP[i]);
            bump(world->soilK[i]);
        }
    }

    const float lifespan = ageYears(slot, tick);
    m_lifespanSum += lifespan;
    ++m_lifespanCount;
    ++m_totalDeaths;
    ++m_stats.deathsByCause[static_cast<int>(cause)];

    // A death breaks the pair bond on the other side too.
    if (m_bondedUid[slot] != 0) {
        const int32_t partner = slotOfUid(m_bondedUid[slot]);
        if (partner >= 0 && m_bondedUid[static_cast<uint32_t>(partner)] == m_uid[slot]) {
            m_bondedUid[static_cast<uint32_t>(partner)] = 0;
            m_flags[static_cast<uint32_t>(partner)] &= static_cast<uint8_t>(~Flag_Bonded);
        }
    }
    // A pregnancy dies with its mother. Routed through kill() rather than
    // queued directly, so the death is counted and recorded in the pedigree
    // like any other -- pushing straight onto the pending list silently lost
    // both the death tally and the ancestry record.
    if (m_embryoUid[slot] != 0) {
        const uint64_t embryoUid = m_embryoUid[slot];
        m_embryoUid[slot] = 0;
        const int32_t embryo = slotOfUid(embryoUid);
        if (embryo >= 0) kill(static_cast<uint32_t>(embryo), DeathCause::Childbirth, tick, nullptr);
    }

    m_pendingDeath.push_back(slot);
}

void Agents::reapDead(World& world, uint64_t tick) {
    (void)world;
    (void)tick;
    if (m_pendingDeath.empty()) return;

    // Sort and unique, because a single tick can queue the same slot twice
    // (a mother dying of starvation while her pregnancy is also cancelled).
    std::sort(m_pendingDeath.begin(), m_pendingDeath.end());
    m_pendingDeath.erase(std::unique(m_pendingDeath.begin(), m_pendingDeath.end()),
                         m_pendingDeath.end());

    for (uint32_t slot : m_pendingDeath) {
        if (!m_alive[slot]) continue;
        releaseSlot(slot);
    }
    m_pendingDeath.clear();

    // Compact the live list. Rebuilding in ascending slot order keeps every
    // subsequent stage's iteration order a pure function of the population.
    m_liveSlots.clear();
    for (uint32_t s = 0; s < m_capacity; ++s)
        if (m_alive[s]) m_liveSlots.push_back(s);
}

// ---------------------------------------------------------------------------
// Spatial index and sensing
// ---------------------------------------------------------------------------

void Agents::buildSpatialIndex(const World& world) {
    const float cell = cfg().getF("agents.spatial_cell_tiles", 8.0f);
    m_spatial.configure(static_cast<float>(world.width()), static_cast<float>(world.height()), cell);

    // The hash indexes positions of the LIVE list, so query results are indices
    // into m_liveSlots and must be mapped back.
    m_spatialX.resize(m_liveSlots.size());
    m_spatialY.resize(m_liveSlots.size());
    for (size_t i = 0; i < m_liveSlots.size(); ++i) {
        m_spatialX[i] = m_x[m_liveSlots[i]];
        m_spatialY[i] = m_y[m_liveSlots[i]];
    }
    m_spatial.build(m_spatialX.data(), m_spatialY.data(), m_liveSlots.size());
}

void Agents::sense(const World& world, uint64_t tick, JobSystem& jobs) {
    const int W = world.width(), H = world.height();
    const float maxBio = cfg().getF("ecology.plant_max_biomass", 400.0f);
    const uint32_t maxPerceived =
        static_cast<uint32_t>(cfg().getInt("agents.max_perceived_neighbours", 24));

    // Insolation depends only on the row and the tick, so it was being
    // recomputed identically for every agent on the same row. Tabulated once
    // per tick instead. Exactly the same numbers, one trig evaluation per row
    // rather than one per agent.
    m_insolationRow.resize(static_cast<size_t>(H));
    for (int y = 0; y < H; ++y)
        m_insolationRow[static_cast<size_t>(y)] =
            static_cast<float>(world.insolation(y, tick));

    jobs.parallelFor(m_liveSlots.size(), [&](size_t b, size_t e, unsigned) {
        for (size_t k = b; k < e; ++k) {
            const uint32_t slot = m_liveSlots[k];
            if (m_stage[slot] == static_cast<uint8_t>(LifeStage::Embryo)) continue;

            const Phenotype& p = m_phenotype[slot];
            float* in = &m_inputs[slot * kBrainInputCount];
            for (int i = 0; i < kBrainInputCount; ++i) in[i] = 0.0f;

            const float px = m_x[slot], py = m_y[slot];
            const float range = p.get(Trait::SensoryRange);
            const float acuity = p.get(Trait::SensoryAcuity);
            const float heading = m_heading[slot];

            // Vision: six sectors around the heading, sampled at three ranges.
            // Acuity scales the signal, so a low-acuity agent literally gets a
            // fainter picture of the same world rather than a differently
            // shaped one.
            //
            // The sector directions are stepped by rotation rather than by a
            // cos/sin pair each: the sectors are evenly spaced 60 degrees apart,
            // so each is the previous one turned by a fixed angle, and the angle
            // addition identity gives it exactly. Twelve transcendental calls per
            // agent per tick become two.
            const float startAng = heading - 2.5f * (kPi / 3.0f);
            float cs = std::cos(startAng), sn = std::sin(startAng);
            constexpr float kStepCos = 0.5f;                 // cos(60 degrees)
            constexpr float kStepSin = 0.86602540378f;        // sin(60 degrees)
            for (int s = 0; s < kVisionSectors; ++s) {
                float bestPlant = 0.0f, bestWater = 0.0f;
                for (int step = 1; step <= 3; ++step) {
                    const float d = range * (static_cast<float>(step) / 3.0f);
                    const int tx = static_cast<int>(px + cs * d);
                    const int ty = static_cast<int>(py + sn * d);
                    if (tx < 0 || ty < 0 || tx >= W || ty >= H) continue;
                    const size_t ti = world.index(tx, ty);
                    const float falloff = 1.0f / static_cast<float>(step);
                    bestPlant = std::max(bestPlant, (world.biomass[ti] / maxBio) * falloff);
                    if (world.waterDepth[ti] > 0.02f) bestWater = std::max(bestWater, falloff);
                }
                in[In_VisionPlant0 + s] = clamp01(bestPlant * acuity);
                in[In_VisionWater0 + s] = clamp01(bestWater * acuity);
                // Rotate to the next sector.
                const float nc = cs * kStepCos - sn * kStepSin;
                const float ns = sn * kStepCos + cs * kStepSin;
                cs = nc;
                sn = ns;
            }

            // Neighbours: one spatial query, binned into the same sectors.
            float nearestD2 = 1e30f;
            int32_t nearestSlot = -1;
            float signalSum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            int signalCount = 0;
            // Attention is BOUNDED, and the bound is a model statement rather
            // than a shortcut. An animal standing in a herd of five hundred does
            // not individually track five hundred others; the existing model
            // already says social memory is finite, and this says perception is
            // too. It also decides how this stage scales: unbounded, its cost
            // followed local crowding rather than population, and at 12,000
            // agents that made it half the tick.
            uint32_t perceived = 0;
            m_spatial.query(px, py, range, [&](uint32_t idx) {
                if (perceived >= maxPerceived) return;
                const uint32_t other = m_liveSlots[idx];
                if (other == slot) return;
                if (m_stage[other] == static_cast<uint8_t>(LifeStage::Embryo)) return;
                const float dx = m_x[other] - px, dy = m_y[other] - py;
                const float d2 = dx * dx + dy * dy;
                if (d2 > range * range) return;
                ++perceived;
                const float d = std::sqrt(d2);
                const float prox = 1.0f - clamp01(d / range);

                float ang = std::atan2(dy, dx) - heading;
                while (ang < -kPi) ang += 2.0f * kPi;
                while (ang > kPi) ang -= 2.0f * kPi;
                int sector = static_cast<int>((ang + kPi) / (2.0f * kPi) * kVisionSectors);
                sector = std::min(kVisionSectors - 1, std::max(0, sector));
                in[In_VisionAgent0 + sector] = std::max(in[In_VisionAgent0 + sector], prox * acuity);

                for (int c = 0; c < 4; ++c) signalSum[c] += m_signals[other * 4 + c] * prox;
                ++signalCount;

                if (d2 < nearestD2) { nearestD2 = d2; nearestSlot = static_cast<int32_t>(other); }
            });

            if (signalCount > 0)
                for (int c = 0; c < 4; ++c)
                    in[In_Signal0 + c] = clampf(signalSum[c] / static_cast<float>(signalCount), -1.0f, 1.0f);

            // Interoception.
            const float energyMax = 100.0f * p.get(Trait::Size);
            const DriveState& d = m_drives[slot];
            in[In_Hunger] = d[Drive::Hunger];
            in[In_Thirst] = d[Drive::Thirst];
            in[In_Energy] = clamp01(m_energy[slot] / energyMax);
            in[In_Health] = clamp01(m_health[slot]);
            in[In_Pain] = clamp01(m_pain[slot]);
            in[In_Stress] = clamp01(m_stress[slot]);
            in[In_BodyTemperature] = clampf((m_bodyTemp[slot] - 20.0f) / 30.0f, -1.0f, 1.0f);
            in[In_Age] = clamp01(ageYears(slot, tick) / std::max(1.0f, p.get(Trait::MaxLifespan)));
            in[In_ReproductiveReadiness] = d[Drive::Reproduction];

            // Ambient environment.
            const int tx = static_cast<int>(px), ty = static_cast<int>(py);
            if (tx >= 0 && ty >= 0 && tx < W && ty < H) {
                const size_t ti = world.index(tx, ty);
                in[In_Light] = m_insolationRow[static_cast<size_t>(ty)];
                in[In_AmbientTemperature] = clampf((world.temperature[ti] - 15.0f) / 30.0f, -1.0f, 1.0f);
                in[In_Terrain] = clamp01(world.elevation[ti] / 3000.0f);
                in[In_SoilMoisture] = world.soilMoisture[ti];
            }

            // The nearest other agent, including how attractive this observer
            // finds them -- attraction is a genuine sensory channel, so
            // behaviour can be conditioned on it.
            if (nearestSlot >= 0) {
                const uint32_t o = static_cast<uint32_t>(nearestSlot);
                const float dx = m_x[o] - px, dy = m_y[o] - py;
                float ang = std::atan2(dy, dx) - heading;
                while (ang < -kPi) ang += 2.0f * kPi;
                while (ang > kPi) ang -= 2.0f * kPi;
                in[In_NearestBearing] = ang / kPi;
                in[In_NearestDistance] = 1.0f - clamp01(std::sqrt(nearestD2) / range);
                in[In_NearestSexExpression] = m_display[o].sexExpression;
                // Perceived kinship, not a pedigree lookup: agents recognise
                // kin by shared parentage and immune-profile similarity, which
                // is what real animals actually have access to -- and it keeps
                // a bounded pedigree recursion off the per-tick path.
                float rel = 0.0f;
                const uint64_t mu = m_motherUid[slot], fu = m_fatherUid[slot];
                const uint64_t omu = m_motherUid[o], ofu = m_fatherUid[o];
                if (m_uid[o] == mu || m_uid[o] == fu || m_uid[slot] == omu || m_uid[slot] == ofu)
                    rel = 0.5f;
                else if (mu != 0 && omu == mu && fu != 0 && ofu == fu) rel = 0.5f;
                else if ((mu != 0 && omu == mu) || (fu != 0 && ofu == fu)) rel = 0.25f;
                else {
                    float mhcD = 0.0f;
                    for (int c = 0; c < 8; ++c) {
                        const float dd = m_display[slot].mhc[c] - m_display[o].mhc[c];
                        mhcD += dd * dd;
                    }
                    rel = clamp01(1.0f - std::sqrt(mhcD) / 6.0f) * 0.3f;
                }
                in[In_NearestRelatedness] = rel;
                in[In_NearestAttraction] = clampf(attractionBetween(slot, o, nullptr, rel) * 0.25f,
                                                  -1.0f, 1.0f);
                in[In_NearestIsBonded] = (m_bondedUid[slot] == m_uid[o]) ? 1.0f : 0.0f;
            }

            // Remembered locations, as a direction rather than a coordinate.
            auto memDir = [&](float mx, float my) -> float {
                if (mx < 0.0f) return 0.0f;
                float ang = std::atan2(my - py, mx - px) - heading;
                while (ang < -kPi) ang += 2.0f * kPi;
                while (ang > kPi) ang -= 2.0f * kPi;
                return ang / kPi;
            };
            in[In_MemoryFoodDirection] = memDir(m_memFoodX[slot], m_memFoodY[slot]);
            in[In_MemoryWaterDirection] = memDir(m_memWaterX[slot], m_memWaterY[slot]);

            // Recurrent channels are fed by the previous tick's vocalisation
            // outputs, giving the network an explicit working-memory path in
            // addition to whatever recurrent edges it evolves internally.
            const float* out = &m_outputs[slot * kBrainOutputCount];
            for (int c = 0; c < 4; ++c) in[In_Recurrent0 + c] = out[Out_Vocalise0 + c];

            in[In_Bias] = 1.0f;
        }
    }, JobSystem::kAgentGrain);
}

void Agents::think(JobSystem& jobs) {
    // Every agent's brain is evaluated, every tick, at every speed. No LOD, no
    // cohorts, no "distant agents resolved statistically" (ARCHITECTURE.md §6).
    jobs.parallelFor(m_liveSlots.size(), [&](size_t b, size_t e, unsigned) {
        for (size_t k = b; k < e; ++k) {
            const uint32_t slot = m_liveSlots[k];
            if (m_stage[slot] == static_cast<uint8_t>(LifeStage::Embryo)) continue;
            m_brains.evaluate(slot, &m_inputs[slot * kBrainInputCount],
                              &m_outputs[slot * kBrainOutputCount]);
        }
    }, JobSystem::kAgentGrain);
}

// ---------------------------------------------------------------------------
// Acting
// ---------------------------------------------------------------------------

// Close-range neighbour lists, in CSR form.
//
// act() is serial by necessity, and it was running up to three separate spatial
// queries per agent -- for aggression, for grooming and for food sharing -- all
// at the same radius, all inside that serial loop. The queries do not depend on
// anything act() writes, and positions do not change until physics() runs
// afterwards, so they can all be answered once, in parallel, before the serial
// stage begins.
//
// Two passes rather than a fixed per-agent cap: a cap would silently drop
// neighbours at high density, which would change behaviour exactly where the
// program is meant to be scaling. Counting first costs one extra traversal and
// keeps the answer exact.
void Agents::buildInteractionNeighbours(JobSystem& jobs, float radius) {
    const size_t n = m_liveSlots.size();
    m_nbRadius = radius;
    m_nbStart.assign(n + 1, 0u);
    if (n == 0) { m_nbList.clear(); return; }

    const float r2 = radius * radius;

    // Pass 1: how many neighbours each agent has.
    jobs.parallelFor(n, [&](size_t b, size_t e, unsigned) {
        for (size_t k = b; k < e; ++k) {
            const uint32_t slot = m_liveSlots[k];
            const float px = m_x[slot], py = m_y[slot];
            uint32_t count = 0;
            m_spatial.query(px, py, radius, [&](uint32_t idx) {
                const uint32_t other = m_liveSlots[idx];
                if (other == slot) return;
                if (m_stage[other] == static_cast<uint8_t>(LifeStage::Embryo)) return;
                const float dx = m_x[other] - px, dy = m_y[other] - py;
                if (dx * dx + dy * dy > r2) return;
                ++count;
            });
            m_nbStart[k + 1] = count;
        }
    }, JobSystem::kAgentGrain);

    // Prefix sum. Serial, and cheap: one add per agent.
    for (size_t k = 0; k < n; ++k) m_nbStart[k + 1] += m_nbStart[k];
    m_nbList.resize(m_nbStart[n]);

    // Pass 2: fill. Each agent writes only its own slice, so this is safe in
    // parallel, and the order within a slice is the spatial hash's fixed
    // traversal order -- which is what keeps the stage reproducible.
    jobs.parallelFor(n, [&](size_t b, size_t e, unsigned) {
        for (size_t k = b; k < e; ++k) {
            const uint32_t slot = m_liveSlots[k];
            const float px = m_x[slot], py = m_y[slot];
            uint32_t w = m_nbStart[k];
            const uint32_t limit = m_nbStart[k + 1];
            m_spatial.query(px, py, radius, [&](uint32_t idx) {
                if (w >= limit) return;
                const uint32_t other = m_liveSlots[idx];
                if (other == slot) return;
                if (m_stage[other] == static_cast<uint8_t>(LifeStage::Embryo)) return;
                const float dx = m_x[other] - px, dy = m_y[other] - py;
                if (dx * dx + dy * dy > r2) return;
                m_nbList[w++] = other;
            });
        }
    }, JobSystem::kAgentGrain);
}

void Agents::act(World& world, RngBank& rng, uint64_t tick, JobSystem& jobs) {
    (void)tick;
    const int W = world.width(), H = world.height();
    const float eatRate = cfg().getF("agents.eat_rate_kg", 3.0f);
    const float energyPerKg = cfg().getF("agents.energy_per_kg", 9.0f);
    const float drinkRate = cfg().getF("agents.drink_rate", 12.0f);
    // Hoisted out of the per-agent loop below. These were being looked up by
    // string hash once per agent per tick, which at 10,000 agents is 60,000
    // hashes a tick for six numbers that cannot change mid-stage.
    const float floorVigour = cfg().getF("agents.min_vigour", 0.30f);
    const float turnRate = cfg().getF("agents.turn_rate", 0.9f);
    const bool  noViolence = cfg().getBool("rules.disable_violence", false);
    const float attackCost = cfg().getF("agents.attack_energy_cost", 2.0f);
    const float attackDamage = cfg().getF("agents.attack_damage", 0.02f);
    const float shareFrac = cfg().getF("agents.share_fraction", 0.02f);
    const uint32_t maxPartners =
        static_cast<uint32_t>(cfg().getInt("agents.max_interactions_per_tick", 8));

    // act() is TWO passes, and the division is the whole reason this stage stopped
    // dominating the tick at scale.
    //
    // Most of what an agent does when it acts touches nothing but itself:
    // steering, thrust, drinking (which only READS the water field), vocalising,
    // and deciding what to call the action. None of that contends, so none of it
    // needs to be serial -- and at 12,000 agents it was 63% of the tick purely
    // because it was sharing a loop with the parts that do contend.
    //
    // What genuinely contends is eating (biomass leaves the tile), attacking,
    // grooming and sharing (each mutates another agent). Those stay serial, in
    // ascending slot order, with conflicts resolved by slot index -- arbitrary
    // but FIXED, which is what determinism requires (ARCHITECTURE.md 3).
    //
    // One deliberate semantic change comes out of this. Previously an agent that
    // was attacked by a lower-slot agent had its health reduced BEFORE its own
    // steering was computed, so how hard it could move depended on whether its
    // attacker happened to sort before it. That was never a modelling decision,
    // just a consequence of one loop doing both jobs, and the split removes it.

    // -- pass 1: everything that touches only the acting agent -----------------
    jobs.parallelFor(m_liveSlots.size(), [&](size_t b, size_t e, unsigned) {
        for (size_t k = b; k < e; ++k) {
            const uint32_t slot = m_liveSlots[k];
            if (m_stage[slot] == static_cast<uint8_t>(LifeStage::Embryo)) continue;
            const float* out = &m_outputs[slot * kBrainOutputCount];
            const Phenotype& p = m_phenotype[slot];

            // Steering. Turn is a rate, acceleration a thrust; both are scaled by
            // the Speed trait and by how much energy is actually left.
            const float energyMax = 100.0f * p.get(Trait::Size);
            // A starving animal is weak, but it is not paralysed. Letting vigour
            // reach zero created a death spiral: no energy meant no movement,
            // which meant no chance of reaching food, which guaranteed
            // starvation even with forage a few tiles away. The floor is what
            // makes a shortfall survivable rather than automatically terminal.
            const float vigour = std::max(floorVigour,
                                          clamp01(m_energy[slot] / (energyMax * 0.4f)) *
                                          clamp01(m_health[slot]));
            m_heading[slot] += out[Out_Turn] * turnRate;
            while (m_heading[slot] > kPi) m_heading[slot] -= 2.0f * kPi;
            while (m_heading[slot] < -kPi) m_heading[slot] += 2.0f * kPi;

            const float thrust = std::max(0.0f, out[Out_Accelerate]) *
                                 p.get(Trait::Speed) * vigour;
            const float fleeBoost = 1.0f + std::max(0.0f, out[Out_Flee]) * 0.8f;
            m_vx[slot] = std::cos(m_heading[slot]) * thrust * fleeBoost;
            m_vy[slot] = std::sin(m_heading[slot]) * thrust * fleeBoost;

            Action action = (thrust > 0.05f) ? Action::Move : Action::Idle;

            const int tx = static_cast<int>(m_x[slot]);
            const int ty = static_cast<int>(m_y[slot]);
            const bool onMap = (tx >= 0 && ty >= 0 && tx < W && ty < H);

            // Drinking works from the bank as well as from the water, because an
            // animal does not have to stand in a river to drink from it. Without
            // this, a tile-wide stream is almost impossible to hit. Parallel-safe:
            // it reads the water field and writes only this agent.
            if (onMap && out[Out_Drink] > 0.2f) {
                const size_t ti = world.index(tx, ty);
                bool water = world.waterDepth[ti] > 0.02f;
                int wx = tx, wy = ty;
                if (!water) {
                    for (int dy = -1; dy <= 1 && !water; ++dy) {
                        for (int dx = -1; dx <= 1 && !water; ++dx) {
                            const int nx = tx + dx, ny = ty + dy;
                            if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                            if (world.waterDepth[world.index(nx, ny)] > 0.02f) {
                                water = true;
                                wx = nx;
                                wy = ny;
                            }
                        }
                    }
                }
                if (water) {
                    m_hydration[slot] += drinkRate * out[Out_Drink];
                    m_memWaterX[slot] = static_cast<float>(wx);
                    m_memWaterY[slot] = static_cast<float>(wy);
                    action = Action::Drink;
                }
            }

            if (out[Out_Rest] > 0.4f && thrust < 0.1f) action = Action::Rest;

            // Everything the serial pass will need from the Phenotype, reduced to
            // plain floats here so that pass never has to read the struct.
            const float energyPerKgEff = energyPerKg * p.get(Trait::DigestiveEfficiency);
            m_eatEnergyPerKg[slot] = energyPerKgEff;
            m_eatMaxTake[slot] = eatRate * out[Out_Eat] * p.get(Trait::Size);
            m_eatAppetite[slot] = (energyPerKgEff > 0.0001f)
                ? std::max(0.0f, energyMax - m_energy[slot]) / energyPerKgEff : 0.0f;
            m_attackPower[slot] = attackDamage * p.get(Trait::Strength) * out[Out_Attack];

            // Abandon a remembered patch once it is exhausted. Without this the
            // homing reflex becomes a trap: an agent returns to the site it
            // grazed to nothing, finds nothing, and keeps returning. Real
            // foragers give up on a depleted patch, and the memory has to be
            // able to go stale.
            //
            // Parallel-safe: it reads the biomass field and writes only this
            // agent's memory. It now sees the PREVIOUS tick's biomass rather
            // than this one's, so a patch is abandoned a tick later than before
            // -- which is if anything more honest, since an animal cannot know a
            // patch is empty until it has tried it.
            if (onMap && m_memFoodX[slot] >= 0.0f) {
                const float mdx = m_memFoodX[slot] - m_x[slot];
                const float mdy = m_memFoodY[slot] - m_y[slot];
                if (mdx * mdx + mdy * mdy < 2.25f &&
                    world.biomass[world.index(tx, ty)] <= 0.5f) {
                    m_memFoodX[slot] = -1.0f;
                    m_memFoodY[slot] = -1.0f;
                }
            }

            // Vocalisation. Meanings are NOT assigned: the channel is just four
            // numbers other agents can hear. Whether any of them comes to mean
            // anything has to stabilise through use.
            for (int c = 0; c < 4; ++c)
                m_signals[slot * 4 + c] = clampf(out[Out_Vocalise0 + c], -1.0f, 1.0f);

            if (out[Out_CourtshipDisplay] > 0.5f) action = Action::Court;
            if (m_action[slot] == static_cast<uint8_t>(Action::Mate)) action = Action::Mate;

            m_action[slot] = static_cast<uint8_t>(action);
        }
    }, JobSystem::kAgentGrain);

    // -- pass 2: the contended interactions, serial in ascending slot order -----
    for (size_t liveIndex = 0; liveIndex < m_liveSlots.size(); ++liveIndex) {
        const uint32_t slot = m_liveSlots[liveIndex];
        if (m_stage[slot] == static_cast<uint8_t>(LifeStage::Embryo)) continue;
        const float* out = &m_outputs[slot * kBrainOutputCount];

        const bool wantsEat = out[Out_Eat] > 0.2f;
        const bool wantsAttack = out[Out_Attack] > 0.55f && !noViolence;
        const bool wantsGroom = out[Out_Groom] > 0.5f;
        const bool wantsShare = out[Out_Share] > 0.6f;
        // Most agents on most ticks want none of these, and skipping them here
        // keeps the serial pass proportional to the number of interactions
        // rather than to the population.
        if (!wantsEat && !wantsAttack && !wantsGroom && !wantsShare) continue;

        const uint32_t nbBegin = m_nbStart[liveIndex];
        const uint32_t nbEnd = m_nbStart[liveIndex + 1];

        const int tx = static_cast<int>(m_x[slot]);
        const int ty = static_cast<int>(m_y[slot]);
        const bool onMap = (tx >= 0 && ty >= 0 && tx < W && ty < H);
        const size_t ti = onMap ? world.index(tx, ty) : 0;
        Action action = static_cast<Action>(m_action[slot]);

        // Eating. Biomass actually leaves the tile, so a grazed patch is
        // genuinely depleted for whoever arrives next.
        if (onMap && wantsEat && world.biomass[ti] > 0.5f) {
            // Intake is capped by what the agent can actually STORE. Without
            // this an animal at 95% reserves still stripped a full ration from
            // the tile and the surplus energy was clamped away -- forage
            // destroyed for nothing. It made the sustainable population density
            // roughly an order of magnitude worse than the ecology could
            // support, and it was why founding groups crashed within months.
            //
            // The three terms were computed in the parallel pass, so this reads
            // three floats rather than the whole Phenotype.
            const float take = std::min(std::min(world.biomass[ti], m_eatMaxTake[slot]),
                                        m_eatAppetite[slot]);
            if (take > 0.0f) {
                world.biomass[ti] -= take;
                m_energy[slot] += take * m_eatEnergyPerKg[slot];
                m_memFoodX[slot] = static_cast<float>(tx);
                m_memFoodY[slot] = static_cast<float>(ty);
                action = Action::Eat;
            }
        }

        // Aggression. Damage scales with Strength, and the target's Hardiness
        // resists it; the loser's stress and the winner's status both move.
        if (wantsAttack) {
            int32_t victim = -1;
            float bestD2 = kInteractionRadius * kInteractionRadius;
            for (uint32_t i = nbBegin; i < nbEnd; ++i) {
                const uint32_t other = m_nbList[i];
                const float dx = m_x[other] - m_x[slot], dy = m_y[other] - m_y[slot];
                const float d2 = dx * dx + dy * dy;
                if (d2 < bestD2) { bestD2 = d2; victim = static_cast<int32_t>(other); }
            }
            if (victim >= 0) {
                const uint32_t v = static_cast<uint32_t>(victim);
                // Fighting costs the attacker energy too, so aggression is a
                // real trade-off rather than a free action.
                m_energy[slot] -= attackCost;
                const float dmg = m_attackPower[slot] /
                                  std::max(0.2f, m_phenotype[v].get(Trait::Hardiness));
                m_health[v] -= dmg;
                // Attribute the injury, so a killing blow is reported as
                // violence rather than falling through to "accident".
                m_deathCause[v] = static_cast<uint8_t>(DeathCause::Violence);
                m_pain[v] = clamp01(m_pain[v] + dmg * 2.0f);
                m_stress[v] = clamp01(m_stress[v] + 0.25f);
                m_status[slot] = clamp01(m_status[slot] + 0.01f);
                m_status[v] = clamp01(m_status[v] - 0.01f);
                if (Relationship* r = relationship(v, m_uid[slot], true)) {
                    r->affinity = clampf(r->affinity - 0.3f, -1.0f, 1.0f);
                    r->flags |= Rel_Rival;
                }
                action = Action::Attack;
            }
        }

        // Affiliation. Grooming raises mutual affinity, which is the substrate
        // reciprocal altruism and alliances can later be built on.
        //
        // Bounded by maxPartners, and that bound is a MODEL FIX rather than a
        // performance trick -- though it was found by chasing performance. Agents
        // converge on water and forage, so a tile can hold dozens of them, and the
        // unbounded version had one individual grooming every single neighbour
        // within reach in a single simulated hour. That was never plausible, and
        // it made this serial stage scale with local density rather than with
        // population: at 12,000 agents it was 63% of the tick.
        if (wantsGroom) {
            uint32_t partners = 0;
            for (uint32_t i = nbBegin; i < nbEnd && partners < maxPartners; ++i, ++partners) {
                const uint32_t other = m_nbList[i];
                if (Relationship* r = relationship(slot, m_uid[other], true)) {
                    r->affinity = clampf(r->affinity + 0.02f, -1.0f, 1.0f);
                    r->familiarity = clamp01(r->familiarity + 0.02f);
                    ++r->interactions;
                }
                if (Relationship* r2 = relationship(other, m_uid[slot], true)) {
                    r2->affinity = clampf(r2->affinity + 0.02f, -1.0f, 1.0f);
                    r2->familiarity = clamp01(r2->familiarity + 0.02f);
                }
            }
            action = Action::Groom;
        }

        // Food sharing. Costly to the giver, so it only persists if reciprocity
        // or kin selection makes it pay.
        // energyMax recovered from the two precomputed terms rather than by
        // reading the Phenotype again: appetite * energyPerKg is exactly the
        // headroom, so headroom + current energy is the maximum.
        const float energyMax = m_energy[slot] +
                                m_eatAppetite[slot] * m_eatEnergyPerKg[slot];
        if (wantsShare && m_energy[slot] > energyMax * 0.5f) {
            uint32_t partners = 0;
            for (uint32_t i = nbBegin; i < nbEnd && partners < maxPartners; ++i, ++partners) {
                const uint32_t other = m_nbList[i];
                // Re-check the reserve INSIDE the loop. Checking it only once
                // let an agent with several neighbours give away a fixed
                // fraction to each of them in the same tick and drain itself to
                // nothing -- generosity is costly, but it should not be suicide.
                if (m_energy[slot] <= energyMax * 0.5f) break;
                const float give = std::min(m_energy[slot] * shareFrac, 10.0f);
                m_energy[slot] -= give;
                m_energy[other] += give;
                if (Relationship* r = relationship(other, m_uid[slot], true))
                    r->affinity = clampf(r->affinity + 0.05f, -1.0f, 1.0f);
            }
            action = Action::Share;
        }

        m_action[slot] = static_cast<uint8_t>(action);
    }
    (void)rng;
}

void Agents::physics(const World& world, JobSystem& jobs) {
    const int W = world.width(), H = world.height();
    const float drag = cfg().getF("agents.drag", 0.35f);

    jobs.parallelFor(m_liveSlots.size(), [&](size_t b, size_t e, unsigned) {
        for (size_t k = b; k < e; ++k) {
            const uint32_t slot = m_liveSlots[k];
            if (m_stage[slot] == static_cast<uint8_t>(LifeStage::Embryo)) continue;

            float nx = m_x[slot] + m_vx[slot];
            float ny = m_y[slot] + m_vy[slot];

            // Terrain resistance: deep water and steep ground both slow
            // movement, and neither can be walked through freely.
            // If the agent is ALREADY standing somewhere impassable -- spawned
            // into a lake, or left high and dry by a rising sea -- it must be
            // allowed to move out. Blocking unconditionally traps it forever,
            // which is exactly what happened when founders were scattered into
            // the ocean: every direction was refused and they starved in place.
            const int cx = static_cast<int>(m_x[slot]), cy = static_cast<int>(m_y[slot]);
            const bool strandedInWater =
                world.inBounds(cx, cy) && world.waterDepth[world.index(cx, cy)] > 1.5f;

            auto passable = [&](float px, float py) {
                const int ix = static_cast<int>(px), iy = static_cast<int>(py);
                if (ix < 0 || iy < 0 || ix >= W || iy >= H) return false;
                // Too deep to wade. Shorelines are real barriers to dispersal,
                // which is what lets a strait or a lake isolate two populations.
                return world.waterDepth[world.index(ix, iy)] <= 1.5f;
            };

            if (!strandedInWater && !passable(nx, ny)) {
                // Slide along the obstacle instead of stopping dead. Refusing
                // the whole step pinned agents against shorelines -- their own
                // water-seeking reflex held them there while they ate their one
                // tile down to nothing and starved with a continent behind them.
                if (passable(m_x[slot] + m_vx[slot], m_y[slot])) {
                    nx = m_x[slot] + m_vx[slot];
                    ny = m_y[slot];
                } else if (passable(m_x[slot], m_y[slot] + m_vy[slot])) {
                    nx = m_x[slot];
                    ny = m_y[slot] + m_vy[slot];
                } else {
                    nx = m_x[slot];
                    ny = m_y[slot];
                }
            }
            m_x[slot] = clampf(nx, 0.0f, static_cast<float>(W) - 0.001f);
            m_y[slot] = clampf(ny, 0.0f, static_cast<float>(H) - 0.001f);
            m_vx[slot] *= (1.0f - drag);
            m_vy[slot] *= (1.0f - drag);
        }
        // No agent grain here, deliberately. Integrating a position is a handful
        // of arithmetic operations, so at these populations waking the pool costs
        // more than the work: forcing this stage parallel made it twenty-five
        // times SLOWER. The grain is per-call precisely so cheap bodies like this
        // one can keep the conservative default.
    });
}

// ---------------------------------------------------------------------------
// Metabolism, drives, reward and aging
// ---------------------------------------------------------------------------

float Agents::computeReward(uint32_t slot, const DriveState& before) {
    const Phenotype& p = m_phenotype[slot];
    const DriveState& now = m_drives[slot];
    float reward = 0.0f;
    // Reward is the REDUCTION in each drive's deficit, weighted by how much
    // this individual has evolved to care about that drive. Motivation itself
    // is heritable, so agents that value the wrong things are selected out.
    for (int i = 0; i < kDriveCount; ++i) {
        const float delta = before.level[i] - now.level[i];
        reward += delta * p.get(driveRewardTrait(static_cast<Drive>(i)));
    }
    return reward;
}

void Agents::metabolism(const World& world, RngBank& rng, uint64_t tick, JobSystem& jobs) {
    const int W = world.width(), H = world.height();
    const float baseBurn = cfg().getF("agents.base_metabolic_burn", 0.55f);
    const float moveCost = cfg().getF("agents.move_energy_cost", 1.4f);
    const float thermoCost = cfg().getF("agents.thermoregulation_cost", 0.20f);
    const float hydrationBurn = cfg().getF("agents.hydration_burn", 0.5f);
    const float baseMortality = cfg().getF("agents.base_mortality", 4e-7f);
    const float gompertz = cfg().getF("agents.gompertz_exponent", 5.5f);
    const float reproRate = cfg().getF("agents.repro_drive_rate", 0.0016f);
    const float bondReward = cfg().getF("agents.bond_proximity_reward", 0.02f);
    // God-mode rule overrides, read once per stage rather than per agent.
    const float metabolismMult = cfg().getF("rules.metabolism_multiplier", 1.0f);
    const float lifespanMult   = cfg().getF("rules.lifespan_multiplier", 1.0f);
    const float learningMult   = cfg().getF("rules.learning_rate_multiplier", 1.0f);
    const bool  noAging        = cfg().getBool("rules.disable_aging", false);
    const bool  noDeath        = cfg().getBool("rules.disable_death", false);

    // Deaths must not be queued from parallel workers, so each chunk collects
    // its own list and they are merged in chunk order afterwards.
    const unsigned chunks = jobs.chunkCount();
    m_chunkDeaths.resize(chunks);
    for (unsigned c = 0; c < chunks; ++c) m_chunkDeaths[c].clear();
    std::vector<std::vector<uint32_t>>& chunkDeaths = m_chunkDeaths;

    jobs.parallelFor(m_liveSlots.size(), [&](size_t b, size_t e, unsigned chunk) {
        for (size_t k = b; k < e; ++k) {
            const uint32_t slot = m_liveSlots[k];
            const Phenotype& p = m_phenotype[slot];
            const bool embryo = m_stage[slot] == static_cast<uint8_t>(LifeStage::Embryo);

            // An embryo is carried: it does not eat, move or thermoregulate.
            // Its mother pays for it, in the reproduction stage.
            if (embryo) continue;

            const DriveState before = m_drives[slot];
            const float size = p.get(Trait::Size);
            const float energyMax = 100.0f * size;
            const float hydrationMax = 100.0f * size;

            // Kleiber's law: metabolic rate scales with mass^0.75, not mass.
            float burn = baseBurn * std::pow(size, 0.75f) * p.get(Trait::MetabolicRate)
                       * metabolismMult;
            const float speed = std::sqrt(m_vx[slot] * m_vx[slot] + m_vy[slot] * m_vy[slot]);
            burn += speed * moveCost * size;
            // Ornaments are not free. This is the brake on Fisherian runaway:
            // without a real cost, display could grow without limit.
            burn += p.ornamentCost;

            const int tx = static_cast<int>(m_x[slot]), ty = static_cast<int>(m_y[slot]);
            float ambient = 15.0f;
            if (tx >= 0 && ty >= 0 && tx < W && ty < H)
                ambient = world.temperature[world.index(tx, ty)];

            const float optimum = p.get(Trait::ThermalOptimum);
            const float tolerance = p.get(Trait::ThermalTolerance);
            const float excursion = std::max(0.0f, std::fabs(ambient - optimum) - tolerance);
            burn += excursion * thermoCost * size;
            m_bodyTemp[slot] += (optimum + 15.0f - m_bodyTemp[slot]) * 0.15f
                              - excursion * 0.02f;

            m_energy[slot] -= burn;
            m_hydration[slot] -= hydrationBurn * size * (1.0f + excursion * 0.05f);

            // Starvation, dehydration and exposure damage health rather than
            // killing outright, so an agent can recover if it finds food in
            // time. The LARGEST contributor is recorded, so the death cause
            // reported afterwards is what actually killed the agent rather than
            // whichever drive happened to be high at the final instant.
            const float resilience = std::max(0.2f, p.get(Trait::Hardiness));
            float dmgStarve = 0.0f, dmgThirst = 0.0f, dmgExposure = 0.0f;
            if (m_energy[slot] < 0.0f) {
                m_energy[slot] = 0.0f;
                dmgStarve = 0.004f / resilience;
            }
            if (m_hydration[slot] < 0.0f) {
                m_hydration[slot] = 0.0f;
                dmgThirst = 0.010f / resilience;
            }
            if (excursion > 12.0f)
                dmgExposure = 0.002f * (excursion - 12.0f) / resilience;

            const float dmg = dmgStarve + dmgThirst + dmgExposure;
            if (dmg > 0.0f) {
                m_health[slot] -= dmg;
                m_deathCause[slot] = static_cast<uint8_t>(
                    (dmgThirst >= dmgStarve && dmgThirst >= dmgExposure) ? DeathCause::Dehydration :
                    (dmgStarve >= dmgExposure) ? DeathCause::Starvation : DeathCause::Exposure);
            }

            m_energy[slot] = std::min(m_energy[slot], energyMax);
            m_hydration[slot] = std::min(m_hydration[slot], hydrationMax);
            if (m_health[slot] < 1.0f && m_energy[slot] > energyMax * 0.6f)
                m_health[slot] = std::min(1.0f, m_health[slot] + 0.0015f);

            m_pain[slot] *= 0.97f;
            m_stress[slot] *= 0.985f;

            // Aging. Cellular damage accumulates and the telomere-analog
            // counter falls; both feed the Gompertz hazard below.
            const float age = noAging ? 0.0f : ageYears(slot, tick);
            const float lifespan = std::max(1.0f, p.get(Trait::MaxLifespan) * lifespanMult);
            m_damage[slot] += 1.0e-5f * (1.0f + excursion * 0.05f) *
                              (1.0f + (1.0f - clamp01(m_health[slot])) * 2.0f);
            m_telomere[slot] = std::max(0.0f, 1.0f - age / lifespan);

            // Drives.
            DriveState& d = m_drives[slot];
            d[Drive::Hunger] = 1.0f - clamp01(m_energy[slot] / energyMax);
            d[Drive::Thirst] = 1.0f - clamp01(m_hydration[slot] / hydrationMax);
            d[Drive::Thermoregulation] = clamp01(excursion / 20.0f);
            d[Drive::Safety] = clamp01(m_stress[slot]);
            d[Drive::Rest] = clamp01(d[Drive::Rest] + speed * 0.01f - 0.02f);
            d[Drive::Social] = clamp01(d[Drive::Social] + 0.002f -
                                       m_inputs[slot * kBrainInputCount + In_VisionAgent0] * 0.01f);
            d[Drive::Curiosity] = clamp01(d[Drive::Curiosity] + 0.001f);

            // Reproductive drive accumulates over time, modulated by health,
            // reserves and life stage. Discharging it on a successful mating is
            // what emits the large reward pulse in reproduction().
            const bool fertile = m_stage[slot] == static_cast<uint8_t>(LifeStage::Adult) ||
                                 m_stage[slot] == static_cast<uint8_t>(LifeStage::Senescent);
            if (fertile && (m_flags[slot] & Flag_Sterile) == 0) {
                const float condition = clamp01(m_energy[slot] / energyMax) * clamp01(m_health[slot]);
                d[Drive::Reproduction] = clamp01(d[Drive::Reproduction] +
                                                 reproRate * condition * p.get(Trait::Fertility));
            } else {
                d[Drive::Reproduction] = 0.0f;
            }

            float reward = computeReward(slot, before);

            // Oxytocin analogue: a small sustained reward simply for being near
            // a bonded partner. This is how monogamy, mate-guarding and
            // long-term bonds can EVOLVE rather than being hardcoded.
            if (m_bondedUid[slot] != 0) {
                const int32_t partner = slotOfUid(m_bondedUid[slot]);
                if (partner >= 0) {
                    const float dx = m_x[static_cast<uint32_t>(partner)] - m_x[slot];
                    const float dy = m_y[static_cast<uint32_t>(partner)] - m_y[slot];
                    if (dx * dx + dy * dy < 25.0f)
                        reward += bondReward * p.get(Trait::RewardSocial);
                }
            }

            m_reward[slot] = reward;
            if ((m_flags[slot] & Flag_LearningFrozen) == 0 && learningMult > 0.0f)
                m_brains.applyReward(slot, reward * learningMult, p);

            // Gompertz mortality: the hazard rises exponentially with age. This
            // is a hazard, not a hard cap, so lifespan is a distribution rather
            // than a number.
            if (noDeath) {
                // Death is switched off: an agent that would have died is held
                // at the brink instead, so you can watch what is killing it
                // without losing it.
                if (m_health[slot] < 0.02f) m_health[slot] = 0.02f;
                continue;
            }
            if (m_health[slot] <= 0.0f) {
                // m_deathCause already holds whichever damage source dominated.
                chunkDeaths[chunk].push_back(slot);
                if (m_deathCause[slot] == static_cast<uint8_t>(DeathCause::None))
                    m_deathCause[slot] = static_cast<uint8_t>(DeathCause::Accident);
                continue;
            }
            const float hazard = baseMortality * std::exp(gompertz * age / lifespan) *
                                 (1.0f + m_damage[slot] * 30.0f) *
                                 (1.0f + p.sublethalPenalty) /
                                 std::max(0.2f, p.get(Trait::Hardiness));
            // Each chunk has its own RNG draw source? No -- a shared stream
            // would race. The hazard test uses a hash of (uid, tick), which is
            // deterministic, independent per agent, and needs no shared state.
            uint64_t h = m_uid[slot] * 0x9E3779B97F4A7C15ull ^ (tick * 0xBF58476D1CE4E5B9ull);
            h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 33;
            const float draw = static_cast<float>(h >> 40) * (1.0f / 16777216.0f);
            if (draw < hazard) {
                chunkDeaths[chunk].push_back(slot);
                m_deathCause[slot] = static_cast<uint8_t>(DeathCause::OldAge);
            }
        }
    }, JobSystem::kAgentGrain);

    // Merge in ascending chunk order so the death list is identical every run.
    for (unsigned c = 0; c < chunks; ++c)
        for (uint32_t slot : chunkDeaths[c])
            kill(slot, static_cast<DeathCause>(m_deathCause[slot]), tick, nullptr);

    // Life stage transitions are cheap and serial.
    for (uint32_t slot : m_liveSlots) updateLifeStage(slot, tick);
    (void)rng;
}

void Agents::updateLifeStage(uint32_t slot, uint64_t tick) {
    if (m_stage[slot] == static_cast<uint8_t>(LifeStage::Embryo)) return;
    if (m_stage[slot] == static_cast<uint8_t>(LifeStage::Dead)) return;

    const Phenotype& p = m_phenotype[slot];
    const float age = ageYears(slot, tick);
    const float maturity = p.get(Trait::MaturityAge);
    const float lifespan = p.get(Trait::MaxLifespan);

    LifeStage s;
    if (age < maturity * 0.35f) s = LifeStage::Juvenile;
    else if (age < maturity) s = LifeStage::Adolescent;
    else if (age < lifespan * 0.75f) s = LifeStage::Adult;
    else s = LifeStage::Senescent;

    if (static_cast<uint8_t>(s) != m_stage[slot]) {
        m_stage[slot] = static_cast<uint8_t>(s);
        if (s == LifeStage::Adult)
            emitEvent(0, static_cast<int32_t>(m_x[slot]), static_cast<int32_t>(m_y[slot]),
                      m_uid[slot], m_name[slot] + " reached adulthood");
    }
}

// ---------------------------------------------------------------------------
// Reproduction
// ---------------------------------------------------------------------------

float Agents::attractionBetween(uint32_t observerSlot, uint32_t targetSlot,
                                AttractionBreakdown* out, float relatednessOverride) const {
    // A god-mode override forces a specific value between a specific pair,
    // which is how "make these three find them irresistible" works.
    const uint64_t key = (static_cast<uint64_t>(observerSlot) << 32) ^ m_uid[targetSlot];
    auto ov = m_attractionOverride.find(key);
    if (ov != m_attractionOverride.end()) {
        if (out) {
            *out = AttractionBreakdown();
            out->total = ov->second;
            out->threshold = 0.0f;
            out->wouldAccept = true;
        }
        return ov->second;
    }

    RelationshipContext ctx;
    for (const Relationship& r : m_relationships[observerSlot]) {
        if (r.otherUid != m_uid[targetSlot]) continue;
        ctx.familiarity = r.familiarity;
        ctx.affinity = r.affinity;
        ctx.previouslyRejected = r.rejections > 0;
        ctx.bondedToEachOther = (r.flags & Rel_Bonded) != 0;
        break;
    }
    ctx.reputation = m_reputation[targetSlot];
    ctx.relatedness = (relatednessOverride >= 0.0f)
        ? relatednessOverride
        : m_pedigree.relatedness(m_uid[observerSlot], m_uid[targetSlot], kKinDepth.i());

    return attractiveness(m_prefs[observerSlot], m_display[observerSlot], m_display[targetSlot],
                          ctx, m_drives[observerSlot][Drive::Reproduction], out);
}

Relationship* Agents::relationship(uint32_t slot, uint64_t otherUid, bool createIfMissing) {
    for (Relationship& r : m_relationships[slot])
        if (r.otherUid == otherUid) return &r;
    if (!createIfMissing) return nullptr;
    const size_t cap = static_cast<size_t>(kRelCapacity.i());
    if (m_relationships[slot].size() >= cap) {
        // Forget the least familiar acquaintance. Social memory is finite, and
        // that finiteness is itself a real constraint on group size.
        size_t worst = 0;
        for (size_t i = 1; i < m_relationships[slot].size(); ++i)
            if (m_relationships[slot][i].familiarity < m_relationships[slot][worst].familiarity)
                worst = i;
        m_relationships[slot][worst] = Relationship();
        m_relationships[slot][worst].otherUid = otherUid;
        return &m_relationships[slot][worst];
    }
    Relationship r;
    r.otherUid = otherUid;
    m_relationships[slot].push_back(r);
    return &m_relationships[slot].back();
}

void Agents::attemptCourtship(uint32_t a, uint32_t b, RngBank& rng, uint64_t tick) {
    // MUTUAL acceptance. Both parties evaluate independently, using their own
    // preferences and their own thresholds, and either can refuse.
    AttractionBreakdown ab, ba;
    attractionBetween(a, b, &ab);
    attractionBetween(b, a, &ba);
    ++m_stats.courtshipsAttempted;
    // Both parties are now committed to a courtship bout and will not start
    // another immediately, whatever the outcome.
    const float cooldown = cfg().getF("agents.courtship_cooldown", 72.0f);
    m_courtCooldown[a] = cooldown;
    m_courtCooldown[b] = cooldown;

    if (Relationship* ra = relationship(a, m_uid[b], true)) {
        ra->familiarity = clamp01(ra->familiarity + 0.05f);
        ++ra->interactions;
    }
    if (Relationship* rb = relationship(b, m_uid[a], true)) {
        rb->familiarity = clamp01(rb->familiarity + 0.05f);
        ++rb->interactions;
    }

    if (!ab.wouldAccept || !ba.wouldAccept) {
        // Rejection is a normal outcome and it is REMEMBERED, which changes
        // the calculus next time these two meet.
        if (!ba.wouldAccept) {
            if (Relationship* ra = relationship(a, m_uid[b], true)) ++ra->rejections;
        }
        if (!ab.wouldAccept) {
            if (Relationship* rb = relationship(b, m_uid[a], true)) ++rb->rejections;
        }
        return;
    }

    ++m_stats.courtshipsMutual;

    // Which one can gestate is set by sex expression, not by a label. Two
    // agents whose expression falls on the same side of the midpoint can pair
    // and bond, and will get the bond reward, but produce no zygote. That is
    // the honest outcome, not a special case.
    const float ea = m_display[a].sexExpression, eb = m_display[b].sexExpression;
    const bool aOva = ea < 0.5f, bOva = eb < 0.5f;

    // The mating act itself: a state transition with a duration, an energy
    // cost, a vulnerability window, and a reward emission. Nothing more.
    const float cost = cfg().getF("agents.mating_energy_cost", 12.0f);
    m_energy[a] -= cost;
    m_energy[b] -= cost;
    m_stress[a] = clamp01(m_stress[a] + 0.15f);   // the vulnerability window
    m_stress[b] = clamp01(m_stress[b] + 0.15f);
    m_action[a] = static_cast<uint8_t>(Action::Mate);
    m_action[b] = static_cast<uint8_t>(Action::Mate);

    // The reward pulse. Because eligibility traces are still warm from the
    // preceding ticks, this reinforces the WHOLE approach-and-display chain
    // that led here, not just the final instant.
    const float pulse = cfg().getF("agents.mating_reward_pulse", 3.0f);
    if ((m_flags[a] & Flag_LearningFrozen) == 0)
        m_brains.applyReward(a, pulse * m_phenotype[a].get(Trait::RewardRepro), m_phenotype[a]);
    if ((m_flags[b] & Flag_LearningFrozen) == 0)
        m_brains.applyReward(b, pulse * m_phenotype[b].get(Trait::RewardRepro), m_phenotype[b]);
    m_drives[a][Drive::Reproduction] = 0.0f;
    m_drives[b][Drive::Reproduction] = 0.0f;

    for (uint32_t s : {a, b}) {
        const uint32_t o = (s == a) ? b : a;
        if (Relationship* r = relationship(s, m_uid[o], true)) {
            r->flags |= Rel_Mated;
            r->affinity = clampf(r->affinity + 0.25f, -1.0f, 1.0f);
        }
    }

    // Pair bonding. Whether it forms at all is governed by how much each values
    // affiliation, so monogamy is an evolved strategy rather than a rule.
    const float bondChance = cfg().getF("agents.bond_formation_chance", 0.25f) *
                             0.5f * (m_phenotype[a].get(Trait::RewardSocial) +
                                     m_phenotype[b].get(Trait::RewardSocial));
    if (m_bondedUid[a] == 0 && m_bondedUid[b] == 0 &&
        rng[Stream::Repro].nextFloat() < bondChance) {
        m_bondedUid[a] = m_uid[b];
        m_bondedUid[b] = m_uid[a];
        m_flags[a] |= Flag_Bonded;
        m_flags[b] |= Flag_Bonded;
        if (Relationship* r = relationship(a, m_uid[b], true)) r->flags |= Rel_Bonded;
        if (Relationship* r = relationship(b, m_uid[a], true)) r->flags |= Rel_Bonded;
        emitEvent(0, static_cast<int32_t>(m_x[a]), static_cast<int32_t>(m_y[a]), m_uid[a],
                  m_name[a] + " and " + m_name[b] + " formed a pair bond");
    }

    if (aOva == bOva) { ++m_stats.matingsSameType; return; }  // bond stands, no zygote

    const uint32_t mother = aOva ? a : b;
    const uint32_t father = aOva ? b : a;
    if (m_pregnantByUid[mother] != 0) return;             // already carrying
    if (m_flags[mother] & Flag_Sterile) return;
    if (m_flags[father] & Flag_Sterile) return;

    // Record this mating for sperm competition. Competitive weight is set by
    // the male-role partner's condition and size, so paternity is contested
    // rather than simply going to the most recent mate.
    const float weight = m_phenotype[father].get(Trait::Size) *
                         clamp01(m_health[father]) *
                         m_phenotype[father].get(Trait::Fertility);
    uint64_t* mates = &m_recentMateUid[mother * kRecentMates];
    float* weights = &m_recentMateWeight[mother * kRecentMates];
    int freeIdx = kRecentMates - 1;
    for (int i = 0; i < kRecentMates; ++i) {
        if (mates[i] == m_uid[father]) { weights[i] = std::max(weights[i], weight); freeIdx = -1; break; }
        if (mates[i] == 0) { freeIdx = i; break; }
    }
    if (freeIdx >= 0) { mates[freeIdx] = m_uid[father]; weights[freeIdx] = weight; }

    // Conception. Intersex expression reduces but does not abolish fertility.
    float fertility = cfg().getF("agents.base_conception_chance", 0.35f) *
                      m_phenotype[mother].get(Trait::Fertility) *
                      m_phenotype[father].get(Trait::Fertility);
    if (isIntersex(ea) || isIntersex(eb))
        fertility *= cfg().getF("sex.intersex_fertility", 0.35f);

    // Reproductive isolation. Computed from the pair's NEUTRAL genetic
    // distance, not from their species labels, so it is continuous: a pair that
    // has barely diverged pays nothing at all, and the penalty grows smoothly
    // with distance. This is what stops a detected species being a mere label:
    // once divergence costs fertility, assortative mating is selected for and
    // the split reinforces itself.
    const double cross = m_speciation.crossFertility(m_genetics, mother, father);
    if (cross < 0.999) {
        ++m_stats.hybridConceptions;
        fertility *= static_cast<float>(cross);
    }
    if (rng[Stream::Repro].nextFloat() >= fertility) {
        if (cross < 0.999) ++m_stats.hybridBlocked;
        return;
    }

    // Sperm competition: paternity is drawn among recent mates, weighted.
    float total = 0.0f;
    for (int i = 0; i < kRecentMates; ++i) if (mates[i] != 0) total += weights[i];
    uint32_t sire = father;
    if (total > 0.0f) {
        float pick = rng[Stream::Repro].rangef(0.0f, total);
        for (int i = 0; i < kRecentMates; ++i) {
            if (mates[i] == 0) continue;
            pick -= weights[i];
            if (pick <= 0.0f) {
                const int32_t s = slotOfUid(mates[i]);
                if (s >= 0) sire = static_cast<uint32_t>(s);
                break;
            }
        }
    }

    // The zygote is created NOW, while both parents are certainly alive, and
    // gestates as an Embryo. That is what lets a father die before the birth
    // without the child losing half its genome.
    const AgentId child = spawnChild(mother, sire, rng, tick);
    if (!child.valid()) return;

    ++m_stats.conceptions;
    m_pregnantByUid[mother] = m_uid[sire];
    m_embryoUid[mother] = m_uid[child.slot];
    m_flags[mother] |= Flag_Pregnant;
    m_gestationRemaining[mother] = m_phenotype[mother].get(Trait::GestationLength) *
                                   static_cast<float>(kHoursPerDay);
}

void Agents::reproduction(World& world, RngBank& rng, uint64_t tick) {
    const float courtRange = cfg().getF("agents.courtship_range", 3.0f);
    const float driveThreshold = cfg().getF("agents.courtship_drive_threshold", 0.35f);
    const float gestationBurn = cfg().getF("agents.gestation_energy_cost", 0.5f);
    const float careRange = cfg().getF("agents.parental_care_range", 4.0f);

    // Serial, ascending slot order, for the same determinism reason as act().
    for (size_t k = 0; k < m_liveSlots.size(); ++k) {
        const uint32_t slot = m_liveSlots[k];
        if (m_stage[slot] == static_cast<uint8_t>(LifeStage::Embryo)) continue;
        if (m_courtCooldown[slot] > 0.0f) m_courtCooldown[slot] -= 1.0f;

        // -- gestation ------------------------------------------------------
        if (m_pregnantByUid[slot] != 0) {
            m_energy[slot] -= gestationBurn * m_phenotype[slot].get(Trait::Size);
            m_gestationRemaining[slot] -=
                1.0f / std::max(0.01f, cfg().getF("rules.gestation_multiplier", 1.0f));
            if (m_gestationRemaining[slot] <= 0.0f) {
                const int32_t embryo = slotOfUid(m_embryoUid[slot]);
                if (embryo >= 0) {
                    const uint32_t c = static_cast<uint32_t>(embryo);
                    m_stage[c] = static_cast<uint8_t>(LifeStage::Juvenile);
                    m_birthTick[c] = tick;
                    m_x[c] = m_x[slot];
                    m_y[c] = m_y[slot];
                    // A newborn's phenotype is expressed only now, so gestation
                    // conditions could later influence development.
                    redevelop(c, rng);

                    // Lethal recessives express at birth. This is what gives
                    // inbreeding a real, countable cost.
                    if (m_phenotype[c].lethalExpressed) {
                        kill(c, DeathCause::LethalGenes, tick, &world);
                        emitEvent(5, static_cast<int32_t>(m_x[slot]), static_cast<int32_t>(m_y[slot]),
                                  m_uid[c], "Stillbirth: homozygous lethal recessive");
                    } else {
                        emitEvent(5, static_cast<int32_t>(m_x[slot]), static_cast<int32_t>(m_y[slot]),
                                  m_uid[c], m_name[slot] + " gave birth");
                        // Parent-offspring relationships, both directions.
                        if (Relationship* r = relationship(slot, m_uid[c], true)) r->flags |= Rel_Offspring;
                        if (Relationship* r = relationship(c, m_uid[slot], true)) {
                            r->flags |= Rel_Parent;
                            r->familiarity = 1.0f;
                            r->affinity = 0.6f;
                        }
                    }
                }
                // Childbirth risk, scaled by the mother's condition.
                const float risk = cfg().getF("agents.childbirth_mortality", 0.012f) /
                                   std::max(0.2f, m_phenotype[slot].get(Trait::Hardiness)) *
                                   (2.0f - clamp01(m_health[slot]));
                m_pregnantByUid[slot] = 0;
                m_embryoUid[slot] = 0;
                m_flags[slot] &= static_cast<uint8_t>(~Flag_Pregnant);
                m_stress[slot] = clamp01(m_stress[slot] + 0.3f);
                if (rng[Stream::Repro].nextFloat() < risk) {
                    kill(slot, DeathCause::Childbirth, tick, &world);
                    continue;
                }
            }
        }

        // -- parental care --------------------------------------------------
        // Provisioning is costly to the parent and is governed by an evolved
        // trait, so the r/K strategy space is open rather than prescribed.
        const float invest = m_phenotype[slot].get(Trait::ParentalInvestment);
        if (invest > 0.1f && m_energy[slot] > 30.0f) {
            m_spatial.query(m_x[slot], m_y[slot], careRange, [&](uint32_t idx) {
                const uint32_t child = m_liveSlots[idx];
                if (child == slot) return;
                if (m_stage[child] != static_cast<uint8_t>(LifeStage::Juvenile)) return;
                if (m_motherUid[child] != m_uid[slot] && m_fatherUid[child] != m_uid[slot]) return;
                const float give = std::min(m_energy[slot] * 0.02f, invest * 2.0f);
                m_energy[slot] -= give;
                m_energy[child] += give;
                m_action[slot] = static_cast<uint8_t>(Action::Nurse);
            });
        }

        // -- courtship ------------------------------------------------------
        const bool wantsToCourt =
            m_courtCooldown[slot] <= 0.0f &&
            m_drives[slot][Drive::Reproduction] > driveThreshold &&
            m_outputs[slot * kBrainOutputCount + Out_CourtshipDisplay] > 0.3f &&
            (m_stage[slot] == static_cast<uint8_t>(LifeStage::Adult) ||
             m_stage[slot] == static_cast<uint8_t>(LifeStage::Senescent));
        if (!wantsToCourt) continue;

        // Court the single most attractive candidate in range that is also
        // signalling acceptance -- not everyone in range.
        int32_t best = -1;
        float bestScore = -1e30f;
        m_spatial.query(m_x[slot], m_y[slot], courtRange, [&](uint32_t idx) {
            const uint32_t other = m_liveSlots[idx];
            if (other == slot || other < slot) return;   // each pair considered once
            if (m_stage[other] != static_cast<uint8_t>(LifeStage::Adult) &&
                m_stage[other] != static_cast<uint8_t>(LifeStage::Senescent)) return;
            if (m_outputs[other * kBrainOutputCount + Out_AcceptMating] < 0.0f) return;
            const float dx = m_x[other] - m_x[slot], dy = m_y[other] - m_y[slot];
            if (dx * dx + dy * dy > courtRange * courtRange) return;
            const float score = attractionBetween(slot, other, nullptr);
            if (score > bestScore) { bestScore = score; best = static_cast<int32_t>(other); }
        });

        if (best >= 0) attemptCourtship(slot, static_cast<uint32_t>(best), rng, tick);
    }
}


// ---------------------------------------------------------------------------
// Chemistry, discovery and teaching.
//
// Serial and in ascending slot order, like act() and reproduction(), because it
// mutates the world (substances leave the ground) and other agents (knowledge
// crosses between them). Nothing here consults a recipe list: an agent brings
// what it is carrying to the hottest conditions its techniques can produce and
// the reaction engine decides what, if anything, happens.
// ---------------------------------------------------------------------------

void Agents::chemistry(World& world, RngBank& rng, uint64_t tick) {
    if (!chem().loaded()) return;

    const float experimentThreshold = cfg().getF("chem.experiment_threshold", 0.55f);
    const float gatherThreshold = cfg().getF("chem.gather_threshold", 0.5f);
    const float teachThreshold = cfg().getF("chem.teach_threshold", 0.55f);
    const float teachRange = cfg().getF("chem.teach_range", 2.0f);
    const float noveltyReward = cfg().getF("chem.novelty_reward", 2.0f);
    const float curiosityNeeded = cfg().getF("chem.experiment_curiosity", 0.25f);
    const float experimentCost = cfg().getF("chem.experiment_energy_cost", 4.0f);

    for (uint32_t slot : m_liveSlots) {
        if (m_stage[slot] == static_cast<uint8_t>(LifeStage::Embryo)) continue;
        if (m_stage[slot] == static_cast<uint8_t>(LifeStage::Juvenile)) continue;
        const float* out = &m_outputs[slot * kBrainOutputCount];

        // Techniques are worked out one at a time, each resting on the last.
        // The ladder is why fire long precedes a kiln and a kiln long precedes
        // iron: nothing here is scheduled, but nothing can be skipped either.
        Technique acquired = Technique::Mixing;
        if (tryAcquireTechnique(m_knowledge, slot, rng[Stream::Chemistry], acquired)) {
            emitEvent(4 /* Discovery */,
                      static_cast<int32_t>(m_x[slot]), static_cast<int32_t>(m_y[slot]),
                      m_uid[slot],
                      m_name[slot] + " worked out " + techniqueName(acquired));
        }

        // Pick up whatever the tile offers.
        if (out[Out_PickUp] > gatherThreshold) {
            const int tx = static_cast<int>(m_x[slot]);
            const int ty = static_cast<int>(m_y[slot]);
            std::string what;
            if (gatherFromTile(m_inventory, slot, world, tx, ty, &what)) {
                // Taking ore out of the ground actually depletes the tile, so
                // a rich seam is a finite thing that a settlement can exhaust.
                if (world.inBounds(tx, ty)) {
                    const size_t i = world.index(tx, ty);
                    if (world.oreGrade[i] > 0) --world.oreGrade[i];
                }
            }
        }

        // Combine what is held. This is the discovery mechanic.
        //
        // Gated on accumulated curiosity as well as on the motor output, and
        // the experiment discharges it. An experiment is a real undertaking --
        // gathering, preparing, building the fire -- so an agent cannot run one
        // every hour of its life, and the cost is what makes exploring a
        // genuine trade against foraging rather than a free action.
        DriveState& dr = m_drives[slot];
        if (out[Out_Use] > experimentThreshold && dr[Drive::Curiosity] > curiosityNeeded) {
            dr[Drive::Curiosity] = 0.0f;
            m_energy[slot] = std::max(0.0f, m_energy[slot] - experimentCost);
            const ExperimentOutcome res = runExperiment(
                m_inventory, m_knowledge, slot, m_uid[slot], m_name[slot], tick,
                rng[Stream::Chemistry], m_phenotype[slot].get(Trait::RewardCuriosity));
            if (res.happened) {
                m_action[slot] = static_cast<uint8_t>(Action::Build);
                if (res.newToAgent) {
                    // The reward is for NOVELTY, not for outcome -- which is what
                    // makes exploring worth doing before you know what it turns up.
                    const float reward =
                        noveltyReward * m_phenotype[slot].get(Trait::RewardCuriosity);
                    if ((m_flags[slot] & Flag_LearningFrozen) == 0)
                        m_brains.applyReward(slot, reward, m_phenotype[slot]);
                }
                if (res.firstEver) {
                    emitEvent(4 /* Discovery */,
                              static_cast<int32_t>(m_x[slot]), static_cast<int32_t>(m_y[slot]),
                              m_uid[slot],
                              "FIRST DISCOVERY: " + m_name[slot] + " discovered " +
                              res.description);
                }
            }
        }

        // Teach a neighbour. Knowledge crosses between individuals here, and it
        // degrades in the telling -- which is why some of it is eventually lost.
        if (out[Out_Teach] > teachThreshold && !m_knowledge.known(slot).empty()) {
            m_spatial.query(m_x[slot], m_y[slot], teachRange, [&](uint32_t idx) {
                const uint32_t student = m_liveSlots[idx];
                if (student == slot) return;
                if (m_stage[student] == static_cast<uint8_t>(LifeStage::Embryo)) return;
                std::string what;
                if (teachKnowledge(m_knowledge, slot, student, tick,
                                   rng[Stream::Culture], &what)) {
                    m_action[slot] = static_cast<uint8_t>(Action::Teach);
                    emitEvent(0 /* Info */,
                              static_cast<int32_t>(m_x[slot]),
                              static_cast<int32_t>(m_y[slot]), m_uid[slot],
                              m_name[slot] + " taught " + m_name[student] + " " + what);
                }
            });
        }
    }
}

// ---------------------------------------------------------------------------
// Speciation
// ---------------------------------------------------------------------------

void Agents::detectSpecies(uint64_t tick) {
    // Embryos are excluded: an unborn individual has a genome but has not yet
    // been tested against the world, and counting it would let a lineage look
    // extant on the strength of pregnancies alone.
    m_speciesCountable.clear();
    for (uint32_t slot : m_liveSlots)
        if (m_stage[slot] != static_cast<uint8_t>(LifeStage::Embryo))
            m_speciesCountable.push_back(slot);

    std::vector<std::string> lines;
    m_speciation.detect(m_genetics, m_speciesCountable, m_x, m_y, tick, lines);
    for (const std::string& l : lines) {
        // Kind 8 is Fixation in the world's event vocabulary -- the existing
        // category for an irreversible genetic milestone.
        emitEvent(8 /* Fixation */, -1, -1, 0xFFFFFFFFFFFFFFFFull, l);
    }
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

void Agents::recomputeStats(uint64_t tick) {
    PopulationStats s;
    s.births = static_cast<uint32_t>(m_totalBirths);
    s.deaths = static_cast<uint32_t>(m_totalDeaths);
    for (int i = 0; i < static_cast<int>(DeathCause::Count); ++i)
        s.deathsByCause[i] = m_stats.deathsByCause[i];
    s.courtshipsAttempted = m_stats.courtshipsAttempted;
    s.courtshipsMutual = m_stats.courtshipsMutual;
    s.matingsSameType = m_stats.matingsSameType;
    s.conceptions = m_stats.conceptions;

    double ageSum = 0, energySum = 0, healthSum = 0, nodeSum = 0, connSum = 0;
    double genomeSum = 0, sexSum = 0, ornamentSum = 0, prefOrnamentSum = 0;
    double fSum = 0, rewardReproSum = 0, driveSum = 0;
    uint32_t intersexCount = 0, bondedCount = 0, counted = 0;
    double knowledgeSum = 0, ladderSum = 0;

    for (uint32_t slot : m_liveSlots) {
        ++s.population;
        ++s.byStage[m_stage[slot]];
        if (m_stage[slot] == static_cast<uint8_t>(LifeStage::Embryo)) continue;
        ++counted;

        const Phenotype& p = m_phenotype[slot];
        ageSum += ageYears(slot, tick);
        energySum += m_energy[slot];
        healthSum += m_health[slot];
        nodeSum += m_brains.brain(slot).nodeCount;
        connSum += m_brains.enabledConnCount(slot);
        genomeSum += m_genetics.genome(slot).count;
        const float sexExpr = p.get(Trait::SexExpression);
        sexSum += sexExpr;
        if (isIntersex(sexExpr)) ++intersexCount;
        ornamentSum += 0.5 * (p.get(Trait::Ornament1) + p.get(Trait::Ornament2));
        prefOrnamentSum += p.get(Trait::PrefOrnament);
        fSum += p.inbreedingF;
        rewardReproSum += p.get(Trait::RewardRepro);
        driveSum += m_drives[slot][Drive::Reproduction];
        // Culture. Counted over the living, so it can FALL -- knowledge held
        // only by the dead is knowledge the world has lost.
        for (const KnowledgeUnit& k : m_knowledge.known(slot))
            if (k.usable()) knowledgeSum += 1.0;
        int highest = -1;
        for (int t = 0; t < static_cast<int>(Technique::Count); ++t)
            if (m_knowledge.hasTechnique(slot, static_cast<Technique>(t))) highest = t;
        ladderSum += static_cast<double>(highest + 1) /
                     static_cast<double>(static_cast<int>(Technique::Count));
        if (m_knowledge.hasTechnique(slot, Technique::OpenFire)) ++s.agentsWithFire;
        if (m_knowledge.hasTechnique(slot, Technique::Kiln)) ++s.agentsWithKiln;
        if (m_knowledge.hasTechnique(slot, Technique::Bellows)) ++s.agentsWithBellows;

        if (m_bondedUid[slot] != 0) ++bondedCount;
        if (m_pregnantByUid[slot] != 0) ++s.pregnancies;
    }

    if (counted > 0) {
        const double n = counted;
        s.meanAgeYears = ageSum / n;
        s.meanEnergy = energySum / n;
        s.meanHealth = healthSum / n;
        s.meanBrainNodes = nodeSum / n;
        s.meanBrainConns = connSum / n;
        s.meanGenomeLength = genomeSum / n;
        s.meanSexExpression = sexSum / n;
        s.intersexFraction = static_cast<double>(intersexCount) / n;
        s.meanOrnament = ornamentSum / n;
        s.meanPrefOrnament = prefOrnamentSum / n;
        s.meanInbreedingF = fSum / n;
        s.meanRewardRepro = rewardReproSum / n;
        s.meanReproDrive = driveSum / n;
        s.bondedFraction = static_cast<double>(bondedCount) / n;
        s.pairBonds = bondedCount / 2;
        s.meanKnowledge = knowledgeSum / n;
        s.technologyIndex = ladderSum / n;
    }
    m_knowledge.recountHolders(m_liveSlots);
    s.extantSpecies = m_speciation.extantCount();
    s.speciesEverNamed = static_cast<uint32_t>(m_speciation.species().size());
    s.hybridConceptions = m_stats.hybridConceptions;
    s.hybridBlocked = m_stats.hybridBlocked;
    s.knowledgeUnits = static_cast<uint32_t>(knowledgeSum);
    s.discoveries = static_cast<uint32_t>(m_knowledge.discoveries().size());
    s.meanLifespanAtDeath = (m_lifespanCount > 0)
        ? m_lifespanSum / static_cast<double>(m_lifespanCount) : 0.0;

    m_stats = s;
}

void Agents::recomputePopulationGenetics() {
    // Sampled rather than exhaustive above a threshold: this is a REPORTING
    // cost, not a simulation shortcut. Every agent is still simulated in full;
    // only the statistics window uses a sample, and the sample size is shown.
    const size_t maxSample = static_cast<size_t>(cfg().getInt("stats.genetics_sample", 600));
    std::vector<uint32_t>& sample = m_geneticsSample;
    sample.clear();
    if (m_liveSlots.size() <= maxSample) {
        sample = m_liveSlots;
    } else {
        // Deterministic even stride, not a random draw.
        const size_t stride = m_liveSlots.size() / maxSample;
        for (size_t i = 0; i < m_liveSlots.size() && sample.size() < maxSample; i += stride)
            sample.push_back(m_liveSlots[i]);
    }
    m_genetics.computePopulationGenetics(sample, m_popGenetics);
    m_genetics.computeHeritability(sample, m_phenotype, m_popGenetics);

    // Fst between geographic quadrants. A barrier that stops gene flow shows up
    // here as Fst climbing away from zero -- speciation in progress.
    const int grid = static_cast<int>(cfg().getInt("stats.fst_grid", 3));
    std::vector<std::vector<uint32_t>> subpops(static_cast<size_t>(grid * grid));
    float maxX = 1.0f, maxY = 1.0f;
    for (uint32_t slot : sample) {
        maxX = std::max(maxX, m_x[slot]);
        maxY = std::max(maxY, m_y[slot]);
    }
    for (uint32_t slot : sample) {
        const int gx = std::min(grid - 1, static_cast<int>(m_x[slot] / maxX * grid));
        const int gy = std::min(grid - 1, static_cast<int>(m_y[slot] / maxY * grid));
        subpops[static_cast<size_t>(gy * grid + gx)].push_back(slot);
    }
    m_fst = m_genetics.computeFst(subpops);
}

// ---------------------------------------------------------------------------
// Queries and god-mode hooks
// ---------------------------------------------------------------------------

int32_t Agents::pickNearest(float x, float y, float radius) const {
    int32_t best = -1;
    float bestD2 = radius * radius;
    m_spatial.query(x, y, radius, [&](uint32_t idx) {
        const uint32_t slot = m_liveSlots[idx];
        const float dx = m_x[slot] - x, dy = m_y[slot] - y;
        const float d2 = dx * dx + dy * dy;
        if (d2 < bestD2) { bestD2 = d2; best = static_cast<int32_t>(slot); }
    });
    return best;
}

void Agents::agentsInRect(float x0, float y0, float x1, float y1,
                          std::vector<uint32_t>& out) const {
    out.clear();
    for (uint32_t slot : m_liveSlots) {
        if (m_x[slot] < x0 || m_x[slot] > x1) continue;
        if (m_y[slot] < y0 || m_y[slot] > y1) continue;
        out.push_back(slot);
    }
}

void Agents::forceBond(uint32_t a, uint32_t b, bool bonded) {
    if (!slotAlive(a) || !slotAlive(b)) return;
    if (bonded) {
        m_bondedUid[a] = m_uid[b];
        m_bondedUid[b] = m_uid[a];
        m_flags[a] |= Flag_Bonded;
        m_flags[b] |= Flag_Bonded;
        if (Relationship* r = relationship(a, m_uid[b], true)) r->flags |= Rel_Bonded;
        if (Relationship* r = relationship(b, m_uid[a], true)) r->flags |= Rel_Bonded;
    } else {
        if (m_bondedUid[a] == m_uid[b]) { m_bondedUid[a] = 0; m_flags[a] &= static_cast<uint8_t>(~Flag_Bonded); }
        if (m_bondedUid[b] == m_uid[a]) { m_bondedUid[b] = 0; m_flags[b] &= static_cast<uint8_t>(~Flag_Bonded); }
        if (Relationship* r = relationship(a, m_uid[b], false)) r->flags &= static_cast<uint8_t>(~Rel_Bonded);
        if (Relationship* r = relationship(b, m_uid[a], false)) r->flags &= static_cast<uint8_t>(~Rel_Bonded);
    }
}

void Agents::setAttractionOverride(uint32_t observer, uint64_t targetUid, float value, bool enabled) {
    const uint64_t key = (static_cast<uint64_t>(observer) << 32) ^ targetUid;
    if (enabled) m_attractionOverride[key] = value;
    else m_attractionOverride.erase(key);
}

void Agents::setImprint(uint32_t slot, uint32_t templateSlot) {
    if (!slotAlive(slot) || !slotAlive(templateSlot)) return;
    m_prefs[slot].imprint = m_display[templateSlot];
    m_prefs[slot].hasImprint = true;
}

// ---------------------------------------------------------------------------
// Serialization
//
// Only LIVE slots are written, each tagged with its slot index, so a snapshot
// of 40 agents in a 12000-slot store costs 40 records rather than 12000. Every
// field is written individually -- never a struct blit -- for the same reason
// as everywhere else in the format: padding is indeterminate and would break
// both reproducibility and cross-compiler portability.
// ---------------------------------------------------------------------------


void Agents::appendAgentBlob(std::vector<uint8_t>& blob, uint32_t slot) const {
    BinaryWriter w(blob);
    serializeAgent(w, slot);
    // The genome is stored in its own arena, so it is not part of the body
    // record and has to be appended explicitly.
    ConstGenomeView g = m_genetics.genome(slot);
    w.pod(g.count);
    for (uint16_t i = 0; i < g.count; ++i) {
        const Gene& gene = g.genes[i];
        w.pod(gene.alleleA); w.pod(gene.alleleB); w.pod(gene.mapPos); w.pod(gene.effect);
        w.pod(gene.id); w.pod(gene.target);
        w.pod(gene.chromosome); w.pod(gene.type); w.pod(gene.dominance); w.pod(gene.flags);
    }
}

int32_t Agents::restoreAgentBlob(const std::vector<uint8_t>& blob, size_t offset,
                                 size_t& consumed) {
    if (offset >= blob.size()) { consumed = 0; return -1; }
    const uint32_t slot = allocateSlot();
    if (slot == 0xFFFFFFFFu) { consumed = 0; return -1; }

    BinaryReader r(blob.data() + offset, blob.size() - offset);
    deserializeAgent(r, slot);

    uint16_t geneCount = 0;
    r.pod(geneCount);
    if (geneCount > m_genetics.geneCapacity()) geneCount = m_genetics.geneCapacity();
    GenomeView gv = m_genetics.genome(slot);
    for (uint16_t i = 0; i < geneCount; ++i) {
        Gene& gene = gv.genes[i];
        r.pod(gene.alleleA); r.pod(gene.alleleB); r.pod(gene.mapPos); r.pod(gene.effect);
        r.pod(gene.id); r.pod(gene.target);
        r.pod(gene.chromosome); r.pod(gene.type); r.pod(gene.dominance); r.pod(gene.flags);
    }
    m_genetics.setGeneCount(slot, geneCount);

    consumed = r.position();
    if (!r.ok()) { releaseSlot(slot); return -1; }

    m_uidToSlot[m_uid[slot]] = slot;
    // A restored uid must not be handed out again to a future birth.
    if (m_uid[slot] >= m_nextUid) m_nextUid = m_uid[slot] + 1;
    rederiveAfterRestore(slot);
    return static_cast<int32_t>(slot);
}

void Agents::rederiveAfterRestore(uint32_t slot) {
    // Everything stored is restored verbatim; only the derived caches are
    // rebuilt. develop() recomputes the brain's CSR index and evaluation order
    // but would otherwise reset the learned weights, so they are preserved.
    const uint16_t nc = m_brains.brain(slot).connCount;
    std::vector<float> learned(m_brains.runtime(slot).weight,
                               m_brains.runtime(slot).weight + nc);
    m_brains.develop(slot, m_phenotype[slot]);
    for (uint16_t c = 0; c < nc; ++c) m_brains.runtime(slot).weight[c] = learned[c];
    buildPreferenceVector(m_phenotype[slot], m_prefs[slot]);
    const float energyMax = 100.0f * m_phenotype[slot].get(Trait::Size);
    buildDisplayVector(m_phenotype[slot], m_health[slot],
                       (energyMax > 0.0f) ? m_energy[slot] / energyMax : 0.0f,
                       0.0f, m_status[slot], m_display[slot]);
}

// One agent, whole: body, phenotype, drives, relationships, brain. The slot
// index is deliberately NOT part of the record -- it belongs to the container,
// not the individual -- so a blob can be restored into any free slot. That is
// what lets god-mode undo bring a killed agent back with its uid, and therefore
// its relationships and pedigree links, intact.
void Agents::serializeAgent(BinaryWriter& w, uint32_t slot) const {
        w.pod(m_generation[slot]);
        w.pod(m_uid[slot]);
        w.pod(m_flags[slot]);
        w.pod(m_x[slot]); w.pod(m_y[slot]);
        w.pod(m_vx[slot]); w.pod(m_vy[slot]); w.pod(m_heading[slot]);
        w.pod(m_energy[slot]); w.pod(m_hydration[slot]); w.pod(m_health[slot]);
        w.pod(m_bodyTemp[slot]); w.pod(m_pain[slot]); w.pod(m_stress[slot]);
        w.pod(m_damage[slot]); w.pod(m_telomere[slot]);
        w.pod(m_birthTick[slot]);
        w.pod(m_ageOffset[slot]);
        w.pod(m_stage[slot]); w.pod(m_action[slot]);
        w.pod(m_chromosomalSex[slot]); w.pod(m_deathCause[slot]);
        w.pod(m_motherUid[slot]); w.pod(m_fatherUid[slot]);
        w.pod(m_bondedUid[slot]); w.pod(m_pregnantByUid[slot]); w.pod(m_embryoUid[slot]);
        w.pod(m_gestationRemaining[slot]);
        w.pod(m_courtCooldown[slot]);
        w.pod(m_offspringCount[slot]);
        w.pod(m_status[slot]); w.pod(m_reputation[slot]);
        w.str(m_name[slot]);

        for (int i = 0; i < kRecentMates; ++i) {
            w.pod(m_recentMateUid[slot * kRecentMates + i]);
            w.pod(m_recentMateWeight[slot * kRecentMates + i]);
        }
        for (int i = 0; i < kDriveCount; ++i) w.pod(m_drives[slot].level[i]);
        for (int i = 0; i < 4; ++i) w.pod(m_signals[slot * 4 + i]);
        w.pod(m_memFoodX[slot]); w.pod(m_memFoodY[slot]);
        w.pod(m_memWaterX[slot]); w.pod(m_memWaterY[slot]);

        // The expressed phenotype is stored rather than re-derived. Development
        // adds noise that is drawn once at birth and is NOT recoverable from
        // the genome, so re-expressing on load would quietly produce a
        // different individual from the one that was saved.
        for (int i = 0; i < kTraitCount; ++i) w.pod(m_phenotype[slot].traits[i]);
        for (int i = 0; i < 8; ++i) w.pod(m_phenotype[slot].mhcSignature[i]);
        w.pod(m_phenotype[slot].heterozygosity);
        w.pod(m_phenotype[slot].inbreedingF);
        w.pod(m_phenotype[slot].lethalCarried);
        const uint8_t lethalExpressed = m_phenotype[slot].lethalExpressed ? 1u : 0u;
        w.pod(lethalExpressed);
        w.pod(m_phenotype[slot].sublethalPenalty);
        w.pod(m_phenotype[slot].ornamentCost);

        // The learned imprint is not derivable from the genome.
        const uint8_t hasImprint = m_prefs[slot].hasImprint ? 1u : 0u;
        w.pod(hasImprint);
        if (hasImprint) {
            for (int i = 0; i < kDisplayCount; ++i) w.pod(m_prefs[slot].imprint.v[i]);
            for (int i = 0; i < 8; ++i) w.pod(m_prefs[slot].imprint.mhc[i]);
            w.pod(m_prefs[slot].imprint.sexExpression);
        }

        const uint32_t relCount = static_cast<uint32_t>(m_relationships[slot].size());
        w.pod(relCount);
        for (const Relationship& r : m_relationships[slot]) {
            w.pod(r.otherUid); w.pod(r.familiarity); w.pod(r.affinity);
            w.pod(r.interactions); w.pod(r.rejections); w.pod(r.flags);
        }

        m_brains.serializeSlot(w, slot);
        m_inventory.serializeAgent(w, slot);
        m_knowledge.serializeAgent(w, slot);
}

void Agents::deserializeAgent(BinaryReader& r, uint32_t slot) {
        r.pod(m_generation[slot]);
        r.pod(m_uid[slot]);
        r.pod(m_flags[slot]);
        r.pod(m_x[slot]); r.pod(m_y[slot]);
        r.pod(m_vx[slot]); r.pod(m_vy[slot]); r.pod(m_heading[slot]);
        r.pod(m_energy[slot]); r.pod(m_hydration[slot]); r.pod(m_health[slot]);
        r.pod(m_bodyTemp[slot]); r.pod(m_pain[slot]); r.pod(m_stress[slot]);
        r.pod(m_damage[slot]); r.pod(m_telomere[slot]);
        r.pod(m_birthTick[slot]);
        r.pod(m_ageOffset[slot]);
        r.pod(m_stage[slot]); r.pod(m_action[slot]);
        r.pod(m_chromosomalSex[slot]); r.pod(m_deathCause[slot]);
        r.pod(m_motherUid[slot]); r.pod(m_fatherUid[slot]);
        r.pod(m_bondedUid[slot]); r.pod(m_pregnantByUid[slot]); r.pod(m_embryoUid[slot]);
        r.pod(m_gestationRemaining[slot]);
        r.pod(m_courtCooldown[slot]);
        r.pod(m_offspringCount[slot]);
        r.pod(m_status[slot]); r.pod(m_reputation[slot]);
        r.str(m_name[slot]);

        for (int k = 0; k < kRecentMates; ++k) {
            r.pod(m_recentMateUid[slot * kRecentMates + k]);
            r.pod(m_recentMateWeight[slot * kRecentMates + k]);
        }
        for (int k = 0; k < kDriveCount; ++k) r.pod(m_drives[slot].level[k]);
        for (int k = 0; k < 4; ++k) r.pod(m_signals[slot * 4 + k]);
        r.pod(m_memFoodX[slot]); r.pod(m_memFoodY[slot]);
        r.pod(m_memWaterX[slot]); r.pod(m_memWaterY[slot]);

        for (int i = 0; i < kTraitCount; ++i) r.pod(m_phenotype[slot].traits[i]);
        for (int i = 0; i < 8; ++i) r.pod(m_phenotype[slot].mhcSignature[i]);
        r.pod(m_phenotype[slot].heterozygosity);
        r.pod(m_phenotype[slot].inbreedingF);
        r.pod(m_phenotype[slot].lethalCarried);
        uint8_t lethalExpressed = 0;
        r.pod(lethalExpressed);
        m_phenotype[slot].lethalExpressed = (lethalExpressed != 0);
        r.pod(m_phenotype[slot].sublethalPenalty);
        r.pod(m_phenotype[slot].ornamentCost);

        uint8_t hasImprint = 0;
        r.pod(hasImprint);
        m_prefs[slot].hasImprint = (hasImprint != 0);
        if (hasImprint) {
            for (int k = 0; k < kDisplayCount; ++k) r.pod(m_prefs[slot].imprint.v[k]);
            for (int k = 0; k < 8; ++k) r.pod(m_prefs[slot].imprint.mhc[k]);
            r.pod(m_prefs[slot].imprint.sexExpression);
        }

        uint32_t relCount = 0;
        r.pod(relCount);
        m_relationships[slot].clear();
        if (relCount > 4096) return;
        for (uint32_t k = 0; k < relCount; ++k) {
            Relationship rel;
            r.pod(rel.otherUid); r.pod(rel.familiarity); r.pod(rel.affinity);
            r.pod(rel.interactions); r.pod(rel.rejections); r.pod(rel.flags);
            m_relationships[slot].push_back(rel);
        }

        m_brains.deserializeSlot(r, slot);
        m_inventory.deserializeAgent(r, slot);
        m_knowledge.deserializeAgent(r, slot);
        m_uidToSlot[m_uid[slot]] = slot;
}

void Agents::serialize(BinaryWriter& w) const {
    const uint32_t capacity = static_cast<uint32_t>(m_capacity);
    w.pod(capacity);
    w.pod(m_nextUid);
    w.pod(m_totalBirths);
    w.pod(m_totalDeaths);
    w.pod(m_lifespanSum);
    w.pod(m_lifespanCount);
    const uint8_t sexSys = static_cast<uint8_t>(m_sexSystem);
    w.pod(sexSys);

    const uint32_t n = static_cast<uint32_t>(m_liveSlots.size());
    w.pod(n);
    for (uint32_t slot : m_liveSlots) {
        w.pod(slot);
        serializeAgent(w, slot);
    }

    m_genetics.serialize(w, m_liveSlots);
    m_pedigree.serialize(w);
    m_knowledge.serializeWorld(w);
    m_speciation.serialize(w);

    const uint32_t overrides = static_cast<uint32_t>(m_attractionOverride.size());
    w.pod(overrides);
    // Sorted, so the file is byte-identical for identical state rather than
    // depending on hash-table iteration order.
    std::vector<std::pair<uint64_t, float>> sortedOverrides(m_attractionOverride.begin(),
                                                            m_attractionOverride.end());
    std::sort(sortedOverrides.begin(), sortedOverrides.end(),
              [](const std::pair<uint64_t, float>& a, const std::pair<uint64_t, float>& b) {
                  return a.first < b.first;
              });
    for (const auto& kv : sortedOverrides) { w.pod(kv.first); w.pod(kv.second); }
}

void Agents::deserialize(BinaryReader& r) {
    uint32_t capacity = 0;
    r.pod(capacity);
    r.pod(m_nextUid);
    r.pod(m_totalBirths);
    r.pod(m_totalDeaths);
    r.pod(m_lifespanSum);
    r.pod(m_lifespanCount);
    uint8_t sexSys = 0;
    r.pod(sexSys);
    m_sexSystem = static_cast<SexSystem>(sexSys);

    // Wipe the store without touching the arenas, which are reloaded per slot.
    for (uint32_t slot : m_liveSlots) {
        m_alive[slot] = 0;
        m_relationships[slot].clear();
        m_inventory.clearAgent(slot);
        m_knowledge.clearAgent(slot);
    }
    m_liveSlots.clear();
    m_freeSlots.clear();
    m_uidToSlot.clear();
    m_attractionOverride.clear();
    m_population = 0;

    uint32_t n = 0;
    r.pod(n);
    if (n > m_capacity) { m_freeSlots.clear(); return; }

    for (uint32_t i = 0; i < n && r.ok(); ++i) {
        uint32_t slot = 0;
        r.pod(slot);
        if (slot >= m_capacity) return;
        m_alive[slot] = 1;
        m_liveSlots.push_back(slot);
        ++m_population;
        deserializeAgent(r, slot);
    }

    // Genomes: the writer emitted one record per live slot, tagged with slot.
    {
        uint16_t geneCapacity = 0;
        uint16_t nextGeneId = 0;
        float binWidth = 0.0f;
        r.pod(geneCapacity);
        r.pod(nextGeneId);
        r.pod(binWidth);

        uint32_t chromosomes = 0;
        float lengthCm = 0.0f;
        r.pod(chromosomes);
        r.pod(lengthCm);
        RecombinationMap& map = m_genetics.mapMutable();
        map.chromosomeCount = static_cast<int>(chromosomes);
        map.lengthCm = lengthCm;
        map.baseRate.assign(chromosomes, 1.4f);
        map.hotspots.assign(chromosomes, {});
        for (uint32_t c = 0; c < chromosomes && r.ok(); ++c) {
            r.pod(map.baseRate[c]);
            uint32_t hn = 0;
            r.pod(hn);
            if (hn > 4096) return;
            for (uint32_t h = 0; h < hn; ++h) {
                RecombinationMap::Hotspot hs;
                r.pod(hs.position); r.pod(hs.width); r.pod(hs.intensity);
                map.hotspots[c].push_back(hs);
            }
        }

        uint32_t templateCount = 0;
        r.pod(templateCount);
        if (templateCount > 100000u) return;
        for (uint32_t t = 0; t < templateCount && r.ok(); ++t) {
            Gene g;
            r.pod(g.alleleA); r.pod(g.alleleB); r.pod(g.mapPos); r.pod(g.effect);
            r.pod(g.id); r.pod(g.target);
            r.pod(g.chromosome); r.pod(g.type); r.pod(g.dominance); r.pod(g.flags);
        }

        uint32_t genomeCount = 0;
        r.pod(genomeCount);
        for (uint32_t k = 0; k < genomeCount && r.ok(); ++k) {
            uint32_t slot = 0;
            r.pod(slot);
            if (slot >= m_capacity) return;
            m_genetics.deserializeSlot(r, slot);
        }
    }

    m_pedigree.deserialize(r);
    m_knowledge.deserializeWorld(r);
    m_speciation.deserialize(r);

    uint32_t overrides = 0;
    r.pod(overrides);
    if (overrides > 1000000u) return;
    for (uint32_t i = 0; i < overrides && r.ok(); ++i) {
        uint64_t key = 0;
        float value = 0.0f;
        r.pod(key);
        r.pod(value);
        m_attractionOverride[key] = value;
    }

    // Rebuild the free list from whatever is not alive, descending so the next
    // allocation takes the lowest free slot.
    for (size_t s = m_capacity; s-- > 0;)
        if (!m_alive[s]) m_freeSlots.push_back(static_cast<uint32_t>(s));

    // Phenotypes and brain runtimes are derived state; rebuild rather than
    // store them. The learned weights ARE stored, and develop() must not
    // overwrite them, so the expressed weights are restored afterwards.
    // Only DERIVED state is rebuilt here. The phenotype was restored above
    // verbatim, and the learned weights are restored after develop() -- which
    // recomputes the CSR index and the evaluation order but would otherwise
    // overwrite a lifetime of learning with the genome's starting weights.
    std::vector<float> learned;
    for (uint32_t slot : m_liveSlots) {
        const uint16_t nc = m_brains.brain(slot).connCount;
        learned.assign(m_brains.runtime(slot).weight, m_brains.runtime(slot).weight + nc);
        m_brains.develop(slot, m_phenotype[slot]);
        for (uint16_t c = 0; c < nc; ++c) m_brains.runtime(slot).weight[c] = learned[c];
        buildPreferenceVector(m_phenotype[slot], m_prefs[slot]);
        const float energyMax = 100.0f * m_phenotype[slot].get(Trait::Size);
        buildDisplayVector(m_phenotype[slot], m_health[slot],
                           (energyMax > 0.0f) ? m_energy[slot] / energyMax : 0.0f,
                           0.0f, m_status[slot], m_display[slot]);
    }
}

}  // namespace gen
