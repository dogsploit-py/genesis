#include "chem/reactions.h"

#include "core/config.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/json.h"

namespace gen {

const char* phaseName(Phase p) {
    switch (p) {
        case Phase::Solid:   return "solid";
        case Phase::Liquid:  return "liquid";
        case Phase::Gas:     return "gas";
        case Phase::Aqueous: return "aqueous";
        case Phase::Plasma:  return "plasma";
        case Phase::Count:   break;
    }
    return "?";
}

const char* phaseSuffix(Phase p) {
    switch (p) {
        case Phase::Solid:   return "(s)";
        case Phase::Liquid:  return "(l)";
        case Phase::Gas:     return "(g)";
        case Phase::Aqueous: return "(aq)";
        case Phase::Plasma:  return "(plasma)";
        case Phase::Count:   break;
    }
    return "";
}

const char* reactionClassName(ReactionClass c) {
    switch (c) {
        case ReactionClass::Combustion:     return "Combustion";
        case ReactionClass::Calcination:    return "Calcination";
        case ReactionClass::Reduction:      return "Reduction (smelting)";
        case ReactionClass::Oxidation:      return "Oxidation / corrosion";
        case ReactionClass::AcidBase:       return "Acid-base";
        case ReactionClass::Fermentation:   return "Fermentation";
        case ReactionClass::Saponification: return "Saponification";
        case ReactionClass::Polymerisation: return "Polymerisation";
        case ReactionClass::Electrolysis:   return "Electrolysis";
        case ReactionClass::Alloying:       return "Alloying";
        case ReactionClass::Precipitation:  return "Precipitation";
        case ReactionClass::PhaseChange:    return "Phase change";
        case ReactionClass::Synthesis:      return "Synthesis";
        case ReactionClass::Cracking:       return "Cracking";
        case ReactionClass::Nuclear:        return "Nuclear";
        case ReactionClass::Other:          return "Other";
        case ReactionClass::Count:          break;
    }
    return "?";
}

namespace {
Phase phaseFromString(const std::string& s) {
    if (s == "s" || s == "solid")   return Phase::Solid;
    if (s == "l" || s == "liquid")  return Phase::Liquid;
    if (s == "g" || s == "gas")     return Phase::Gas;
    if (s == "aq" || s == "aqueous") return Phase::Aqueous;
    if (s == "plasma")              return Phase::Plasma;
    return Phase::Solid;
}

ReactionClass classFromString(const std::string& s) {
    for (int i = 0; i < static_cast<int>(ReactionClass::Count); ++i) {
        const ReactionClass c = static_cast<ReactionClass>(i);
        std::string n = reactionClassName(c);
        // Match the leading word, so "Reduction" matches "Reduction (smelting)".
        const size_t sp = n.find(' ');
        if (sp != std::string::npos) n = n.substr(0, sp);
        if (s == n) return c;
    }
    return ReactionClass::Other;
}
}  // namespace

// ---------------------------------------------------------------------------

Chemistry& Chemistry::instance() {
    static Chemistry c;
    return c;
}

const Substance* Chemistry::substance(uint16_t id) const {
    for (const Substance& s : m_substances)
        if (s.id == id) return &s;
    return nullptr;
}

const Substance* Chemistry::substanceByFormula(const std::string& formula, Phase phase) const {
    for (const Substance& s : m_substances)
        if (s.formulaText == formula && s.phase == phase) return &s;
    // Fall back to ignoring phase, so "Fe" finds solid iron.
    for (const Substance& s : m_substances)
        if (s.formulaText == formula) return &s;
    return nullptr;
}

const Substance* Chemistry::substanceByName(const std::string& name) const {
    for (const Substance& s : m_substances)
        if (s.name == name || s.commonName == name) return &s;
    return nullptr;
}

const Reaction* Chemistry::reaction(uint16_t id) const {
    for (const Reaction& r : m_reactions)
        if (r.id == id) return &r;
    return nullptr;
}

size_t Chemistry::unbalancedCount() const {
    size_t n = 0;
    for (const Reaction& r : m_reactions)
        if (!r.balanced) ++n;
    return n;
}

// ---------------------------------------------------------------------------
// The balance check. This is the assertion the spec demands.
// ---------------------------------------------------------------------------

bool Chemistry::checkBalance(Reaction& r, std::string& error) const {
    // A nuclear reaction transmutes elements, so checking elemental balance
    // would be checking the wrong law. What a nuclear reaction conserves is
    // NUCLEON NUMBER and CHARGE -- and not mass, because the mass defect is
    // precisely the energy released.
    if (r.cls == ReactionClass::Nuclear) {
        int lp = 0, ln = 0, rp = 0, rn = 0;
        auto nuc = [&](const std::vector<ReactionTerm>& side, int& prot, int& nucl) -> bool {
            for (const ReactionTerm& t : side) {
                const Substance* s = substance(t.substance);
                if (!s) { error = "unknown substance in nuclear reaction"; return false; }
                if (!s->nuclear) {
                    error = "reaction '" + r.name + "' mixes nuclide '" + s->name +
                            "' with an ordinary substance; nuclear reactions take nuclides only";
                    return false;
                }
                prot += static_cast<int>(s->protons * t.coefficient);
                nucl += static_cast<int>(s->nucleons * t.coefficient);
            }
            return true;
        };
        if (!nuc(r.reactants, lp, ln)) return false;
        if (!nuc(r.products, rp, rn)) return false;
        if (lp != rp || ln != rn) {
            char buf[220];
            std::snprintf(buf, sizeof(buf),
                          "reaction '%s' does not conserve nucleons or charge: "
                          "%d protons / %d nucleons in, %d protons / %d nucleons out",
                          r.name.c_str(), lp, ln, rp, rn);
            error = buf;
            r.balanced = false;
            r.balanceError = error;
            return false;
        }
        r.balanced = true;
        r.balanceError.clear();
        return true;
    }

    // Atoms of each element, and total charge, on both sides.
    double left[128] = {0.0}, right[128] = {0.0};
    double leftCharge = 0.0, rightCharge = 0.0;
    double leftMass = 0.0, rightMass = 0.0;

    auto accumulate = [&](const std::vector<ReactionTerm>& side, double* counts,
                          double& charge, double& mass) -> bool {
        for (const ReactionTerm& t : side) {
            const Substance* s = substance(t.substance);
            if (!s) {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "unknown substance id %u", t.substance);
                error = buf;
                return false;
            }
            for (const FormulaTerm& ft : s->formula.terms)
                counts[ft.z] += ft.count * t.coefficient;
            charge += s->formula.charge * t.coefficient;
            mass += s->molarMass() * t.coefficient;
        }
        return true;
    };

