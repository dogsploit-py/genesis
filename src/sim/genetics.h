// sim/genetics.h — the genome, its expression into a phenotype, and the
// machinery of inheritance.
//
// Design decisions that matter:
//
//   * The genome is DIPLOID and VARIABLE LENGTH. Each gene carries two alleles
//     (maternal, paternal) and its own identity, chromosome, map position,
//     dominance model and target. Carrying the metadata per gene rather than in
//     a shared schema costs memory, but it is what makes gene duplication real:
//     a duplicated gene is a genuinely new locus that can then diverge, which
//     is the only honest way for genome complexity to grow across generations.
//
//   * Genes are matched between parents by `id`, not by position. That is what
//     lets two genomes with different lengths and different duplication
//     histories still recombine sensibly.
//
//   * Every visible trait is polygenic: the sum of many coding genes, scaled by
//     regulatory genes that target the same trait, plus developmental noise.
//     Nothing is one-gene-one-trait.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/rng.h"

namespace gen {

class BinaryWriter;
class BinaryReader;

// ---------------------------------------------------------------------------
// Traits
// ---------------------------------------------------------------------------

enum class Trait : uint8_t {
    // -- morphology (also the sexually displayed traits, see M4) --
    Size = 0,
    Symmetry,           // developmental stability; an honest fitness signal
    ColourR, ColourG, ColourB,
    Ornament1, Ornament2,   // costly display structures; metabolic cost applied

    // -- physiology --
    MetabolicRate,
    MaxLifespan,
    ThermalOptimum,
    ThermalTolerance,
    Hardiness,
    ImmuneStrength,
    Fertility,
    GestationLength,
    DigestiveEfficiency,

    // -- performance --
    Speed,
    Strength,
    SensoryAcuity,
    SensoryRange,

    // -- sex and reproduction --
    SexExpression,          // shifted by the sex locus at development
    OrientationMale,        // attraction strength toward male-expressed phenotypes
    OrientationFemale,      // attraction strength toward female-expressed phenotypes
    PartnerSelectivity,
    ParentalInvestment,
    MaturityAge,

    // -- mate preference weights over displayed traits (M4) --
    PrefSize, PrefSymmetry, PrefColour, PrefOrnament,
    PrefHealth, PrefEnergy, PrefStatus, PrefAge,
    PrefMhcDissimilarity,   // heterozygote advantage expressed as a preference
    PrefHomophily,          // like-with-like on non-MHC traits

    // -- brain and learning (M3) --
    PlasticityRate,
    LearningRate,
    RewardHunger, RewardThirst, RewardThermal, RewardSafety,
    RewardRest, RewardSocial, RewardCuriosity, RewardRepro,

    // -- evolvability --
    MutationRateModifier,

