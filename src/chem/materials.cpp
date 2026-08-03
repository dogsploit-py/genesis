#include "chem/materials.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace gen {

const char* processStepName(ProcessStep s) {
    switch (s) {
        case ProcessStep::Cast:       return "Cast";
        case ProcessStep::Forged:     return "Forged";
        case ProcessStep::ColdWorked: return "Cold-worked";
        case ProcessStep::Annealed:   return "Annealed";
        case ProcessStep::Quenched:   return "Quenched";
        case ProcessStep::Tempered:   return "Tempered";
        case ProcessStep::Carburised: return "Carburised";
        case ProcessStep::Ground:     return "Ground";
        case ProcessStep::Count:      break;
    }
    return "?";
}

const char* processStepNote(ProcessStep s) {
    switch (s) {
        case ProcessStep::Cast:
            return "Solidified from the melt. Coarse grain, gas porosity, and weaker than the "
                   "same metal worked.";
        case ProcessStep::Forged:
            return "Hot-worked. Breaks up the cast structure and refines the grain, which raises "
                   "strength and toughness together.";
        case ProcessStep::ColdWorked:
            return "Worked below the recrystallisation temperature. Dislocations pile up, so it "
                   "gets harder and stronger -- and less ductile with every blow.";
        case ProcessStep::Annealed:
            return "Heated and cooled slowly. Recrystallises the grain, undoing work hardening: "
                   "soft and ductile again.";
        case ProcessStep::Quenched:
            return "Cooled too fast for the carbon to leave solution. In steel that traps a hard, "
                   "strained phase -- very hard, and brittle enough to shatter.";
        case ProcessStep::Tempered:
            return "Reheated moderately after quenching. Some of the trapped strain relaxes, "
                   "trading hardness back for toughness. This is the step that makes a usable "
                   "blade rather than a glass one.";
        case ProcessStep::Carburised:
            return "Carbon driven into the surface of low-carbon iron. A hard skin over a tough "
                   "core -- case hardening, and the oldest way to get steel behaviour without "
                   "steel throughout.";
        case ProcessStep::Ground:
            return "Shaped and edged. Does not change the material, only the geometry -- but the "
                   "geometry is half of what makes a tool work.";
        case ProcessStep::Count: break;
    }
    return "";
}

const char* toolKindName(ToolKind k) {
    switch (k) {
        case ToolKind::Blade:     return "Blade";
        case ToolKind::Axe:       return "Axe";
        case ToolKind::Hammer:    return "Hammer";
        case ToolKind::Point:     return "Point";
        case ToolKind::Container: return "Container";
        case ToolKind::Ornament:  return "Ornament";
        case ToolKind::Count:     break;
    }
    return "?";
}

namespace {

// Atomic-radius mismatch drives solid-solution strengthening: a solute atom of
// a different size strains the lattice around it, and dislocations have to push
// through that strain field. Radii in picometres.
double atomicRadius(uint8_t z) {
    switch (z) {
        case 6:  return 70;    // C -- interstitial, tiny, hence its outsized effect
        case 13: return 143;   // Al
        case 14: return 111;   // Si
        case 26: return 126;   // Fe
        case 28: return 124;   // Ni
        case 29: return 128;   // Cu
        case 30: return 134;   // Zn
        case 50: return 140;   // Sn
        case 82: return 175;   // Pb
        case 24: return 128;   // Cr
        case 25: return 127;   // Mn
        case 47: return 144;   // Ag
        case 79: return 144;   // Au
        case 74: return 139;   // W
        default: return 130;
    }
}

// Does this solute sit BETWEEN the host atoms rather than replacing one?
// Carbon in iron does, and that is why a fraction of a percent of it transforms
// the metal while several percent of tin in copper is merely helpful.
bool isInterstitial(uint8_t z) { return z == 6 || z == 7 || z == 1; }

struct Blend {
    double density = 0.0, hardness = 0.0, melting = 0.0;
    double thermal = 0.0, electrical = 0.0;
    uint8_t dominantZ = 0;
    double dominantFraction = 0.0;
    double carbonFraction = 0.0;
    double tinFraction = 0.0;
    double zincFraction = 0.0;
    double chromiumFraction = 0.0;
    double moleMismatch = 0.0;   // size-weighted solute mismatch
};

}  // namespace