    if (!accumulate(r.reactants, left, leftCharge, leftMass)) return false;
    if (!accumulate(r.products, right, rightCharge, rightMass)) return false;

    // Per-element atom conservation. Reported element by element, because
    // "does not balance" is useless when you are fixing the data file.
    std::string detail;
    for (int z = 0; z < 128; ++z) {
        if (std::fabs(left[z] - right[z]) < 1e-6) continue;
        const Element* e = elements().byZ(static_cast<uint8_t>(z));
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s%s: %.4g on the left, %.4g on the right",
                      detail.empty() ? "" : "; ",
                      e ? e->symbol.c_str() : "?", left[z], right[z]);
        detail += buf;
    }
    if (std::fabs(leftCharge - rightCharge) > 1e-6) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%scharge: %+.4g on the left, %+.4g on the right",
                      detail.empty() ? "" : "; ", leftCharge, rightCharge);
        detail += buf;
    }

    if (!detail.empty()) {
        r.balanced = false;
        r.balanceError = detail;
        error = "reaction '" + r.name + "' does not balance -- " + detail;
        return false;
    }

    // Mass follows from atom counts, but check it anyway: a mismatch here after
    // the atom check passes would mean the element table itself is inconsistent.
    if (std::fabs(leftMass - rightMass) > 1e-3 * std::max(1.0, leftMass)) {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "reaction '%s' conserves atoms but not mass (%.4f vs %.4f g/mol) -- "
                      "the element table is inconsistent",
                      r.name.c_str(), leftMass, rightMass);
        error = buf;
        r.balanced = false;
        r.balanceError = error;
        return false;
    }

    r.balanced = true;
    r.balanceError.clear();
    return true;
}

