// ui/cards.cpp — the Individual Card, Brain Inspector, Genome Browser and the
// population search table.
//
// Threading contract (ARCHITECTURE.md §1): reads happen inside
// sim.readAgents(), which holds the world lock shared and must stay short.
// Writes NEVER happen here. Every edit is queued through sim.editAgents(),
// which applies the closure on the sim thread at a tick boundary with the lock
// held exclusively, and records it in the Intervention Log. That is why an
// allele can be edited from the UI mid-run without tearing state or desyncing
// the simulation.
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "chem/reactions.h"
#include "core/config.h"
#include "imgui.h"
#include "ui/app.h"

namespace gen {

namespace {

// Resolve a uid to a live slot, or -1.
int32_t slotOf(const Agents& a, uint64_t uid) { return a.slotOfUid(uid); }

void labelValue(const char* label, const char* fmt, ...) {
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(178.0f);
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}

// A slider that queues an edit instead of writing directly. `apply` receives
// the mutable store and the new value.
template <typename Apply>
bool editSlider(Simulation& sim, const char* label, float current, float lo, float hi,
                const char* fmt, const char* description, const std::string& logText,
                Apply apply) {
    float v = current;
    ImGui::SetNextItemWidth(-120.0f);
    const bool changed = ImGui::SliderFloat(label, &v, lo, hi, fmt);
    if (description && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(420.0f);
        ImGui::TextUnformatted(description);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    if (changed) {
        const float nv = v;
        sim.editAgents([apply, nv](Agents& a, RngBank& rng) { apply(a, rng, nv); }, logText);
    }
    return changed;
}

ImU32 stageColour(LifeStage s) {
    switch (s) {
        case LifeStage::Embryo:     return IM_COL32(150, 130, 200, 255);
        case LifeStage::Juvenile:   return IM_COL32(120, 200, 240, 255);
        case LifeStage::Adolescent: return IM_COL32(120, 220, 160, 255);
        case LifeStage::Adult:      return IM_COL32(230, 230, 230, 255);
        case LifeStage::Senescent:  return IM_COL32(210, 170, 110, 255);
        default:                    return IM_COL32(140, 140, 140, 255);
    }
}

// Sex expression drawn as a continuous bar rather than a label, because that
// is what it actually is.
void drawSexExpressionBar(float e, float width) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    dl->AddRectFilledMultiColor(p, ImVec2(p.x + width, p.y + h),
                                IM_COL32(210, 110, 160, 255), IM_COL32(90, 150, 230, 255),
                                IM_COL32(90, 150, 230, 255), IM_COL32(210, 110, 160, 255));
    const float x = p.x + width * e;
    dl->AddLine(ImVec2(x, p.y - 2.0f), ImVec2(x, p.y + h + 2.0f), IM_COL32(255, 255, 255, 255), 2.0f);
    if (isIntersex(e)) {
        const float lo = cfg().getF("sex.intersex_low", 0.42f);
        const float hi = cfg().getF("sex.intersex_high", 0.58f);
        dl->AddRect(ImVec2(p.x + width * lo, p.y), ImVec2(p.x + width * hi, p.y + h),
                    IM_COL32(255, 220, 80, 220), 0.0f, 0, 1.5f);
    }
    ImGui::Dummy(ImVec2(width, h));
}

}  // namespace

void openIndividualCard(UiState& ui, uint64_t uid) {
    if (uid == 0) return;
    if (std::find(ui.openCards.begin(), ui.openCards.end(), uid) == ui.openCards.end())
        ui.openCards.push_back(uid);
    ui.focusAgentUid = uid;
}

// ---------------------------------------------------------------------------
// Tabs
// ---------------------------------------------------------------------------

namespace {

void tabIdentity(Simulation& sim, UiState& ui, Viewport& vp, const Agents& a,
                 uint32_t slot, uint64_t uid, uint64_t tick) {
    const Phenotype& p = a.m_phenotype[slot];

    // A crude but informative "portrait": the individual's own colouration,
    // scaled by size, with ornament spikes and a health-tinted ring.
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c = ImGui::GetCursorScreenPos();
        const float box = 84.0f;
        const ImVec2 centre(c.x + box * 0.5f, c.y + box * 0.5f);
        dl->AddRectFilled(c, ImVec2(c.x + box, c.y + box), IM_COL32(20, 22, 26, 255), 4.0f);

        const float radius = 10.0f + 14.0f * std::min(2.0f, p.get(Trait::Size)) * 0.5f;
        const int cr = static_cast<int>(p.get(Trait::ColourR) * 255.0f);
        const int cg = static_cast<int>(p.get(Trait::ColourG) * 255.0f);
        const int cb = static_cast<int>(p.get(Trait::ColourB) * 255.0f);

        const float ornament = 0.5f * (p.get(Trait::Ornament1) + p.get(Trait::Ornament2));
        if (ornament > 0.05f) {
            const int spikes = 6 + static_cast<int>(ornament * 6.0f);
            for (int i = 0; i < spikes; ++i) {
                const float ang = 6.2831853f * static_cast<float>(i) / static_cast<float>(spikes);
                const float len = radius + 6.0f + ornament * 12.0f;
                dl->AddLine(centre,
                            ImVec2(centre.x + std::cos(ang) * len, centre.y + std::sin(ang) * len),
                            IM_COL32(cr, cg, cb, 190), 2.0f);
            }
        }
        dl->AddCircleFilled(centre, radius, IM_COL32(cr, cg, cb, 255), 24);
        const int hp = static_cast<int>(a.m_health[slot] * 255.0f);
        dl->AddCircle(centre, radius + 3.0f, IM_COL32(255 - hp, hp, 60, 255), 24, 2.5f);
        ImGui::Dummy(ImVec2(box, box));
    }
    ImGui::SameLine();
    ImGui::BeginGroup();

    // Name is editable.
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s", a.m_name[slot].c_str());
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::InputText("Name", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
            const std::string nv = buf;
            sim.editAgents([uid, nv](Agents& ag, RngBank&) {
                const int32_t s = ag.slotOfUid(uid);
                if (s >= 0) ag.m_name[static_cast<uint32_t>(s)] = nv;
            }, "Renamed individual to " + nv);
        }
    }
    labelValue("Unique id", "%llu", static_cast<unsigned long long>(uid));
    labelValue("Slot", "%u  (generation %u)", slot, a.m_generation[slot]);
    const float age = (tick > a.m_birthTick[slot])
        ? static_cast<float>(tick - a.m_birthTick[slot]) / static_cast<float>(kHoursPerYear) : 0.0f;
    labelValue("Age", "%.2f years", static_cast<double>(age));
    labelValue("Born", "%s", tickToDate(a.m_birthTick[slot]).toString().c_str());
    ImGui::TextDisabled("Life stage");
    ImGui::SameLine(178.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, stageColour(static_cast<LifeStage>(a.m_stage[slot])));
    ImGui::TextUnformatted(lifeStageName(static_cast<LifeStage>(a.m_stage[slot])));
    ImGui::PopStyleColor();
    labelValue("Doing", "%s", actionName(static_cast<Action>(a.m_action[slot])));
    labelValue("Location", "(%.1f, %.1f)", static_cast<double>(a.m_x[slot]),
               static_cast<double>(a.m_y[slot]));
    ImGui::EndGroup();

    ImGui::Separator();
    bool immortal = a.immortal(slot);
    if (ImGui::Checkbox("Immortal", &immortal)) {
        sim.editAgents([uid, immortal](Agents& ag, RngBank&) {
            const int32_t s = ag.slotOfUid(uid);
            if (s >= 0) ag.setImmortal(static_cast<uint32_t>(s), immortal);
        }, immortal ? "Granted immortality" : "Revoked immortality");
    }
    ImGui::SameLine();
    helpMarker("An immortal individual is exempt from every mortality source except an explicit "
               "divine kill. Aging, damage and starvation still accumulate and are still shown, "
               "so you can watch what would have killed them.");
    ImGui::SameLine();
    bool tagged = a.tagged(slot);
    if (ImGui::Checkbox("Tag / favourite", &tagged)) {
        sim.editAgents([uid, tagged](Agents& ag, RngBank&) {
            const int32_t s = ag.slotOfUid(uid);
            if (s >= 0) ag.setTagged(static_cast<uint32_t>(s), tagged);
        }, tagged ? "Tagged individual" : "Untagged individual");
    }

    if (ImGui::Button("Centre camera")) {
        vp.focusOn(static_cast<int>(a.m_x[slot]), static_cast<int>(a.m_y[slot]));
        vp.select(static_cast<int>(a.m_x[slot]), static_cast<int>(a.m_y[slot]));
    }
    ImGui::SameLine();
    if (ImGui::Button("Pin for comparison")) ui.compareAgentUid = uid;
    ImGui::SameLine();
    if (ImGui::Button("Brain inspector")) { ui.focusAgentUid = uid; ui.showBrainInspector = true; }
    ImGui::SameLine();
    if (ImGui::Button("Genome browser")) { ui.focusAgentUid = uid; ui.showGenomeBrowser = true; }

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.16f, 0.16f, 1.0f));
    if (ImGui::Button("Kill (divine)")) {
        sim.editAgents([uid](Agents& ag, RngBank&) {
            const int32_t s = ag.slotOfUid(uid);
            if (s >= 0) ag.kill(static_cast<uint32_t>(s), DeathCause::Divine, 0, nullptr);
        }, "Struck down an individual");
    }
    ImGui::PopStyleColor();
}