MaterialProperties Materials::evaluate(const std::vector<MaterialComponent>& composition,
                                       const std::vector<ProcessStep>& history) {
    MaterialProperties out;
    if (composition.empty()) {
        out.explanation = "no composition given";
        return out;
    }

    // Normalise the mass fractions, so a caller need not get them exactly right.
    double total = 0.0;
    for (const MaterialComponent& c : composition) total += c.massFraction;
    if (total <= 0.0) { out.explanation = "composition sums to zero"; return out; }

    // Reduce the composition to elemental mass fractions. A material made of
    // compounds is still made of elements, and it is the elements that set the
    // lattice behaviour.
    double elementMass[128] = {0.0};
    for (const MaterialComponent& c : composition) {
        const Substance* s = chem().substance(c.substance);
        if (!s || s->molarMass() <= 0.0) continue;
        const double frac = c.massFraction / total;
        for (const FormulaTerm& t : s->formula.terms) {
            const Element* e = elements().byZ(t.z);
            if (!e) continue;
            elementMass[t.z] += frac * (e->atomicMass * t.count) / s->molarMass();
        }
    }

    Blend b;
    double massSum = 0.0;
    for (int z = 0; z < 128; ++z) {
        if (elementMass[z] <= 0.0) continue;
        const Element* e = elements().byZ(static_cast<uint8_t>(z));
        if (!e) continue;
        const double f = elementMass[z];
        massSum += f;
        b.density    += f * e->density;
        b.hardness   += f * e->hardness;
        b.melting    += f * e->meltingPoint;
        b.thermal    += f * e->thermalConductivity;
        b.electrical += f * e->electricalConductivity;
        if (f > b.dominantFraction) { b.dominantFraction = f; b.dominantZ = static_cast<uint8_t>(z); }
        if (z == 6)  b.carbonFraction = f;
        if (z == 50) b.tinFraction = f;
        if (z == 30) b.zincFraction = f;
        if (z == 24) b.chromiumFraction = f;
    }
    if (massSum <= 0.0) { out.explanation = "composition contains no known elements"; return out; }

    const Element* host = elements().byZ(b.dominantZ);
    out.dominant = host ? host->name : "unknown";

    // ---- solid-solution strengthening -------------------------------------
    // Hardening scales with the SQUARE ROOT of solute concentration and with
    // the square of the size mismatch. That is the standard Fleischer form, and
    // it is why a few percent of tin does so much and why more stops helping.
    const double hostRadius = atomicRadius(b.dominantZ);
    double solutionHardening = 0.0;
    double interstitialHardening = 0.0;
    for (int z = 0; z < 128; ++z) {
        if (elementMass[z] <= 0.0 || z == b.dominantZ) continue;
        const double frac = elementMass[z];
        const double mismatch = std::fabs(atomicRadius(static_cast<uint8_t>(z)) - hostRadius) /
                                std::max(1.0, hostRadius);
        const double term = mismatch * mismatch * std::sqrt(frac);
        if (isInterstitial(static_cast<uint8_t>(z))) interstitialHardening += term * 90.0;
        else solutionHardening += term * 26.0;
        b.moleMismatch += mismatch * frac;
    }

    // ---- process history ---------------------------------------------------
    double workHardening = 0.0;
    double grainRefinement = 0.0;
    double quenchHardening = 0.0;
    double toughnessFactor = 0.55;   // as-cast
    bool quenched = false, tempered = false, annealed = false;

    for (ProcessStep s : history) {
        switch (s) {
            case ProcessStep::Cast:
                grainRefinement = 0.0;
                toughnessFactor = 0.55;
                break;
            case ProcessStep::Forged:
                grainRefinement = 0.35;
                toughnessFactor = std::min(1.0, toughnessFactor + 0.25);
                break;
            case ProcessStep::ColdWorked:
                // Each pass hardens less than the last and costs ductility.
                workHardening = std::min(1.2, workHardening + 0.45);
                toughnessFactor = std::max(0.1, toughnessFactor - 0.18);
                break;
            case ProcessStep::Annealed:
                workHardening = 0.0;
                quenchHardening = 0.0;
                quenched = false;
                annealed = true;
                toughnessFactor = std::min(1.0, toughnessFactor + 0.30);
                break;
            case ProcessStep::Quenched:
                // Only means anything if there is carbon to trap. Quenching pure
                // iron does almost nothing, which is exactly why the discovery
                // of steel required carburising first.
                quenchHardening = 14.0 * std::min(0.02, b.carbonFraction);
                quenched = true;
                tempered = false;
                toughnessFactor = std::max(0.05, toughnessFactor - 0.40);
                break;
            case ProcessStep::Tempered:
                if (quenched) {
                    quenchHardening *= 0.65;
                    toughnessFactor = std::min(1.0, toughnessFactor + 0.45);
                    tempered = true;
                }
                break;
            case ProcessStep::Carburised:
                b.carbonFraction = std::min(0.012, b.carbonFraction + 0.006);
                break;
            case ProcessStep::Ground:
            case ProcessStep::Count:
                break;
        }
    }

    out.density = b.density / massSum;
    out.meltingPoint = b.melting / massSum;
    out.hardness = b.hardness / massSum + solutionHardening + interstitialHardening +
                   workHardening + quenchHardening + grainRefinement;
    out.hardness = std::min(10.0, std::max(0.1, out.hardness));
    out.toughness = std::min(1.0, std::max(0.02, toughnessFactor * (1.0 - 0.35 * quenchHardening)));

    // Tensile strength tracks hardness closely in metals -- the old rule of
    // thumb is about 3.5 x Vickers, and Mohs maps onto that well enough here.
    out.tensileStrength = 45.0 * out.hardness * out.hardness;

    // Alloying wrecks conductivity out of all proportion to its amount: solute
    // atoms scatter electrons and phonons alike. This is why bronze is a much
    // worse conductor than copper, and why pure copper is used for wire.
    const double impurity = std::min(0.6, 1.0 - b.dominantFraction);
    const double scatter = 1.0 / (1.0 + 14.0 * impurity);
    out.thermalConductivity = (b.thermal / massSum) * scatter;
    out.electricalConductivity = (b.electrical / massSum) * scatter;

    // Corrosion resistance: noble hosts resist, chromium passivates.
    const double nobility = host ? std::min(1.0, host->electronegativity / 2.6) : 0.4;
    out.corrosionResistance = std::min(1.0,
        nobility * 0.7 + std::min(0.13, b.chromiumFraction) * 4.0);

    // ---- classification, which is a LABEL for the computed result ----------
    char buf[512];
    if (b.dominantZ == 29 && b.tinFraction > 0.02) {
        out.classification = (b.tinFraction > 0.20) ? "brittle high-tin bronze" : "bronze";
        std::snprintf(buf, sizeof(buf),
            "Copper with %.1f%% tin. The tin atoms are about 9%% larger than copper's and strain "
            "the lattice, so dislocations move less easily: hardness rises from %.1f to %.1f. "
            "Past roughly 20%% tin the strain forms a separate brittle phase and the alloy is "
            "worse than either parent.",
            b.tinFraction * 100.0, host ? host->hardness : 3.0, out.hardness);
        out.explanation = buf;
    } else if (b.dominantZ == 29 && b.zincFraction > 0.05) {
        out.classification = "brass";
        std::snprintf(buf, sizeof(buf),
            "Copper with %.1f%% zinc. Similar strengthening to bronze but from a smaller size "
            "mismatch, so it is softer -- and zinc is far commoner than tin.", b.zincFraction * 100.0);
        out.explanation = buf;
    } else if (b.dominantZ == 26) {
        const double c = b.carbonFraction;
        if (c < 0.0008) out.classification = "wrought iron";
        else if (c < 0.006) out.classification = quenched && !tempered ? "hardened steel"
                                               : tempered ? "tempered steel" : "steel";
        else if (c < 0.021) out.classification = quenched && !tempered ? "brittle high-carbon steel"
                                                                      : "high-carbon steel";
        else out.classification = "cast iron";
        std::snprintf(buf, sizeof(buf),
            "Iron with %.3f%% carbon. Carbon is interstitial -- it sits between the iron atoms "
            "rather than replacing them -- which is why a fraction of a percent does what several "
            "percent of tin does in copper. %s Hardness %.1f, toughness %.2f.",
            c * 100.0,
            quenched && !tempered
                ? "Quenched and not tempered: the carbon is trapped in a strained lattice, so it "
                  "is very hard and will shatter."
                : tempered
                ? "Quenched then tempered: some strain relaxed, trading hardness for toughness."
                : annealed ? "Annealed: soft and ductile."
                : "As-worked.",
            out.hardness, out.toughness);
        out.explanation = buf;
    } else {
        out.classification = out.dominant;
        std::snprintf(buf, sizeof(buf),
            "%.0f%% %s. Hardness %.1f from the base metal plus %.1f of solution strengthening and "
            "%.1f of work hardening.",
            b.dominantFraction * 100.0, out.dominant.c_str(),
            b.hardness / massSum, solutionHardening + interstitialHardening, workHardening);
        out.explanation = buf;
    }
    return out;
}