std::string Chemistry::balanceReport(const Reaction& r) const {
    if (r.balanced) return "balanced";
    return r.balanceError.empty() ? "unbalanced" : r.balanceError;
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

ReactionResult Chemistry::evaluate(const Reaction& r, const ReactionConditions& c) const {
    ReactionResult out;
    const double T = std::max(1.0, c.temperature);

    // Standard enthalpy and entropy of reaction: products minus reactants.
    auto sum = [&](const std::vector<ReactionTerm>& side, double& h, double& s) {
        for (const ReactionTerm& t : side) {
            const Substance* sub = substance(t.substance);
            if (!sub) continue;
            h += sub->formationEnthalpy * t.coefficient;
            s += sub->standardEntropy * t.coefficient;
        }
    };
    double hL = 0.0, sL = 0.0, hR = 0.0, sR = 0.0;
    sum(r.reactants, hL, sL);
    sum(r.products, hR, sR);

    out.deltaH = hR - hL;               // kJ/mol
    out.deltaS = sR - sL;               // J/(mol.K)
    // dG = dH - T.dS, with dS converted from J to kJ.
    out.deltaGStandard = out.deltaH - T * out.deltaS / 1000.0;
    out.deltaG = out.deltaGStandard;

    // A nuclear reaction's energy comes from the mass defect, not from bond
    // rearrangement, so chemical formation enthalpies say nothing about it.
    // 1 MeV per event is 96.485 kJ per mole of events.
    if (r.cls == ReactionClass::Nuclear && r.energyMeV > 0.0) {
        out.deltaG = -r.energyMeV * 96.485;
        out.deltaGStandard = out.deltaG;
        out.deltaH = out.deltaG;
    }

    // LE CHATELIER. A reaction that consumes more gas than it makes is pushed
    // forward by pressure, and one that makes more gas is pushed back:
    //     dG(P) = dG0 + R.T.dn_gas.ln(P/P0)
    // This is not decoration -- it is the whole reason Haber-Bosch is run at
    // 150 atmospheres, and without it high pressure would only speed the
    // reaction up rather than shifting where it settles.
    {
        double gasIn = 0.0, gasOut = 0.0;
        for (const ReactionTerm& t : r.reactants) {
            const Substance* sub = substance(t.substance);
            if (sub && sub->phase == Phase::Gas) gasIn += t.coefficient;
        }
        for (const ReactionTerm& t : r.products) {
            const Substance* sub = substance(t.substance);
            if (sub && sub->phase == Phase::Gas) gasOut += t.coefficient;
        }
        out.gasMoleChange = gasOut - gasIn;
        if (std::fabs(out.gasMoleChange) > 1e-9 && c.pressure > 0.0) {
            const double shift = kGasConstant * T * out.gasMoleChange *
                                 std::log(c.pressure / kStandardPressure) / 1000.0;
            out.deltaG += shift;
        }
    }
    out.spontaneous = out.deltaG < 0.0;

    // Equilibrium constant from dG = -RT ln K. Clamped, because at low
    // temperature a strongly negative dG overflows the exponential long before
    // it means anything physical.
    const double exponent = -(out.deltaG * 1000.0) / (kGasConstant * T);
    out.equilibriumK = std::exp(std::min(300.0, std::max(-300.0, exponent)));

    // Kinetics. A catalyst lowers the activation energy without being consumed;
    // that is the whole of what a catalyst does here, and it is enough to make
    // Haber-Bosch depend on having one.
    out.effectiveEa = r.activationEnergy;
    if (c.catalystPresent && r.catalyst != 0 && r.catalysedActivationEnergy > 0.0)
        out.effectiveEa = r.catalysedActivationEnergy;

    const double k = r.preExponential *
                     std::exp(-(out.effectiveEa * 1000.0) / (kGasConstant * T));

    // Rate law: first order in the summed reactant concentration, and for
    // reactions that consume gas, proportional to pressure as well.
    double order = 0.0;
    bool consumesGas = false;
    for (const ReactionTerm& t : r.reactants) {
        order += t.coefficient;
        const Substance* sub = substance(t.substance);
        if (sub && sub->phase == Phase::Gas) consumesGas = true;
    }
    double drive = std::pow(std::max(0.0, c.concentration), std::min(3.0, order));
    if (consumesGas) drive *= c.pressure / kStandardPressure;
    // The divine rate knob. Applied to the rate and NOT to dG, because speeding
    // a reaction up must not make an uphill one spontaneous -- that would be
    // rewriting thermodynamics, not adjusting kinetics.
    out.rate = k * drive * cfg().getF("rules.reaction_rate_multiplier", 1.0f);

    // Conditions. Each failure is reported in plain language, because "nothing
    // happened" is the least useful thing a chemistry engine can say.
    out.conditionsMet = true;
    auto block = [&](const std::string& why) {
        if (out.blockedBy.empty()) out.blockedBy = why;
        out.conditionsMet = false;
    };

    char buf[192];
    if (r.minimumTemperature > 0.0 && T < r.minimumTemperature) {
        std::snprintf(buf, sizeof(buf), "needs %.0f K, only %.0f K available",
                      r.minimumTemperature, T);
        block(buf);
    }
    if (r.minimumPressure > 0.0 && c.pressure < r.minimumPressure) {
        std::snprintf(buf, sizeof(buf), "needs %.0f kPa, only %.0f kPa available",
                      r.minimumPressure / 1000.0, c.pressure / 1000.0);
        block(buf);
    }
    if (r.requiresElectricity && !c.electricity)
        block("needs an electric current");
    if (r.requiresIgnition && !c.ignition)
        block("needs a flame to start it");
    // An electrolytic reaction is uphill BY DEFINITION -- that is what the
    // current is for. Once electricity is supplied, free energy is no longer
    // the constraint, so it must not be reported as one.
    const bool energyFromOutside = r.requiresElectricity && c.electricity;
    if (!r.reversible && !out.spontaneous && !energyFromOutside) {
        std::snprintf(buf, sizeof(buf), "thermodynamically uphill here (dG = %+.1f kJ/mol)",
                      out.deltaG);
        block(buf);
    }
    // A reaction that is downhill but immeasurably slow has not "happened".
    // This is what stops iron oxide reducing at room temperature just because
    // the free energy allows it.
    if (out.conditionsMet && out.rate < 1e-12) {
        std::snprintf(buf, sizeof(buf),
                      "far too slow at %.0f K (Ea = %.0f kJ/mol)", T, out.effectiveEa);
        block(buf);
    }
    return out;
}

void Chemistry::findApplicable(const std::vector<uint16_t>& available,
                               const ReactionConditions& c,
                               std::vector<uint16_t>& out) const {
    out.clear();
    for (const Reaction& r : m_reactions) {
        if (!r.balanced) continue;

        // Cheap gates first. evaluate() builds a full result including an
        // explanatory string, and this loop runs for every agent that
        // experiments, so anything decidable by a comparison is decided here.
        if (r.minimumTemperature > 0.0 && c.temperature < r.minimumTemperature) continue;
        if (r.minimumPressure > 0.0 && c.pressure < r.minimumPressure) continue;
        if (r.requiresElectricity && !c.electricity) continue;
        if (r.requiresIgnition && !c.ignition) continue;

        // Every reactant must be present. Catalysts are checked separately,
        // because a catalyst is not consumed and need not be "used up".
        bool haveAll = true;
        for (const ReactionTerm& t : r.reactants) {
            if (std::find(available.begin(), available.end(), t.substance) == available.end()) {
                haveAll = false;
                break;
            }
        }
        if (!haveAll) continue;

        ReactionConditions cond = c;
        cond.catalystPresent = r.catalyst != 0 &&
            std::find(available.begin(), available.end(), r.catalyst) != available.end();

        const ReactionResult res = evaluate(r, cond);
        if (res.conditionsMet) out.push_back(r.id);
    }
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

void Chemistry::reindex() {
    for (size_t i = 0; i < m_substances.size(); ++i)
        if (m_substances[i].id == 0) m_substances[i].id = static_cast<uint16_t>(i + 1);
}

bool Chemistry::load(const std::string& path, std::string& error) {
    JsonValue root;
    if (!loadJsonFile(path, root, error)) return false;

    m_substances.clear();
    m_reactions.clear();

    const JsonValue& subs = root["substances"];
    if (!subs.isArray()) { error = path + ": missing 'substances' array"; return false; }

    for (size_t i = 0; i < subs.size(); ++i) {
        const JsonValue& j = subs[i];
        Substance s;
        s.id = static_cast<uint16_t>(m_substances.size() + 1);
        s.name = j["name"].asString();
        s.formulaText = j["formula"].asString();
        s.phase = phaseFromString(j["phase"].asString("s"));
        s.formationEnthalpy = j["dHf"].asNumber(0.0);
        s.standardEntropy = j["S"].asNumber(0.0);
        s.heatCapacity = j["Cp"].asNumber(0.0);
        s.meltingPoint = j["melt"].asNumber(0.0);
        s.boilingPoint = j["boil"].asNumber(0.0);
        s.density = j["density"].asNumber(0.0);
        s.solubility = j["solubility"].asNumber(0.0);
        s.commonName = j["common"].asString();
        s.note = j["note"].asString();

        if (s.name.empty() || s.formulaText.empty()) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s: substance %zu needs a name and a formula",
                          path.c_str(), i);
            error = buf;
            return false;
        }
        s.nuclear = j["nuclear"].asBool(false);
        s.protons = static_cast<int>(j["protons"].asNumber(0.0));
        s.nucleons = static_cast<int>(j["nucleons"].asNumber(0.0));
        if (s.nuclear) {
            // The formula string is a label ("U-235", "n"), not a composition.
            if (s.nucleons <= 0) {
                error = path + ": nuclide '" + s.name + "' needs a nucleon count";
                return false;
            }
            s.formula.valid = true;
            s.formula.molarMass = static_cast<double>(s.nucleons);
            m_substances.push_back(std::move(s));
            continue;
        }

        s.formula = elements().parse(s.formulaText);
        if (!s.formula.valid) {
            error = path + ": substance '" + s.name + "' has an unparsable formula '" +
                    s.formulaText + "' -- " + s.formula.error;
            return false;
        }
        m_substances.push_back(std::move(s));
    }

    const JsonValue& rxns = root["reactions"];
    if (!rxns.isArray()) { error = path + ": missing 'reactions' array"; return false; }

    for (size_t i = 0; i < rxns.size(); ++i) {
        const JsonValue& j = rxns[i];
        Reaction r;
        r.id = static_cast<uint16_t>(m_reactions.size() + 1);
        r.name = j["name"].asString();
        r.cls = classFromString(j["class"].asString("Other"));
        r.preExponential = j["A"].asNumber(1.0e6);
        r.activationEnergy = j["Ea"].asNumber(150.0);
        r.catalysedActivationEnergy = j["EaCatalysed"].asNumber(0.0);
        r.reversible = j["reversible"].asBool(false);
        r.minimumTemperature = j["minT"].asNumber(0.0);
        r.minimumPressure = j["minP"].asNumber(0.0);
        r.requiresElectricity = j["electricity"].asBool(false);
        r.requiresIgnition = j["ignition"].asBool(false);
        r.energyMeV = j["energyMeV"].asNumber(0.0);
        r.note = j["note"].asString();

        auto readSide = [&](const char* key, std::vector<ReactionTerm>& side) -> bool {
            const JsonValue& arr = j[key];
            if (!arr.isArray()) {
                error = path + ": reaction '" + r.name + "' has no '" + key + "' array";
                return false;
            }
            for (size_t k = 0; k < arr.size(); ++k) {
                const JsonValue& t = arr[k];
                const std::string formula = t["formula"].asString();
                const Phase phase = phaseFromString(t["phase"].asString("s"));
                const Substance* sub = substanceByFormula(formula, phase);
                if (!sub) {
                    error = path + ": reaction '" + r.name + "' refers to unknown substance '" +
                            formula + "'";
                    return false;
                }
                ReactionTerm term;
                term.substance = sub->id;
                term.coefficient = t["n"].asNumber(1.0);
                side.push_back(term);
            }
            return true;
        };
        if (!readSide("reactants", r.reactants)) return false;
        if (!readSide("products", r.products)) return false;

        const std::string catalyst = j["catalyst"].asString();
        if (!catalyst.empty()) {
            const Substance* sub = substanceByFormula(catalyst, Phase::Solid);
            if (!sub) {
                error = path + ": reaction '" + r.name + "' names an unknown catalyst '" +
                        catalyst + "'";
                return false;
            }
            r.catalyst = sub->id;
        }

        // THE assertion. An unbalanced equation fails the load outright.
        std::string balanceError;
        if (!checkBalance(r, balanceError)) {
            error = path + ": " + balanceError;
            return false;
        }
        m_reactions.push_back(std::move(r));
    }

    reindex();
    return true;
}

