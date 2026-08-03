// ui/chem_ui.cpp — the Chemistry Lab, the Materials workbench and the
// Knowledge & Culture panel.
//
// The spec's rule is that if a piece of information exists in the simulation
// there is a place in the UI where it can be seen. Chemistry is the largest
// body of such information in the program, so all of it is here: every element
// with its real measured properties, every substance with its thermodynamic
// data, every reaction with its balance report, and a runner that evaluates any
// reaction under conditions you choose so you can watch dG cross zero.
//
// The runner is not a demo. It calls exactly the same Chemistry::evaluate() the
// agents call, so what it shows is what they experience. If the lab says
// hematite will not reduce at 900 K, then no agent in the world will reduce
// hematite at 900 K either.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "chem/materials.h"
#include "chem/reactions.h"
#include "core/config.h"
#include "imgui.h"
#include "sim/knowledge.h"
#include "ui/app.h"

namespace gen {

namespace {

bool matchFilter(const char* filter, const std::string& a, const std::string& b = std::string(),
                 const std::string& c = std::string()) {
    if (!filter || filter[0] == '\0') return true;
    std::string needle(filter);
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    auto has = [&](const std::string& hay) {
        std::string low(hay);
        std::transform(low.begin(), low.end(), low.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return low.find(needle) != std::string::npos;
    };
    return has(a) || has(b) || has(c);
}

// dG in kJ/mol, coloured by what it means: downhill green, uphill red.
void deltaGText(double dG) {
    const ImVec4 col = dG < 0.0 ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f)
                                : ImVec4(0.90f, 0.45f, 0.40f, 1.0f);
    ImGui::TextColored(col, "%+.1f kJ/mol", dG);
}

std::string equationText(const Reaction& r) {
    std::string s;
    auto side = [&](const std::vector<ReactionTerm>& terms) {
        for (size_t i = 0; i < terms.size(); ++i) {
            if (i) s += " + ";
            char buf[64];
            if (std::fabs(terms[i].coefficient - 1.0) > 1e-9) {
                std::snprintf(buf, sizeof buf, "%g ", terms[i].coefficient);
                s += buf;
            }
            const Substance* sub = chem().substance(terms[i].substance);
            s += sub ? sub->formulaText : std::string("?");
            if (sub) s += phaseSuffix(sub->phase);
        }
    };
    side(r.reactants);
    s += r.reversible ? "  <=>  " : "  ->  ";
    side(r.products);
    return s;
}

// ---------------------------------------------------------------------------
// Tab: elements
// ---------------------------------------------------------------------------

void drawElementsTab(UiState& ui) {
    const ElementTable& tab = ElementTable::instance();
    ImGui::TextDisabled(
        "%zu elements. These are real measured values, loaded from data/elements.csv. "
        "They are not decoration: melting point decides what a fire can smelt, "
        "electronegativity decides which reductions are plausible, and hardness feeds "
        "straight into how good a tool is.", tab.count());
    ImGui::SetNextItemWidth(240.0f);
    ImGui::InputTextWithHint("##elemfilter", "filter by symbol, name or category",
                             ui.chemElementFilter, sizeof ui.chemElementFilter);
    ImGui::Separator();

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("elements", 11, flags, ImVec2(0, -1))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Z", ImGuiTableColumnFlags_WidthFixed, 32.0f);
        ImGui::TableSetupColumn("Sym", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Mass");
        ImGui::TableSetupColumn("EN");
        ImGui::TableSetupColumn("Ox. states");
        ImGui::TableSetupColumn("rho");
        ImGui::TableSetupColumn("Melt K");
        ImGui::TableSetupColumn("Boil K");
        ImGui::TableSetupColumn("Mohs");
        ImGui::TableSetupColumn("Category");
        ImGui::TableHeadersRow();

        for (const Element& e : tab.elements()) {
            if (!matchFilter(ui.chemElementFilter, e.symbol, e.name, e.category)) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%u", static_cast<unsigned>(e.z));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.symbol.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.name.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.3f", e.atomicMass);
            ImGui::TableNextColumn();
            if (e.electronegativity > 0.0) ImGui::Text("%.2f", e.electronegativity);
            else ImGui::TextDisabled("--");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.oxidationStates.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.2f", e.density);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", e.meltingPoint);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", e.boilingPoint);
            ImGui::TableNextColumn();
            if (e.hardness > 0.0) ImGui::Text("%.1f", e.hardness);
            else ImGui::TextDisabled("--");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.category.c_str());
        }
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------------------
// Tab: substances
// ---------------------------------------------------------------------------

void drawSubstancesTab(UiState& ui) {
    ImGui::TextDisabled(
        "%zu substances. dHf and S are the standard formation enthalpy and entropy -- the two "
        "numbers every feasibility question in this world ultimately reduces to.",
        chem().substances().size());
    ImGui::SetNextItemWidth(240.0f);
    ImGui::InputTextWithHint("##subfilter", "filter by name, formula or common name",
                             ui.chemSubstanceFilter, sizeof ui.chemSubstanceFilter);
    ImGui::Separator();

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("substances", 9, flags, ImVec2(0, -1))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Formula", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Also known as");
        ImGui::TableSetupColumn("Phase", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("M g/mol");
        ImGui::TableSetupColumn("dHf kJ/mol");
        ImGui::TableSetupColumn("S J/mol.K");
        ImGui::TableSetupColumn("Melt K");
        ImGui::TableSetupColumn("rho");
        ImGui::TableHeadersRow();

        for (const Substance& s : chem().substances()) {
            if (!matchFilter(ui.chemSubstanceFilter, s.name, s.formulaText, s.commonName))
                continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted((s.formulaText + phaseSuffix(s.phase)).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.name.c_str());
            if (!s.note.empty() && ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(420.0f);
                ImGui::TextUnformatted(s.note.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            ImGui::TableNextColumn();
            if (!s.commonName.empty()) ImGui::TextUnformatted(s.commonName.c_str());
            else ImGui::TextDisabled("--");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(phaseName(s.phase));
            ImGui::TableNextColumn();
            if (s.nuclear) ImGui::TextDisabled("%d/%d", s.protons, s.nucleons);
            else ImGui::Text("%.2f", s.molarMass());
            ImGui::TableNextColumn(); ImGui::Text("%+.1f", s.formationEnthalpy);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", s.standardEntropy);
            ImGui::TableNextColumn();
            if (s.meltingPoint > 0.0) ImGui::Text("%.0f", s.meltingPoint);
            else ImGui::TextDisabled("--");
            ImGui::TableNextColumn();
            if (s.density > 0.0) ImGui::Text("%.2f", s.density);
            else ImGui::TextDisabled("--");
        }
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------------------
// Tab: reactions, with the runner
// ---------------------------------------------------------------------------

void drawReactionRunner(UiState& ui) {
    const Reaction* r = chem().reaction(static_cast<uint16_t>(ui.chemSelectedReaction));
    if (!r) {
        ImGui::TextDisabled("Select a reaction on the left.");
        return;
    }

    ImGui::TextUnformatted(r->name.c_str());
    ImGui::TextDisabled("%s", reactionClassName(r->cls));
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.85f, 0.60f, 1.0f));
    ImGui::TextWrapped("%s", equationText(*r).c_str());
    ImGui::PopStyleColor();

    if (!r->note.empty()) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", r->note.c_str());
        ImGui::PopTextWrapPos();
    }

    ImGui::Spacing();
    if (r->balanced) {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "Balanced.");
    } else {
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.30f, 1.0f), "UNBALANCED: %s",
                           r->balanceError.c_str());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Balance report")) ImGui::OpenPopup("balreport");
    if (ImGui::BeginPopup("balreport")) {
        ImGui::TextUnformatted(chem().balanceReport(*r).c_str());
        ImGui::EndPopup();
    }

    ImGui::SeparatorText("Conditions");

    ImGui::SetNextItemWidth(-160.0f);
    ImGui::SliderFloat("Temperature (K)", &ui.chemTemperature, 200.0f, 3000.0f, "%.0f");
    helpMarker("dG = dH - T.dS. Raising T makes any entropy-increasing reaction more "
               "favourable, which is precisely why carbothermic reduction needs a furnace: "
               "the -T.dS term has to grow enough to overcome a positive dH.");

    ImGui::SetNextItemWidth(-160.0f);
    ImGui::SliderFloat("Pressure (atm)", &ui.chemPressureAtm, 0.01f, 1000.0f, "%.2f",
                       ImGuiSliderFlags_Logarithmic);
    helpMarker("Le Chatelier. Pressure only matters where the number of gas molecules "
               "changes: dG(P) = dG* + R.T.dn_gas.ln(P/P*). Haber-Bosch goes from four gas "
               "molecules to two, so squeezing it helps.");

    ImGui::SetNextItemWidth(-160.0f);
    ImGui::SliderFloat("Concentration (mol/L)", &ui.chemConcentration, 0.001f, 100.0f, "%.3f",
                       ImGuiSliderFlags_Logarithmic);
    helpMarker("Enters the rate, not the feasibility. Grinding a solid raises the effective "
               "concentration by exposing surface, which is why grinding is a real technique.");

    ImGui::Checkbox("Catalyst present", &ui.chemCatalyst);
    if (r->catalyst != 0) {
        ImGui::SameLine();
        const Substance* cat = chem().substance(r->catalyst);
        ImGui::TextDisabled("(%s, Ea %.0f -> %.0f kJ/mol)", cat ? cat->formulaText.c_str() : "?",
                            r->activationEnergy, r->catalysedActivationEnergy);
    } else {
        ImGui::SameLine();
        ImGui::TextDisabled("(no catalyst known for this reaction)");
    }
    ImGui::Checkbox("Electric current supplied", &ui.chemElectricity);
    ImGui::SameLine();
    ImGui::Checkbox("Ignition source", &ui.chemIgnition);

    ReactionConditions c;
    c.temperature = static_cast<double>(ui.chemTemperature);
    c.pressure = static_cast<double>(ui.chemPressureAtm) * kStandardPressure;
    c.concentration = static_cast<double>(ui.chemConcentration);
    c.catalystPresent = ui.chemCatalyst;
    c.electricity = ui.chemElectricity;
    c.ignition = ui.chemIgnition;

    const ReactionResult res = chem().evaluate(*r, c);

    ImGui::SeparatorText("Result");
    ImGui::BeginTable("res", 2, ImGuiTableFlags_SizingFixedFit);

    auto row = [](const char* label) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", label);
        ImGui::TableNextColumn();
    };

    row("dH");            ImGui::Text("%+.1f kJ/mol", res.deltaH);
    row("dS");            ImGui::Text("%+.1f J/(mol.K)", res.deltaS);
    row("-T.dS");         ImGui::Text("%+.1f kJ/mol", -c.temperature * res.deltaS / 1000.0);
    row("dG (1 bar)");    ImGui::Text("%+.1f kJ/mol", res.deltaGStandard);
    if (std::fabs(res.gasMoleChange) > 1e-9) {
        row("dn(gas)");   ImGui::Text("%+.0f mol", res.gasMoleChange);
        row("pressure shift");
        ImGui::Text("%+.1f kJ/mol", res.deltaG - res.deltaGStandard);
    }
    row("dG");            deltaGText(res.deltaG);
    row("K");             ImGui::Text("%.3g", res.equilibriumK);
    row("Ea used");       ImGui::Text("%.1f kJ/mol", res.effectiveEa);
    row("rate");          ImGui::Text("%.3g mol/s", res.rate);
    ImGui::EndTable();

    ImGui::Spacing();
    if (res.conditionsMet) {
        ImGui::TextColored(ImVec4(0.40f, 0.90f, 0.45f, 1.0f), "PROCEEDS under these conditions.");
    } else {
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.30f, 1.0f), "Will not run.");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextWrapped("%s", res.blockedBy.c_str());
        ImGui::PopTextWrapPos();
    }

    ImGui::Spacing();
    ImGui::TextDisabled(
        "This is the same evaluate() the agents call. Whatever it says here is what they "
        "experience -- there is no separate path and no recipe list anywhere.");

    // The temperature at which this crosses over, found by bisection on dG.
    // Reported because it is the single most useful number about a reduction:
    // it is the furnace you have to build.
    ImGui::Spacing();
    if (ImGui::Button("Find crossover temperature")) {
        ReactionConditions probe = c;
        double lo = 200.0, hi = 4000.0;
        probe.temperature = lo;
        const bool lowSpontaneous = chem().evaluate(*r, probe).deltaG < 0.0;
        probe.temperature = hi;
        const bool highSpontaneous = chem().evaluate(*r, probe).deltaG < 0.0;
        if (lowSpontaneous == highSpontaneous) {
            ui.chemCrossover = lowSpontaneous ? -1.0f : -2.0f;
        } else {
            for (int i = 0; i < 60; ++i) {
                const double mid = 0.5 * (lo + hi);
                probe.temperature = mid;
                const bool spont = chem().evaluate(*r, probe).deltaG < 0.0;
                if (spont == lowSpontaneous) lo = mid; else hi = mid;
            }
            ui.chemCrossover = static_cast<float>(0.5 * (lo + hi));
        }
    }
    ImGui::SameLine();
    if (ui.chemCrossover == -1.0f) {
        ImGui::TextDisabled("Downhill at every temperature from 200 to 4000 K.");
    } else if (ui.chemCrossover == -2.0f) {
        ImGui::TextDisabled("Uphill at every temperature from 200 to 4000 K.");
    } else if (ui.chemCrossover > 0.0f) {
        ImGui::Text("dG crosses zero at %.0f K.", static_cast<double>(ui.chemCrossover));
    }
}