double Materials::toolEffectiveness(const MaterialProperties& m, ToolKind kind,
                                    const ToolGeometry& g, std::string* whyOut) {
    // Every tool wants a different mix. A hammer wants mass and toughness and
    // does not care about an edge; a blade wants hardness and a fine angle and
    // will shatter if it has no toughness at all.
    double score = 0.0;
    std::string why;
    char buf[320];

    const double hard = m.hardness / 10.0;
    const double tough = m.toughness;
    const double heft = std::min(1.0, g.massKg / 2.0);
    // A sharper edge cuts better but chips sooner, and how much sooner depends
    // on how tough the material is.
    const double sharpness = std::min(1.0, 40.0 / std::max(5.0, g.edgeAngleDegrees));
    const double chipRisk = std::max(0.0, sharpness - tough);

    switch (kind) {
        case ToolKind::Blade:
            score = 0.55 * hard + 0.25 * sharpness + 0.20 * tough - 0.6 * chipRisk;
            std::snprintf(buf, sizeof(buf),
                "A blade is mostly hardness (holds an edge) and geometry (%.0f degrees). "
                "Toughness stops it chipping: here toughness %.2f against sharpness %.2f gives "
                "a chip risk of %.2f.", g.edgeAngleDegrees, tough, sharpness, chipRisk);
            why = buf;
            break;
        case ToolKind::Axe:
            score = 0.35 * hard + 0.30 * tough + 0.30 * heft + 0.05 * sharpness - 0.4 * chipRisk;
            why = "An axe needs mass to carry momentum and toughness to survive the impact; "
                  "edge hardness matters less than for a blade.";
            break;
        case ToolKind::Hammer:
            score = 0.20 * hard + 0.35 * tough + 0.45 * heft;
            why = "A hammer is momentum and survival. A hard brittle head cracks; a soft heavy "
                  "one works fine.";
            break;
        case ToolKind::Point:
            score = 0.60 * hard + 0.25 * sharpness + 0.15 * tough - 0.5 * chipRisk;
            why = "A point concentrates force on a tiny area, so hardness dominates.";
            break;
        case ToolKind::Container:
            score = 0.20 * tough + 0.35 * (1.0 - hard) + 0.45 * m.corrosionResistance;
            why = "A container wants to be formable rather than hard, and above all not to "
                  "corrode into whatever it holds.";
            break;
        case ToolKind::Ornament:
            score = 0.55 * m.corrosionResistance + 0.25 * (1.0 - hard) + 0.20 * heft;
            why = "An ornament wants to stay bright and be workable. That is why gold, which is "
                  "hopeless as a tool, was worked first.";
            break;
        case ToolKind::Count:
            break;
    }

    // A material that melts below a working fire cannot be a tool at all.
    if (m.meltingPoint > 0.0 && m.meltingPoint < 500.0) {
        score *= 0.2;
        why += " It also melts far too easily to hold a shape in use.";
    }

    score = std::min(1.0, std::max(0.0, score));
    if (whyOut) *whyOut = why;
    return score;
}

std::vector<MaterialComponent> Materials::alloy(const char* formulaA, double fractionA,
                                                const char* formulaB) {
    std::vector<MaterialComponent> out;
    const Substance* a = chem().substanceByFormula(formulaA, Phase::Solid);
    const Substance* b = chem().substanceByFormula(formulaB, Phase::Solid);
    if (a) { MaterialComponent c; c.substance = a->id; c.massFraction = fractionA; out.push_back(c); }
    if (b) { MaterialComponent c; c.substance = b->id; c.massFraction = 1.0 - fractionA; out.push_back(c); }
    return out;
}

}  // namespace gen