void tabGenome(Simulation& sim, UiState& ui, const Agents& a, uint32_t slot, uint64_t uid) {
    const Genetics& g = a.genetics();
    ConstGenomeView genome = g.genome(slot);
    const Phenotype& p = a.m_phenotype[slot];

    labelValue("Genes", "%u of %u capacity", genome.count, g.geneCapacity());
    labelValue("Chromosomes", "%d", g.map().chromosomeCount);
    labelValue("Heterozygosity", "%.4f", static_cast<double>(p.heterozygosity));
    labelValue("Inbreeding F", "%.4f", static_cast<double>(p.inbreedingF));
    ImGui::SameLine();
    helpMarker("F = 1 - Ho/He: how much less heterozygous this individual is than a random "
               "member of the population. It is computed from genome-wide homozygosity against "
               "the population's expected heterozygosity, not asserted from the pedigree.");
    labelValue("Recessive lethals carried", "%u", static_cast<unsigned>(p.lethalCarried));
    labelValue("Sublethal load", "%.3f", static_cast<double>(p.sublethalPenalty));
    labelValue("Mutation rate modifier", "%.3fx",
               static_cast<double>(p.get(Trait::MutationRateModifier)));

    ImGui::Separator();
    if (ImGui::Button("Purge deleterious alleles")) {
        sim.editAgents([uid](Agents& ag, RngBank& rng) {
            const int32_t s = ag.slotOfUid(uid);
            if (s < 0) return;
            GenomeView gv = ag.genetics().genome(static_cast<uint32_t>(s));
            for (uint16_t i = 0; i < gv.count; ++i) {
                if (alleleIsBroken(gv.genes[i].alleleA)) gv.genes[i].alleleA = 0.0f;
                if (alleleIsBroken(gv.genes[i].alleleB)) gv.genes[i].alleleB = 0.0f;
            }
            ag.redevelop(static_cast<uint32_t>(s), rng);
        }, "Purged deleterious alleles from a genome");
    }
    ImGui::SameLine();
    if (ImGui::Button("Maximise heterozygosity")) {
        sim.editAgents([uid](Agents& ag, RngBank& rng) {
            const int32_t s = ag.slotOfUid(uid);
            if (s < 0) return;
            GenomeView gv = ag.genetics().genome(static_cast<uint32_t>(s));
            for (uint16_t i = 0; i < gv.count; ++i) {
                if (static_cast<LocusType>(gv.genes[i].type) == LocusType::SexDetermining) continue;
                // Push the two alleles apart around their mean, which is what
                // "more heterozygous" actually means for continuous alleles.
                const float mean = 0.5f * (gv.genes[i].alleleA + gv.genes[i].alleleB);
                const float spread = 0.8f;
                gv.genes[i].alleleA = mean - spread;
                gv.genes[i].alleleB = mean + spread;
            }
            ag.redevelop(static_cast<uint32_t>(s), rng);
        }, "Maximised heterozygosity of a genome");
    }
    ImGui::SameLine();
    if (ImGui::Button("Randomise genome")) {
        sim.editAgents([uid](Agents& ag, RngBank& rng) {
            const int32_t s = ag.slotOfUid(uid);
            if (s < 0) return;
            GenomeView gv = ag.genetics().genome(static_cast<uint32_t>(s));
            for (uint16_t i = 0; i < gv.count; ++i) {
                if (static_cast<LocusType>(gv.genes[i].type) == LocusType::SexDetermining) continue;
                gv.genes[i].alleleA = rng[Stream::God].gaussian(0.0f, 0.8f);
                gv.genes[i].alleleB = rng[Stream::God].gaussian(0.0f, 0.8f);
            }
            ag.redevelop(static_cast<uint32_t>(s), rng);
        }, "Randomised a genome");
    }
    ImGui::SameLine();
    if (ImGui::Button("Clone this individual")) {
        sim.editAgents([uid](Agents& ag, RngBank& rng) {
            const int32_t s = ag.slotOfUid(uid);
            if (s < 0) return;
            const uint32_t src = static_cast<uint32_t>(s);
            const AgentId child = ag.spawnFounder(ag.m_x[src] + 1.0f, ag.m_y[src] + 1.0f,
                                                  rng, ag.m_birthTick[src], false);
            if (!child.valid()) return;
            ag.genetics().copyGenome(child.slot, src);
            ag.brains().copyBrain(child.slot, src);
            ag.redevelop(child.slot, rng);
            ag.m_name[child.slot] = ag.m_name[src] + " (clone)";
        }, "Cloned an individual");
    }
    ImGui::SameLine();
    helpMarker("A clone copies the genome and the brain, including everything the original has "
               "learned. It does NOT copy age, condition or relationships, and developmental "
               "noise is re-rolled, so the clone is a genetic twin rather than a duplicate.");

    ImGui::Separator();
    if (ImGui::Button("Open in Genome Browser")) { ui.focusAgentUid = uid; ui.showGenomeBrowser = true; }
    ImGui::TextDisabled("The browser gives the full chromosome-by-chromosome view with every "
                        "allele individually editable.");
}