    Count
};

constexpr int kTraitCount = static_cast<int>(Trait::Count);

struct TraitSpec {
    const char* name;
    const char* description;
    float baseline;   // phenotype when the genetic contribution sums to zero
    float scale;      // phenotype units per unit of summed allele contribution
    float minValue;
    float maxValue;
    const char* unit;
    bool  isDisplayed;   // part of the sexually displayed trait vector
    bool  isPreference;  // a weight over displayed traits
};

const TraitSpec& traitSpec(Trait t);
const char* traitName(Trait t);

// ---------------------------------------------------------------------------
// Genes
// ---------------------------------------------------------------------------

enum class LocusType : uint8_t {
    Coding = 0,     // contributes additively to a trait
    Regulatory,     // multiplies the expression of every coding gene for a trait
    Junk,           // neutral: drifts freely, used for phylogeny and Fst
    Mhc,            // immune recognition; high diversity, heterozygote advantage
    SexDetermining, // the X/Y (or Z/W) locus itself
    Count
};

enum class Dominance : uint8_t {
    Additive = 0,       // (a+b)/2 -- incomplete dominance
    CompleteDominant,   // max(a,b)
    CompleteRecessive,  // min(a,b)
    Codominant,         // both alleles expressed; numerically the mean, but the
                        // heterozygote is flagged distinctly in the UI
    Overdominant,       // heterozygote advantage: mean + bonus * |a-b|
    Count
};

const char* locusTypeName(LocusType t);
const char* dominanceName(Dominance d);

enum GeneFlags : uint8_t {
    GeneFlag_None          = 0,
    GeneFlag_CanCarryLethal = 1 << 0,  // a broken allele here is recessive lethal
    GeneFlag_Sublethal      = 1 << 1,  // broken allele costs fitness instead of life
    GeneFlag_Inverted       = 1 << 2,  // segment inverted; suppresses local crossover
    GeneFlag_Hemizygous     = 1 << 3,  // only one copy present (a fresh duplicate
                                       // inherited from a single parent); alleleB
                                       // is meaningless and expression uses A alone
};

// An allele at or below this value is a broken (loss-of-function) copy. Normal
// alleles live in roughly [-3, 3], so the broken range is unreachable by point
// mutation alone -- it is entered only by the dedicated lethal mutation.
constexpr float kBrokenAllele = -9.0f;
inline bool alleleIsBroken(float v) { return v <= kBrokenAllele * 0.5f; }

// Exactly 24 bytes, no padding, so a genome is one contiguous cache-friendly
// block and copying one is a memcpy.
struct Gene {
    float    alleleA = 0.0f;   // maternal
    float    alleleB = 0.0f;   // paternal
    float    mapPos  = 0.0f;   // centiMorgans along its chromosome
    float    effect  = 1.0f;   // per-unit contribution to the target trait
    uint16_t id      = 0;      // homology identity; matched between parents
    uint16_t target  = 0;      // Trait index for Coding and Regulatory genes
    uint8_t  chromosome = 0;
    uint8_t  type    = 0;      // LocusType
    uint8_t  dominance = 0;    // Dominance
    uint8_t  flags   = 0;      // GeneFlags
};
static_assert(sizeof(Gene) == 24, "Gene must stay tightly packed");

// ---------------------------------------------------------------------------
// Genome: a slice of the shared arena, owned by one agent.
// ---------------------------------------------------------------------------

struct GenomeView {
    Gene*  genes = nullptr;
    uint16_t count = 0;
    uint16_t capacity = 0;

    Gene&       operator[](size_t i)       { return genes[i]; }
    const Gene& operator[](size_t i) const { return genes[i]; }
    bool full() const { return count >= capacity; }
};

struct ConstGenomeView {
    const Gene* genes = nullptr;
    uint16_t count = 0;
    uint16_t capacity = 0;

    ConstGenomeView() = default;
    // Implicit, so a non-const accessor's result can be passed anywhere a
    // read-only view is wanted without an explicit cast at every call site.
    ConstGenomeView(const GenomeView& v)
        : genes(v.genes), count(v.count), capacity(v.capacity) {}

    const Gene& operator[](size_t i) const { return genes[i]; }
};

// The expressed phenotype. Rebuilt at development and whenever an allele is
// edited from the UI, never per tick.
struct Phenotype {
    float traits[kTraitCount] = {0.0f};
    float mhcSignature[8] = {0.0f};   // the expressed immune profile
    float heterozygosity = 0.0f;      // fraction of loci with differing alleles
    float inbreedingF = 0.0f;         // 1 - Ho/He, genome-wide
    uint8_t lethalCarried = 0;        // recessive lethals carried but not expressed
    bool  lethalExpressed = false;    // homozygous broken at a lethal locus
    float sublethalPenalty = 0.0f;    // 0..1 fitness cost from expressed sublethals
    float ornamentCost = 0.0f;        // metabolic surcharge from display structures

    float operator[](Trait t) const { return traits[static_cast<int>(t)]; }
    float get(Trait t) const { return traits[static_cast<int>(t)]; }
};

// ---------------------------------------------------------------------------
// Chromosome recombination map
// ---------------------------------------------------------------------------

struct RecombinationMap {
    // Per chromosome: base crossover rate plus hotspots. Linkage emerges from
    // this rather than being asserted: genes close together on the same
    // chromosome rarely have a crossover between them, so they are inherited
    // together, and that is measurable as linkage disequilibrium.
    struct Hotspot { float position; float width; float intensity; };

