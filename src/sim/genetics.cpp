#include "sim/genetics.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "core/config.h"
#include "core/serialize.h"

namespace gen {

// ---------------------------------------------------------------------------
// Trait table
// ---------------------------------------------------------------------------

namespace {
// name, description, baseline, scale, min, max, unit, displayed, preference
const TraitSpec kTraits[kTraitCount] = {
    {"Size", "Body mass index. Drives energy stores, strength, food need and how far a step carries.", 1.0f, 0.15f, 0.20f, 4.0f, "rel", true, false},
    {"Symmetry", "Developmental stability. Falls with inbreeding, disease and starvation during growth, which is exactly why it is an honest signal of quality.", 0.85f, 0.06f, 0.0f, 1.0f, "", true, false},
    {"ColourR", "Red channel of coat/skin colouration. Neutral by itself; becomes a signal only if preferences evolve for it.", 0.5f, 0.12f, 0.0f, 1.0f, "", true, false},
    {"ColourG", "Green channel of colouration.", 0.5f, 0.12f, 0.0f, 1.0f, "", true, false},
    {"ColourB", "Blue channel of colouration.", 0.5f, 0.12f, 0.0f, 1.0f, "", true, false},
    {"Ornament1", "A costly display structure. Carries a real metabolic surcharge, so it can only be maintained by an individual that can afford it.", 0.0f, 0.16f, 0.0f, 3.0f, "", true, false},
    {"Ornament2", "A second, independent display structure.", 0.0f, 0.16f, 0.0f, 3.0f, "", true, false},

    {"MetabolicRate", "Baseline energy burn per hour, before activity and thermoregulation.", 1.0f, 0.12f, 0.20f, 3.0f, "rel", false, false},
    {"MaxLifespan", "Intrinsic lifespan. Sets the scale of the Gompertz mortality hazard, not a hard cap.", 30.0f, 5.0f, 2.0f, 200.0f, "yr", false, false},
    {"ThermalOptimum", "Body temperature setpoint expressed as the ambient temperature that costs nothing to maintain.", 22.0f, 2.5f, -20.0f, 45.0f, "C", false, false},
    {"ThermalTolerance", "Width of the comfortable ambient band before thermoregulation starts costing energy.", 12.0f, 2.0f, 1.0f, 40.0f, "C", false, false},
    {"Hardiness", "Resistance to injury, exposure and starvation damage.", 1.0f, 0.15f, 0.10f, 3.0f, "rel", false, false},
    {"ImmuneStrength", "Generic immune competence, on top of the specific MHC match against a given pathogen.", 1.0f, 0.15f, 0.05f, 4.0f, "rel", true, false},
    {"Fertility", "Probability multiplier on conception per mating.", 1.0f, 0.15f, 0.0f, 3.0f, "rel", false, false},
    {"GestationLength", "Days from fertilisation to birth. Long gestation buys a more developed newborn.", 40.0f, 8.0f, 1.0f, 400.0f, "d", false, false},
    {"DigestiveEfficiency", "Fraction of consumed biomass converted into usable energy.", 0.60f, 0.08f, 0.05f, 1.0f, "", false, false},

    {"Speed", "Maximum movement rate.", 1.0f, 0.15f, 0.05f, 4.0f, "tiles/h", false, false},
    {"Strength", "Force available for fighting, carrying and building.", 1.0f, 0.15f, 0.05f, 4.0f, "rel", false, false},
    {"SensoryAcuity", "Signal-to-noise of perception. Low acuity means the brain gets a noisier picture of the same world.", 1.0f, 0.15f, 0.10f, 3.0f, "rel", false, false},
    {"SensoryRange", "How far perception reaches, in tiles.", 6.0f, 1.2f, 1.0f, 40.0f, "tiles", false, false},

    {"SexExpression", "Continuous sexual phenotype: 0 fully female-typical, 1 fully male-typical. Driven by the sex locus, hormone-analog regulatory genes and developmental noise, so intermediate outcomes are possible.", 0.5f, 0.10f, 0.0f, 1.0f, "", true, false},
    {"OrientationMale", "Strength of attraction toward male-expressed phenotypes. Independent of OrientationFemale, so any point in the 2D space is representable.", 0.5f, 0.28f, 0.0f, 2.0f, "", false, false},
    {"OrientationFemale", "Strength of attraction toward female-expressed phenotypes.", 0.5f, 0.28f, 0.0f, 2.0f, "", false, false},
    {"PartnerSelectivity", "How high the attraction score must run before this individual will accept a mating. High selectivity means fewer but better-matched partners.", 0.5f, 0.18f, 0.0f, 2.0f, "", false, false},
    {"ParentalInvestment", "Effort diverted into feeding, defending and teaching offspring instead of into the parent's own condition.", 0.5f, 0.18f, 0.0f, 2.0f, "", false, false},
    {"MaturityAge", "Age at which the adult stage and fertility begin. Short relative to lifespan, as it is for most mammals: a long juvenile period buys a better-prepared adult but risks the lineage ending before it breeds.", 4.0f, 1.0f, 0.5f, 60.0f, "yr", false, false},

    {"PrefSize", "Weight on a potential partner's size. Negative means smaller is preferred.", 0.0f, 0.30f, -3.0f, 3.0f, "", false, true},
    {"PrefSymmetry", "Weight on a partner's symmetry.", 0.0f, 0.30f, -3.0f, 3.0f, "", false, true},
    {"PrefColour", "Weight on colour saturation.", 0.0f, 0.30f, -3.0f, 3.0f, "", false, true},
    {"PrefOrnament", "Weight on display ornaments. This is the parameter that can run away: preference and ornament reinforce each other across generations.", 0.0f, 0.30f, -3.0f, 3.0f, "", false, true},
    {"PrefHealth", "Weight on visible health markers.", 0.0f, 0.30f, -3.0f, 3.0f, "", false, true},
    {"PrefEnergy", "Weight on visible energy reserves (condition).", 0.0f, 0.30f, -3.0f, 3.0f, "", false, true},
    {"PrefStatus", "Weight on social status and reputation.", 0.0f, 0.30f, -3.0f, 3.0f, "", false, true},
    {"PrefAge", "Weight on age. Positive prefers older partners.", 0.0f, 0.30f, -3.0f, 3.0f, "", false, true},
    {"PrefMhcDissimilarity", "Preference for an immune profile unlike one's own. Positive values push toward heterozygous offspring, which resist a wider range of pathogens.", 0.4f, 0.30f, -3.0f, 3.0f, "", false, true},
    {"PrefHomophily", "Preference for partners similar to oneself on non-MHC traits.", 0.0f, 0.30f, -3.0f, 3.0f, "", false, true},

    {"PlasticityRate", "How strongly eligibility traces accumulate on plastic connections. High plasticity learns fast and forgets fast.", 1.0f, 0.20f, 0.0f, 4.0f, "rel", false, false},
    {"LearningRate", "Step size applied to plastic weights per unit of reward-prediction error.", 0.05f, 0.02f, 0.0f, 0.5f, "", false, false},
    {"RewardHunger", "How much satisfying hunger is worth to this individual. Motivation itself is heritable, so agents that care about the wrong things are selected out.", 1.0f, 0.25f, 0.0f, 4.0f, "", false, false},
    {"RewardThirst", "Value placed on satisfying thirst.", 1.0f, 0.25f, 0.0f, 4.0f, "", false, false},
    {"RewardThermal", "Value placed on thermal comfort.", 1.0f, 0.25f, 0.0f, 4.0f, "", false, false},
    {"RewardSafety", "Value placed on avoiding threat.", 1.0f, 0.25f, 0.0f, 4.0f, "", false, false},
    {"RewardRest", "Value placed on rest and recovery.", 1.0f, 0.25f, 0.0f, 4.0f, "", false, false},
    {"RewardSocial", "Value placed on affiliation and proximity to others.", 1.0f, 0.25f, 0.0f, 4.0f, "", false, false},
    {"RewardCuriosity", "Value placed on novelty. This is what drives exploration, and later what drives discovery.", 1.0f, 0.25f, 0.0f, 4.0f, "", false, false},
    {"RewardRepro", "Value placed on reproduction. The discharge of accumulated reproductive drive emits a large reward pulse that reinforces the whole preceding behaviour chain.", 1.0f, 0.25f, 0.0f, 4.0f, "", false, false},

    {"MutationRateModifier", "Multiplier this individual applies to the mutation rate of its own offspring. A mutator allele: evolvability is itself heritable and under selection.", 1.0f, 0.25f, 0.05f, 8.0f, "x", false, false},
};
}  // namespace

const TraitSpec& traitSpec(Trait t) {
    const int i = static_cast<int>(t);
    return kTraits[(i >= 0 && i < kTraitCount) ? i : 0];
}
const char* traitName(Trait t) { return traitSpec(t).name; }

const char* locusTypeName(LocusType t) {
    switch (t) {
        case LocusType::Coding:         return "Coding";
        case LocusType::Regulatory:     return "Regulatory";
        case LocusType::Junk:           return "Junk";
        case LocusType::Mhc:            return "MHC";
        case LocusType::SexDetermining: return "Sex";
        case LocusType::Count:          break;
    }
    return "?";
}

const char* dominanceName(Dominance d) {
    switch (d) {
        case Dominance::Additive:          return "Additive";
        case Dominance::CompleteDominant:  return "Dominant";
        case Dominance::CompleteRecessive: return "Recessive";
        case Dominance::Codominant:        return "Codominant";
        case Dominance::Overdominant:      return "Overdominant";
        case Dominance::Count:             break;
    }
    return "?";
}

float expressAllele(float a, float b, Dominance d) {
    // A broken (loss-of-function) allele contributes nothing, whatever the
    // dominance model says -- that is what makes lethals recessive.
    const bool ba = alleleIsBroken(a), bb = alleleIsBroken(b);
    if (ba && bb) return 0.0f;
    if (ba) return (d == Dominance::Additive || d == Dominance::Codominant) ? b * 0.5f : b;
    if (bb) return (d == Dominance::Additive || d == Dominance::Codominant) ? a * 0.5f : a;

    switch (d) {
        case Dominance::Additive:          return 0.5f * (a + b);
        case Dominance::CompleteDominant:  return (a > b) ? a : b;
        case Dominance::CompleteRecessive: return (a < b) ? a : b;
        case Dominance::Codominant:        return 0.5f * (a + b);
        case Dominance::Overdominant:      return 0.5f * (a + b) + 0.35f * std::fabs(a - b);
        case Dominance::Count:             break;
    }
    return 0.5f * (a + b);
}

// ---------------------------------------------------------------------------
// Recombination map
// ---------------------------------------------------------------------------

void RecombinationMap::build(int chromosomes, Rng& rng) {
    chromosomeCount = chromosomes;
    lengthCm = cfg().getF("genetics.chromosome_length_cm", 100.0f);
    baseRate.assign(static_cast<size_t>(chromosomes), 0.0f);
    hotspots.assign(static_cast<size_t>(chromosomes), {});

    const float base = cfg().getF("genetics.crossover_rate", 1.4f);
    const int hotspotsPer = static_cast<int>(cfg().getInt("genetics.hotspots_per_chromosome", 3));

    for (int c = 0; c < chromosomes; ++c) {
        // Chromosomes differ in length and therefore in map rate, which is why
        // linkage is stronger on some than others.
        baseRate[static_cast<size_t>(c)] = base * rng.rangef(0.6f, 1.5f);
        for (int h = 0; h < hotspotsPer; ++h) {
            Hotspot hs;
            hs.position = rng.rangef(0.0f, lengthCm);
            hs.width = rng.rangef(1.5f, 6.0f);
            hs.intensity = rng.rangef(1.5f, 6.0f);
            hotspots[static_cast<size_t>(c)].push_back(hs);
        }
    }
}

int RecombinationMap::drawCrossovers(int chromosome, Rng& rng,
                                     float* positionsOut, int maxOut) const {
    if (chromosome < 0 || chromosome >= static_cast<int>(baseRate.size())) return 0;
    const size_t c = static_cast<size_t>(chromosome);

    // Poisson-distributed crossover count with the chromosome's map rate as the
    // mean; positions land uniformly, then hotspots pull extra events in.
    const float mean = baseRate[c];
    int count = 0;
    {
        // Knuth's method; the mean is small (order 1) so this terminates fast.
        const double limit = std::exp(-static_cast<double>(mean));
        double p = 1.0;
        do {
            ++count;
            p *= rng.nextDouble();
        } while (p > limit && count < maxOut);
        --count;
    }
    for (int i = 0; i < count && i < maxOut; ++i)
        positionsOut[i] = rng.rangef(0.0f, lengthCm);

    int n = std::min(count, maxOut);
    for (const Hotspot& hs : hotspots[c]) {
        if (n >= maxOut) break;
        // Each hotspot fires with a probability set by its intensity, and when
        // it does the crossover lands within its narrow window.
        if (rng.nextFloat() < hs.intensity * 0.08f)
            positionsOut[n++] = hs.position + rng.gaussian(0.0f, hs.width * 0.4f);
    }
    if (n > 1) std::sort(positionsOut, positionsOut + n);
    return n;
}

// ---------------------------------------------------------------------------

void MutationRates::loadFromConfig() {
    point            = cfg().getF("mutation.point_rate", 0.004f);
    pointSigma       = cfg().getF("mutation.point_sigma", 0.18f);
    insertion        = cfg().getF("mutation.insertion_rate", 0.0006f);
    deletion         = cfg().getF("mutation.deletion_rate", 0.0008f);
    duplication      = cfg().getF("mutation.duplication_rate", 0.0012f);
    inversion        = cfg().getF("mutation.inversion_rate", 0.0004f);
    translocation    = cfg().getF("mutation.translocation_rate", 0.0003f);
    chromosomeDup    = cfg().getF("mutation.chromosome_dup_rate", 0.00002f);
    lethal           = cfg().getF("mutation.lethal_rate", 0.00025f);
    regulatoryShift  = cfg().getF("mutation.regulatory_shift_rate", 0.002f);
}

// ---------------------------------------------------------------------------
// Genetics
// ---------------------------------------------------------------------------

void Genetics::configure(size_t maxAgents, uint16_t geneCapacity, Rng& rng) {
    m_geneCapacity = geneCapacity;
    m_slotCount = maxAgents;
    m_arena.assign(maxAgents * geneCapacity, Gene());
    m_count.assign(maxAgents, 0);
    m_binWidth = cfg().getF("genetics.allele_bin_width", 0.25f);
    m_rates.loadFromConfig();
    m_map.build(static_cast<int>(cfg().getInt("genetics.chromosomes", 8)), rng);
    m_nextGeneId = 1;
    buildTemplate(rng);
    m_gameteA.reserve(geneCapacity * 2);
    m_gameteB.reserve(geneCapacity * 2);
}

GenomeView Genetics::genome(size_t slot) {
    GenomeView v;
    v.genes = &m_arena[slot * m_geneCapacity];
    v.count = m_count[slot];
    v.capacity = m_geneCapacity;
    return v;
}

ConstGenomeView Genetics::genome(size_t slot) const {
    ConstGenomeView v;
    v.genes = &m_arena[slot * m_geneCapacity];
    v.count = m_count[slot];
    v.capacity = m_geneCapacity;
    return v;
}

void Genetics::buildTemplate(Rng& rng) {
    m_template.clear();
    const int chromosomes = m_map.chromosomeCount;
    const int perTrait = static_cast<int>(cfg().getInt("genetics.genes_per_trait", 4));
    const int regulatory = static_cast<int>(cfg().getInt("genetics.regulatory_genes", 14));
    const int junk = static_cast<int>(cfg().getInt("genetics.junk_genes", 26));
    const int mhc = static_cast<int>(cfg().getInt("genetics.mhc_genes", 6));
    const float lethalFraction = cfg().getF("genetics.lethal_capable_fraction", 0.06f);

    auto placeGene = [&](Gene& g) {
        g.id = m_nextGeneId++;
        g.chromosome = static_cast<uint8_t>(rng.range(0, chromosomes));
        g.mapPos = rng.rangef(0.0f, m_map.lengthCm);
    };

    // Coding genes: every trait is polygenic by construction. Effect sizes are
    // drawn from a skewed distribution so a few loci matter much more than the
    // rest, which is what real quantitative traits look like and what makes
    // heritability estimates interesting rather than trivial.
    for (int t = 0; t < kTraitCount; ++t) {
        for (int k = 0; k < perTrait; ++k) {
            Gene g;
            g.type = static_cast<uint8_t>(LocusType::Coding);
            g.target = static_cast<uint16_t>(t);
            const float u = rng.nextFloat();
            g.effect = 0.25f + 1.75f * u * u;
            // Dominance is drawn per locus, so the same trait can have additive,
            // dominant and overdominant loci contributing to it at once.
            const float d = rng.nextFloat();
            g.dominance = static_cast<uint8_t>(
                d < 0.55f ? Dominance::Additive :
                d < 0.72f ? Dominance::CompleteDominant :
                d < 0.84f ? Dominance::CompleteRecessive :
                d < 0.94f ? Dominance::Codominant : Dominance::Overdominant);
            if (rng.nextFloat() < lethalFraction)
                g.flags |= (rng.nextFloat() < 0.6f) ? GeneFlag_CanCarryLethal : GeneFlag_Sublethal;
            placeGene(g);
            m_template.push_back(g);
        }
    }

    // Regulatory genes: each modulates the total expression of one trait. This
    // is genuine epistasis -- the phenotypic effect of a coding allele depends
    // on the genotype at a different, unlinked locus.
    for (int i = 0; i < regulatory; ++i) {
        Gene g;
        g.type = static_cast<uint8_t>(LocusType::Regulatory);
        g.target = static_cast<uint16_t>(rng.range(0, kTraitCount));
        g.effect = rng.rangef(0.3f, 1.2f);
        g.dominance = static_cast<uint8_t>(rng.nextFloat() < 0.5f ? Dominance::Additive
                                                                  : Dominance::CompleteDominant);
        placeGene(g);
        m_template.push_back(g);
    }

    // Junk: no phenotypic effect at all. Drifts under mutation alone, which is
    // exactly what makes it the right clock for phylogeny and for Fst.
    for (int i = 0; i < junk; ++i) {
        Gene g;
        g.type = static_cast<uint8_t>(LocusType::Junk);
        g.effect = 0.0f;
        g.dominance = static_cast<uint8_t>(Dominance::Additive);
        placeGene(g);
        m_template.push_back(g);
    }

    // MHC: overdominant by construction, which is what maintains the extreme
    // allelic diversity real MHC loci show.
    for (int i = 0; i < mhc; ++i) {
        Gene g;
        g.type = static_cast<uint8_t>(LocusType::Mhc);
        g.target = static_cast<uint16_t>(i % 8);
        g.effect = 1.0f;
        g.dominance = static_cast<uint8_t>(Dominance::Overdominant);
        placeGene(g);
        m_template.push_back(g);
    }

    // The sex locus. Placed on its own chromosome so it segregates cleanly.
    {
        Gene g;
        g.type = static_cast<uint8_t>(LocusType::SexDetermining);
        g.effect = 1.0f;
        g.dominance = static_cast<uint8_t>(Dominance::CompleteDominant);
        g.id = m_nextGeneId++;
        g.chromosome = static_cast<uint8_t>(chromosomes - 1);
        g.mapPos = m_map.lengthCm * 0.5f;
        m_template.push_back(g);
    }

    // Keep the canonical order sorted by (chromosome, map position); every
    // genome maintains this invariant so meiosis can walk it linearly.
    std::sort(m_template.begin(), m_template.end(), [](const Gene& a, const Gene& b) {
        if (a.chromosome != b.chromosome) return a.chromosome < b.chromosome;
        if (a.mapPos != b.mapPos) return a.mapPos < b.mapPos;
        return a.id < b.id;
    });
}

void Genetics::makeFounder(size_t slot, Rng& rng, bool heterogameticSex) {
    GenomeView g = genome(slot);
    const uint16_t n = static_cast<uint16_t>(
        std::min<size_t>(m_template.size(), m_geneCapacity));

    const float founderSigma = cfg().getF("genetics.founder_allele_sigma", 0.6f);
    const float mhcSigma = cfg().getF("genetics.founder_mhc_sigma", 1.8f);

    for (uint16_t i = 0; i < n; ++i) {
        g.genes[i] = m_template[i];
        Gene& gene = g.genes[i];
        const LocusType t = static_cast<LocusType>(gene.type);

        if (t == LocusType::SexDetermining) {
            // X carries a low allele, Y a high one. Homogametic individuals get
            // two X, heterogametic one X and one Y.
            gene.alleleA = 0.0f;
            gene.alleleB = heterogameticSex ? 1.0f : 0.0f;
        } else if (t == LocusType::Mhc) {
            // MHC starts diverse: that is the whole point of the locus class.
            gene.alleleA = rng.gaussian(0.0f, mhcSigma);
            gene.alleleB = rng.gaussian(0.0f, mhcSigma);
        } else {
            gene.alleleA = rng.gaussian(0.0f, founderSigma);
            gene.alleleB = rng.gaussian(0.0f, founderSigma);
        }
    }
    m_count[slot] = n;
}

void Genetics::copyGenome(size_t dstSlot, size_t srcSlot) {
    std::memcpy(&m_arena[dstSlot * m_geneCapacity], &m_arena[srcSlot * m_geneCapacity],
                sizeof(Gene) * m_geneCapacity);
    m_count[dstSlot] = m_count[srcSlot];
}

void Genetics::makeGamete(std::vector<Gene>& out, ConstGenomeView parent, Rng& rng) const {
    out.clear();
    if (parent.count == 0) return;

    float crossings[32];
    uint16_t i = 0;
    while (i < parent.count) {
        const uint8_t chromosome = parent.genes[i].chromosome;
        uint16_t end = i;
        while (end < parent.count && parent.genes[end].chromosome == chromosome) ++end;

        // Independent assortment: which of the two homologues this gamete
        // starts reading is an independent coin flip per chromosome.
        int strand = rng.nextFloat() < 0.5f ? 0 : 1;
        const int nCross = m_map.drawCrossovers(chromosome, rng, crossings, 32);
        int nextCross = 0;

        for (uint16_t k = i; k < end; ++k) {
            const Gene& src = parent.genes[k];
            // Advance past every crossover that lies before this gene, flipping
            // strand each time. Linkage falls out of this automatically: two
            // genes with no crossover between them travel together.
            while (nextCross < nCross && crossings[nextCross] <= src.mapPos) {
                // An inverted segment suppresses crossover, because the
                // homologues cannot pair along it.
                if ((src.flags & GeneFlag_Inverted) == 0) strand ^= 1;
                ++nextCross;
            }
            Gene g = src;
            const float chosen = (strand == 0) ? src.alleleA : src.alleleB;
            g.alleleA = chosen;
            g.alleleB = chosen;
            out.push_back(g);
        }
        i = end;
    }
}

void Genetics::recombine(size_t childSlot, size_t motherSlot, size_t fatherSlot,
                         Rng& rng, float mutationRateMultiplier) {
    makeGamete(m_gameteA, genome(motherSlot), rng);
    makeGamete(m_gameteB, genome(fatherSlot), rng);

    // Fertilisation matches genes by identity, not by position, which is what
    // lets two genomes with different duplication histories still combine.
    auto byId = [](const Gene& a, const Gene& b) { return a.id < b.id; };
    std::sort(m_gameteA.begin(), m_gameteA.end(), byId);
    std::sort(m_gameteB.begin(), m_gameteB.end(), byId);

    GenomeView child = genome(childSlot);
    uint16_t n = 0;
    size_t ia = 0, ib = 0;
    const size_t na = m_gameteA.size(), nb = m_gameteB.size();

    auto emit = [&](const Gene& g) {
        if (n < m_geneCapacity) child.genes[n++] = g;
    };

    while (ia < na || ib < nb) {
        if (ia < na && (ib >= nb || m_gameteA[ia].id < m_gameteB[ib].id)) {
            // Present in the mother's gamete only: a single copy. Marked
            // hemizygous rather than silently doubled, so a fresh duplicate
            // does not pretend to be homozygous.
            Gene g = m_gameteA[ia++];
            g.flags |= GeneFlag_Hemizygous;
            emit(g);
        } else if (ib < nb && (ia >= na || m_gameteB[ib].id < m_gameteA[ia].id)) {
            Gene g = m_gameteB[ib++];
            g.flags |= GeneFlag_Hemizygous;
            emit(g);
        } else {
            // Present in both: a proper diploid locus.
            Gene g = m_gameteA[ia];
            g.alleleA = m_gameteA[ia].alleleA;
            g.alleleB = m_gameteB[ib].alleleA;
            g.flags = static_cast<uint8_t>(
                (m_gameteA[ia].flags | m_gameteB[ib].flags) & ~GeneFlag_Hemizygous);
            emit(g);
            ++ia;
            ++ib;
        }
    }
    m_count[childSlot] = n;
    mutateGenome(childSlot, rng, mutationRateMultiplier);

    // Restore the (chromosome, map position) ordering the next meiosis assumes.
    Gene* g = child.genes;
    std::sort(g, g + m_count[childSlot], [](const Gene& a, const Gene& b) {
        if (a.chromosome != b.chromosome) return a.chromosome < b.chromosome;
        if (a.mapPos != b.mapPos) return a.mapPos < b.mapPos;
        return a.id < b.id;
    });
}

void Genetics::mutateGenome(size_t slot, Rng& rng, float rateMultiplier) {
    const MutationRates& r = m_rates;
    // The individual's own mutator alleles, times whatever multiplier god
    // mode has decreed globally.
    const float m = ((rateMultiplier > 0.0f) ? rateMultiplier : 1.0f)
                  * cfg().getF("rules.mutation_rate_multiplier", 1.0f);
    GenomeView g = genome(slot);
    uint16_t n = g.count;

    // Point mutation and allele breakage, per gene.
    for (uint16_t i = 0; i < n; ++i) {
        Gene& gene = g.genes[i];
        if (rng.nextFloat() < r.point * m) {
            float& a = (rng.nextFloat() < 0.5f) ? gene.alleleA : gene.alleleB;
            if (!alleleIsBroken(a)) a += rng.gaussian(0.0f, r.pointSigma);
        }
        if ((gene.flags & (GeneFlag_CanCarryLethal | GeneFlag_Sublethal)) &&
            rng.nextFloat() < r.lethal * m) {
            float& a = (rng.nextFloat() < 0.5f) ? gene.alleleA : gene.alleleB;
            a = kBrokenAllele;
        }
        if (static_cast<LocusType>(gene.type) == LocusType::Regulatory &&
            rng.nextFloat() < r.regulatoryShift * m) {
            gene.target = static_cast<uint16_t>(rng.range(0, kTraitCount));
        }
        if (rng.nextFloat() < r.translocation * m) {
            gene.chromosome = static_cast<uint8_t>(rng.range(0, m_map.chromosomeCount));
            gene.mapPos = rng.rangef(0.0f, m_map.lengthCm);
        }
    }

    // Deletion, walking backwards so the swap-erase cannot skip a gene. The sex
    // locus is never deleted: losing it would make sex determination undefined.
    for (int i = static_cast<int>(n) - 1; i >= 0; --i) {
        Gene& gene = g.genes[i];
        if (static_cast<LocusType>(gene.type) == LocusType::SexDetermining) continue;
        if (rng.nextFloat() < r.deletion * m) {
            g.genes[i] = g.genes[n - 1];
            --n;
        }
    }

    // Duplication. THIS is what lets genome complexity grow: the copy gets a
    // fresh identity, so it is a new locus that can diverge from its parent
    // gene and pick up a different function -- which is how brains gain
    // capacity across generations rather than being capped at birth.
    const uint16_t beforeDup = n;
    for (uint16_t i = 0; i < beforeDup && n < g.capacity; ++i) {
        if (rng.nextFloat() < r.duplication * m) {
            Gene copy = g.genes[i];
            copy.id = m_nextGeneId++;
            copy.mapPos += rng.gaussian(0.0f, 2.0f);
            copy.flags &= static_cast<uint8_t>(~GeneFlag_Hemizygous);
            g.genes[n++] = copy;
        }
    }

    // Insertion of an entirely novel gene.
    if (n < g.capacity && rng.nextFloat() < r.insertion * m) {
        Gene ng;
        ng.id = m_nextGeneId++;
        ng.type = static_cast<uint8_t>(rng.nextFloat() < 0.5f ? LocusType::Coding
                                                              : LocusType::Junk);
        ng.target = static_cast<uint16_t>(rng.range(0, kTraitCount));
        ng.effect = rng.rangef(0.1f, 1.0f);
        ng.dominance = static_cast<uint8_t>(Dominance::Additive);
        ng.chromosome = static_cast<uint8_t>(rng.range(0, m_map.chromosomeCount));
        ng.mapPos = rng.rangef(0.0f, m_map.lengthCm);
        ng.alleleA = rng.gaussian(0.0f, 0.5f);
        ng.alleleB = rng.gaussian(0.0f, 0.5f);
        g.genes[n++] = ng;
    }

    // Inversion: reverse the map positions of a run of genes on one chromosome
    // and flag them, which suppresses crossover through the segment and so
    // locks the alleles inside it into a co-inherited block.
    if (n > 4 && rng.nextFloat() < r.inversion * m) {
        const uint16_t start = static_cast<uint16_t>(rng.range(0, n - 3));
        const uint16_t len = static_cast<uint16_t>(rng.range(2, std::min<int>(12, n - start)));
        const uint8_t chromosome = g.genes[start].chromosome;
        float lo = 1e30f, hi = -1e30f;
        for (uint16_t i = start; i < start + len; ++i) {
            if (g.genes[i].chromosome != chromosome) continue;
            lo = std::min(lo, g.genes[i].mapPos);
            hi = std::max(hi, g.genes[i].mapPos);
        }
        if (hi > lo) {
            for (uint16_t i = start; i < start + len; ++i) {
                if (g.genes[i].chromosome != chromosome) continue;
                g.genes[i].mapPos = lo + (hi - g.genes[i].mapPos);
                g.genes[i].flags |= GeneFlag_Inverted;
            }
        }
    }

    // Whole-chromosome duplication: rare, and the origin of polyploidy events.
    if (rng.nextFloat() < r.chromosomeDup * m) {
        const uint8_t source = static_cast<uint8_t>(rng.range(0, m_map.chromosomeCount));
        const uint8_t dest = static_cast<uint8_t>(m_map.chromosomeCount);
        const uint16_t before = n;
        for (uint16_t i = 0; i < before && n < g.capacity; ++i) {
            if (g.genes[i].chromosome != source) continue;
            Gene copy = g.genes[i];
            copy.id = m_nextGeneId++;
            copy.chromosome = dest;
            copy.flags &= static_cast<uint8_t>(~GeneFlag_Hemizygous);
            g.genes[n++] = copy;
        }
        // The new chromosome must exist in the map or its genes would never
        // recombine again.
        if (n > before && dest >= m_map.baseRate.size()) {
            m_map.chromosomeCount = dest + 1;
            m_map.baseRate.push_back(cfg().getF("genetics.crossover_rate", 1.4f));
            m_map.hotspots.push_back({});
        }
    }

    m_count[slot] = n;
}

// ---------------------------------------------------------------------------
// Expression
// ---------------------------------------------------------------------------

// Genotypic and additive values, with no developmental noise and no clamping to
// the trait's legal range.
//
// Deliberately NOT clamped: clamping is part of turning a genotype into a living
// body, and a variance computed on clamped values understates how much genetic
// variation is really there whenever a trait is pressed against its limit.
void Genetics::genotypicValues(size_t slot, float* genotypicOut, float* additiveOut) const {
    ConstGenomeView g = genome(slot);
    float coding[kTraitCount];
    float regulatory[kTraitCount];
    for (int i = 0; i < kTraitCount; ++i) { coding[i] = 0.0f; regulatory[i] = 0.0f; }

    for (uint16_t i = 0; i < g.count; ++i) {
        const Gene& gene = g.genes[i];
        const bool hemi = (gene.flags & GeneFlag_Hemizygous) != 0;
        const Dominance dom = static_cast<Dominance>(gene.dominance);
        const float v = hemi ? (alleleIsBroken(gene.alleleA) ? 0.0f : gene.alleleA * 0.5f)
                             : expressAllele(gene.alleleA, gene.alleleB, dom);
        const LocusType t = static_cast<LocusType>(gene.type);
        if (t == LocusType::Coding && gene.target < kTraitCount)
            coding[gene.target] += v * gene.effect;
        else if (t == LocusType::Regulatory && gene.target < kTraitCount)
            regulatory[gene.target] += v * gene.effect;
    }

    for (int t = 0; t < kTraitCount; ++t) {
        const TraitSpec& spec = kTraits[t];
        const float reg = std::min(4.0f, std::max(0.05f, 1.0f + regulatory[t] * 0.25f));
        if (genotypicOut) genotypicOut[t] = spec.baseline + spec.scale * coding[t] * reg;
        // Additive: the same sum with regulation held at unity.
        if (additiveOut) additiveOut[t] = spec.baseline + spec.scale * coding[t];
    }
}

void Genetics::computeHeritability(const std::vector<uint32_t>& slots,
                                   const std::vector<Phenotype>& phenotypes,
                                   PopulationGenetics& out) const {
    for (int t = 0; t < kTraitCount; ++t) out.heritability[t] = TraitHeritability{};
    if (slots.size() < 2) return;

    // Plain two-pass sums in a fixed order. Numerically less elegant than
    // Welford, but the operand order is identical every run, which is what
    // reproducibility needs.
    std::vector<double> sumA(kTraitCount, 0.0), sumA2(kTraitCount, 0.0);
    std::vector<double> sumG(kTraitCount, 0.0), sumG2(kTraitCount, 0.0);
    std::vector<double> sumP(kTraitCount, 0.0), sumP2(kTraitCount, 0.0);
    std::vector<float> gv(kTraitCount), av(kTraitCount);

    for (uint32_t slot : slots) {
        genotypicValues(slot, gv.data(), av.data());
        for (int t = 0; t < kTraitCount; ++t) {
            const double a = static_cast<double>(av[t]);
            const double gg = static_cast<double>(gv[t]);
            const double pp = static_cast<double>(phenotypes[slot].traits[t]);
            sumA[t] += a;  sumA2[t] += a * a;
            sumG[t] += gg; sumG2[t] += gg * gg;
            sumP[t] += pp; sumP2[t] += pp * pp;
        }
    }

    // How many coding loci target each trait, from the shared template. A trait
    // driven by one gene is Mendelian, not polygenic, and the UI says so.
    std::vector<uint32_t> loci(kTraitCount, 0u);
    for (const Gene& gene : m_template)
        if (static_cast<LocusType>(gene.type) == LocusType::Coding &&
            gene.target < kTraitCount)
            ++loci[gene.target];

    const double n = static_cast<double>(slots.size());
    for (int t = 0; t < kTraitCount; ++t) {
        TraitHeritability& h = out.heritability[t];
        const double mA = sumA[t] / n, mG = sumG[t] / n, mP = sumP[t] / n;
        h.additiveVariance   = std::max(0.0, sumA2[t] / n - mA * mA);
        h.genotypicVariance  = std::max(0.0, sumG2[t] / n - mG * mG);
        h.phenotypicVariance = std::max(0.0, sumP2[t] / n - mP * mP);
        h.codingLoci = loci[t];
        if (h.phenotypicVariance > 1e-12) {
            // Capped at 1, because a heritability above 1 is meaningless. But the
            // fact that it WANTED to exceed 1 is meaningful: it means the trait is
            // clamped against its range and carrying genetic variation the body
            // cannot express, so that is recorded rather than swallowed.
            const double raw = h.additiveVariance / h.phenotypicVariance;
            h.rangeLimited = raw > 1.02;
            h.narrowH2 = std::min(1.0, raw);
            h.broadH2  = std::min(1.0, h.genotypicVariance / h.phenotypicVariance);
        }
    }
}

void Genetics::express(size_t slot, Rng& rng, float developmentalNoise, Phenotype& out) const {
    ConstGenomeView g = genome(slot);

    float coding[kTraitCount] = {0.0f};
    float regulatory[kTraitCount] = {0.0f};
    for (int i = 0; i < kTraitCount; ++i) { coding[i] = 0.0f; regulatory[i] = 0.0f; }
    for (int i = 0; i < 8; ++i) out.mhcSignature[i] = 0.0f;

    int hetCount = 0, diploidCount = 0;
    int lethalCarried = 0;
    bool lethalExpressed = false;
    float sublethal = 0.0f;
    float sexShift = 0.0f;

    for (uint16_t i = 0; i < g.count; ++i) {
        const Gene& gene = g.genes[i];
        const bool hemi = (gene.flags & GeneFlag_Hemizygous) != 0;
        const Dominance dom = static_cast<Dominance>(gene.dominance);
        const float v = hemi ? (alleleIsBroken(gene.alleleA) ? 0.0f : gene.alleleA * 0.5f)
                             : expressAllele(gene.alleleA, gene.alleleB, dom);

        if (!hemi) {
            ++diploidCount;
            if (std::fabs(gene.alleleA - gene.alleleB) > 1e-4f) ++hetCount;
        }

        // Lethal and sublethal accounting.
        if (gene.flags & (GeneFlag_CanCarryLethal | GeneFlag_Sublethal)) {
            const bool ba = alleleIsBroken(gene.alleleA);
            const bool bb = hemi ? ba : alleleIsBroken(gene.alleleB);
            if (ba && bb) {
                if (gene.flags & GeneFlag_CanCarryLethal) lethalExpressed = true;
                else sublethal += 0.12f;
            } else if (ba || bb) {
                ++lethalCarried;
            }
        }

        switch (static_cast<LocusType>(gene.type)) {
            case LocusType::Coding:
                if (gene.target < kTraitCount) coding[gene.target] += v * gene.effect;
                break;
            case LocusType::Regulatory:
                if (gene.target < kTraitCount) regulatory[gene.target] += v * gene.effect;
                break;
            case LocusType::Mhc:
                out.mhcSignature[gene.target & 7] += v;
                break;
            case LocusType::SexDetermining:
                // A high allele is the Y (or W). Complete dominance means one
                // copy is enough to push development toward the heterogametic
                // phenotype -- but it is a SHIFT, not a switch, which is what
                // makes intermediate outcomes possible.
                sexShift = (std::max(gene.alleleA, gene.alleleB) > 0.5f) ? 1.0f : -1.0f;
                break;
            case LocusType::Junk:
            case LocusType::Count:
                break;
        }
    }

    for (int t = 0; t < kTraitCount; ++t) {
        const TraitSpec& spec = kTraits[t];
        // Regulatory genes multiply the coding total. Clamped well away from
        // zero so a regulatory knockout suppresses a trait without inverting it.
        const float reg = std::min(4.0f, std::max(0.05f, 1.0f + regulatory[t] * 0.25f));
        float value = spec.baseline + spec.scale * coding[t] * reg;

        // Developmental noise: the same genotype does not produce the same
        // phenotype twice. Scaled by the trait's own range.
        if (developmentalNoise > 0.0f)
            value += rng.gaussian(0.0f, developmentalNoise * spec.scale);

        out.traits[t] = std::min(spec.maxValue, std::max(spec.minValue, value));
    }

    // Sex expression gets the sex-locus shift on top of its polygenic and
    // hormone-analog regulatory input.
    {
        const int si = static_cast<int>(Trait::SexExpression);
        const TraitSpec& spec = kTraits[si];
        const float shifted = out.traits[si] + sexShift * cfg().getF("genetics.sex_locus_shift", 0.34f);
        out.traits[si] = std::min(spec.maxValue, std::max(spec.minValue, shifted));
    }

    // Symmetry is degraded by inbreeding: homozygosity exposes deleterious
    // recessives and destabilises development. This is the mechanism that
    // makes symmetry an honest signal rather than an arbitrary label.
    out.heterozygosity = (diploidCount > 0)
        ? static_cast<float>(hetCount) / static_cast<float>(diploidCount) : 0.0f;
    const double he = (m_populationHe > 1e-6) ? m_populationHe : 0.5;
    out.inbreedingF = static_cast<float>(
        std::min(1.0, std::max(0.0, 1.0 - static_cast<double>(out.heterozygosity) / he)));
    {
        const int si = static_cast<int>(Trait::Symmetry);
        out.traits[si] = std::max(0.0f, out.traits[si] *
                                        (1.0f - 0.45f * out.inbreedingF) - sublethal * 0.5f);
    }

    out.lethalCarried = static_cast<uint8_t>(std::min(255, lethalCarried));
    out.lethalExpressed = lethalExpressed;
    out.sublethalPenalty = std::min(0.9f, sublethal);

    // Ornaments cost energy to build and carry. Without this, sexual selection
    // would be free and runaway would never be checked by anything.
    const float ornamentCost = cfg().getF("genetics.ornament_metabolic_cost", 0.22f);
    out.ornamentCost = (out.traits[static_cast<int>(Trait::Ornament1)] +
                        out.traits[static_cast<int>(Trait::Ornament2)]) * ornamentCost;
}

// ---------------------------------------------------------------------------
// Population genetics
// ---------------------------------------------------------------------------

namespace {
inline int32_t binOf(float allele, float width) {
    return static_cast<int32_t>(std::floor(allele / width));
}
}  // namespace

void Genetics::computePopulationGenetics(const std::vector<uint32_t>& liveSlots,
                                         PopulationGenetics& out) {
    out = PopulationGenetics();
    const size_t n = liveSlots.size();
    if (n < 2) return;

    // Gather allele states per gene id. Genes present in only part of the
    // population (fresh duplicates) are handled naturally: their sample size is
    // simply smaller.
    std::unordered_map<uint16_t, std::vector<int32_t>> alleles;
    alleles.reserve(512);
    double genomeLengthSum = 0.0;
    double hoSum = 0.0;

    for (uint32_t slot : liveSlots) {
        ConstGenomeView g = genome(slot);
        genomeLengthSum += g.count;
        int het = 0, dip = 0;
        for (uint16_t i = 0; i < g.count; ++i) {
            const Gene& gene = g.genes[i];
            std::vector<int32_t>& v = alleles[gene.id];
            const int32_t ba = binOf(gene.alleleA, m_binWidth);
            v.push_back(ba);
            if ((gene.flags & GeneFlag_Hemizygous) == 0) {
                const int32_t bb = binOf(gene.alleleB, m_binWidth);
                v.push_back(bb);
                ++dip;
                if (ba != bb) ++het;
            }
        }
        if (dip > 0) hoSum += static_cast<double>(het) / static_cast<double>(dip);
    }

    // Expected heterozygosity per locus, 1 - sum(p^2), averaged over loci.
    // Iterating an unordered_map would be order-dependent, so gene ids are
    // collected and sorted first -- determinism is not negotiable here either.
    std::vector<uint16_t> ids;
    ids.reserve(alleles.size());
    for (const auto& kv : alleles) ids.push_back(kv.first);
    std::sort(ids.begin(), ids.end());

    double heSum = 0.0;
    uint32_t lociCounted = 0, segregating = 0, fixed = 0;
    double piSum = 0.0;

    std::unordered_map<int32_t, uint32_t> counts;
    for (uint16_t id : ids) {
        const std::vector<int32_t>& v = alleles[id];
        if (v.size() < 2) continue;
        counts.clear();
        for (int32_t b : v) ++counts[b];
        double sumP2 = 0.0;
        const double total = static_cast<double>(v.size());
        for (const auto& kv : counts) {
            const double p = static_cast<double>(kv.second) / total;
            sumP2 += p * p;
        }
        const double he = 1.0 - sumP2;
        heSum += he;
        piSum += he;
        ++lociCounted;
        if (counts.size() > 1) ++segregating; else ++fixed;
    }

    if (lociCounted == 0) return;

    out.sampleSize = static_cast<uint32_t>(n);
    out.observedHeterozygosity = hoSum / static_cast<double>(n);
    out.expectedHeterozygosity = heSum / static_cast<double>(lociCounted);
    out.meanGenomeLength = genomeLengthSum / static_cast<double>(n);
    out.segregatingSites = segregating;
    out.fixedLoci = fixed;
    m_populationHe = out.expectedHeterozygosity;

    out.meanInbreedingF = (out.expectedHeterozygosity > 1e-9)
        ? std::max(0.0, 1.0 - out.observedHeterozygosity / out.expectedHeterozygosity)
        : 0.0;

    // Tajima's D. pi is the mean pairwise difference (estimated here by the
    // mean expected heterozygosity), S the number of segregating sites, and a1
    // the harmonic number that makes Watterson's estimator unbiased. D near
    // zero means neutrality; strongly negative follows a bottleneck or a
    // selective sweep, positive indicates balancing selection.
    const double sampleAlleles = 2.0 * static_cast<double>(n);
    double a1 = 0.0, a2 = 0.0;
    for (double i = 1.0; i < sampleAlleles; i += 1.0) { a1 += 1.0 / i; a2 += 1.0 / (i * i); }
    if (a1 > 0.0 && lociCounted > 0) {
        const double pi = piSum / static_cast<double>(lociCounted);
        const double S = static_cast<double>(segregating) / static_cast<double>(lociCounted);
        const double theta = S / a1;
        const double nn = sampleAlleles;
        const double b1 = (nn + 1.0) / (3.0 * (nn - 1.0));
        const double b2 = 2.0 * (nn * nn + nn + 3.0) / (9.0 * nn * (nn - 1.0));
        const double c1 = b1 - 1.0 / a1;
        const double c2 = b2 - (nn + 2.0) / (a1 * nn) + a2 / (a1 * a1);
        const double e1 = c1 / a1;
        const double e2 = c2 / (a1 * a1 + a2);
        const double var = e1 * S + e2 * S * (S - 1.0 / a1);
        out.tajimasD = (var > 1e-12) ? (pi - theta) / std::sqrt(var) : 0.0;
    }

    // Effective population size under the infinite-alleles model:
    // He = 4*Ne*mu / (1 + 4*Ne*mu)  =>  Ne = He / (4*mu*(1 - He)).
    const double mu = static_cast<double>(m_rates.point);
    if (mu > 1e-12 && out.expectedHeterozygosity < 0.999)
        out.effectivePopulationSize =
            out.expectedHeterozygosity / (4.0 * mu * (1.0 - out.expectedHeterozygosity));

    // Linkage disequilibrium, measured as the mean squared correlation of
    // allele values between pairs of loci on the SAME chromosome. If linkage is
    // real this sits above the same measure for unlinked pairs.
    {
        ConstGenomeView ref = genome(liveSlots[0]);
        double ldSum = 0.0;
        int ldPairs = 0;
        const int maxPairs = 160;
        for (uint16_t i = 0; i + 1 < ref.count && ldPairs < maxPairs; i += 3) {
            const uint16_t j = static_cast<uint16_t>(i + 1);
            if (ref.genes[i].chromosome != ref.genes[j].chromosome) continue;
            const uint16_t idA = ref.genes[i].id, idB = ref.genes[j].id;
            double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
            int cnt = 0;
            for (uint32_t slot : liveSlots) {
                ConstGenomeView g = genome(slot);
                float va = 0.0f, vb = 0.0f;
                bool fa = false, fb = false;
                for (uint16_t k = 0; k < g.count; ++k) {
                    if (g.genes[k].id == idA) { va = g.genes[k].alleleA + g.genes[k].alleleB; fa = true; }
                    else if (g.genes[k].id == idB) { vb = g.genes[k].alleleA + g.genes[k].alleleB; fb = true; }
                }
                if (!fa || !fb) continue;
                sa += va; sb += vb; saa += va * va; sbb += vb * vb; sab += va * vb;
                ++cnt;
            }
            if (cnt > 3) {
                const double dn = cnt;
                const double cov = sab / dn - (sa / dn) * (sb / dn);
                const double va2 = saa / dn - (sa / dn) * (sa / dn);
                const double vb2 = sbb / dn - (sb / dn) * (sb / dn);
                if (va2 > 1e-9 && vb2 > 1e-9) {
                    const double r = cov / std::sqrt(va2 * vb2);
                    ldSum += r * r;
                    ++ldPairs;
                }
            }
        }
        out.linkageDisequilibrium = (ldPairs > 0) ? ldSum / ldPairs : 0.0;
    }

    // Allele variance, a blunt but useful summary of how much raw genetic
    // material the population still has to work with.
    double varSum = 0.0;
    for (uint16_t id : ids) {
        const std::vector<int32_t>& v = alleles[id];
        if (v.size() < 2) continue;
        double s = 0.0, s2 = 0.0;
        for (int32_t b : v) { const double x = b * m_binWidth; s += x; s2 += x * x; }
        const double dn = static_cast<double>(v.size());
        varSum += s2 / dn - (s / dn) * (s / dn);
    }
    out.alleleVariance = varSum / static_cast<double>(lociCounted);
}

double Genetics::computeFst(const std::vector<std::vector<uint32_t>>& subpopulations) const {
    // Wright's Fst: the fraction of total genetic variance that sits BETWEEN
    // subpopulations rather than within them. Rising Fst across a barrier is
    // what incipient speciation looks like.
    std::vector<uint32_t> all;
    int usable = 0;
    for (const auto& sub : subpopulations) {
        if (sub.size() >= 4) ++usable;
        all.insert(all.end(), sub.begin(), sub.end());
    }
    if (usable < 2 || all.size() < 8) return 0.0;

    auto meanHe = [this](const std::vector<uint32_t>& slots) -> double {
        if (slots.size() < 2) return 0.0;
        std::unordered_map<uint16_t, std::vector<int32_t>> alleles;
        for (uint32_t slot : slots) {
            ConstGenomeView g = genome(slot);
            for (uint16_t i = 0; i < g.count; ++i) {
                // Junk loci only: neutral markers, so Fst measures drift and
                // isolation rather than selection acting differently by region.
                if (static_cast<LocusType>(g.genes[i].type) != LocusType::Junk) continue;
                std::vector<int32_t>& v = alleles[g.genes[i].id];
                v.push_back(binOf(g.genes[i].alleleA, m_binWidth));
                if ((g.genes[i].flags & GeneFlag_Hemizygous) == 0)
                    v.push_back(binOf(g.genes[i].alleleB, m_binWidth));
            }
        }
        std::vector<uint16_t> ids;
        ids.reserve(alleles.size());
        for (const auto& kv : alleles) ids.push_back(kv.first);
        std::sort(ids.begin(), ids.end());

        double heSum = 0.0;
        int loci = 0;
        std::unordered_map<int32_t, uint32_t> counts;
        for (uint16_t id : ids) {
            const std::vector<int32_t>& v = alleles[id];
            if (v.size() < 2) continue;
            counts.clear();
            for (int32_t b : v) ++counts[b];
            double sumP2 = 0.0;
            const double total = static_cast<double>(v.size());
            for (const auto& kv : counts) {
                const double p = static_cast<double>(kv.second) / total;
                sumP2 += p * p;
            }
            heSum += 1.0 - sumP2;
            ++loci;
        }
        return (loci > 0) ? heSum / loci : 0.0;
    };

    const double hT = meanHe(all);
    if (hT < 1e-9) return 0.0;
    double hSsum = 0.0;
    int counted = 0;
    for (const auto& sub : subpopulations) {
        if (sub.size() < 4) continue;
        hSsum += meanHe(sub);
        ++counted;
    }
    if (counted == 0) return 0.0;
    const double hS = hSsum / counted;
    return std::max(0.0, (hT - hS) / hT);
}

float Genetics::alleleMean(const std::vector<uint32_t>& liveSlots, uint16_t geneId) const {
    double sum = 0.0;
    int count = 0;
    for (uint32_t slot : liveSlots) {
        ConstGenomeView g = genome(slot);
        for (uint16_t i = 0; i < g.count; ++i) {
            if (g.genes[i].id != geneId) continue;
            sum += g.genes[i].alleleA;
            ++count;
            if ((g.genes[i].flags & GeneFlag_Hemizygous) == 0) {
                sum += g.genes[i].alleleB;
                ++count;
            }
            break;
        }
    }
    return (count > 0) ? static_cast<float>(sum / count) : 0.0f;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

void Genetics::serialize(BinaryWriter& w, const std::vector<uint32_t>& liveSlots) const {
    w.pod(m_geneCapacity);
    w.pod(m_nextGeneId);
    w.pod(m_binWidth);

    const uint32_t chromosomes = static_cast<uint32_t>(m_map.chromosomeCount);
    w.pod(chromosomes);
    w.pod(m_map.lengthCm);
    for (size_t c = 0; c < m_map.baseRate.size(); ++c) {
        w.pod(m_map.baseRate[c]);
        const uint32_t hn = static_cast<uint32_t>(m_map.hotspots[c].size());
        w.pod(hn);
        for (const auto& hs : m_map.hotspots[c]) {
            w.pod(hs.position);
            w.pod(hs.width);
            w.pod(hs.intensity);
        }
    }

    const uint32_t templateCount = static_cast<uint32_t>(m_template.size());
    w.pod(templateCount);
    for (const Gene& g : m_template) {
        w.pod(g.alleleA); w.pod(g.alleleB); w.pod(g.mapPos); w.pod(g.effect);
        w.pod(g.id); w.pod(g.target);
        w.pod(g.chromosome); w.pod(g.type); w.pod(g.dominance); w.pod(g.flags);
    }

    const uint32_t n = static_cast<uint32_t>(liveSlots.size());
    w.pod(n);
    for (uint32_t slot : liveSlots) {
        w.pod(slot);
        const uint16_t count = m_count[slot];
        w.pod(count);
        const Gene* g = &m_arena[slot * m_geneCapacity];
        for (uint16_t i = 0; i < count; ++i) {
            w.pod(g[i].alleleA); w.pod(g[i].alleleB); w.pod(g[i].mapPos); w.pod(g[i].effect);
            w.pod(g[i].id); w.pod(g[i].target);
            w.pod(g[i].chromosome); w.pod(g[i].type); w.pod(g[i].dominance); w.pod(g[i].flags);
        }
    }
}

void Genetics::deserializeSlot(BinaryReader& r, size_t slot) {
    uint16_t count = 0;
    r.pod(count);
    if (count > m_geneCapacity) count = m_geneCapacity;
    Gene* g = &m_arena[slot * m_geneCapacity];
    for (uint16_t i = 0; i < count; ++i) {
        r.pod(g[i].alleleA); r.pod(g[i].alleleB); r.pod(g[i].mapPos); r.pod(g[i].effect);
        r.pod(g[i].id); r.pod(g[i].target);
        r.pod(g[i].chromosome); r.pod(g[i].type); r.pod(g[i].dominance); r.pod(g[i].flags);
    }
    m_count[slot] = count;
}

}  // namespace gen