void Chemistry::loadBuiltin() {
    // A minimal fallback so a missing data file degrades to a working engine.
    // The JSON is the authoritative and complete version.
    m_substances.clear();
    m_reactions.clear();

    struct S { const char* name; const char* formula; Phase ph; double h, s, cp; const char* common; };
    static const S kSubs[] = {
        {"Carbon (graphite)", "C", Phase::Solid, 0.0, 5.74, 8.5, "charcoal"},
        {"Oxygen", "O2", Phase::Gas, 0.0, 205.15, 29.4, ""},
        {"Carbon dioxide", "CO2", Phase::Gas, -393.51, 213.79, 37.1, ""},
        {"Carbon monoxide", "CO", Phase::Gas, -110.53, 197.66, 29.1, ""},
        {"Iron", "Fe", Phase::Solid, 0.0, 27.28, 25.1, ""},
        {"Iron(III) oxide", "Fe2O3", Phase::Solid, -824.2, 87.4, 103.9, "hematite"},
    };
    for (const S& s : kSubs) {
        Substance sub;
        sub.id = static_cast<uint16_t>(m_substances.size() + 1);
        sub.name = s.name;
        sub.formulaText = s.formula;
        sub.phase = s.ph;
        sub.formationEnthalpy = s.h;
        sub.standardEntropy = s.s;
        sub.heatCapacity = s.cp;
        sub.commonName = s.common;
        sub.formula = elements().parse(s.formula);
        m_substances.push_back(std::move(sub));
    }

    Reaction r;
    r.id = 1;
    r.name = "Carbothermic reduction of hematite";
    r.cls = ReactionClass::Reduction;
    r.activationEnergy = 230.0;
    r.minimumTemperature = 1400.0;
    auto add = [&](std::vector<ReactionTerm>& side, const char* formula, double n) {
        const Substance* s = substanceByFormula(formula, Phase::Solid);
        if (!s) s = substanceByFormula(formula, Phase::Gas);
        if (!s) return;
        ReactionTerm t;
        t.substance = s->id;
        t.coefficient = n;
        side.push_back(t);
    };
    add(r.reactants, "Fe2O3", 1.0);
    add(r.reactants, "CO", 3.0);
    add(r.products, "Fe", 2.0);
    add(r.products, "CO2", 3.0);
    std::string err;
    checkBalance(r, err);
    m_reactions.push_back(std::move(r));
    reindex();
}

}  // namespace gen