    int   chromosomeCount = 8;
    float lengthCm = 100.0f;              // map length of each chromosome
    std::vector<float> baseRate;          // per chromosome, crossovers per morgan
    std::vector<std::vector<Hotspot>> hotspots;

    void build(int chromosomes, Rng& rng);
    // Number of crossovers and where, for one chromosome, drawn from the map.
    int  drawCrossovers(int chromosome, Rng& rng, float* positionsOut, int maxOut) const;
};

// ---------------------------------------------------------------------------
// Mutation rates. Each is per-gene per-meiosis unless noted.
// ---------------------------------------------------------------------------

struct MutationRates {
    float point = 0.004f;            // Gaussian nudge to one allele
    float pointSigma = 0.18f;
    float insertion = 0.0006f;       // a brand-new random gene appears
    float deletion = 0.0008f;        // a gene is lost
    float duplication = 0.0012f;     // a gene is copied; the copy can diverge
    float inversion = 0.0004f;       // a segment reverses; suppresses crossover
    float translocation = 0.0003f;   // a gene moves to another chromosome
    float chromosomeDup = 0.00002f;  // whole-chromosome duplication (per meiosis)
    float lethal = 0.00025f;         // an allele breaks outright
    float regulatoryShift = 0.002f;  // regulatory genes retarget

    void loadFromConfig();
};

// ---------------------------------------------------------------------------
// Genetics: the arena plus every operation on genomes.
// ---------------------------------------------------------------------------

class Genetics {
public:
    void configure(size_t maxAgents, uint16_t geneCapacity, Rng& rng);

    uint16_t geneCapacity() const { return m_geneCapacity; }
    size_t   slotCount() const { return m_slotCount; }
    const RecombinationMap& map() const { return m_map; }
    RecombinationMap& mapMutable() { return m_map; }
    const MutationRates& rates() const { return m_rates; }
    MutationRates& ratesMutable() { return m_rates; }

    GenomeView      genome(size_t slot);
    ConstGenomeView genome(size_t slot) const;
    void setGeneCount(size_t slot, uint16_t n) { m_count[slot] = n; }

    // Builds a founder genome from the canonical template: polygenic coding
    // genes for every trait, regulatory genes, junk, MHC and a sex locus.
    void makeFounder(size_t slot, Rng& rng, bool heterogameticSex);

    // Meiosis + fertilisation. Produces the child genome in `childSlot` from
    // the two parents, applying crossover, independent assortment and mutation.
    // `mutationRateMultiplier` comes from the PARENTS' MutationRateModifier
    // traits -- a child cannot govern the mutation rate of its own creation.
    // That is precisely what makes mutator alleles heritable and evolvability
    // itself an evolving property.
    void recombine(size_t childSlot, size_t motherSlot, size_t fatherSlot,
                   Rng& rng, float mutationRateMultiplier);

    void copyGenome(size_t dstSlot, size_t srcSlot);

    // Expression. Coding genes sum into their target trait; regulatory genes
    // multiply that sum; developmental noise is added last.
    void express(size_t slot, Rng& rng, float developmentalNoise, Phenotype& out) const;

    // Population-level statistics, recomputed on the telemetry cadence.
    // Per-trait variance decomposition, which is what heritability actually is.
    //
    // Vp is the variance of the expressed trait across the population. Vg is the
    // variance of the genotypic value -- the same expression with the
    // developmental noise term removed. Va is the ADDITIVE part of that: the
    // coding contribution with regulatory multipliers held at unity, because a
    // regulatory gene scaling a coding total is epistasis, not additive
    // variance, and lumping the two would overstate what selection can move.
    //
    // h2 = Va/Vp is narrow-sense: the fraction of variation that responds to
    // selection, and the number in the breeder's equation. H2 = Vg/Vp is
    // broad-sense: everything heritable including the non-additive part.
    struct TraitHeritability {
        double additiveVariance = 0.0;
        double genotypicVariance = 0.0;
        double phenotypicVariance = 0.0;
        double narrowH2 = 0.0;
        double broadH2 = 0.0;
        uint32_t codingLoci = 0;   // how many genes contribute; 1 is not polygenic
        // True when the additive variance EXCEEDS the phenotypic variance, which
        // is not a rounding artefact: it means the trait is pressed against one
        // end of its legal range, so genetic variation exists that the body
        // cannot express. Reporting a capped h2 of 1.000 without saying so would
        // hide the more interesting fact.
        bool rangeLimited = false;
    };

