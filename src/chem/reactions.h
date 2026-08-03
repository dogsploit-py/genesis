// chem/reactions.h — substances and the reaction engine.
//
// The central rule from the spec: a reaction that does not balance MUST FAIL TO
// LOAD. Mass and charge conservation are checked per element at load time, and
// an unbalanced equation is reported with the exact discrepancy rather than
// quietly producing matter from nothing.
//
// Feasibility comes from dG = dH - T.dS, rates from Arrhenius
// k = A.exp(-Ea/RT), with catalysts lowering Ea. Nothing here consults a tech
// tree: whether an ore can be smelted is decided by whether the reduction is
// thermodynamically downhill at the temperature available and fast enough to
// matter, which is the whole point.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "chem/elements.h"

namespace gen {

// The gas constant, J/(mol.K). Used everywhere below.
constexpr double kGasConstant = 8.314462618;
constexpr double kStandardTemperature = 298.15;   // K
constexpr double kStandardPressure = 101325.0;    // Pa

enum class Phase : uint8_t { Solid = 0, Liquid, Gas, Aqueous, Plasma, Count };
const char* phaseName(Phase p);
const char* phaseSuffix(Phase p);   // "(s)", "(l)", "(g)", "(aq)"

// A chemical substance: an element in a given phase, or a compound.
struct Substance {
    uint16_t    id = 0;
    std::string name;         // "Iron(III) oxide"
    std::string formulaText;  // "Fe2O3"
    Formula     formula;
    Phase       phase = Phase::Solid;

    // Standard thermodynamic data. These are the real measured quantities that
    // decide what is possible; everything downstream is computed from them.
    double formationEnthalpy = 0.0;   // dHf, kJ/mol
    double standardEntropy = 0.0;     // S, J/(mol.K)
    double heatCapacity = 0.0;        // Cp, J/(mol.K)

    // Phase behaviour, so a substance can melt, boil and dissolve.
    double meltingPoint = 0.0;        // K, 0 = not applicable
    double boilingPoint = 0.0;        // K
    double density = 0.0;             // g/cm3
    double solubility = 0.0;          // g per 100 g water at 298 K, -1 = miscible

    // Nuclides. A nuclear reaction TRANSMUTES elements, so elemental balance
    // is simply the wrong conservation law for it; what is conserved is nucleon
    // number and charge. A substance flagged nuclear carries those explicitly
    // and its formula string is a label rather than a parsed composition.
    bool     nuclear = false;
    int      protons = 0;
    int      nucleons = 0;

    std::string commonName;           // "rust", "quicklime", "table salt"
    std::string note;                 // why it matters, shown in the lab UI

    double molarMass() const { return formula.molarMass; }
};

// One side of an equation.
struct ReactionTerm {
    uint16_t substance = 0;
    double   coefficient = 1.0;
};

enum class ReactionClass : uint8_t {
    Combustion = 0, Calcination, Reduction, Oxidation, AcidBase, Fermentation,
    Saponification, Polymerisation, Electrolysis, Alloying, Precipitation,
    PhaseChange, Synthesis, Cracking, Nuclear, Other, Count
};
const char* reactionClassName(ReactionClass c);

struct Reaction {
    uint16_t    id = 0;
    std::string name;
    ReactionClass cls = ReactionClass::Other;
    std::vector<ReactionTerm> reactants, products;

    // Kinetics. A is the pre-exponential factor (per second), Ea the activation
    // energy in kJ/mol. A catalyst substitutes a lower Ea without being consumed.
    double preExponential = 1.0e6;
    double activationEnergy = 150.0;     // kJ/mol
    double catalysedActivationEnergy = 0.0;
    uint16_t catalyst = 0;               // substance id, 0 = none

    bool reversible = false;
    // Conditions the reaction genuinely needs, checked before it will run.
    double minimumTemperature = 0.0;     // K, 0 = none
    double minimumPressure = 0.0;        // Pa
    bool   requiresElectricity = false;  // electrolysis
    bool   requiresIgnition = false;     // needs a flame, not just heat

    // Nuclear energy release per event, MeV. Chemical dG says nothing about a
    // nuclear reaction: the energy comes from the mass defect, not from bond
    // rearrangement, so it is stated explicitly.
    double energyMeV = 0.0;

    std::string note;                    // the real-world basis, shown in the UI

    // Filled in by the balance check at load.
    bool   balanced = false;
    std::string balanceError;
};

// The result of evaluating a reaction under specific conditions.
struct ReactionConditions {
    double temperature = kStandardTemperature;  // K
    double pressure = kStandardPressure;        // Pa
    bool   electricity = false;
    bool   ignition = false;
    bool   catalystPresent = false;
    double concentration = 1.0;    // effective molar concentration of reactants
};

struct ReactionResult {
    double deltaH = 0.0;        // kJ/mol of reaction
    double deltaS = 0.0;        // J/(mol.K)
    double deltaG = 0.0;        // kJ/mol
    double equilibriumK = 0.0;  // dimensionless
    double deltaGStandard = 0.0;  // dG at 1 bar, before the pressure shift
    double gasMoleChange = 0.0;   // moles of gas produced minus consumed
    double rate = 0.0;          // mol/s at unit concentration
    double effectiveEa = 0.0;   // kJ/mol actually used
    bool   spontaneous = false; // dG < 0
    bool   conditionsMet = false;
    std::string blockedBy;      // why it will not run, in plain language
};

class Chemistry {
public:
    static Chemistry& instance();

    // Loads substances and reactions. Returns false with a precise message if
    // anything fails to parse OR fails to balance.
    bool load(const std::string& jsonPath, std::string& error);
    void loadBuiltin();

    bool loaded() const { return !m_substances.empty(); }

    const std::vector<Substance>& substances() const { return m_substances; }
    const std::vector<Reaction>&  reactions() const { return m_reactions; }

    const Substance* substance(uint16_t id) const;
    const Substance* substanceByFormula(const std::string& formula, Phase phase) const;
    const Substance* substanceByName(const std::string& name) const;
    const Reaction*  reaction(uint16_t id) const;

    // Evaluates a reaction under conditions. This is the whole engine: it is
    // what decides whether an ore reduces, and nothing consults a recipe list.
    ReactionResult evaluate(const Reaction& r, const ReactionConditions& c) const;

    // Every reaction whose reactant set is a subset of what is available, and
    // which would actually proceed under the given conditions. This is what
    // makes discovery a search rather than a lookup: an agent mixes what it
    // has, and the engine says what happens.
    void findApplicable(const std::vector<uint16_t>& availableSubstances,
                        const ReactionConditions& c,
                        std::vector<uint16_t>& reactionIdsOut) const;

    // Balance report for the UI, recomputed on demand.
    std::string balanceReport(const Reaction& r) const;

    size_t unbalancedCount() const;

private:
    Chemistry() = default;
    bool checkBalance(Reaction& r, std::string& error) const;
    void reindex();

    std::vector<Substance> m_substances;
    std::vector<Reaction>  m_reactions;
};

inline Chemistry& chem() { return Chemistry::instance(); }

}  // namespace gen
