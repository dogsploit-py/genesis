// sim/attraction.h — sex expression, orientation and relational attractiveness.
//
// The central claim this file has to make good on is that ATTRACTIVENESS IS NOT
// A NUMBER AN INDIVIDUAL HAS. It is a function of an ordered pair. B can be a
// 9/10 to A and a 2/10 to C, because A and C carry different evolved preference
// weights, have different MHC profiles to contrast against, different histories
// with B, and different imprints from whoever raised them.
//
//     attractiveness(A -> B) = dot(A.preferences, B.displayedTraits)
//                            + MHC dissimilarity term (weighted by A's evolved taste)
//                            + homophily term
//                            + familiarity + reputation + relationship history
//                            + A's imprint similarity to B
//
// Mate choice then requires MUTUAL acceptance: both agents' own internal drive
// and their own attraction score must clear their own selectivity threshold.
// Rejection is a normal outcome and is remembered.
#pragma once

#include <cstdint>

#include "sim/brain.h"
#include "sim/genetics.h"

namespace gen {

// The dimensions a potential partner is actually judged on. Kept as an explicit
// vector rather than reading traits directly, because some entries (health,
// energy, status, age) are CONDITION, not genotype -- and condition-dependence
// is what makes some of these signals honest.
enum class Display : uint8_t {
    Size = 0,
    Symmetry,
    ColourSaturation,
    Ornament,
    HealthMarker,
    EnergyReserve,
    Status,
    Age,
    Count
};
constexpr int kDisplayCount = static_cast<int>(Display::Count);

const char* displayName(Display d);
// The heritable preference weight that scores this display dimension.
Trait displayPreferenceTrait(Display d);

struct DisplayVector {
    float v[kDisplayCount] = {0.0f};
    float mhc[8] = {0.0f};
    float sexExpression = 0.5f;

    float operator[](Display d) const { return v[static_cast<int>(d)]; }
    float& operator[](Display d) { return v[static_cast<int>(d)]; }
};

// Everything about A's taste. Assembled from A's phenotype plus what A learned.
struct PreferenceVector {
    float weight[kDisplayCount] = {0.0f};
    float mhcDissimilarity = 0.0f;
    float homophily = 0.0f;
    float orientationMale = 0.5f;
    float orientationFemale = 0.5f;
    float selectivity = 0.5f;

    // Learned during development by imprinting on parents and early social
    // contacts. Not genetic, but it shifts adult choice for life.
    DisplayVector imprint;
    bool  hasImprint = false;
};

// Everything relational: what A specifically knows about B.
struct RelationshipContext {
    float familiarity = 0.0f;      // 0..1, repeated encounters
    float affinity = 0.0f;         // -1..1, remembered valence of past interactions
    float reputation = 0.0f;       // B's standing in the group generally
    float relatedness = 0.0f;      // pedigree coefficient of relatedness
    bool  bondedToEachOther = false;
    bool  previouslyRejected = false;
};

// A full breakdown, so the UI can show WHY the number is what it is rather than
// just asserting it.
struct AttractionBreakdown {
    float displayTerm = 0.0f;
    float mhcTerm = 0.0f;
    float homophilyTerm = 0.0f;
    float orientationGate = 1.0f;
    float familiarityTerm = 0.0f;
    float reputationTerm = 0.0f;
    float historyTerm = 0.0f;
    float imprintTerm = 0.0f;
    float inbreedingPenalty = 0.0f;
    float total = 0.0f;

    // Whether A would accept B given A's own threshold and current drive.
    float threshold = 0.0f;
    bool  wouldAccept = false;
};

// Builds the display vector from genotype AND condition.
void buildDisplayVector(const Phenotype& p, float health, float energyFraction,
                        float ageYears, float status, DisplayVector& out);

// Builds A's preference vector from A's phenotype (plus any learned imprint,
// which the caller supplies by filling `out.imprint` before or after).
void buildPreferenceVector(const Phenotype& p, PreferenceVector& out);

// The core relational computation. `observer` is A, `target` is B.
// `observerDrive` is A's current reproductive drive, which lowers A's effective
// threshold -- selectivity is real but it is not absolute.
float attractiveness(const PreferenceVector& observerPrefs,
                     const DisplayVector& observerDisplay,
                     const DisplayVector& targetDisplay,
                     const RelationshipContext& ctx,
                     float observerDrive,
                     AttractionBreakdown* breakdownOut);

// Chromosomal sex systems. Which one a world uses is a world option.
enum class SexSystem : uint8_t { XY = 0, ZW, TemperatureDependent, Count };
const char* sexSystemName(SexSystem s);

enum class ChromosomalSex : uint8_t { Homogametic = 0, Heterogametic, Undetermined };
const char* chromosomalSexName(SexSystem system, ChromosomalSex s);

// Reads the sex locus out of a genome. For temperature-dependent systems the
// caller passes the incubation temperature and the locus is ignored.
ChromosomalSex determineChromosomalSex(ConstGenomeView genome, SexSystem system,
                                       float incubationTemperature,
                                       float pivotTemperature);

// A textual label for a continuous sex expression value, for the UI. The
// underlying value is always the continuous one; this is presentation only.
const char* sexExpressionLabel(float expression);

// True when expression sits far enough from both poles to count as ambiguous.
bool isIntersex(float expression);

}  // namespace gen