void drawReactionsTab(UiState& ui) {
    ImGui::TextDisabled(
        "%zu reactions, %zu unbalanced. An unbalanced equation cannot be loaded at all, so "
        "that second number is structurally always zero.",
        chem().reactions().size(), chem().unbalancedCount());

    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##rxfilter", "filter", ui.chemReactionFilter,
                             sizeof ui.chemReactionFilter);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    const char* classNames[static_cast<int>(ReactionClass::Count) + 1] = {nullptr};
    for (int i = 0; i < static_cast<int>(ReactionClass::Count); ++i)
        classNames[i + 1] = reactionClassName(static_cast<ReactionClass>(i));
    classNames[0] = "All classes";
    ImGui::Combo("##rxclass", &ui.chemClassFilter, classNames,
                 static_cast<int>(ReactionClass::Count) + 1);

    ImGui::Separator();

    ImGui::BeginChild("rxlist", ImVec2(360.0f, 0), ImGuiChildFlags_Borders);
    for (const Reaction& r : chem().reactions()) {
        if (ui.chemClassFilter > 0 &&
            static_cast<int>(r.cls) != ui.chemClassFilter - 1) continue;
        if (!matchFilter(ui.chemReactionFilter, r.name, equationText(r),
                         reactionClassName(r.cls)))
            continue;
        const bool selected = ui.chemSelectedReaction == static_cast<int>(r.id);
        if (ImGui::Selectable(r.name.c_str(), selected)) {
            ui.chemSelectedReaction = static_cast<int>(r.id);
            ui.chemCrossover = 0.0f;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("  %s", reactionClassName(r.cls));
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("rxdetail", ImVec2(0, 0));
    drawReactionRunner(ui);
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Tab: materials workbench
// ---------------------------------------------------------------------------

void drawMaterialsTab(UiState& ui) {
    ImGui::TextWrapped(
        "Properties come from composition and process history. Nothing is looked up by name: "
        "bronze is harder than copper because tin atoms are a different size from copper "
        "atoms and strain the lattice, and the strengthening is computed from that.");
    ImGui::Separator();

    // Build a solid-substance list for the two component pickers.
    static std::vector<uint16_t> solids;
    static std::vector<std::string> solidLabels;
    if (solids.empty()) {
        for (const Substance& s : chem().substances()) {
            if (s.phase != Phase::Solid || s.nuclear) continue;
            solids.push_back(s.id);
            solidLabels.push_back(s.formulaText + "  " + s.name);
        }
    }
    auto picker = [&](const char* label, int& index) {
        if (solids.empty()) return;
        if (index < 0 || index >= static_cast<int>(solids.size())) index = 0;
        ImGui::SetNextItemWidth(280.0f);
        if (ImGui::BeginCombo(label, solidLabels[static_cast<size_t>(index)].c_str())) {
            for (int i = 0; i < static_cast<int>(solids.size()); ++i) {
                if (ImGui::Selectable(solidLabels[static_cast<size_t>(i)].c_str(), i == index))
                    index = i;
            }
            ImGui::EndCombo();
        }
    };

    picker("Base", ui.matBaseIndex);
    ImGui::SetNextItemWidth(280.0f);
    ImGui::SliderFloat("Base mass fraction", &ui.matBaseFraction, 0.5f, 1.0f, "%.3f");
    picker("Solute", ui.matSoluteIndex);

    ImGui::SeparatorText("Process history");
    ImGui::TextDisabled("Order matters. Quench-then-temper is not the same material as "
                        "temper-then-quench.");
    for (int i = 0; i < static_cast<int>(ProcessStep::Count); ++i) {
        const ProcessStep step = static_cast<ProcessStep>(i);
        if (i % 4 != 0) ImGui::SameLine();
        if (ImGui::Button(processStepName(step), ImVec2(112.0f, 0.0f))) {
            if (ui.matHistoryCount < 8) ui.matHistory[ui.matHistoryCount++] = static_cast<int>(i);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(400.0f);
            ImGui::TextUnformatted(processStepNote(step));
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    ImGui::Spacing();
    if (ui.matHistoryCount == 0) {
        ImGui::TextDisabled("As-solidified. No processing.");
    } else {
        std::string chain;
        for (int i = 0; i < ui.matHistoryCount; ++i) {
            if (i) chain += " -> ";
            chain += processStepName(static_cast<ProcessStep>(ui.matHistory[i]));
        }
        ImGui::TextWrapped("%s", chain.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) ui.matHistoryCount = 0;
    }

    if (solids.empty()) return;

    std::vector<MaterialComponent> comp;
    MaterialComponent a, b;
    a.substance = solids[static_cast<size_t>(ui.matBaseIndex)];
    a.massFraction = static_cast<double>(ui.matBaseFraction);
    b.substance = solids[static_cast<size_t>(ui.matSoluteIndex)];
    b.massFraction = 1.0 - static_cast<double>(ui.matBaseFraction);
    comp.push_back(a);
    if (b.massFraction > 1e-6) comp.push_back(b);

    std::vector<ProcessStep> history;
    for (int i = 0; i < ui.matHistoryCount; ++i)
        history.push_back(static_cast<ProcessStep>(ui.matHistory[i]));

    const MaterialProperties m = Materials::evaluate(comp, history);

    ImGui::SeparatorText("Result");
    ImGui::Text("%s", m.classification.empty() ? m.dominant.c_str() : m.classification.c_str());
    ImGui::BeginTable("matres", 2, ImGuiTableFlags_SizingFixedFit);
    auto row = [](const char* label, const char* fmt, double v) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", label);
        ImGui::TableNextColumn(); ImGui::Text(fmt, v);
    };
    row("Density",      "%.2f g/cm3", m.density);
    row("Hardness",     "%.2f Mohs",  m.hardness);
    row("Tensile",      "%.0f MPa",   m.tensileStrength);
    row("Toughness",    "%.2f",       m.toughness);
    row("Melting",      "%.0f K",     m.meltingPoint);
    row("Thermal",      "%.1f W/m.K", m.thermalConductivity);
    row("Electrical",   "%.2f MS/m",  m.electricalConductivity);
    row("Corrosion",    "%.2f",       m.corrosionResistance);
    ImGui::EndTable();

    if (!m.explanation.empty()) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextWrapped("%s", m.explanation.c_str());
        ImGui::PopTextWrapPos();
    }

    ImGui::SeparatorText("As a tool");
    ImGui::SetNextItemWidth(160.0f);
    const char* toolNames[static_cast<int>(ToolKind::Count)];
    for (int i = 0; i < static_cast<int>(ToolKind::Count); ++i)
        toolNames[i] = toolKindName(static_cast<ToolKind>(i));
    ImGui::Combo("Kind", &ui.matToolKind, toolNames, static_cast<int>(ToolKind::Count));
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Edge angle", &ui.matEdgeAngle, 10.0f, 90.0f, "%.0f deg");
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Head mass", &ui.matToolMass, 0.05f, 5.0f, "%.2f kg");

    ToolGeometry g;
    g.edgeAngleDegrees = static_cast<double>(ui.matEdgeAngle);
    g.massKg = static_cast<double>(ui.matToolMass);
    std::string why;
    const double eff = Materials::toolEffectiveness(
        m, static_cast<ToolKind>(ui.matToolKind), g, &why);
    ImGui::Text("Effectiveness: %.3f", eff);
    ImGui::ProgressBar(static_cast<float>(eff), ImVec2(-1, 0));
    if (!why.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", why.c_str());
        ImGui::PopTextWrapPos();
    }
}

}  // namespace

// ---------------------------------------------------------------------------

void drawChemistryLab(Simulation& sim, UiState& ui) {
    (void)sim;
    ImGui::SetNextWindowSize(ImVec2(940, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Chemistry Lab", &ui.showChemistryLab)) { ImGui::End(); return; }

    if (!chem().loaded()) {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.0f),
                           "No chemistry loaded. data/chemistry.json failed to load.");
        ImGui::TextWrapped("An unbalanced equation is a hard load failure by design -- the "
                           "program will not run a chemistry that creates matter from nothing.");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("chemtabs")) {
        if (ImGui::BeginTabItem("Elements"))   { drawElementsTab(ui);   ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Substances")) { drawSubstancesTab(ui); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Reactions"))  { drawReactionsTab(ui);  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Materials"))  { drawMaterialsTab(ui);  ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Knowledge & Culture
//
// Culture is tracked distinctly from genetics, so it gets its own panel. The
// number that matters most here is `holders`: when it reaches zero a technique
// has been lost, and the world has to find it again from scratch.
// ---------------------------------------------------------------------------

void drawKnowledgePanel(Simulation& sim, UiState& ui) {
    ImGui::SetNextWindowSize(ImVec2(720, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Knowledge & Culture", &ui.showKnowledge)) { ImGui::End(); return; }

    struct Row {
        uint16_t reactionId;
        uint64_t firstTick;
        uint64_t uid;
        std::string discoverer;
        uint32_t holders;
        bool everLost;
    };
    std::vector<Row> rows;
    uint32_t techCounts[static_cast<int>(Technique::Count)] = {0};
    uint32_t living = 0;
    double meanUnits = 0.0;

    sim.readAgents([&](const Agents& a) {
        for (const DiscoveryRecord& d : a.knowledge().discoveries()) {
            Row r;
            r.reactionId = d.reactionId;
            r.firstTick = d.firstTick;
            r.uid = d.discovererUid;
            r.discoverer = d.discovererName;
            r.holders = d.holders;
            r.everLost = d.everLost;
            rows.push_back(std::move(r));
        }
        for (uint32_t slot : a.liveSlots()) {
            ++living;
            meanUnits += static_cast<double>(a.knowledge().known(slot).size());
            for (int t = 0; t < static_cast<int>(Technique::Count); ++t)
                if (a.knowledge().hasTechnique(slot, static_cast<Technique>(t)))
                    ++techCounts[t];
        }
    });
    if (living > 0) meanUnits /= static_cast<double>(living);

    ImGui::Text("%zu things have ever been discovered.", rows.size());
    ImGui::SameLine();
    ImGui::TextDisabled("| %.2f knowledge units per living individual", meanUnits);

    ImGui::SeparatorText("Techniques");
    ImGui::TextDisabled(
        "Each rests on the one before it. This ladder is why fire long precedes a kiln and a "
        "kiln long precedes iron -- nothing is scheduled, but nothing can be skipped.");
    if (ImGui::BeginTable("techs", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Technique", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("Reaches", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Holders", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("What it is");
        ImGui::TableHeadersRow();
        for (int t = 0; t < static_cast<int>(Technique::Count); ++t) {
            const Technique tech = static_cast<Technique>(t);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (techCounts[t] > 0) ImGui::TextUnformatted(techniqueName(tech));
            else ImGui::TextDisabled("%s", techniqueName(tech));
            ImGui::TableNextColumn();
            ImGui::Text("%.0f K", techniqueTemperature(tech));
            ImGui::TableNextColumn();
            if (living > 0) {
                const float frac = static_cast<float>(techCounts[t]) / static_cast<float>(living);
                char buf[32];
                std::snprintf(buf, sizeof buf, "%u (%.0f%%)", techCounts[t], frac * 100.0f);
                ImGui::ProgressBar(frac, ImVec2(-1, 0), buf);
            } else {
                ImGui::TextDisabled("--");
            }
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", techniqueNote(tech));
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("First discoveries");
    if (rows.empty()) {
        ImGui::TextDisabled("Nothing yet. Discovery is a search, so it takes as long as it "
                            "takes -- there is no schedule pushing it along.");
    } else if (ImGui::BeginTable("discs", 5,
                                 ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                 ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Tick", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("What");
        ImGui::TableSetupColumn("Discovered by");
        ImGui::TableSetupColumn("Holders", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableHeadersRow();
        for (const Row& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%llu",
                                                 static_cast<unsigned long long>(r.firstTick));
            ImGui::TableNextColumn();
            const Reaction* rx = chem().reaction(r.reactionId);
            ImGui::TextUnformatted(rx ? rx->name.c_str() : "?");
            if (rx && ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(equationText(*rx).c_str());
                ImGui::EndTooltip();
            }
            if (rx && ImGui::IsItemClicked()) {
                ui.chemSelectedReaction = static_cast<int>(rx->id);
                ui.showChemistryLab = true;
            }
            ImGui::TableNextColumn();
            ImGui::Text("%s", r.discoverer.empty() ? "(unknown)" : r.discoverer.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%u", r.holders);
            ImGui::TableNextColumn();
            if (r.holders == 0)
                ImGui::TextColored(ImVec4(0.95f, 0.40f, 0.35f, 1.0f), "LOST");
            else if (r.everLost)
                ImGui::TextColored(ImVec4(0.90f, 0.75f, 0.35f, 1.0f), "rediscovered");
            else
                ImGui::TextDisabled("held");
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

}  // namespace gen
