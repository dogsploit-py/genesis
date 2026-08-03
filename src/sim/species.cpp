#include "sim/species.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/config.h"
#include "core/serialize.h"

namespace gen {

namespace {

// Latin-ish name fragments. Deterministic: the id picks the syllables, so the
// same run always produces the same names and a name is a stable handle rather
// than a cosmetic label that shifts between sessions.
const char* kGenus[] = {
    "Aster", "Bathy", "Chloro", "Dendro", "Erythro", "Ferro", "Glauco", "Helio",
    "Iso", "Kryo", "Litho", "Melano", "Noto", "Ortho", "Phyllo", "Rhodo",
    "Strepto", "Thermo", "Xantho", "Zygo"
};
const char* kGenusTail[] = { "morpha", "poda", "cera", "derma", "notus", "chirus" };
const char* kSpecies[] = {
    "agilis", "borealis", "candidus", "densus", "erectus", "fugax", "gracilis",
    "hirtus", "insularis", "lucidus", "montanus", "nocturnus", "obscurus",
    "pallidus", "robustus", "sylvestris", "tenuis", "umbrosus", "vagans", "zonatus"
};

constexpr size_t kGenusCount = sizeof(kGenus) / sizeof(kGenus[0]);
constexpr size_t kGenusTailCount = sizeof(kGenusTail) / sizeof(kGenusTail[0]);
constexpr size_t kSpeciesCount = sizeof(kSpecies) / sizeof(kSpecies[0]);

// What an unmatched locus contributes. Gaining or losing a gene is structurally
// larger than changing one, so it is worth a few substitutions' worth.
constexpr double kIndelPenalty = 1.5;

// Floor on the spacing, so a population of clones does not produce a zero scale
// and a threshold of zero.
constexpr double kScaleFloor = 1e-3;

}  // namespace

void buildNeutralSignature(ConstGenomeView genome, NeutralSignature& out) {
    out.loci.clear();
    for (uint16_t i = 0; i < genome.count; ++i) {
        const Gene& g = genome[i];
        const LocusType t = static_cast<LocusType>(g.type);
        if (t != LocusType::Junk && t != LocusType::Mhc) continue;
        NeutralSignature::Locus l;
        l.id = g.id;
        l.value = 0.5f * (g.alleleA + g.alleleB);
        out.loci.push_back(l);
    }
    std::sort(out.loci.begin(), out.loci.end(),
              [](const NeutralSignature::Locus& a, const NeutralSignature::Locus& b) {
                  return a.id < b.id;
              });
    // A duplicated gene keeps its id, so the same id can appear twice. Collapse
    // duplicates to their mean, which keeps every comparison below a clean
    // two-pointer merge instead of needing to handle runs.
    size_t w = 0;
    for (size_t r = 0; r < out.loci.size();) {
        size_t e = r;
        double sum = 0.0;
        while (e < out.loci.size() && out.loci[e].id == out.loci[r].id) {
            sum += out.loci[e].value;
            ++e;
        }
        out.loci[w].id = out.loci[r].id;
        out.loci[w].value = static_cast<float>(sum / static_cast<double>(e - r));
        ++w;
        r = e;
    }
    out.loci.resize(w);
}

double neutralDistance(const NeutralSignature& a, const NeutralSignature& b) {
    if (a.loci.empty() || b.loci.empty()) return 0.0;

    double sum = 0.0;
    size_t compared = 0;
    size_t i = 0, j = 0;
    while (i < a.loci.size() && j < b.loci.size()) {
        if (a.loci[i].id == b.loci[j].id) {
            sum += std::fabs(static_cast<double>(a.loci[i].value) -
                             static_cast<double>(b.loci[j].value));
            ++compared;
            ++i; ++j;
        } else if (a.loci[i].id < b.loci[j].id) {
            sum += kIndelPenalty; ++compared; ++i;
        } else {
            sum += kIndelPenalty; ++compared; ++j;
        }
    }
    while (i < a.loci.size()) { sum += kIndelPenalty; ++compared; ++i; }
    while (j < b.loci.size()) { sum += kIndelPenalty; ++compared; ++j; }

    if (compared == 0) return 0.0;
    return sum / static_cast<double>(compared);
}

// ---------------------------------------------------------------------------

void Speciation::configure(size_t maxAgents) {
    m_slotSpecies.assign(maxAgents, 0u);
    clear();
}

void Speciation::clear() {
    std::fill(m_slotSpecies.begin(), m_slotSpecies.end(), 0u);
    m_species.clear();
    m_events.clear();
    m_nextId = 0;
    m_scale = 0.0;
    m_lastSample = 0;
}

const SpeciesRecord* Speciation::find(uint32_t id) const {
    for (const SpeciesRecord& s : m_species)
        if (s.id == id) return &s;
    return nullptr;
}

uint32_t Speciation::extantCount() const {
    uint32_t n = 0;
    for (const SpeciesRecord& s : m_species) if (s.extant) ++n;
    return n;
}

std::string Speciation::makeName(uint32_t id, uint32_t parentId) {
    // A daughter lineage keeps its parent's genus and takes a new epithet, which
    // is what makes a phylogeny readable: sister species look like sisters.
    const uint32_t genusSeed = (parentId != 0) ? parentId : id;
    std::string name = kGenus[genusSeed % kGenusCount];
    name += kGenusTail[(genusSeed / kGenusCount) % kGenusTailCount];
    name += " ";
    name += kSpecies[id % kSpeciesCount];
    if (id > kSpeciesCount) {
        char suffix[16];
        std::snprintf(suffix, sizeof suffix, " %u",
                      1u + id / static_cast<uint32_t>(kSpeciesCount));
        name += suffix;
    }
    return name;
}

uint32_t Speciation::createSpecies(uint32_t parentId, const NeutralSignature& sig,
                                   uint64_t tick, double splitDistance) {
    SpeciesRecord r;
    r.id = ++m_nextId;
    r.parentId = parentId;
    r.name = makeName(r.id, parentId);
    r.firstTick = tick;
    r.lastTick = tick;
    r.extant = true;
    r.centroid = sig;
    r.splitDistance = splitDistance;
    m_species.push_back(std::move(r));

    PhylogenyEvent e;
    e.tick = tick;
    e.speciesId = m_nextId;
    e.parentId = parentId;
    e.kind = (parentId == 0) ? 0 : 1;
    e.distance = splitDistance;
    m_events.push_back(e);
    return m_nextId;
}

void Speciation::accumulate(NeutralSignature& runningMean, uint32_t count,
                            const NeutralSignature& add) {
    if (count == 0) { runningMean = add; return; }
    const double n = static_cast<double>(count);
    m_merge.loci.clear();
    size_t i = 0, j = 0;
    while (i < runningMean.loci.size() && j < add.loci.size()) {
        if (runningMean.loci[i].id == add.loci[j].id) {
            NeutralSignature::Locus l;
            l.id = runningMean.loci[i].id;
            l.value = static_cast<float>(
                (static_cast<double>(runningMean.loci[i].value) * n +
                 static_cast<double>(add.loci[j].value)) / (n + 1.0));
            m_merge.loci.push_back(l);
            ++i; ++j;
        } else if (runningMean.loci[i].id < add.loci[j].id) {
            m_merge.loci.push_back(runningMean.loci[i]); ++i;
        } else {
            m_merge.loci.push_back(add.loci[j]); ++j;
        }
    }
    while (i < runningMean.loci.size()) m_merge.loci.push_back(runningMean.loci[i++]);
    while (j < add.loci.size()) m_merge.loci.push_back(add.loci[j++]);
    runningMean.loci.swap(m_merge.loci);
}

void Speciation::detect(const Genetics& genetics, const std::vector<uint32_t>& countableSlots,
                        const std::vector<float>& x, const std::vector<float>& y,
                        uint64_t tick, std::vector<std::string>& newEventsOut) {
    const double gapFactor = static_cast<double>(cfg().getF("species.gap_factor", 4.0f));
    const uint32_t minFounders =
        static_cast<uint32_t>(cfg().getInt("species.min_founders", 8));
    const uint64_t grace =
        static_cast<uint64_t>(cfg().getInt("species.extinction_grace_ticks", 8640));
    const size_t maxSample =
        static_cast<size_t>(cfg().getInt("species.cluster_sample", 400));

    if (countableSlots.size() < 2) return;

    // A deterministic even-stride sample. Clustering is O(n^2) in the sample, so
    // it is bounded -- and like the population-genetics window this is a
    // REPORTING sample, never a simulation shortcut: every agent is still
    // simulated in full, and every agent is still assigned to a lineage below.
    m_sampleSlots.clear();
    if (countableSlots.size() <= maxSample) {
        m_sampleSlots = countableSlots;
    } else {
        const size_t stride = countableSlots.size() / maxSample;
        for (size_t i = 0; i < countableSlots.size() && m_sampleSlots.size() < maxSample;
             i += stride)
            m_sampleSlots.push_back(countableSlots[i]);
    }

    m_sampleSigs.resize(m_sampleSlots.size());
    size_t kept = 0;
    for (size_t i = 0; i < m_sampleSlots.size(); ++i) {
        buildNeutralSignature(genetics.genome(m_sampleSlots[i]), m_scratch);
        if (m_scratch.empty()) continue;
        m_sampleSlots[kept] = m_sampleSlots[i];
        m_sampleSigs[kept] = m_scratch;
        ++kept;
    }
    m_sampleSlots.resize(kept);
    m_sampleSigs.resize(kept);
    m_lastSample = static_cast<uint32_t>(kept);
    if (kept < 2) return;

    // The scale: median nearest-neighbour distance. This is the typical spacing
    // between an individual and its closest relative, which stays small however
    // far two subpopulations separate -- which is exactly why it works as a
    // yardstick where the pooled variance does not.
    m_nnDist.assign(kept, 1e30);
    for (size_t i = 0; i < kept; ++i) {
        for (size_t j = i + 1; j < kept; ++j) {
            const double d = neutralDistance(m_sampleSigs[i], m_sampleSigs[j]);
            if (d < m_nnDist[i]) m_nnDist[i] = d;
            if (d < m_nnDist[j]) m_nnDist[j] = d;
        }
    }
    std::vector<double> sorted = m_nnDist;
    std::sort(sorted.begin(), sorted.end());
    m_scale = std::max(kScaleFloor, sorted[sorted.size() / 2]);
    const double link = gapFactor * m_scale;

    // Single-linkage clustering at that threshold. Single linkage rather than
    // complete linkage on purpose: it is the criterion that asks "is there an
    // unbroken chain of near-neighbours between these two", which is precisely
    // what a continuous population is and what a reproductively isolated one is
    // not.
    m_clusterOf.assign(kept, -1);
    int32_t nClusters = 0;
    std::vector<size_t> frontier;
    for (size_t seed = 0; seed < kept; ++seed) {
        if (m_clusterOf[seed] >= 0) continue;
        const int32_t cluster = nClusters++;
        m_clusterOf[seed] = cluster;
        frontier.clear();
        frontier.push_back(seed);
        while (!frontier.empty()) {
            const size_t cur = frontier.back();
            frontier.pop_back();
            for (size_t other = 0; other < kept; ++other) {
                if (m_clusterOf[other] >= 0) continue;
                if (neutralDistance(m_sampleSigs[cur], m_sampleSigs[other]) > link) continue;
                m_clusterOf[other] = cluster;
                frontier.push_back(other);
            }
        }
    }

    // Cluster centroids.
    m_clusterCentroid.assign(static_cast<size_t>(nClusters), NeutralSignature{});
    m_clusterCount.assign(static_cast<size_t>(nClusters), 0u);
    for (size_t i = 0; i < kept; ++i) {
        const size_t c = static_cast<size_t>(m_clusterOf[i]);
        accumulate(m_clusterCentroid[c], m_clusterCount[c], m_sampleSigs[i]);
        ++m_clusterCount[c];
    }

    // Match each cluster to an existing lineage, nearest first. A lineage that
    // has recently had members is still a candidate host: otherwise one that
    // dipped to zero between two passes would be re-founded under a new name and
    // the phylogeny would sprout a split that never happened.
    std::vector<uint32_t> clusterSpecies(static_cast<size_t>(nClusters), 0u);
    std::vector<bool> lineageTaken(m_species.size(), false);
    for (int32_t c = 0; c < nClusters; ++c) {
        size_t best = m_species.size();
        double bestDist = link;
        for (size_t i = 0; i < m_species.size(); ++i) {
            if (lineageTaken[i]) continue;
            if (!m_species[i].extant && m_species[i].lastTick + grace < tick) continue;
            const double d = neutralDistance(m_clusterCentroid[static_cast<size_t>(c)],
                                             m_species[i].centroid);
            if (d < bestDist) { bestDist = d; best = i; }
        }
        if (best < m_species.size()) {
            clusterSpecies[static_cast<size_t>(c)] = m_species[best].id;
            lineageTaken[best] = true;
        }
    }

    // Unmatched clusters large enough to be a breeding population become new
    // lineages. A cluster of two is a pair of odd siblings, not a species.
    for (int32_t c = 0; c < nClusters; ++c) {
        const size_t ci = static_cast<size_t>(c);
        if (clusterSpecies[ci] != 0) continue;
        if (m_clusterCount[ci] < minFounders) continue;

        // Parent is whichever extant lineage the cluster sits closest to, so the
        // split attaches to the branch it actually came off.
        uint32_t parentId = 0;
        double parentDist = 0.0;
        double nearest = 1e30;
        for (const SpeciesRecord& sp : m_species) {
            if (!sp.extant) continue;
            const double d = neutralDistance(m_clusterCentroid[ci], sp.centroid);
            if (d < nearest) { nearest = d; parentId = sp.id; parentDist = d; }
        }
        const uint32_t id = createSpecies(parentId, m_clusterCentroid[ci], tick, parentDist);
        clusterSpecies[ci] = id;

        char buf[380];
        const SpeciesRecord& fresh = m_species.back();
        if (parentId == 0) {
            std::snprintf(buf, sizeof buf,
                          "A founding lineage is named: %s (%u sampled individuals)",
                          fresh.name.c_str(), m_clusterCount[ci]);
        } else {
            const SpeciesRecord* pr = find(parentId);
            std::snprintf(buf, sizeof buf,
                          "SPECIATION: %s has split from %s -- separated by %.3f in neutral "
                          "loci, which is %.1f times the population's own spacing",
                          fresh.name.c_str(), pr ? pr->name.c_str() : "?",
                          parentDist, parentDist / m_scale);
        }
        newEventsOut.push_back(buf);
    }

    // Assign EVERY countable individual, sampled or not, to the nearest lineage
    // centroid. This is what keeps the sample a reporting device: the clustering
    // is sampled, the membership is not.
    m_accum.assign(m_species.size(), NeutralSignature{});
    m_accumCount.assign(m_species.size(), 0u);
    m_dispSum.assign(m_species.size(), 0.0);
    m_cx.assign(m_species.size(), 0.0);
    m_cy.assign(m_species.size(), 0.0);
    for (SpeciesRecord& s : m_species) s.population = 0;

    for (uint32_t slot : countableSlots) {
        buildNeutralSignature(genetics.genome(slot), m_scratch);
        if (m_scratch.empty()) continue;
        size_t best = m_species.size();
        double bestDist = 1e30;
        for (size_t i = 0; i < m_species.size(); ++i) {
            if (!m_species[i].extant && m_species[i].lastTick + grace < tick) continue;
            const double d = neutralDistance(m_scratch, m_species[i].centroid);
            if (d < bestDist) { bestDist = d; best = i; }
        }
        if (best >= m_species.size()) continue;
        m_slotSpecies[slot] = m_species[best].id;
        ++m_species[best].population;
        m_species[best].lastTick = tick;
        accumulate(m_accum[best], m_accumCount[best], m_scratch);
        ++m_accumCount[best];
        m_dispSum[best] += bestDist;
        if (slot < x.size()) { m_cx[best] += x[slot]; m_cy[best] += y[slot]; }
    }

    // Commit the running centroids and settle extinctions.
    for (size_t i = 0; i < m_species.size() && i < m_accum.size(); ++i) {
        SpeciesRecord& s = m_species[i];
        if (m_accumCount[i] > 0) {
            s.drift += neutralDistance(s.centroid, m_accum[i]);
            s.centroid = m_accum[i];
            s.dispersion = m_dispSum[i] / static_cast<double>(m_accumCount[i]);
            s.centroidX = static_cast<int32_t>(m_cx[i] / m_accumCount[i]);
            s.centroidY = static_cast<int32_t>(m_cy[i] / m_accumCount[i]);
        }
        if (s.population > s.peakPopulation) s.peakPopulation = s.population;

        if (s.extant && s.population == 0) {
            // Only after a grace period. A lineage that momentarily has no
            // developed members -- everyone is an embryo, say -- has not gone
            // extinct.
            if (s.lastTick + grace < tick) {
                s.extant = false;
                PhylogenyEvent e;
                e.tick = tick;
                e.speciesId = s.id;
                e.parentId = s.parentId;
                e.kind = 2;
                m_events.push_back(e);
                char buf[280];
                std::snprintf(buf, sizeof buf,
                              "EXTINCTION: %s is gone. It lasted %.1f years and peaked at %u.",
                              s.name.c_str(),
                              static_cast<double>(s.lastTick - s.firstTick) / 8640.0,
                              s.peakPopulation);
                newEventsOut.push_back(buf);
            }
        } else if (!s.extant && s.population > 0) {
            s.extant = true;
        }
    }
}

double Speciation::crossFertility(const Genetics& genetics, uint32_t slotA,
                                  uint32_t slotB) const {
    const double strength = static_cast<double>(cfg().getF("species.hybrid_penalty", 1.0f));
    // Before the population has been measured there is no established scale, so
    // there is nothing to read a divergence against and nothing to penalise.
    if (strength <= 0.0 || m_scale <= 0.0) return 1.0;

    buildNeutralSignature(genetics.genome(slotA), m_scratch);
    buildNeutralSignature(genetics.genome(slotB), m_scratchB);
    const double d = neutralDistance(m_scratch, m_scratchB) / m_scale;

    const double onset = static_cast<double>(cfg().getF("species.hybrid_onset", 4.0f));
    if (d <= onset) return 1.0;   // no cliff: ordinary pairs pay nothing

    const double full = static_cast<double>(cfg().getF("species.hybrid_full", 12.0f));
    if (full <= onset) return std::max(0.0, 1.0 - strength);

    // Beyond the onset, fertility falls off smoothly. Squared rather than linear
    // because hybrid breakdown compounds: the further apart the parents, the more
    // independent incompatibilities a zygote has to survive all at once.
    const double t = std::min(1.0, (d - onset) / (full - onset));
    return std::max(0.0, 1.0 - strength * t * t);
}

// ---------------------------------------------------------------------------

namespace {
void writeSignature(BinaryWriter& w, const NeutralSignature& s) {
    const uint32_t n = static_cast<uint32_t>(s.loci.size());
    w.pod(n);
    for (const NeutralSignature::Locus& l : s.loci) { w.pod(l.id); w.pod(l.value); }
}
bool readSignature(BinaryReader& r, NeutralSignature& s) {
    s.loci.clear();
    uint32_t n = 0;
    r.pod(n);
    if (n > 100000u) return false;
    for (uint32_t i = 0; i < n && r.ok(); ++i) {
        NeutralSignature::Locus l;
        r.pod(l.id); r.pod(l.value);
        s.loci.push_back(l);
    }
    return true;
}
}  // namespace

void Speciation::serialize(BinaryWriter& w) const {
    w.pod(m_nextId);
    // The scale is saved because the reproductive-isolation term reads it.
    // Resuming without it would silently suspend isolation until the next
    // detection pass, which would be a change in the rules disguised as a load.
    w.pod(m_scale);
    w.pod(m_lastSample);
    const uint32_t n = static_cast<uint32_t>(m_species.size());
    w.pod(n);
    for (const SpeciesRecord& s : m_species) {
        w.pod(s.id); w.pod(s.parentId);
        w.str(s.name);
        w.pod(s.firstTick); w.pod(s.lastTick);
        const uint8_t extant = s.extant ? 1u : 0u;
        w.pod(extant);
        w.pod(s.population); w.pod(s.peakPopulation);
        w.pod(s.splitDistance); w.pod(s.drift); w.pod(s.dispersion);
        w.pod(s.centroidX); w.pod(s.centroidY);
        writeSignature(w, s.centroid);
    }
    const uint32_t ec = static_cast<uint32_t>(m_events.size());
    w.pod(ec);
    for (const PhylogenyEvent& e : m_events) {
        w.pod(e.tick); w.pod(e.speciesId); w.pod(e.parentId);
        w.pod(e.kind); w.pod(e.distance);
    }
}

void Speciation::deserialize(BinaryReader& r) {
    m_species.clear();
    m_events.clear();
    std::fill(m_slotSpecies.begin(), m_slotSpecies.end(), 0u);

    r.pod(m_nextId);
    r.pod(m_scale);
    r.pod(m_lastSample);
    uint32_t n = 0;
    r.pod(n);
    if (n > 100000u) return;
    for (uint32_t i = 0; i < n && r.ok(); ++i) {
        SpeciesRecord s;
        r.pod(s.id); r.pod(s.parentId);
        r.str(s.name);
        r.pod(s.firstTick); r.pod(s.lastTick);
        uint8_t extant = 0;
        r.pod(extant);
        s.extant = extant != 0;
        r.pod(s.population); r.pod(s.peakPopulation);
        r.pod(s.splitDistance); r.pod(s.drift); r.pod(s.dispersion);
        r.pod(s.centroidX); r.pod(s.centroidY);
        if (!readSignature(r, s.centroid)) return;
        m_species.push_back(std::move(s));
    }
    uint32_t ec = 0;
    r.pod(ec);
    if (ec > 1000000u) return;
    for (uint32_t i = 0; i < ec && r.ok(); ++i) {
        PhylogenyEvent e;
        r.pod(e.tick); r.pod(e.speciesId); r.pod(e.parentId);
        r.pod(e.kind); r.pod(e.distance);
        m_events.push_back(e);
    }
}

}  // namespace gen