    struct PopulationGenetics {
        double observedHeterozygosity = 0.0;
        double expectedHeterozygosity = 0.0;
        double meanInbreedingF = 0.0;
        double alleleVariance = 0.0;
        double tajimasD = 0.0;
        double effectivePopulationSize = 0.0;
        double meanGenomeLength = 0.0;
        double linkageDisequilibrium = 0.0;
        uint32_t segregatingSites = 0;
        uint32_t fixedLoci = 0;
        uint32_t sampleSize = 0;
        TraitHeritability heritability[kTraitCount];
    };
    void computePopulationGenetics(const std::vector<uint32_t>& liveSlots,
                                   PopulationGenetics& out);

    // The variance decomposition above. Needs the expressed phenotypes as well
    // as the genomes, because Vp is a property of what the population actually
    // is and Va is a property of what it carries.
    void computeHeritability(const std::vector<uint32_t>& slots,
                             const std::vector<Phenotype>& phenotypes,
                             PopulationGenetics& out) const;

    // Genotypic and additive values for one individual, with no developmental
    // noise. Exposed because the Genome Browser shows them next to the expressed
    // value, which is the clearest way to see what noise and epistasis are doing.
    void genotypicValues(size_t slot, float* genotypicOut, float* additiveOut) const;

    // Wright's Fst between geographic subpopulations: (He_total - mean He_sub)
    // / He_total. Non-zero Fst across a mountain range is what speciation looks
    // like before it finishes.
    double computeFst(const std::vector<std::vector<uint32_t>>& subpopulations) const;

    // Alleles are continuous, so the classic population-genetic statistics
    // discretise them into bins of this width -- the natural resolution, since
    // it is the scale of a single point mutation.
    float alleleBinWidth() const { return m_binWidth; }
    double populationHe() const { return m_populationHe; }

    // Allele frequency of a gene id across the sampled population, used by the
    // allele-frequency overlay and the fixation event detector.
    float alleleMean(const std::vector<uint32_t>& liveSlots, uint16_t geneId) const;

    void serialize(BinaryWriter& w, const std::vector<uint32_t>& liveSlots) const;
    void deserializeSlot(BinaryReader& r, size_t slot);

    // The canonical template every founder is built from, kept so that the UI
    // can name genes and so new founders spawned later stay compatible.
    const std::vector<Gene>& templateGenes() const { return m_template; }
    uint16_t nextGeneId() const { return m_nextGeneId; }

private:
    void buildTemplate(Rng& rng);
    // Crossover-aware gamete construction for one parent.
    void makeGamete(std::vector<Gene>& out, ConstGenomeView parent, Rng& rng) const;
    void mutateGenome(size_t slot, Rng& rng, float rateMultiplier);

    std::vector<Gene>     m_arena;
    std::vector<uint16_t> m_count;
    std::vector<Gene>     m_template;
    RecombinationMap      m_map;
    MutationRates         m_rates;
    uint16_t              m_geneCapacity = 320;
    size_t                m_slotCount = 0;
    uint16_t              m_nextGeneId = 1;
    float                 m_binWidth = 0.25f;
    double                m_populationHe = 0.5;

    // Scratch for meiosis and statistics, owned here so the hot paths never
    // allocate. Mutable because expression and statistics are logically const.
    mutable std::vector<Gene>  m_gameteA, m_gameteB;
    mutable std::vector<int32_t> m_binScratch;
    mutable std::vector<float>   m_valueScratch;
};

// Combine two alleles under a dominance model.
float expressAllele(float a, float b, Dominance d);

}  // namespace gen
