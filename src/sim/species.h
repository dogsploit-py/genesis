// sim/species.h — species detection and the phylogeny.
//
// Nothing in this simulation declares a species. There is no species field on an
// agent, no `species.json` roster, and no speciation event scheduled anywhere.
// What exists is a population of genomes that drift, and a detector that looks
// at how far apart they have got.
//
// The clock is the NEUTRAL loci. Junk genes are under no selection and MHC is
// under diversifying rather than directional selection, so the rate at which two
// lineages' neutral loci diverge is a function of time and gene flow and nothing
// else. Coding loci would be the wrong measure entirely: two populations under
// identical selection converge on similar trait values however long they have
// been separated, and two under opposite selection diverge within a generation
// without being distinct species.
//
// HOW THE THRESHOLD IS SET, which is the part that actually matters.
//
// Two wrong answers were tried first and are worth recording, because both look
// reasonable.
//
//   1. An absolute distance threshold. Fails immediately: in any randomly mating
//      population two individuals differ at every neutral locus, so any absolute
//      threshold either declares every individual its own species or lumps
//      everything together, with nothing useful in between.
//
//   2. Distance standardised by the population's pooled per-locus standard
//      deviation. Fails in exactly the case it is needed. Once the population IS
//      bimodal, the between-group separation is itself the dominant contribution
//      to the pooled variance, so the divergence inflates the very yardstick it
//      is being measured against and the ratio never crosses any fixed line. A
//      metric that goes blind precisely when there is something to see is worse
//      than useless.
//
// What works is a GAP criterion. A species boundary is not a distance, it is a
// discontinuity: members of one population sit close to some other member, and
// the nearest thing to a member of a diverged population is far away. So the
// scale is taken from the median nearest-neighbour distance -- the typical
// spacing between relatives, which is a within-population quantity and stays
// small no matter how far the groups separate -- and clusters are grown by single
// linkage at a multiple of it. That measures the gap rather than the spread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sim/genetics.h"

namespace gen {

class BinaryWriter;
class BinaryReader;

// One genome's neutral-locus signature: sorted (gene id, expressed mean) pairs.
// Sorted because every comparison below is a merge, and a merge needs order.
struct NeutralSignature {
    struct Locus {
        uint16_t id = 0;
        float    value = 0.0f;
    };
    std::vector<Locus> loci;

    void clear() { loci.clear(); }
    bool empty() const { return loci.empty(); }
};

// Builds the signature from a genome's Junk and MHC loci.
void buildNeutralSignature(ConstGenomeView genome, NeutralSignature& out);

// Raw genetic distance: mean per-locus allele difference, with unmatched loci
// charged a fixed penalty. Unmatched loci cost more than a substitution because
// gaining or losing a gene is a structurally larger event than changing one.
//
// This is in allele units, not standard deviations. It is only ever interpreted
// relative to the population's own nearest-neighbour spacing -- see the header
// comment for why any absolute or variance-normalised reading of it is wrong.
double neutralDistance(const NeutralSignature& a, const NeutralSignature& b);

// A detected lineage.
struct SpeciesRecord {
    uint32_t id = 0;
    uint32_t parentId = 0;          // 0 = a founding lineage, no parent
    std::string name;               // deterministic binomial
    uint64_t firstTick = 0;
    uint64_t lastTick = 0;          // last tick with a living member
    bool     extant = false;

    uint32_t population = 0;
    uint32_t peakPopulation = 0;

    // The lineage's current position in neutral space: the mean over its members.
    // Tracks them, so a lineage that drifts steadily is followed rather than
    // repeatedly split.
    NeutralSignature centroid;

    double splitDistance = 0.0;   // gap from the parent lineage at the split
    double drift = 0.0;           // cumulative movement of the centroid
    double dispersion = 0.0;      // mean member distance from the centroid

