#include "sim/attraction.h"

#include <algorithm>
#include <cmath>

#include "core/config.h"

namespace gen {

const char* displayName(Display d) {
    switch (d) {
        case Display::Size:             return "Size";
        case Display::Symmetry:         return "Symmetry";
        case Display::ColourSaturation: return "Colour";
        case Display::Ornament:         return "Ornament";
        case Display::HealthMarker:     return "Health";
        case Display::EnergyReserve:    return "Condition";
        case Display::Status:           return "Status";
        case Display::Age:              return "Age";
        case Display::Count:            break;
    }
    return "?";
}

Trait displayPreferenceTrait(Display d) {
    switch (d) {
        case Display::Size:             return Trait::PrefSize;
        case Display::Symmetry:         return Trait::PrefSymmetry;
        case Display::ColourSaturation: return Trait::PrefColour;
        case Display::Ornament:         return Trait::PrefOrnament;
        case Display::HealthMarker:     return Trait::PrefHealth;
        case Display::EnergyReserve:    return Trait::PrefEnergy;
        case Display::Status:           return Trait::PrefStatus;
        case Display::Age:              return Trait::PrefAge;
        case Display::Count:            break;
    }
    return Trait::PrefSize;
}

const char* sexSystemName(SexSystem s) {
    switch (s) {
        case SexSystem::XY:                   return "XY (male heterogametic)";
        case SexSystem::ZW:                   return "ZW (female heterogametic)";
        case SexSystem::TemperatureDependent: return "Temperature-dependent";
        case SexSystem::Count:                break;
    }
    return "?";
}

const char* chromosomalSexName(SexSystem system, ChromosomalSex s) {
    if (s == ChromosomalSex::Undetermined) return "undetermined";
    switch (system) {
        case SexSystem::XY: return (s == ChromosomalSex::Heterogametic) ? "XY" : "XX";
        case SexSystem::ZW: return (s == ChromosomalSex::Heterogametic) ? "ZW" : "ZZ";
        case SexSystem::TemperatureDependent:
            return (s == ChromosomalSex::Heterogametic) ? "temp-high" : "temp-low";
        case SexSystem::Count: break;
    }
    return "?";
}

ChromosomalSex determineChromosomalSex(ConstGenomeView genome, SexSystem system,
                                       float incubationTemperature, float pivotTemperature) {
    if (system == SexSystem::TemperatureDependent) {
        // Real reptilian TSD: a narrow pivot band, outside which the outcome is
        // effectively determined by incubation temperature alone.
        return (incubationTemperature >= pivotTemperature) ? ChromosomalSex::Heterogametic
                                                           : ChromosomalSex::Homogametic;
    }
    for (uint16_t i = 0; i < genome.count; ++i) {
        if (static_cast<LocusType>(genome.genes[i].type) != LocusType::SexDetermining) continue;
        const Gene& g = genome.genes[i];
        const bool hemi = (g.flags & GeneFlag_Hemizygous) != 0;
        const float hi = hemi ? g.alleleA : std::max(g.alleleA, g.alleleB);
        return (hi > 0.5f) ? ChromosomalSex::Heterogametic : ChromosomalSex::Homogametic;
    }
    // A genome that lost its sex locus entirely -- possible after a deletion
    // near the locus. Reported honestly rather than defaulted silently.
    return ChromosomalSex::Undetermined;
}

const char* sexExpressionLabel(float e) {
    if (e < 0.20f) return "female-typical";
    if (e < 0.40f) return "female-leaning";
    if (e <= 0.60f) return "intermediate";
    if (e <= 0.80f) return "male-leaning";
    return "male-typical";
}

bool isIntersex(float e) {
    const float lo = cfg().getF("sex.intersex_low", 0.42f);
    const float hi = cfg().getF("sex.intersex_high", 0.58f);
    return e > lo && e < hi;
}

// ---------------------------------------------------------------------------

void buildDisplayVector(const Phenotype& p, float health, float energyFraction,
                        float ageYears, float status, DisplayVector& out) {
    // Genotype-driven dimensions, normalised to roughly 0..1 so preference
    // weights are comparable across dimensions.
    const TraitSpec& sizeSpec = traitSpec(Trait::Size);
    out[Display::Size] = (p.get(Trait::Size) - sizeSpec.minValue) /
                         (sizeSpec.maxValue - sizeSpec.minValue);
    out[Display::Symmetry] = p.get(Trait::Symmetry);

    const float r = p.get(Trait::ColourR), g = p.get(Trait::ColourG), b = p.get(Trait::ColourB);
    const float mx = std::max(r, std::max(g, b));
    const float mn = std::min(r, std::min(g, b));
    out[Display::ColourSaturation] = (mx > 1e-5f) ? (mx - mn) / mx : 0.0f;

    // Ornaments are what sexual selection can run away on, so they are
    // deliberately unbounded upward rather than normalised into 0..1.
    out[Display::Ornament] = 0.5f * (p.get(Trait::Ornament1) + p.get(Trait::Ornament2));

    // CONDITION-dependent dimensions. These are the honest signals: an animal
    // cannot display good condition it does not have, which is exactly why
    // preferences for them are not arbitrary.
    out[Display::HealthMarker] = std::min(1.0f, std::max(0.0f, health));
    out[Display::EnergyReserve] = std::min(1.0f, std::max(0.0f, energyFraction));
    out[Display::Status] = std::min(1.0f, std::max(0.0f, status));

    // Age enters as a hump rather than a ramp: very young and very old are both
    // poorer bets, so a positive PrefAge means "prime adult", not "ancient".
    const float prime = p.get(Trait::MaturityAge) * 1.8f;
    const float spread = std::max(2.0f, prime * 0.7f);
    const float z = (ageYears - prime) / spread;
    out[Display::Age] = std::exp(-z * z);

    for (int i = 0; i < 8; ++i) out.mhc[i] = p.mhcSignature[i];
    out.sexExpression = p.get(Trait::SexExpression);
}

void buildPreferenceVector(const Phenotype& p, PreferenceVector& out) {
    for (int i = 0; i < kDisplayCount; ++i)
        out.weight[i] = p.get(displayPreferenceTrait(static_cast<Display>(i)));
    out.mhcDissimilarity = p.get(Trait::PrefMhcDissimilarity);
    out.homophily = p.get(Trait::PrefHomophily);
    out.orientationMale = p.get(Trait::OrientationMale);
    out.orientationFemale = p.get(Trait::OrientationFemale);
    out.selectivity = p.get(Trait::PartnerSelectivity);
}

// ---------------------------------------------------------------------------

float attractiveness(const PreferenceVector& prefs,
                     const DisplayVector& observerDisplay,
                     const DisplayVector& targetDisplay,
                     const RelationshipContext& ctx,
                     float observerDrive,
                     AttractionBreakdown* out) {
    AttractionBreakdown b;

    // 1. The dot product of A's evolved weights with B's displayed traits.
    //    This is the term that lets the same individual be rated completely
    //    differently by two observers.
    for (int i = 0; i < kDisplayCount; ++i)
        b.displayTerm += prefs.weight[i] * targetDisplay.v[i];

    // 2. MHC. Distance between the two immune signatures, weighted by A's own
    //    evolved taste for dissimilarity. Offspring of dissimilar parents are
    //    heterozygous at the immune loci and resist a wider range of pathogens,
    //    so this preference pays -- but the simulation does not assert that,
    //    the disease system does.
    float mhcDist = 0.0f;
    for (int i = 0; i < 8; ++i) {
        const float d = observerDisplay.mhc[i] - targetDisplay.mhc[i];
        mhcDist += d * d;
    }
    mhcDist = std::sqrt(mhcDist) / 8.0f;
    b.mhcTerm = prefs.mhcDissimilarity * std::min(1.5f, mhcDist);

    // 3. Homophily on the non-MHC dimensions: like with like, or its opposite
    //    if the weight is negative.
    float similarity = 0.0f;
    for (int i = 0; i < kDisplayCount; ++i)
        similarity += std::fabs(observerDisplay.v[i] - targetDisplay.v[i]);
    similarity = 1.0f - std::min(1.0f, similarity / static_cast<float>(kDisplayCount));
    b.homophilyTerm = prefs.homophily * (similarity - 0.5f);

    // 4. Orientation. A multiplicative gate, not an additive term, and NOT a
    //    binary: the two orientation axes are independent, so any point in the
    //    space is representable -- exclusive attraction to either
    //    sex-expression pole, attraction to both, or to neither. It is
    //    heritable but noisy, and it does not have to be reproductively
    //    optimal. That is the point of an honest simulation.
    const float maleness = targetDisplay.sexExpression;
    const float femaleness = 1.0f - maleness;
    b.orientationGate = prefs.orientationMale * maleness + prefs.orientationFemale * femaleness;

    // 5. Familiarity, reputation and remembered history.
    b.familiarityTerm = cfg().getF("attraction.familiarity_weight", 0.45f) * ctx.familiarity;
    b.reputationTerm = cfg().getF("attraction.reputation_weight", 0.35f) * ctx.reputation;
    b.historyTerm = cfg().getF("attraction.history_weight", 0.80f) * ctx.affinity;
    if (ctx.bondedToEachOther)
        b.historyTerm += cfg().getF("attraction.bond_bonus", 1.20f);
    if (ctx.previouslyRejected)
        b.historyTerm -= cfg().getF("attraction.rejection_memory", 0.60f);

    // 6. Imprinting: sexual preference partly learned in early life from
    //    whoever was around. Shifts adult choice toward the remembered template
    //    without overriding the evolved weights.
    if (prefs.hasImprint) {
        float d = 0.0f;
        for (int i = 0; i < kDisplayCount; ++i)
            d += std::fabs(prefs.imprint.v[i] - targetDisplay.v[i]);
        d = 1.0f - std::min(1.0f, d / static_cast<float>(kDisplayCount));
        b.imprintTerm = cfg().getF("attraction.imprint_weight", 0.55f) * (d - 0.5f);
    }

    // 7. Inbreeding avoidance. Kin recognition is available through pedigree
    //    relatedness, MHC similarity and familiarity; agents that use it leave
    //    more surviving offspring, because inbreeding depression is real in
    //    this model rather than decorative.
    b.inbreedingPenalty = cfg().getF("attraction.kin_avoidance", 2.2f) *
                          std::max(0.0f, ctx.relatedness - 0.06f);

    const float raw = b.displayTerm + b.mhcTerm + b.homophilyTerm + b.familiarityTerm +
                      b.reputationTerm + b.historyTerm + b.imprintTerm - b.inbreedingPenalty;
    b.total = raw * std::max(0.0f, b.orientationGate);

    // Acceptance threshold. Selectivity raises it; accumulated reproductive
    // drive lowers it. Both are real: a picky individual is genuinely picky,
    // and a long-unmated one genuinely relaxes its standards.
    const float base = cfg().getF("attraction.base_threshold", 0.35f);
    const float driveRelief = cfg().getF("attraction.drive_relief", 0.75f);
    b.threshold = base + prefs.selectivity * cfg().getF("attraction.selectivity_scale", 1.1f)
                  - driveRelief * std::min(1.0f, observerDrive);
    b.wouldAccept = b.total >= b.threshold;

    if (out) *out = b;
    return b.total;
}

}  // namespace gen
