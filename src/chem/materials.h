// chem/materials.h — materials whose properties come from composition and
// process history, not from a tier table.
//
// The spec's test case: "Bronze is harder than copper because the alloy model
// says so, not because a table says 'bronze: tier 2'." That is exactly what
// happens here. Bronze is copper with tin dissolved in it; the tin atoms are a
// different size, they strain the lattice, and dislocations move less easily.
// The model computes that strengthening from the composition, so the hardness
// of 8% tin bronze is a RESULT, and 40% tin bronze is correctly brittle rubbish
// rather than "even better bronze".
//
// Steel is the same story with a second axis: how much carbon, and what you did
// with the heat. Quenched high-carbon steel is hard and brittle; tempering
// trades some of that hardness back for toughness; annealing gives it all up.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "chem/reactions.h"

namespace gen {

// What was done to a material, in order. History matters: quench-then-temper is
// not the same material as temper-then-quench, and neither is the same as cast.
enum class ProcessStep : uint8_t {
    Cast = 0,       // solidified from melt, coarse grain
    Forged,         // hot-worked, grain refined
    ColdWorked,     // work-hardened, stronger and more brittle
    Annealed,       // heated and slow-cooled, softened, ductility restored
    Quenched,       // cooled fast from above the transition, hard and brittle
    Tempered,       // reheated moderately after quenching, some hardness traded for toughness
    Carburised,     // carbon driven into the surface
    Ground,         // shaped and edged
    Count
};
const char* processStepName(ProcessStep s);
const char* processStepNote(ProcessStep s);

struct MaterialComponent {
    uint16_t substance = 0;   // Chemistry substance id
    double   massFraction = 0.0;
};

// Everything that falls out of composition and history.
struct MaterialProperties {
    double density = 0.0;                  // g/cm3
    double hardness = 0.0;                 // Mohs-like, 0..10
    double tensileStrength = 0.0;          // MPa
    double toughness = 0.0;                // 0..1, resistance to brittle fracture
    double meltingPoint = 0.0;             // K
    double thermalConductivity = 0.0;      // W/(m.K)
    double electricalConductivity = 0.0;   // MS/m
    double corrosionResistance = 0.0;      // 0..1
    std::string dominant;                  // the majority component's name
    std::string classification;            // "bronze", "steel", "wrought iron", ...
    std::string explanation;               // why it came out this way, for the UI
};

// What a tool made of a material is actually good for.
enum class ToolKind : uint8_t {
    Blade = 0, Axe, Hammer, Point, Container, Ornament, Count
};
const char* toolKindName(ToolKind k);

struct ToolGeometry {
    double edgeAngleDegrees = 30.0;   // sharper cuts better and chips sooner
    double massKg = 0.5;
    double lengthM = 0.4;
};

class Materials {
public:
    // Computes properties from composition and history. This is the whole
    // model: nothing is looked up by name.
    static MaterialProperties evaluate(const std::vector<MaterialComponent>& composition,
                                       const std::vector<ProcessStep>& history);

    // Effectiveness of a tool, 0..1, from the material and the geometry.
    // A heavy soft hammer head is good; a heavy soft blade is not.
    static double toolEffectiveness(const MaterialProperties& m, ToolKind kind,
                                    const ToolGeometry& g, std::string* whyOut = nullptr);

    // Convenience: build a two-component alloy by mass fraction.
    static std::vector<MaterialComponent> alloy(const char* formulaA, double fractionA,
                                                const char* formulaB);
};

}  // namespace gen