    int32_t centroidX = -1, centroidY = -1;   // geographic centre, for the map
};

// An event in the phylogeny. Kept separately from the world event log so the
// tree survives a save and can be rebuilt without replaying the run.
struct PhylogenyEvent {
    uint64_t tick = 0;
    uint32_t speciesId = 0;
    uint32_t parentId = 0;
    uint8_t  kind = 0;          // 0 = origin, 1 = split, 2 = extinction
    double   distance = 0.0;
};

class Speciation {
public:
    void configure(size_t maxAgents);
    void clear();

    uint32_t speciesOf(uint32_t slot) const {
        return slot < m_slotSpecies.size() ? m_slotSpecies[slot] : 0u;
    }
    void setSpeciesOf(uint32_t slot, uint32_t id) {
        if (slot < m_slotSpecies.size()) m_slotSpecies[slot] = id;
    }

    const std::vector<SpeciesRecord>& species() const { return m_species; }
    const SpeciesRecord* find(uint32_t id) const;
    const std::vector<PhylogenyEvent>& events() const { return m_events; }

    // The within-population spacing the distances are read against, and how many
    // individuals the last pass clustered. Both are shown in the UI, because a
    // threshold nobody can see the units of is a threshold nobody can trust.
    double scale() const { return m_scale; }
    uint32_t lastSampleSize() const { return m_lastSample; }

    uint32_t extantCount() const;

    // The detection pass. Runs on its own cadence, not every tick: it is a
    // measurement of the population, and measuring it more often than divergence
    // can change would be pure cost.
    //
    // `countableSlots` is the caller's already-filtered list -- developed agents
    // only. The filter lives with the caller because life stages belong to the
    // agent store, and having the detector reach into that would couple the two
    // the wrong way round.
    void detect(const Genetics& genetics, const std::vector<uint32_t>& countableSlots,
                const std::vector<float>& x, const std::vector<float>& y,
                uint64_t tick, std::vector<std::string>& newEventsOut);

    // Reproductive isolation. Returns a fertility multiplier in [0,1] for a
    // cross, from the pair's neutral distance relative to the population's own
    // spacing -- never from their species labels, so it is continuous and there
    // is no cliff at the boundary. This is what gives a detected species teeth
    // instead of leaving it a label: once divergence costs fertility, assortative
    // mating is selected for and the split reinforces itself.
    double crossFertility(const Genetics& genetics, uint32_t slotA, uint32_t slotB) const;

    void serialize(BinaryWriter& w) const;
    void deserialize(BinaryReader& r);

private:
    uint32_t createSpecies(uint32_t parentId, const NeutralSignature& sig, uint64_t tick,
                           double splitDistance);
    static std::string makeName(uint32_t id, uint32_t parentId);
    // Accumulates `add` into a running mean that already holds `count` members.
    void accumulate(NeutralSignature& runningMean, uint32_t count,
                    const NeutralSignature& add);

    std::vector<uint32_t> m_slotSpecies;
    std::vector<SpeciesRecord> m_species;
    std::vector<PhylogenyEvent> m_events;
    uint32_t m_nextId = 0;

    double   m_scale = 0.0;       // median nearest-neighbour distance
    uint32_t m_lastSample = 0;

    // Scratch, reused so a pass does not allocate per agent.
    mutable NeutralSignature m_scratch;
    mutable NeutralSignature m_scratchB;
    NeutralSignature m_merge;
    std::vector<uint32_t> m_sampleSlots;
    std::vector<NeutralSignature> m_sampleSigs;
    std::vector<double> m_nnDist;
    std::vector<int32_t> m_clusterOf;
    std::vector<NeutralSignature> m_clusterCentroid;
    std::vector<uint32_t> m_clusterCount;
    std::vector<NeutralSignature> m_accum;
    std::vector<uint32_t> m_accumCount;
    std::vector<double> m_dispSum;
    std::vector<double> m_cx, m_cy;
};

}  // namespace gen