void tabPhenotype(Simulation& sim, const Agents& a, uint32_t slot, uint64_t uid) {
    const Phenotype& p = a.m_phenotype[slot];

    ImGui::TextDisabled("Sex");
    labelValue("Chromosomal sex", "%s",
               chromosomalSexName(a.sexSystem(),
                                  static_cast<ChromosomalSex>(a.m_chromosomalSex[slot])));
    const float sexExpr = p.get(Trait::SexExpression);
    ImGui::TextDisabled("Expressed sex");
    ImGui::SameLine(178.0f);
    ImGui::Text("%.3f  (%s)%s", static_cast<double>(sexExpr), sexExpressionLabel(sexExpr),
                isIntersex(sexExpr) ? "  [intersex]" : "");
    drawSexExpressionBar(sexExpr, 280.0f);
    ImGui::SameLine();
    helpMarker("Chromosomal sex and expressed sex are stored SEPARATELY and can disagree. "
               "Expression is continuous, driven by the sex locus, hormone-analog regulatory "
               "genes and developmental noise, so intermediate outcomes occur at low rate "
               "without being a special case.");

    editSlider(sim, "Sex expression", sexExpr, 0.0f, 1.0f, "%.3f",
               "Directly sets the expressed sexual phenotype, overriding what the genome "
               "produced. Everything downstream -- who can gestate, how others perceive them, "
               "which orientation axis applies to them -- follows immediately.",
               "Edited sex expression",
               [uid](Agents& ag, RngBank&, float v) {
                   const int32_t s = ag.slotOfUid(uid);
                   if (s < 0) return;
                   ag.m_phenotype[static_cast<uint32_t>(s)]
                       .traits[static_cast<int>(Trait::SexExpression)] = v;
                   ag.m_display[static_cast<uint32_t>(s)].sexExpression = v;
               });

    ImGui::Separator();
    ImGui::TextDisabled("Every expressed trait. Editing writes the PHENOTYPE, not the genome, so "
                        "the change affects this individual but is not inherited.");

    if (ImGui::BeginTable("traits", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0, 320))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Trait", ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Unit", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Edit");
        ImGui::TableHeadersRow();

        for (int t = 0; t < kTraitCount; ++t) {
            const TraitSpec& spec = traitSpec(static_cast<Trait>(t));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(spec.name);
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(440.0f);
                ImGui::TextUnformatted(spec.description);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.4g", static_cast<double>(p.traits[t]));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("%s", spec.unit);
            ImGui::TableSetColumnIndex(3);
            ImGui::PushID(t);
            float v = p.traits[t];
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("##v", &v, spec.minValue, spec.maxValue, "%.4g")) {
                const int ti = t;
                const float nv = v;
                sim.editAgents([uid, ti, nv](Agents& ag, RngBank&) {
                    const int32_t s = ag.slotOfUid(uid);
                    if (s < 0) return;
                    ag.m_phenotype[static_cast<uint32_t>(s)].traits[ti] = nv;
                    buildPreferenceVector(ag.m_phenotype[static_cast<uint32_t>(s)],
                                          ag.m_prefs[static_cast<uint32_t>(s)]);
                }, std::string("Edited trait ") + traitSpec(static_cast<Trait>(ti)).name);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::TextDisabled("Immune profile (MHC signature)");
    for (int i = 0; i < 8; ++i) {
        ImGui::Text("MHC[%d] %+.3f", i, static_cast<double>(p.mhcSignature[i]));
        if (i % 4 != 3) ImGui::SameLine(static_cast<float>((i % 4 + 1) * 140));
    }
    labelValue("Ornament metabolic cost", "%.3f energy/hour",
               static_cast<double>(p.ornamentCost));
}

void tabAttraction(Simulation& sim, UiState& ui, Viewport& vp, const Agents& a,
                   uint32_t slot, uint64_t uid) {
    const PreferenceVector& prefs = a.m_prefs[slot];

    ImGui::TextDisabled("ORIENTATION");
    ImGui::TextWrapped("Two independent axes, not a binary. Any point in this space is "
                       "representable: exclusive attraction to either pole, to both, or to "
                       "neither. It is heritable but noisy, and it does not have to be "
                       "reproductively optimal.");

    // 2D orientation pad.
    {
        const float pad = 150.0f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(origin, ImVec2(origin.x + pad, origin.y + pad),
                          IM_COL32(18, 20, 24, 255), 3.0f);
        dl->AddRect(origin, ImVec2(origin.x + pad, origin.y + pad), IM_COL32(60, 64, 74, 255), 3.0f);
        for (int i = 1; i < 4; ++i) {
            const float f = pad * static_cast<float>(i) / 4.0f;
            dl->AddLine(ImVec2(origin.x + f, origin.y), ImVec2(origin.x + f, origin.y + pad),
                        IM_COL32(40, 44, 52, 255));
            dl->AddLine(ImVec2(origin.x, origin.y + f), ImVec2(origin.x + pad, origin.y + f),
                        IM_COL32(40, 44, 52, 255));
        }
        const float px = origin.x + pad * std::min(1.0f, prefs.orientationFemale / 2.0f);
        const float py = origin.y + pad * (1.0f - std::min(1.0f, prefs.orientationMale / 2.0f));
        dl->AddCircleFilled(ImVec2(px, py), 6.0f, IM_COL32(255, 210, 60, 255), 16);

        ImGui::InvisibleButton("##orientpad", ImVec2(pad, pad));
        if (ImGui::IsItemActive()) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            const float nf = std::min(2.0f, std::max(0.0f, (m.x - origin.x) / pad * 2.0f));
            const float nm = std::min(2.0f, std::max(0.0f, (1.0f - (m.y - origin.y) / pad) * 2.0f));
            sim.editAgents([uid, nf, nm](Agents& ag, RngBank&) {
                const int32_t s = ag.slotOfUid(uid);
                if (s < 0) return;
                const uint32_t sl = static_cast<uint32_t>(s);
                ag.m_phenotype[sl].traits[static_cast<int>(Trait::OrientationFemale)] = nf;
                ag.m_phenotype[sl].traits[static_cast<int>(Trait::OrientationMale)] = nm;
                ag.m_prefs[sl].orientationFemale = nf;
                ag.m_prefs[sl].orientationMale = nm;
            }, "Edited mating orientation");
        }
        dl->AddText(ImVec2(origin.x + 4.0f, origin.y + 2.0f), IM_COL32(150, 155, 165, 255),
                    "toward male-expressed");
        dl->AddText(ImVec2(origin.x + 4.0f, origin.y + pad - 16.0f), IM_COL32(150, 155, 165, 255),
                    "toward female-expressed ->");
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    labelValue("Toward male-expressed", "%.3f", static_cast<double>(prefs.orientationMale));
    labelValue("Toward female-expressed", "%.3f", static_cast<double>(prefs.orientationFemale));
    labelValue("Selectivity", "%.3f", static_cast<double>(prefs.selectivity));
    if (ImGui::Button("Invert orientation")) {
        sim.editAgents([uid](Agents& ag, RngBank&) {
            const int32_t s = ag.slotOfUid(uid);
            if (s < 0) return;
            const uint32_t sl = static_cast<uint32_t>(s);
            std::swap(ag.m_prefs[sl].orientationMale, ag.m_prefs[sl].orientationFemale);
            std::swap(ag.m_phenotype[sl].traits[static_cast<int>(Trait::OrientationMale)],
                      ag.m_phenotype[sl].traits[static_cast<int>(Trait::OrientationFemale)]);
        }, "Inverted an individual's orientation");
    }
    ImGui::EndGroup();

    ImGui::Separator();
    ImGui::TextDisabled("PREFERENCE WEIGHTS over displayed traits");
    for (int i = 0; i < kDisplayCount; ++i) {
        const Display d = static_cast<Display>(i);
        ImGui::PushID(i);
        float v = prefs.weight[i];
        ImGui::SetNextItemWidth(-160.0f);
        if (ImGui::SliderFloat(displayName(d), &v, -3.0f, 3.0f, "%.3f")) {
            const int di = i;
            const float nv = v;
            sim.editAgents([uid, di, nv](Agents& ag, RngBank&) {
                const int32_t s = ag.slotOfUid(uid);
                if (s < 0) return;
                const uint32_t sl = static_cast<uint32_t>(s);
                ag.m_prefs[sl].weight[di] = nv;
                ag.m_phenotype[sl].traits[static_cast<int>(
                    displayPreferenceTrait(static_cast<Display>(di)))] = nv;
            }, std::string("Edited preference weight for ") + displayName(static_cast<Display>(di)));
        }
        ImGui::PopID();
    }
    labelValue("MHC dissimilarity pref", "%.3f", static_cast<double>(prefs.mhcDissimilarity));
    labelValue("Homophily pref", "%.3f", static_cast<double>(prefs.homophily));
    labelValue("Imprinted", "%s", prefs.hasImprint ? "yes (early-life template)" : "no");

    ImGui::Separator();
    ImGui::TextDisabled("ATTRACTION MATRIX");
    ImGui::TextWrapped("Attractiveness is a function of an ORDERED PAIR, not a number an "
                       "individual has. The same target can be rated completely differently by "
                       "two observers with different preference weights, MHC profiles and "
                       "histories. Both directions are shown.");

    static std::vector<uint32_t> nearby;
    nearby.clear();
    const float radius = cfg().getF("ui.attraction_matrix_radius", 40.0f);
    for (uint32_t other : a.liveSlots()) {
        if (other == slot) continue;
        if (a.m_stage[other] == static_cast<uint8_t>(LifeStage::Embryo)) continue;
        const float dx = a.m_x[other] - a.m_x[slot], dy = a.m_y[other] - a.m_y[slot];
        if (dx * dx + dy * dy > radius * radius) continue;
        nearby.push_back(other);
        if (nearby.size() >= 400) break;
    }

    if (nearby.empty()) {
        ImGui::TextDisabled("Nobody else within %.0f tiles.", static_cast<double>(radius));
    } else if (ImGui::BeginTable("attraction", 6,
                                 ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                 ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY,
                                 ImVec2(0, 260))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Individual", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("this -> them", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("them -> this", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Mutual?", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("r", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Force");
        ImGui::TableHeadersRow();

        for (uint32_t other : nearby) {
            AttractionBreakdown ab, ba;
            a.attractionBetween(slot, other, &ab);
            a.attractionBetween(other, slot, &ba);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(static_cast<int>(other));
            if (ImGui::Selectable(a.m_name[other].c_str())) openIndividualCard(ui, a.m_uid[other]);
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("Breakdown of this -> them");
                ImGui::Separator();
                ImGui::Text("display traits   %+.3f", static_cast<double>(ab.displayTerm));
                ImGui::Text("MHC dissimilarity%+.3f", static_cast<double>(ab.mhcTerm));
                ImGui::Text("homophily        %+.3f", static_cast<double>(ab.homophilyTerm));
                ImGui::Text("familiarity      %+.3f", static_cast<double>(ab.familiarityTerm));
                ImGui::Text("reputation       %+.3f", static_cast<double>(ab.reputationTerm));
                ImGui::Text("history          %+.3f", static_cast<double>(ab.historyTerm));
                ImGui::Text("imprint          %+.3f", static_cast<double>(ab.imprintTerm));
                ImGui::Text("kin penalty      %+.3f", -static_cast<double>(ab.inbreedingPenalty));
                ImGui::Text("orientation gate  x%.3f", static_cast<double>(ab.orientationGate));
                ImGui::Separator();
                ImGui::Text("total %.3f vs threshold %.3f", static_cast<double>(ab.total),
                            static_cast<double>(ab.threshold));
                ImGui::EndTooltip();
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(ab.wouldAccept ? ImVec4(0.5f, 0.9f, 0.5f, 1.0f)
                                              : ImVec4(0.8f, 0.5f, 0.5f, 1.0f),
                               "%.3f", static_cast<double>(ab.total));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(ba.wouldAccept ? ImVec4(0.5f, 0.9f, 0.5f, 1.0f)
                                              : ImVec4(0.8f, 0.5f, 0.5f, 1.0f),
                               "%.3f", static_cast<double>(ba.total));
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted((ab.wouldAccept && ba.wouldAccept) ? "mutual" : "-");
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.2f", static_cast<double>(
                a.pedigree().relatedness(a.m_uid[slot], a.m_uid[other])));
            ImGui::TableSetColumnIndex(5);
            const uint64_t otherUid = a.m_uid[other];
            if (ImGui::SmallButton("irresistible")) {
                sim.editAgents([uid, otherUid](Agents& ag, RngBank&) {
                    const int32_t s = ag.slotOfUid(uid);
                    const int32_t o = ag.slotOfUid(otherUid);
                    if (s < 0 || o < 0) return;
                    // Force it in BOTH directions, which is what "make them
                    // find each other irresistible" has to mean.
                    ag.setAttractionOverride(static_cast<uint32_t>(o), uid, 99.0f, true);
                    ag.setAttractionOverride(static_cast<uint32_t>(s), otherUid, 99.0f, true);
                }, "Forced mutual attraction");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("destroy")) {
                sim.editAgents([uid, otherUid](Agents& ag, RngBank&) {
                    const int32_t s = ag.slotOfUid(uid);
                    const int32_t o = ag.slotOfUid(otherUid);
                    if (s < 0 || o < 0) return;
                    ag.setAttractionOverride(static_cast<uint32_t>(o), uid, -99.0f, true);
                    ag.setAttractionOverride(static_cast<uint32_t>(s), otherUid, -99.0f, true);
                }, "Destroyed attraction between two individuals");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("clear")) {
                sim.editAgents([uid, otherUid](Agents& ag, RngBank&) {
                    const int32_t s = ag.slotOfUid(uid);
                    const int32_t o = ag.slotOfUid(otherUid);
                    if (s < 0 || o < 0) return;
                    ag.setAttractionOverride(static_cast<uint32_t>(o), uid, 0.0f, false);
                    ag.setAttractionOverride(static_cast<uint32_t>(s), otherUid, 0.0f, false);
                }, "Cleared a forced attraction");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("imprint on")) {
                sim.editAgents([uid, otherUid](Agents& ag, RngBank&) {
                    const int32_t s = ag.slotOfUid(uid);
                    const int32_t o = ag.slotOfUid(otherUid);
                    if (s >= 0 && o >= 0)
                        ag.setImprint(static_cast<uint32_t>(s), static_cast<uint32_t>(o));
                }, "Forced a sexual imprint");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    (void)vp;
}

void tabBrain(Simulation& sim, UiState& ui, const Agents& a, uint32_t slot, uint64_t uid) {
    const Brains& b = a.brains();
    ConstBrainView bv = b.brain(slot);
    const BrainRuntime& rt = b.runtime(slot);
    const Phenotype& p = a.m_phenotype[slot];

    labelValue("Neurons", "%u  (%u hidden)", bv.nodeCount, b.hiddenNodeCount(slot));
    labelValue("Synapses", "%u  (%u enabled)", bv.connCount, b.enabledConnCount(slot));
    labelValue("Plasticity rate", "%.3f", static_cast<double>(p.get(Trait::PlasticityRate)));
    labelValue("Learning rate", "%.4f", static_cast<double>(p.get(Trait::LearningRate)));
    labelValue("Reward baseline", "%.4f", static_cast<double>(rt.valueBaseline));
    labelValue("Last reward", "%+.4f", static_cast<double>(rt.lastReward));
    labelValue("Last prediction error", "%+.4f", static_cast<double>(rt.lastRpe));
    ImGui::SameLine();
    helpMarker("The dopamine analogue. Learning is driven by reward MINUS expectation, so a "
               "fully predicted reward teaches nothing -- which is what stops an agent endlessly "
               "reinforcing a behaviour it has already mastered.");

    ImGui::Separator();
    ImGui::TextDisabled("PER-DRIVE REWARD WEIGHTS  (heritable: motivation itself evolves)");
    for (int d = 0; d < kDriveCount; ++d) {
        const Drive drive = static_cast<Drive>(d);
        const Trait t = driveRewardTrait(drive);
        ImGui::PushID(d);
        float v = p.get(t);
        ImGui::SetNextItemWidth(-200.0f);
        if (ImGui::SliderFloat(driveName(drive), &v, 0.0f, 4.0f, "%.3f")) {
            const int ti = static_cast<int>(t);
            const float nv = v;
            sim.editAgents([uid, ti, nv](Agents& ag, RngBank&) {
                const int32_t s = ag.slotOfUid(uid);
                if (s >= 0) ag.m_phenotype[static_cast<uint32_t>(s)].traits[ti] = nv;
            }, std::string("Edited reward weight ") + traitSpec(static_cast<Trait>(ti)).name);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("deficit %.3f", static_cast<double>(a.m_drives[slot].level[d]));
        ImGui::PopID();
    }

    ImGui::Separator();
    bool frozen = (a.m_flags[slot] & Agents::Flag_LearningFrozen) != 0;
    if (ImGui::Checkbox("Freeze learning", &frozen)) {
        sim.editAgents([uid, frozen](Agents& ag, RngBank&) {
            const int32_t s = ag.slotOfUid(uid);
            if (s < 0) return;
            if (frozen) ag.m_flags[static_cast<uint32_t>(s)] |= Agents::Flag_LearningFrozen;
            else ag.m_flags[static_cast<uint32_t>(s)] &=
                     static_cast<uint8_t>(~Agents::Flag_LearningFrozen);
        }, frozen ? "Froze an individual's learning" : "Unfroze an individual's learning");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset brain (retrain from scratch)")) {
        sim.editAgents([uid](Agents& ag, RngBank& rng) {
            const int32_t s = ag.slotOfUid(uid);
            if (s < 0) return;
            ag.brains().resetBrain(static_cast<uint32_t>(s), rng[Stream::God]);
            ag.redevelop(static_cast<uint32_t>(s), rng);
        }, "Reset an individual's brain");
    }
    ImGui::SameLine();
    if (ImGui::Button("Open Brain Inspector")) { ui.focusAgentUid = uid; ui.showBrainInspector = true; }

    ImGui::Separator();
    ImGui::TextDisabled("LIVE INPUT / OUTPUT");
    if (ImGui::BeginTable("io", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("Sensory inputs");
        for (int i = 0; i < kBrainInputCount; ++i) {
            const float v = a.m_inputs[slot * kBrainInputCount + i];
            ImGui::Text("%-22s", brainInputName(i));
            ImGui::SameLine(160.0f);
            ImGui::ProgressBar((v + 1.0f) * 0.5f, ImVec2(-1.0f, 10.0f), "");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.4f", static_cast<double>(v));
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("Motor outputs");
        for (int i = 0; i < kBrainOutputCount; ++i) {
            const float v = a.m_outputs[slot * kBrainOutputCount + i];
            ImGui::Text("%-18s", brainOutputName(i));
            ImGui::SameLine(140.0f);
            ImGui::ProgressBar((v + 1.0f) * 0.5f, ImVec2(-1.0f, 10.0f), "");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.4f", static_cast<double>(v));
        }
        ImGui::EndTable();
    }
}

void tabNeeds(Simulation& sim, const Agents& a, uint32_t slot, uint64_t uid) {
    const Phenotype& p = a.m_phenotype[slot];
    const float energyMax = 100.0f * p.get(Trait::Size);

    auto stateSlider = [&](const char* label, float value, float lo, float hi, const char* desc,
                           int field) {
        float v = value;
        ImGui::SetNextItemWidth(-140.0f);
        if (ImGui::SliderFloat(label, &v, lo, hi, "%.3f")) {
            const float nv = v;
            sim.editAgents([uid, field, nv](Agents& ag, RngBank&) {
                const int32_t s = ag.slotOfUid(uid);
                if (s < 0) return;
                const uint32_t sl = static_cast<uint32_t>(s);
                switch (field) {
                    case 0: ag.m_energy[sl] = nv; break;
                    case 1: ag.m_hydration[sl] = nv; break;
                    case 2: ag.m_health[sl] = nv; break;
                    case 3: ag.m_bodyTemp[sl] = nv; break;
                    case 4: ag.m_pain[sl] = nv; break;
                    case 5: ag.m_stress[sl] = nv; break;
                    case 6: ag.m_damage[sl] = nv; break;
                    default: break;
                }
            }, std::string("Edited ") + label);
        }
        if (desc && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", desc);
    };

    ImGui::TextDisabled("BODY");
    stateSlider("Energy", a.m_energy[slot], 0.0f, energyMax,
                "Usable energy reserve. Reaching zero starts damaging health.", 0);
    stateSlider("Hydration", a.m_hydration[slot], 0.0f, energyMax,
                "Water reserve. Dehydration damages health about twice as fast as starvation.", 1);
    stateSlider("Health", a.m_health[slot], 0.0f, 1.0f,
                "Reaching zero is death. Recovers slowly when well fed.", 2);
    stateSlider("Body temperature", a.m_bodyTemp[slot], -10.0f, 50.0f,
                "Maintained against ambient at a real energy cost.", 3);
    stateSlider("Pain", a.m_pain[slot], 0.0f, 1.0f, "Decays about 3% per hour.", 4);
    stateSlider("Stress", a.m_stress[slot], 0.0f, 1.0f,
                "Feeds the safety drive. Raised by injury and by the mating vulnerability window.", 5);
    stateSlider("Cellular damage", a.m_damage[slot], 0.0f, 1.0f,
                "Accumulates irreversibly and multiplies the Gompertz mortality hazard.", 6);
    labelValue("Telomere counter", "%.4f", static_cast<double>(a.m_telomere[slot]));

    ImGui::Separator();
    ImGui::TextDisabled("DRIVES  (deficit: 0 satisfied, 1 desperate)");
    for (int d = 0; d < kDriveCount; ++d) {
        const Drive drive = static_cast<Drive>(d);
        ImGui::PushID(100 + d);
        float v = a.m_drives[slot].level[d];
        ImGui::SetNextItemWidth(-140.0f);
        if (ImGui::SliderFloat(driveName(drive), &v, 0.0f, 1.0f, "%.3f")) {
            const int di = d;
            const float nv = v;
            sim.editAgents([uid, di, nv](Agents& ag, RngBank&) {
                const int32_t s = ag.slotOfUid(uid);
                if (s >= 0) ag.m_drives[static_cast<uint32_t>(s)].level[di] = nv;
            }, std::string("Edited drive ") + driveName(static_cast<Drive>(di)));
        }
        ImGui::PopID();
    }
    ImGui::SameLine();
    helpMarker("Reproductive drive accumulates over time in a healthy, well-fed adult and is "
               "discharged by a successful mating, which emits a large reward pulse into the "
               "learning system.");

    ImGui::Separator();
    ImGui::TextDisabled("STATUS EFFECTS AND DISEASE");
    ImGui::TextWrapped("Pathogens are their own evolving entities with transmissibility, "
                       "virulence, latency and host-genotype specificity against the MHC loci "
                       "above. That subsystem arrives in M5; this individual currently carries "
                       "no infections because none exist in the world yet.");
    bool sterile = (a.m_flags[slot] & Agents::Flag_Sterile) != 0;
    if (ImGui::Checkbox("Sterile", &sterile)) {
        sim.editAgents([uid, sterile](Agents& ag, RngBank&) {
            const int32_t s = ag.slotOfUid(uid);
            if (s < 0) return;
            if (sterile) ag.m_flags[static_cast<uint32_t>(s)] |= Agents::Flag_Sterile;
            else ag.m_flags[static_cast<uint32_t>(s)] &= static_cast<uint8_t>(~Agents::Flag_Sterile);
        }, sterile ? "Sterilised an individual" : "Restored fertility");
    }
}

void tabMemory(Simulation& sim, const Agents& a, uint32_t slot, uint64_t uid) {
    ImGui::TextDisabled("REMEMBERED LOCATIONS");
    if (a.m_memFoodX[slot] >= 0.0f)
        labelValue("Last good foraging", "(%.0f, %.0f)",
                   static_cast<double>(a.m_memFoodX[slot]), static_cast<double>(a.m_memFoodY[slot]));
    else ImGui::TextDisabled("No remembered foraging site.");
    if (a.m_memWaterX[slot] >= 0.0f)
        labelValue("Last water", "(%.0f, %.0f)",
                   static_cast<double>(a.m_memWaterX[slot]), static_cast<double>(a.m_memWaterY[slot]));
    else ImGui::TextDisabled("No remembered water source.");

    if (ImGui::Button("Erase spatial memory")) {
        sim.editAgents([uid](Agents& ag, RngBank&) {
            const int32_t s = ag.slotOfUid(uid);
            if (s < 0) return;
            const uint32_t sl = static_cast<uint32_t>(s);
            ag.m_memFoodX[sl] = ag.m_memFoodY[sl] = -1.0f;
            ag.m_memWaterX[sl] = ag.m_memWaterY[sl] = -1.0f;
        }, "Erased an individual's spatial memory");
    }

    ImGui::Separator();
    ImGui::TextDisabled("REMEMBERED INDIVIDUALS");
    ImGui::Text("%zu of %lld remembered", a.relationships(slot).size(),
                static_cast<long long>(cfg().getInt("agents.relationship_capacity", 24)));
    ImGui::TextDisabled("Social memory is finite, and that finiteness is itself a real constraint "
                        "on how large a stable group can be. See the Relationships tab.");

    ImGui::Separator();
    ImGui::TextDisabled("TECHNIQUES");
    ImGui::TextWrapped("What conditions this individual can actually bring about. Everything "
                       "below in the knowledge list was found under one of these, and nothing "
                       "that needs a hotter one is reachable however much it experiments.");
    for (int t = 0; t < static_cast<int>(Technique::Count); ++t) {
        const Technique tech = static_cast<Technique>(t);
        const bool has = a.knowledge().hasTechnique(slot, tech);
        if (t % 3 != 0) ImGui::SameLine();
        bool held = has;
        if (ImGui::Checkbox(techniqueName(tech), &held) && held != has) {
            const int ti = t;
            const bool want = held;
            sim.editAgents([uid, ti, want](Agents& ag, RngBank&) {
                const int32_t sl = ag.slotOfUid(uid);
                if (sl < 0) return;
                const uint32_t u = static_cast<uint32_t>(sl);
                uint32_t mask = ag.knowledge().techniqueMask(u);
                if (want) mask |= (1u << static_cast<uint32_t>(ti));
                else mask &= ~(1u << static_cast<uint32_t>(ti));
                ag.knowledge().setTechniqueMask(u, mask);
            }, std::string(want ? "Granted " : "Removed ") + techniqueName(tech) +
               " to an individual");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(400.0f);
            ImGui::Text("Reaches %.0f K.", techniqueTemperature(tech));
            ImGui::TextUnformatted(techniqueNote(tech));
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
    ImGui::Text("Hottest conditions available: %.0f K at %.2f atm",
                a.knowledge().bestTemperature(slot),
                a.knowledge().bestPressure(slot) / kStandardPressure);

    ImGui::Separator();
    ImGui::TextDisabled("KNOWLEDGE UNITS");
    const std::vector<KnowledgeUnit>& units = a.knowledge().known(slot);
    ImGui::Text("%zu held, capacity %lld.", units.size(),
                static_cast<long long>(cfg().getInt("knowledge.capacity", 64)));
    ImGui::TextDisabled("Fidelity below 0.35 means the unit has been garbled past usefulness in "
                        "transmission: still remembered, no longer works.");
    if (units.empty()) {
        ImGui::TextDisabled("This individual knows nothing yet.");
    } else if (ImGui::BeginTable("kunits", 6,
                                 ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                 ImGuiTableFlags_ScrollY, ImVec2(0, 200))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Knows how to");
        ImGui::TableSetupColumn("Via", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Fidelity", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Learned", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableHeadersRow();
        for (const KnowledgeUnit& k : units) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const Reaction* rx = chem().reaction(k.reactionId);
            if (k.usable()) ImGui::TextUnformatted(rx ? rx->name.c_str() : "?");
            else ImGui::TextDisabled("%s (garbled)", rx ? rx->name.c_str() : "?");
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", techniqueName(static_cast<Technique>(k.technique)));
            ImGui::TableNextColumn(); ImGui::Text("%.2f", static_cast<double>(k.valuation));
            ImGui::TableNextColumn(); ImGui::Text("%.2f", static_cast<double>(k.fidelity));
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(k.discoveredTick));
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(k.id));
            if (ImGui::SmallButton("Forget")) {
                const uint16_t rid = k.reactionId;
                sim.editAgents([uid, rid](Agents& ag, RngBank&) {
                    const int32_t sl = ag.slotOfUid(uid);
                    if (sl >= 0) ag.knowledge().forget(static_cast<uint32_t>(sl), rid);
                }, "Made an individual forget a reaction");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // Teaching a reaction directly. Granted at full fidelity, because divine
    // instruction does not garble.
    ImGui::SetNextItemWidth(300.0f);
    if (ImGui::BeginCombo("##teachrx", "Teach this individual a reaction")) {
        for (const Reaction& r : chem().reactions()) {
            if (!ImGui::Selectable(r.name.c_str())) continue;
            const uint16_t rid = r.id;
            const std::string rname = r.name;
            sim.editAgents([uid, rid](Agents& ag, RngBank&) {
                const int32_t sl = ag.slotOfUid(uid);
                if (sl < 0) return;
                ag.knowledge().grant(static_cast<uint32_t>(sl), rid, Technique::Mixing,
                                     1.0f, 1.0f, 0, uid);
            }, "Taught an individual: " + rname);
        }
        ImGui::EndCombo();
    }
}

void tabRelationships(Simulation& sim, UiState& ui, const Agents& a, uint32_t slot, uint64_t uid) {
    labelValue("Social status", "%.3f", static_cast<double>(a.m_status[slot]));
    labelValue("Reputation", "%+.3f", static_cast<double>(a.m_reputation[slot]));
    labelValue("Offspring", "%u", static_cast<unsigned>(a.m_offspringCount[slot]));
    if (a.m_bondedUid[slot] != 0) {
        const int32_t partner = slotOf(a, a.m_bondedUid[slot]);
        labelValue("Pair bond", "%s", (partner >= 0)
            ? a.m_name[static_cast<uint32_t>(partner)].c_str() : "(deceased)");
        if (ImGui::Button("Sever pair bond")) {
            const uint64_t partnerUid = a.m_bondedUid[slot];
            sim.editAgents([uid, partnerUid](Agents& ag, RngBank&) {
                const int32_t s = ag.slotOfUid(uid);
                const int32_t o = ag.slotOfUid(partnerUid);
                if (s >= 0 && o >= 0)
                    ag.forceBond(static_cast<uint32_t>(s), static_cast<uint32_t>(o), false);
            }, "Severed a pair bond");
        }
    } else {
        ImGui::TextDisabled("Not pair-bonded.");
    }
    if (a.m_pregnantByUid[slot] != 0) {
        labelValue("Gestating", "%.1f days remaining",
                   static_cast<double>(a.m_gestationRemaining[slot]) / 24.0);
    }

    ImGui::Separator();
    if (ImGui::BeginTable("rels", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_ScrollY, ImVec2(0, 260))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Individual", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Familiarity", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Affinity", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Rejections", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Ties", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("Force");
        ImGui::TableHeadersRow();

        for (const Relationship& r : a.relationships(slot)) {
            const int32_t other = slotOf(a, r.otherUid);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(static_cast<int>(r.otherUid & 0x7FFFFFFF));
            if (other >= 0) {
                if (ImGui::Selectable(a.m_name[static_cast<uint32_t>(other)].c_str()))
                    openIndividualCard(ui, r.otherUid);
            } else {
                ImGui::TextDisabled("%llu (dead)", static_cast<unsigned long long>(r.otherUid));
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.3f", static_cast<double>(r.familiarity));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(r.affinity >= 0.0f ? ImVec4(0.5f, 0.9f, 0.5f, 1.0f)
                                                  : ImVec4(0.9f, 0.5f, 0.5f, 1.0f),
                               "%+.3f", static_cast<double>(r.affinity));
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%u", static_cast<unsigned>(r.rejections));
            ImGui::TableSetColumnIndex(4);
            std::string ties;
            if (r.flags & Rel_Bonded)    ties += "bonded ";
            if (r.flags & Rel_Mated)     ties += "mated ";
            if (r.flags & Rel_Offspring) ties += "child ";
            if (r.flags & Rel_Parent)    ties += "parent ";
            if (r.flags & Rel_Rival)     ties += "rival ";
            if (r.flags & Rel_Ally)      ties += "ally ";
            ImGui::TextUnformatted(ties.empty() ? "-" : ties.c_str());
            ImGui::TableSetColumnIndex(5);
            if (other >= 0) {
                const uint64_t otherUid = r.otherUid;
                if (ImGui::SmallButton("bond")) {
                    sim.editAgents([uid, otherUid](Agents& ag, RngBank&) {
                        const int32_t s = ag.slotOfUid(uid);
                        const int32_t o = ag.slotOfUid(otherUid);
                        if (s >= 0 && o >= 0)
                            ag.forceBond(static_cast<uint32_t>(s), static_cast<uint32_t>(o), true);
                    }, "Forced a pair bond");
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("ally")) {
                    sim.editAgents([uid, otherUid](Agents& ag, RngBank&) {
                        const int32_t s = ag.slotOfUid(uid);
                        if (s < 0) return;
                        if (Relationship* rr = ag.relationship(static_cast<uint32_t>(s), otherUid, true)) {
                            rr->flags |= Rel_Ally;
                            rr->affinity = 1.0f;
                        }
                    }, "Forced an alliance");
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("sever")) {
                    sim.editAgents([uid, otherUid](Agents& ag, RngBank&) {
                        const int32_t s = ag.slotOfUid(uid);
                        if (s < 0) return;
                        if (Relationship* rr = ag.relationship(static_cast<uint32_t>(s), otherUid, false)) {
                            rr->flags = 0;
                            rr->affinity = 0.0f;
                            rr->familiarity = 0.0f;
                        }
                    }, "Severed a relationship");
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void drawAncestor(const Agents& a, UiState& ui, uint64_t uid, int depth, int maxDepth) {
    if (uid == 0 || depth > maxDepth) return;
    const PedigreeRecord* rec = a.pedigree().find(uid);
    if (!rec) { ImGui::TextDisabled("(ancestry beyond the retained pedigree)"); return; }

    const int32_t slot = a.slotOfUid(uid);
    char label[128];
    std::snprintf(label, sizeof(label), "%s%s##%llu",
                  rec->name.empty() ? "?" : rec->name.c_str(),
                  (slot >= 0) ? "" : " (deceased)",
                  static_cast<unsigned long long>(uid));

    const bool hasParents = rec->motherUid != 0 || rec->fatherUid != 0;
    if (!hasParents) {
        ImGui::Bullet();
        if (ImGui::Selectable(label)) openIndividualCard(ui, uid);
        return;
    }
    if (ImGui::TreeNodeEx(label, depth < 2 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) openIndividualCard(ui, uid);
        drawAncestor(a, ui, rec->motherUid, depth + 1, maxDepth);
        drawAncestor(a, ui, rec->fatherUid, depth + 1, maxDepth);
        ImGui::TreePop();
    }
}

void tabLineage(UiState& ui, const Agents& a, uint32_t slot, uint64_t uid) {
    const PedigreeRecord* rec = a.pedigree().find(uid);
    labelValue("Mother", "%llu", static_cast<unsigned long long>(a.m_motherUid[slot]));
    labelValue("Father", "%llu", static_cast<unsigned long long>(a.m_fatherUid[slot]));
    labelValue("Offspring so far", "%u", rec ? rec->offspringCount : 0u);

    ImGui::Separator();
    ImGui::TextDisabled("ANCESTRY");
    static int depth = 4;
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderInt("Generations", &depth, 1, 8);
    drawAncestor(a, ui, uid, 0, depth);

    ImGui::Separator();
    ImGui::TextDisabled("DESCENDANTS");
    int found = 0;
    for (uint32_t other : a.liveSlots()) {
        if (a.m_motherUid[other] != uid && a.m_fatherUid[other] != uid) continue;
        ImGui::Bullet();
        if (ImGui::Selectable(a.m_name[other].c_str())) openIndividualCard(ui, a.m_uid[other]);
        ++found;
    }
    if (found == 0) ImGui::TextDisabled("No living offspring.");

    ImGui::Separator();
    ImGui::TextDisabled("RELATEDNESS TO THE PINNED INDIVIDUAL");
    if (ui.compareAgentUid != 0 && ui.compareAgentUid != uid) {
        const float r = a.pedigree().relatedness(uid, ui.compareAgentUid);
        ImGui::Text("r = %.4f to uid %llu", static_cast<double>(r),
                    static_cast<unsigned long long>(ui.compareAgentUid));
        ImGui::TextDisabled("0.5 = parent/offspring or full sibling, 0.25 = half sibling or "
                            "grandparent, 0.125 = first cousin.");
    } else {
        ImGui::TextDisabled("Pin an individual for comparison from the Identity tab.");
    }
}

void tabInventory(Simulation& sim, const Agents& a, uint32_t slot, uint64_t uid) {
    ImGui::TextDisabled("CARRIED SUBSTANCES");
    ImGui::TextWrapped("What this individual is holding, in moles. These are real substances with "
                       "real thermodynamic data, and they are what it has to work with when it "
                       "experiments.");

    const Inventory& inv = a.inventory();
    int held = 0;
    if (ImGui::BeginTable("inv", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Formula", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Substance");
        ImGui::TableSetupColumn("Amount", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Mass", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < inv.slots(); ++i) {
            const uint16_t sid = inv.substanceAt(slot, i);
            const float amount = inv.amountAt(slot, i);
            if (sid == 0 || amount <= 0.0f) continue;
            ++held;
            const Substance* sub = chem().substance(sid);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(sub ? (sub->formulaText + phaseSuffix(sub->phase)).c_str()
                                       : "?");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(sub ? sub->name.c_str() : "unknown");
            ImGui::TableNextColumn(); ImGui::Text("%.3f mol", static_cast<double>(amount));
            ImGui::TableNextColumn();
            if (sub && !sub->nuclear)
                ImGui::Text("%.1f g", static_cast<double>(amount) * sub->molarMass());
            else ImGui::TextDisabled("--");
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            if (ImGui::SmallButton("Drop")) {
                const uint16_t s2 = sid;
                const float amt = amount;
                sim.editAgents([uid, s2, amt](Agents& ag, RngBank&) {
                    const int32_t sl = ag.slotOfUid(uid);
                    if (sl >= 0) ag.inventory().consume(static_cast<uint32_t>(sl), s2, amt);
                }, "Emptied an inventory slot");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (held == 0) ImGui::TextDisabled("Carrying nothing.");
    ImGui::TextDisabled("%d of %d slots used.", held, inv.slots());

    ImGui::SetNextItemWidth(300.0f);
    if (ImGui::BeginCombo("##giveSub", "Give this individual a substance")) {
        for (const Substance& sub : chem().substances()) {
            const std::string label = sub.formulaText + "  " + sub.name;
            if (!ImGui::Selectable(label.c_str())) continue;
            const uint16_t sid = sub.id;
            const std::string sname = sub.name;
            sim.editAgents([uid, sid](Agents& ag, RngBank&) {
                const int32_t sl = ag.slotOfUid(uid);
                if (sl >= 0) ag.inventory().add(static_cast<uint32_t>(sl), sid, 1.0f);
            }, "Gave an individual 1 mol of " + sname);
        }
        ImGui::EndCombo();
    }

    ImGui::Separator();
    // Per the economy rule: with no currency in the world, there is no wealth
    // row here at all. Not a greyed-out zero -- nothing.
    ImGui::TextDisabled("No currency exists in this world, so this tab shows possessions only. "
                        "A wealth row would appear here if one ever came into existence.");
}

void tabLog(Simulation& sim, const Agents& a, uint32_t slot, uint64_t uid) {
    (void)a;
    (void)slot;
    static std::vector<WorldEvent> events;
    sim.copyEvents(events, 20000);

    int shown = 0;
    if (ImGui::BeginTable("indlog", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_ScrollY, ImVec2(0, 320))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Event");
        ImGui::TableHeadersRow();
        for (const WorldEvent& e : events) {
            if (e.subject != uid) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", tickToDate(e.tick).toShortString().c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(eventKindName(e.kind));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(e.text.c_str());
            ++shown;
        }
        ImGui::EndTable();
    }
    if (shown == 0)
        ImGui::TextDisabled("Nothing recorded for this individual yet. Life events (birth, "
                            "adulthood, pair bonding, death) are written here as they happen, "
                            "within the retained event window.");
}

}  // namespace

// ---------------------------------------------------------------------------
// The card itself
// ---------------------------------------------------------------------------

void drawIndividualCards(Simulation& sim, UiState& ui, Viewport& vp) {
    const SimSnapshot snap = sim.snapshot();

    for (size_t i = 0; i < ui.openCards.size();) {
        const uint64_t uid = ui.openCards[i];
        bool open = true;
        char title[128];

        bool exists = false;
        uint32_t slot = 0;
        sim.readAgents([&](const Agents& a) {
            const int32_t s = a.slotOfUid(uid);
            if (s >= 0) { exists = true; slot = static_cast<uint32_t>(s); }
        });

        if (!exists) {
            // The individual died. Say so rather than silently closing the card.
            std::snprintf(title, sizeof(title), "Individual %llu (deceased)###card%llu",
                          static_cast<unsigned long long>(uid),
                          static_cast<unsigned long long>(uid));
            if (ImGui::Begin(title, &open)) {
                sim.readAgents([&](const Agents& a) {
                    const PedigreeRecord* rec = a.pedigree().find(uid);
                    if (rec) {
                        ImGui::Text("%s", rec->name.empty() ? "(unnamed)" : rec->name.c_str());
                        ImGui::Separator();
                        labelValue("Born", "%s", tickToDate(rec->birthTick).toString().c_str());
                        labelValue("Died", "%s", tickToDate(rec->deathTick).toString().c_str());
                        labelValue("Cause", "%s",
                                   deathCauseName(static_cast<DeathCause>(rec->deathCause)));
                        labelValue("Lifespan", "%.2f years",
                                   static_cast<double>(rec->deathTick - rec->birthTick) /
                                   static_cast<double>(kHoursPerYear));
                        labelValue("Offspring", "%u", rec->offspringCount);
                        ImGui::Separator();
                        ImGui::TextDisabled("The pedigree record survives death, so this "
                                            "individual still contributes to lineage and "
                                            "relatedness for its descendants.");
                    } else {
                        ImGui::TextDisabled("No pedigree record retained for uid %llu.",
                                            static_cast<unsigned long long>(uid));
                    }
                });
            }
            ImGui::End();
            if (!open) { ui.openCards.erase(ui.openCards.begin() + static_cast<long>(i)); continue; }
            ++i;
            continue;
        }

        std::string name;
        sim.readAgents([&](const Agents& a) { name = a.m_name[slot]; });
        std::snprintf(title, sizeof(title), "%s  [%llu]###card%llu", name.c_str(),
                      static_cast<unsigned long long>(uid),
                      static_cast<unsigned long long>(uid));

        ImGui::SetNextWindowSize(ImVec2(620, 560), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(title, &open)) {
            if (ImGui::IsWindowFocused()) ui.focusAgentUid = uid;

            if (ImGui::BeginTabBar("cardtabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
                // Each tab takes the read lock only for as long as it draws, so
                // the sim thread is never blocked longer than one panel.
                auto tab = [&](const char* label, void (*fn)(Simulation&, UiState&, Viewport&,
                                                             const Agents&, uint32_t, uint64_t,
                                                             uint64_t)) {
                    if (ImGui::BeginTabItem(label)) {
                        sim.readAgents([&](const Agents& a) {
                            fn(sim, ui, vp, a, slot, uid, snap.tick);
                        });
                        ImGui::EndTabItem();
                    }
                };
                tab("Identity", [](Simulation& s, UiState& u, Viewport& v, const Agents& a,
                                   uint32_t sl, uint64_t id, uint64_t tk) {
                    tabIdentity(s, u, v, a, sl, id, tk);
                });

                if (ImGui::BeginTabItem("Genome")) {
                    sim.readAgents([&](const Agents& a) { tabGenome(sim, ui, a, slot, uid); });
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Phenotype")) {
                    sim.readAgents([&](const Agents& a) { tabPhenotype(sim, a, slot, uid); });
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Attraction")) {
                    sim.readAgents([&](const Agents& a) { tabAttraction(sim, ui, vp, a, slot, uid); });
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Brain")) {
                    sim.readAgents([&](const Agents& a) { tabBrain(sim, ui, a, slot, uid); });
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Needs & State")) {
                    sim.readAgents([&](const Agents& a) { tabNeeds(sim, a, slot, uid); });
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Memory & Knowledge")) {
                    sim.readAgents([&](const Agents& a) { tabMemory(sim, a, slot, uid); });
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Relationships")) {
                    sim.readAgents([&](const Agents& a) { tabRelationships(sim, ui, a, slot, uid); });
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Lineage")) {
                    sim.readAgents([&](const Agents& a) { tabLineage(ui, a, slot, uid); });
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Inventory")) {
                    sim.readAgents([&](const Agents& a) { tabInventory(sim, a, slot, uid); });
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Log")) {
                    sim.readAgents([&](const Agents& a) { tabLog(sim, a, slot, uid); });
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();

        if (!open) ui.openCards.erase(ui.openCards.begin() + static_cast<long>(i));
        else ++i;
    }
}

// ---------------------------------------------------------------------------
// Brain Inspector
// ---------------------------------------------------------------------------

void drawBrainInspector(Simulation& sim, UiState& ui) {
    if (!ImGui::Begin("Brain inspector", &ui.showBrainInspector)) { ImGui::End(); return; }

    const uint64_t uid = ui.focusAgentUid;
    if (uid == 0) {
        ImGui::TextDisabled("Select an individual and open its Brain tab, or click an agent in "
                            "the world view.");
        ImGui::End();
        return;
    }

    sim.readAgents([&](const Agents& a) {
        const int32_t s = a.slotOfUid(uid);
        if (s < 0) { ImGui::TextDisabled("That individual is dead."); return; }
        const uint32_t slot = static_cast<uint32_t>(s);
        const Brains& b = a.brains();
        ConstBrainView bv = b.brain(slot);
        const BrainRuntime& rt = b.runtime(slot);

        ImGui::Text("%s   %u neurons (%u hidden), %u/%u synapses enabled",
                    a.m_name[slot].c_str(), bv.nodeCount, b.hiddenNodeCount(slot),
                    b.enabledConnCount(slot), bv.connCount);
        ImGui::Checkbox("Show weights", &ui.brainShowWeights);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        ImGui::SliderFloat("Node size", &ui.brainNodeScale, 0.4f, 3.0f, "%.2f");

        // --- network graph ---
        ImGui::Separator();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float graphH = std::max(220.0f, avail.y * 0.45f);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float graphW = avail.x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(origin, ImVec2(origin.x + graphW, origin.y + graphH),
                          IM_COL32(14, 16, 20, 255), 3.0f);

        // Layout: inputs down the left, outputs down the right, hidden nodes in
        // a band between, positioned by a stable hash of their id so the layout
        // does not jitter between frames.
        std::vector<ImVec2> pos(bv.nodeCount);
        const uint16_t hiddenTotal = b.hiddenNodeCount(slot);
        uint16_t hiddenSeen = 0;
        for (uint16_t i = 0; i < bv.nodeCount; ++i) {
            const NodeKind kind = static_cast<NodeKind>(bv.nodes[i].kind);
            if (kind == NodeKind::Input) {
                pos[i] = ImVec2(origin.x + 26.0f,
                                origin.y + 12.0f + (graphH - 24.0f) *
                                static_cast<float>(i) / static_cast<float>(kBrainInputCount));
            } else if (kind == NodeKind::Output) {
                const int o = static_cast<int>(bv.nodes[i].id) - kBrainInputCount;
                pos[i] = ImVec2(origin.x + graphW - 26.0f,
                                origin.y + 12.0f + (graphH - 24.0f) *
                                static_cast<float>(o) / static_cast<float>(kBrainOutputCount));
            } else {
                const uint32_t h = bv.nodes[i].id * 2654435761u;
                const float fx = 0.30f + 0.40f * (static_cast<float>(h & 0xFFFF) / 65535.0f);
                const float fy = (hiddenTotal > 0)
                    ? (static_cast<float>(hiddenSeen) + 0.5f) / static_cast<float>(hiddenTotal)
                    : 0.5f;
                pos[i] = ImVec2(origin.x + graphW * fx, origin.y + 12.0f + (graphH - 24.0f) * fy);
                ++hiddenSeen;
            }
        }

        for (uint16_t c = 0; c < bv.connCount; ++c) {
            if ((bv.conns[c].flags & ConnFlag_Enabled) == 0) continue;
            const int f = Brains::nodeIndexOf(bv.nodes, bv.nodeCount, bv.conns[c].from);
            const int t = Brains::nodeIndexOf(bv.nodes, bv.nodeCount, bv.conns[c].to);
            if (f < 0 || t < 0) continue;
            const float w = rt.weight[c];
            const int alpha = static_cast<int>(std::min(220.0f, 40.0f + std::fabs(w) * 90.0f));
            ImU32 col = (w >= 0.0f) ? IM_COL32(90, 190, 255, alpha) : IM_COL32(255, 120, 90, alpha);
            if (bv.conns[c].flags & ConnFlag_Recurrent) col = IM_COL32(220, 180, 80, alpha);
            dl->AddLine(pos[f], pos[t], col, std::min(3.0f, 0.5f + std::fabs(w) * 0.5f));
        }
        for (uint16_t i = 0; i < bv.nodeCount; ++i) {
            const float act = rt.activation[i];
            const int v = static_cast<int>(std::min(255.0f, std::fabs(act) * 255.0f));
            const ImU32 col = (act >= 0.0f) ? IM_COL32(60 + v * 3 / 4, 200, 120, 255)
                                            : IM_COL32(220, 90 + v / 3, 90, 255);
            const float r = (static_cast<NodeKind>(bv.nodes[i].kind) == NodeKind::Hidden ? 5.0f : 3.5f)
                            * ui.brainNodeScale;
            dl->AddCircleFilled(pos[i], r, col, 10);
        }
        ImGui::Dummy(ImVec2(graphW, graphH));
        ImGui::TextDisabled("Blue = excitatory, red = inhibitory, amber = recurrent. Node colour "
                            "is live activation. Recurrent edges read the previous tick, which is "
                            "what gives the network memory.");

        // --- weight matrix ---
        ImGui::Separator();
        ImGui::TextDisabled("SYNAPSE TABLE  (weights editable cell by cell)");
        if (ImGui::BeginTable("weights", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                            ImGuiTableFlags_ScrollY, ImVec2(0, 240))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableSetupColumn("From", ImGuiTableColumnFlags_WidthFixed, 140.0f);
            ImGui::TableSetupColumn("To", ImGuiTableColumnFlags_WidthFixed, 140.0f);
            ImGui::TableSetupColumn("Expressed w", ImGuiTableColumnFlags_WidthFixed, 130.0f);
            ImGui::TableSetupColumn("Allele A", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Allele B", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Plastic", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("Trace");
            ImGui::TableHeadersRow();

            auto nodeLabel = [&](uint32_t id) -> const char* {
                if (id < kBrainInputCount) return brainInputName(static_cast<int>(id));
                if (id < kBrainInputCount + kBrainOutputCount)
                    return brainOutputName(static_cast<int>(id) - kBrainInputCount);
                return nullptr;
            };

            for (uint16_t c = 0; c < bv.connCount; ++c) {
                ImGui::TableNextRow();
                ImGui::PushID(c);
                ImGui::TableSetColumnIndex(0);
                if (bv.conns[c].flags & ConnFlag_Enabled) ImGui::Text("%u", c);
                else ImGui::TextDisabled("%u", c);
                ImGui::TableSetColumnIndex(1);
                if (const char* n = nodeLabel(bv.conns[c].from)) ImGui::TextUnformatted(n);
                else ImGui::Text("hidden#%u", bv.conns[c].from);
                ImGui::TableSetColumnIndex(2);
                if (const char* n = nodeLabel(bv.conns[c].to)) ImGui::TextUnformatted(n);
                else ImGui::Text("hidden#%u", bv.conns[c].to);
                ImGui::TableSetColumnIndex(3);
                float w = rt.weight[c];
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::DragFloat("##w", &w, 0.01f, -8.0f, 8.0f, "%.4f")) {
                    const uint16_t ci = c;
                    const float nw = w;
                    sim.editAgents([uid, ci, nw](Agents& ag, RngBank&) {
                        const int32_t sl = ag.slotOfUid(uid);
                        if (sl < 0) return;
                        BrainRuntime& r = ag.brains().runtime(static_cast<uint32_t>(sl));
                        BrainView v = ag.brains().brain(static_cast<uint32_t>(sl));
                        if (ci >= v.connCount) return;
                        r.weight[ci] = nw;
                        // Write the alleles too, so the edit is heritable rather
                        // than washed out at the next develop().
                        v.conns[ci].weightA = nw;
                        v.conns[ci].weightB = nw;
                    }, "Edited a synapse weight");
                }
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%+.3f", static_cast<double>(bv.conns[c].weightA));
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%+.3f", static_cast<double>(bv.conns[c].weightB));
                ImGui::TableSetColumnIndex(6);
                ImGui::TextUnformatted((bv.conns[c].flags & ConnFlag_Plastic) ? "yes" : "-");
                ImGui::TableSetColumnIndex(7);
                ImGui::Text("%+.4f", static_cast<double>(rt.eligibility[c]));
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    });

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Genome Browser
// ---------------------------------------------------------------------------

void drawGenomeBrowser(Simulation& sim, UiState& ui) {
    if (!ImGui::Begin("Genome browser", &ui.showGenomeBrowser)) { ImGui::End(); return; }

    const uint64_t uid = ui.focusAgentUid;
    if (uid == 0) {
        ImGui::TextDisabled("Select an individual first.");
        ImGui::End();
        return;
    }

    sim.readAgents([&](const Agents& a) {
        const int32_t s = a.slotOfUid(uid);
        if (s < 0) { ImGui::TextDisabled("That individual is dead."); return; }
        const uint32_t slot = static_cast<uint32_t>(s);
        const Genetics& g = a.genetics();
        ConstGenomeView genome = g.genome(slot);

        ImGui::Text("%s   %u genes across %d chromosomes", a.m_name[slot].c_str(),
                    genome.count, g.map().chromosomeCount);

        ImGui::SetNextItemWidth(150.0f);
        ImGui::Combo("Chromosome", &ui.genomeChromosome,
                     "All\0" "0\0" "1\0" "2\0" "3\0" "4\0" "5\0" "6\0" "7\0" "8\0" "9\0");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        ImGui::Combo("Locus type", &ui.genomeLocusFilter,
                     "All\0Coding\0Regulatory\0Junk\0MHC\0Sex\0");
        ImGui::SameLine();
        ImGui::Checkbox("Heterozygosity heatmap", &ui.genomeShowHeatmap);

        // Heterozygosity strip: one column per gene, in map order.
        if (ui.genomeShowHeatmap && genome.count > 0) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            const float h = 26.0f;
            const float cw = w / static_cast<float>(genome.count);
            for (uint16_t i = 0; i < genome.count; ++i) {
                const Gene& gene = genome.genes[i];
                const bool hemi = (gene.flags & GeneFlag_Hemizygous) != 0;
                const float het = hemi ? 0.0f : std::min(1.0f, std::fabs(gene.alleleA - gene.alleleB) / 2.0f);
                ImU32 col = IM_COL32(static_cast<int>(40 + 200 * (1.0f - het)),
                                     static_cast<int>(40 + 180 * het), 70, 255);
                if (alleleIsBroken(gene.alleleA) || alleleIsBroken(gene.alleleB))
                    col = IM_COL32(220, 40, 40, 255);
                dl->AddRectFilled(ImVec2(p.x + cw * i, p.y),
                                  ImVec2(p.x + cw * (i + 1), p.y + h), col);
                // A tick at each chromosome boundary.
                if (i > 0 && genome.genes[i - 1].chromosome != gene.chromosome)
                    dl->AddLine(ImVec2(p.x + cw * i, p.y - 2.0f),
                                ImVec2(p.x + cw * i, p.y + h + 2.0f), IM_COL32(255, 255, 255, 200));
            }
            ImGui::Dummy(ImVec2(w, h + 4.0f));
            ImGui::TextDisabled("Green = heterozygous, grey = homozygous, red = broken allele. "
                                "White ticks mark chromosome boundaries.");
        }

        ImGui::Separator();
        if (ImGui::BeginTable("genes", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                              ImVec2(0, 420))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Chr", ImGuiTableColumnFlags_WidthFixed, 36.0f);
            ImGui::TableSetupColumn("cM", ImGuiTableColumnFlags_WidthFixed, 52.0f);
            ImGui::TableSetupColumn("id", ImGuiTableColumnFlags_WidthFixed, 48.0f);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Target / effect", ImGuiTableColumnFlags_WidthFixed, 190.0f);
            ImGui::TableSetupColumn("Dominance", ImGuiTableColumnFlags_WidthFixed, 96.0f);
            ImGui::TableSetupColumn("Allele A", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn("Allele B", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn("Expressed");
            ImGui::TableHeadersRow();

            for (uint16_t i = 0; i < genome.count; ++i) {
                const Gene& gene = genome.genes[i];
                if (ui.genomeChromosome > 0 && gene.chromosome != ui.genomeChromosome - 1) continue;
                if (ui.genomeLocusFilter > 0 && gene.type != ui.genomeLocusFilter - 1) continue;

                ImGui::TableNextRow();
                ImGui::PushID(i);
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%u", static_cast<unsigned>(gene.chromosome));
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.1f", static_cast<double>(gene.mapPos));
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%u", static_cast<unsigned>(gene.id));
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(locusTypeName(static_cast<LocusType>(gene.type)));
                ImGui::TableSetColumnIndex(4);
                {
                    const LocusType lt = static_cast<LocusType>(gene.type);
                    if (lt == LocusType::Coding || lt == LocusType::Regulatory)
                        ImGui::Text("%s  x%.2f",
                                    traitSpec(static_cast<Trait>(
                                        gene.target < kTraitCount ? gene.target : 0)).name,
                                    static_cast<double>(gene.effect));
                    else if (lt == LocusType::Mhc) ImGui::Text("MHC[%u]", gene.target & 7);
                    else ImGui::TextDisabled("-");
                }
                ImGui::TableSetColumnIndex(5);
                ImGui::TextUnformatted(dominanceName(static_cast<Dominance>(gene.dominance)));

                auto alleleCell = [&](bool isA) {
                    float v = isA ? gene.alleleA : gene.alleleB;
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::PushID(isA ? 0 : 1);
                    const bool changed = ImGui::DragFloat("##a", &v, 0.01f, -12.0f, 12.0f, "%+.4f");
                    ImGui::PopID();
                    if (changed) {
                        const uint16_t gi = i;
                        const float nv = v;
                        const bool a_ = isA;
                        sim.editAgents([uid, gi, nv, a_](Agents& ag, RngBank& rng) {
                            const int32_t sl = ag.slotOfUid(uid);
                            if (sl < 0) return;
                            GenomeView gv = ag.genetics().genome(static_cast<uint32_t>(sl));
                            if (gi >= gv.count) return;
                            if (a_) gv.genes[gi].alleleA = nv; else gv.genes[gi].alleleB = nv;
                            // Re-express immediately: an allele edit must show
                            // up in the phenotype on the very next tick.
                            ag.redevelop(static_cast<uint32_t>(sl), rng);
                        }, "Edited an allele");
                    }
                };
                ImGui::TableSetColumnIndex(6);
                if (alleleIsBroken(gene.alleleA))
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                alleleCell(true);
                if (alleleIsBroken(gene.alleleA)) ImGui::PopStyleColor();

                ImGui::TableSetColumnIndex(7);
                if (gene.flags & GeneFlag_Hemizygous) ImGui::TextDisabled("(hemizygous)");
                else {
                    if (alleleIsBroken(gene.alleleB))
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                    alleleCell(false);
                    if (alleleIsBroken(gene.alleleB)) ImGui::PopStyleColor();
                }

                ImGui::TableSetColumnIndex(8);
                const float expressed = (gene.flags & GeneFlag_Hemizygous)
                    ? gene.alleleA * 0.5f
                    : expressAllele(gene.alleleA, gene.alleleB,
                                    static_cast<Dominance>(gene.dominance));
                ImGui::Text("%+.4f", static_cast<double>(expressed));
                if (gene.flags & GeneFlag_CanCarryLethal) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("[lethal-capable]");
                }
                if (gene.flags & GeneFlag_Inverted) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("[inverted]");
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    });

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Population search
// ---------------------------------------------------------------------------

void drawPopulationSearch(Simulation& sim, UiState& ui, Viewport& vp) {
    if (!ImGui::Begin("Population", &ui.showPopulation)) { ImGui::End(); return; }

    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputTextWithHint("##name", "name contains", ui.popNameFilter, sizeof(ui.popNameFilter));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    ImGui::Combo("Stage", &ui.popStageFilter,
                 "Any\0Embryo\0Juvenile\0Adolescent\0Adult\0Senescent\0");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::DragFloat("min age", &ui.popMinAge, 0.5f, 0.0f, 500.0f, "%.0f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::DragFloat("max age", &ui.popMaxAge, 0.5f, 0.0f, 500.0f, "%.0f");
    ImGui::SameLine();
    ImGui::Checkbox("Tagged only", &ui.popOnlyTagged);

    // Trait-range filter over any trait.
    {
        std::string names;
        names += "(no trait filter)";
        names.push_back('\0');
        for (int t = 0; t < kTraitCount; ++t) {
            names += traitSpec(static_cast<Trait>(t)).name;
            names.push_back('\0');
        }
        names.push_back('\0');
        int sel = ui.popTraitFilter + 1;
        ImGui::SetNextItemWidth(190.0f);
        if (ImGui::Combo("Trait", &sel, names.c_str())) ui.popTraitFilter = sel - 1;
        if (ui.popTraitFilter >= 0) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0f);
            ImGui::DragFloat("min##t", &ui.popTraitMin, 0.01f);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0f);
            ImGui::DragFloat("max##t", &ui.popTraitMax, 0.01f);
        }
    }

    const SimSnapshot snap = sim.snapshot();
    const size_t nameLen = std::strlen(ui.popNameFilter);
    int shown = 0;

    sim.readAgents([&](const Agents& a) {
        ImGui::TextDisabled("%u alive", a.population());
        if (!ImGui::BeginTable("pop", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                         ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                               ImVec2(0, 0))) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Stage", ImGuiTableColumnFlags_WidthFixed, 88.0f);
        ImGui::TableSetupColumn("Age", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("Sex expr", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Health", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Energy", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Ornament", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("F", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("Location");
        ImGui::TableHeadersRow();

        for (uint32_t slot : a.liveSlots()) {
            if (ui.popOnlyTagged && !a.tagged(slot)) continue;
            if (ui.popStageFilter > 0 && a.m_stage[slot] != ui.popStageFilter - 1) continue;
            if (nameLen > 0 && a.m_name[slot].find(ui.popNameFilter) == std::string::npos) continue;
            const float age = (snap.tick > a.m_birthTick[slot])
                ? static_cast<float>(snap.tick - a.m_birthTick[slot]) /
                  static_cast<float>(kHoursPerYear) : 0.0f;
            if (age < ui.popMinAge || age > ui.popMaxAge) continue;
            if (ui.popTraitFilter >= 0) {
                const float tv = a.m_phenotype[slot].traits[ui.popTraitFilter];
                if (tv < ui.popTraitMin || tv > ui.popTraitMax) continue;
            }
            if (++shown > 2000) break;

            const Phenotype& p = a.m_phenotype[slot];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(static_cast<int>(slot));
            if (ImGui::Selectable(a.m_name[slot].c_str(), false,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                openIndividualCard(ui, a.m_uid[slot]);
                vp.focusOn(static_cast<int>(a.m_x[slot]), static_cast<int>(a.m_y[slot]));
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(lifeStageName(static_cast<LifeStage>(a.m_stage[slot])));
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.1f", static_cast<double>(age));
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.2f", static_cast<double>(p.get(Trait::SexExpression)));
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.2f", static_cast<double>(a.m_health[slot]));
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%.0f", static_cast<double>(a.m_energy[slot]));
            ImGui::TableSetColumnIndex(6);
            ImGui::Text("%.2f", static_cast<double>(
                0.5f * (p.get(Trait::Ornament1) + p.get(Trait::Ornament2))));
            ImGui::TableSetColumnIndex(7);
            ImGui::Text("%.3f", static_cast<double>(p.inbreedingF));
            ImGui::TableSetColumnIndex(8);
            ImGui::TextDisabled("(%.0f, %.0f)", static_cast<double>(a.m_x[slot]),
                                static_cast<double>(a.m_y[slot]));
            ImGui::PopID();
        }
        ImGui::EndTable();
    });

    ImGui::End();
}

}  // namespace gen
