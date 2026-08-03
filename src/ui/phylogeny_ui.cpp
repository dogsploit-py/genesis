// ui/phylogeny_ui.cpp — the Phylogeny panel.
//
// Two trees, because there are two genuinely different questions to ask.
//
// The SPECIES tree is drawn against a time axis, so a branch's horizontal extent
// is how long that lineage actually lasted and the fork is the tick the split was
// detected. Branch depth is inherited, not computed from a layout algorithm,
// which means the tree reads the same way twice in a row -- a phylogeny that
// rearranges itself between frames is unreadable.
//
// The LINEAGE tree is one individual's ancestors and descendants out of the
// pedigree. That is a different structure with a different question behind it:
// not "where did this population come from" but "who is this one related to".
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/config.h"
#include "imgui.h"
#include "sim/time.h"
#include "ui/app.h"

namespace gen {

namespace {

// A stable colour per lineage. Derived from the id by a hash rather than cycled
// from a palette, so a lineage keeps its colour as others appear and vanish.
ImU32 speciesColour(uint32_t id, float alpha = 1.0f) {
    uint32_t h = id * 2654435761u;
    h ^= h >> 15;
    const float hue = static_cast<float>(h % 360u) / 360.0f;
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(hue, 0.62f, 0.92f, r, g, b);
    return ImGui::GetColorU32(ImVec4(r, g, b, alpha));
}

struct Row {
    uint32_t id;
    int      depth;
    float    y;
};

// Depth-first walk of the lineage tree, so a daughter is always drawn directly
// under its parent and siblings sit together.
void collectRows(const std::vector<SpeciesRecord>& species, uint32_t parentId, int depth,
                 std::vector<Row>& out) {
    for (const SpeciesRecord& s : species) {
        if (s.parentId != parentId) continue;
        Row r;
        r.id = s.id;
        r.depth = depth;
        r.y = 0.0f;
        out.push_back(r);
        collectRows(species, s.id, depth + 1, out);
    }
}

void drawSpeciesTree(Simulation& sim, UiState& ui) {
    struct Snap {
        std::vector<SpeciesRecord> species;
        double scale = 0.0;
        uint32_t sample = 0;
        uint64_t tick = 0;
    };
    static Snap snap;
    sim.readAgents([&](const Agents& a) {
        snap.species = a.speciation().species();
        snap.scale = a.speciation().scale();
        snap.sample = a.speciation().lastSampleSize();
    });
    snap.tick = sim.snapshot().tick;

    if (snap.species.empty()) {
        ImGui::TextDisabled(
            "No lineage has been named yet. Detection needs a population and a pass of the "
            "detector; if the population is tiny or brand new there is genuinely nothing to "
            "report, and a placeholder tree would be a lie.");
        return;
    }

    ImGui::TextDisabled(
        "Neutral spacing in this population is %.4f (median nearest-neighbour distance over "
        "%u sampled individuals). A gap of %.1f times that counts as a species boundary.",
        snap.scale, snap.sample, cfg().getFloat("species.gap_factor", 4.0));
    helpMarker(
        "The threshold is relative to the population's own spacing rather than absolute, "
        "because a species boundary is a discontinuity and not a distance. Two absolute or "
        "variance-normalised alternatives were tried first and both failed -- see the comment "
        "at the top of sim/species.h for why.");

    ImGui::Checkbox("Show extinct lineages", &ui.phyloShowExtinct);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Row height", &ui.phyloRowHeight, 14.0f, 48.0f, "%.0f px");

    std::vector<Row> rows;
    collectRows(snap.species, 0u, 0, rows);
    // Anything whose parent is not in the list (a lineage whose parent record was
    // dropped) would otherwise vanish, so pick up strays at depth 0.
    for (const SpeciesRecord& s : snap.species) {
        bool present = false;
        for (const Row& r : rows) if (r.id == s.id) { present = true; break; }
        if (present) continue;
        Row r;
        r.id = s.id;
        r.depth = 0;
        r.y = 0.0f;
        rows.push_back(r);
    }

    auto recordOf = [&](uint32_t id) -> const SpeciesRecord* {
        for (const SpeciesRecord& s : snap.species) if (s.id == id) return &s;
        return nullptr;
    };

    // Time axis. From the earliest origin to now, so the whole history fits.
    uint64_t firstTick = snap.tick;
    for (const SpeciesRecord& s : snap.species) firstTick = std::min(firstTick, s.firstTick);
    const double span = std::max(1.0, static_cast<double>(snap.tick - firstTick));

    const float rowH = ui.phyloRowHeight;
    const float labelW = 230.0f;
    const float pad = 10.0f;

    int visible = 0;
    for (const Row& r : rows) {
        const SpeciesRecord* s = recordOf(r.id);
        if (!s) continue;
        if (!s->extant && !ui.phyloShowExtinct) continue;
        ++visible;
    }
    if (visible == 0) {
        ImGui::TextDisabled("Every named lineage is extinct. Tick \"Show extinct lineages\".");
        return;
    }

    const float height = static_cast<float>(visible) * rowH + 40.0f;
    ImGui::BeginChild("phylotree", ImVec2(0, std::min(height + 20.0f, 520.0f)),
                      ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float availW = std::max(360.0f, ImGui::GetContentRegionAvail().x);
    const float plotX0 = origin.x + labelW;
    const float plotW = std::max(120.0f, availW - labelW - pad);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    auto tickToX = [&](uint64_t t) {
        const double f = static_cast<double>(t - firstTick) / span;
        return plotX0 + static_cast<float>(f) * plotW;
    };

    // Year gridlines, at whatever interval keeps them legible.
    {
        const double years = span / static_cast<double>(kHoursPerYear);
        double step = 1.0;
        while (years / step > 12.0) step *= (step < 5.0 ? 5.0 : 2.0);
        const ImU32 grid = ImGui::GetColorU32(ImVec4(1, 1, 1, 0.07f));
        for (double yr = 0.0; yr <= years + 1e-9; yr += step) {
            const uint64_t t = firstTick + static_cast<uint64_t>(yr * kHoursPerYear);
            const float gx = tickToX(t);
            dl->AddLine(ImVec2(gx, origin.y), ImVec2(gx, origin.y + height - 26.0f), grid);
            char buf[32];
            std::snprintf(buf, sizeof buf, "yr %.0f",
                          static_cast<double>(firstTick) / kHoursPerYear + yr);
            dl->AddText(ImVec2(gx + 2.0f, origin.y + height - 22.0f),
                        ImGui::GetColorU32(ImGuiCol_TextDisabled), buf);
        }
    }

    // Assign screen rows in walk order, then draw.
    std::unordered_map<uint32_t, float> rowY;
    int slot = 0;
    for (Row& r : rows) {
        const SpeciesRecord* s = recordOf(r.id);
        if (!s) continue;
        if (!s->extant && !ui.phyloShowExtinct) continue;
        r.y = origin.y + static_cast<float>(slot) * rowH + rowH * 0.5f;
        rowY[r.id] = r.y;
        ++slot;
    }

    for (const Row& r : rows) {
        const SpeciesRecord* s = recordOf(r.id);
        if (!s) continue;
        if (!s->extant && !ui.phyloShowExtinct) continue;

        const float y = r.y;
        const ImU32 col = speciesColour(s->id, s->extant ? 1.0f : 0.45f);
        const float x0 = tickToX(s->firstTick);
        const float x1 = s->extant ? tickToX(snap.tick) : tickToX(s->lastTick);

        // The branch. Thickness scales with population, so an abundant lineage
        // reads as abundant.
        const float thickness = 2.0f + std::min(7.0f,
            std::sqrt(static_cast<float>(s->population)) * 0.45f);
        dl->AddLine(ImVec2(x0, y), ImVec2(std::max(x1, x0 + 2.0f), y), col, thickness);

        // The elbow back to the parent's row at the fork tick.
        if (s->parentId != 0) {
            auto it = rowY.find(s->parentId);
            if (it != rowY.end()) {
                dl->AddLine(ImVec2(x0, it->second), ImVec2(x0, y), col, 1.6f);
            }
        }

        // A cross at the end of an extinct branch.
        if (!s->extant) {
            const float e = 4.0f;
            dl->AddLine(ImVec2(x1 - e, y - e), ImVec2(x1 + e, y + e), col, 1.8f);
            dl->AddLine(ImVec2(x1 - e, y + e), ImVec2(x1 + e, y - e), col, 1.8f);
        }

        // The label, indented by depth so the nesting is visible even where the
        // branches are short.
        char label[160];
        std::snprintf(label, sizeof label, "%s%s  (%u)",
                      std::string(static_cast<size_t>(r.depth) * 2, ' ').c_str(),
                      s->name.c_str(), s->population);
        dl->AddText(ImVec2(origin.x + 4.0f, y - ImGui::GetFontSize() * 0.5f), col, label);

        // Hover for the detail. Placed over the label and the branch together.
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y - rowH * 0.5f));
        ImGui::InvisibleButton(s->name.c_str(), ImVec2(availW - pad, rowH));
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(s->name.c_str());
            ImGui::Separator();
            ImGui::Text("Named          year %.2f", static_cast<double>(s->firstTick) / kHoursPerYear);
            if (!s->extant)
                ImGui::Text("Extinct        year %.2f", static_cast<double>(s->lastTick) / kHoursPerYear);
            ImGui::Text("Population     %u  (peak %u)", s->population, s->peakPopulation);
            ImGui::Text("Dispersion     %.4f", s->dispersion);
            ImGui::Text("Centroid drift %.4f", s->drift);
            if (s->parentId != 0) {
                const SpeciesRecord* p = recordOf(s->parentId);
                ImGui::Text("Split from     %s", p ? p->name.c_str() : "?");
                ImGui::Text("Gap at split   %.4f  (%.1fx spacing)", s->splitDistance,
                            snap.scale > 0.0 ? s->splitDistance / snap.scale : 0.0);
            } else {
                ImGui::TextDisabled("A founding lineage. Nothing split off to make it.");
            }
            if (s->centroidX >= 0)
                ImGui::Text("Centred at     (%d, %d)", s->centroidX, s->centroidY);
            ImGui::EndTooltip();
        }
        if (ImGui::IsItemClicked() && s->centroidX >= 0) {
            ui.setStatus(s->name + " is centred at (" +
                         std::to_string(s->centroidX) + ", " +
                         std::to_string(s->centroidY) + ")");
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + height));
    ImGui::Dummy(ImVec2(availW, 1.0f));
    ImGui::EndChild();

    // The table underneath, because a tree is good for structure and bad for
    // reading numbers off.
    if (ImGui::BeginTable("phylotable", 8,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                          ImVec2(0, 170.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Lineage");
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Named yr", ImGuiTableColumnFlags_WidthFixed, 75.0f);
        ImGui::TableSetupColumn("Pop", ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableSetupColumn("Peak", ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableSetupColumn("Spread", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Drift", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Split from");
        ImGui::TableHeadersRow();
        for (const SpeciesRecord& s : snap.species) {
            if (!s.extant && !ui.phyloShowExtinct) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, speciesColour(s.id));
            ImGui::TextUnformatted(s.name.c_str());
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            if (s.extant) ImGui::TextUnformatted("extant");
            else ImGui::TextColored(ImVec4(0.90f, 0.45f, 0.40f, 1.0f), "extinct");
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", static_cast<double>(s.firstTick) / kHoursPerYear);
            ImGui::TableNextColumn(); ImGui::Text("%u", s.population);
            ImGui::TableNextColumn(); ImGui::Text("%u", s.peakPopulation);
            ImGui::TableNextColumn(); ImGui::Text("%.4f", s.dispersion);
            ImGui::TableNextColumn(); ImGui::Text("%.4f", s.drift);
            ImGui::TableNextColumn();
            if (s.parentId == 0) {
                ImGui::TextDisabled("--");
            } else {
                const SpeciesRecord* p = recordOf(s.parentId);
                ImGui::Text("%s (%.3f)", p ? p->name.c_str() : "?", s.splitDistance);
            }
        }
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------------------

void drawLineageTree(Simulation& sim, UiState& ui) {
    if (ui.focusAgentUid == 0) {
        ImGui::TextDisabled("No individual is focused. Open an Individual Card first -- this tree "
                            "is one agent's ancestry and descent, so it needs an agent.");
        return;
    }

    struct Node {
        uint64_t uid = 0;
        std::string name;
        int generation = 0;      // negative = ancestor, positive = descendant
        uint64_t birthTick = 0;
        uint64_t deathTick = 0;
        bool alive = false;
        uint64_t mother = 0, father = 0;
    };
    std::vector<Node> nodes;
    std::string rootName;
    const int depth = ui.lineageDepth;

    sim.readAgents([&](const Agents& a) {
        const Pedigree& ped = a.pedigree();
        const PedigreeRecord* root = ped.find(ui.focusAgentUid);
        if (!root) return;
        rootName = root->name;

        auto push = [&](const PedigreeRecord& r, int gen) {
            for (const Node& n : nodes) if (n.uid == r.uid) return;
            Node n;
            n.uid = r.uid;
            n.name = r.name;
            n.generation = gen;
            n.birthTick = r.birthTick;
            n.deathTick = r.deathTick;
            n.alive = r.deathTick == 0;
            n.mother = r.motherUid;
            n.father = r.fatherUid;
            nodes.push_back(std::move(n));
        };
        push(*root, 0);

        // Ancestors, breadth-first up to the requested depth.
        std::vector<std::pair<uint64_t, int>> frontier;
        frontier.push_back({root->motherUid, -1});
        frontier.push_back({root->fatherUid, -1});
        while (!frontier.empty()) {
            const auto cur = frontier.back();
            frontier.pop_back();
            if (cur.first == 0 || cur.second < -depth) continue;
            const PedigreeRecord* r = ped.find(cur.first);
            if (!r) continue;
            push(*r, cur.second);
            frontier.push_back({r->motherUid, cur.second - 1});
            frontier.push_back({r->fatherUid, cur.second - 1});
        }

        // Descendants. The pedigree is indexed by uid and not by parent, so this
        // is a scan -- acceptable because it only runs for one focused agent and
        // only while the panel is open.
        std::vector<uint64_t> layer;
        layer.push_back(ui.focusAgentUid);
        for (int gen = 1; gen <= depth && !layer.empty(); ++gen) {
            std::vector<uint64_t> next;
            for (const PedigreeRecord& r : ped.records()) {
                bool isChild = false;
                for (uint64_t p : layer)
                    if (r.motherUid == p || r.fatherUid == p) { isChild = true; break; }
                if (!isChild) continue;
                push(r, gen);
                next.push_back(r.uid);
            }
            layer.swap(next);
        }
    });

    if (nodes.empty()) {
        ImGui::TextDisabled("No pedigree record for the focused individual. Founders spawned "
                            "before the pedigree existed, or records dropped past the capacity "
                            "cap, genuinely have no ancestry to show.");
        return;
    }

    ImGui::Text("Lineage of %s", rootName.c_str());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderInt("Generations", &ui.lineageDepth, 1, 8);
    ImGui::TextDisabled("%zu individuals within %d generations either way.", nodes.size(), depth);

    // Group by generation, oldest ancestors at the top.
    int minGen = 0, maxGen = 0;
    for (const Node& n : nodes) {
        minGen = std::min(minGen, n.generation);
        maxGen = std::max(maxGen, n.generation);
    }

    ImGui::BeginChild("lintree", ImVec2(0, 420.0f), ImGuiChildFlags_Borders);
    for (int gen = minGen; gen <= maxGen; ++gen) {
        int count = 0;
        for (const Node& n : nodes) if (n.generation == gen) ++count;
        if (count == 0) continue;

        if (gen < 0)      ImGui::TextDisabled("%d generation(s) back  (%d)", -gen, count);
        else if (gen == 0) ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.40f, 1.0f), "this individual");
        else               ImGui::TextDisabled("%d generation(s) forward  (%d)", gen, count);
        ImGui::Indent();
        for (const Node& n : nodes) {
            if (n.generation != gen) continue;
            const ImVec4 col = n.alive ? ImVec4(0.82f, 0.86f, 0.92f, 1.0f)
                                       : ImVec4(0.55f, 0.57f, 0.62f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            char label[192];
            std::snprintf(label, sizeof label, "%s%s   born yr %.1f%s", n.name.c_str(),
                          n.alive ? "" : "  (dead)",
                          static_cast<double>(n.birthTick) / kHoursPerYear,
                          n.uid == ui.focusAgentUid ? "   <- focused" : "");
            if (ImGui::Selectable(label, n.uid == ui.focusAgentUid)) {
                openIndividualCard(ui, n.uid);
                ui.focusAgentUid = n.uid;
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && (n.mother != 0 || n.father != 0)) {
                ImGui::BeginTooltip();
                ImGui::Text("uid %llu", static_cast<unsigned long long>(n.uid));
                ImGui::Text("mother uid %llu", static_cast<unsigned long long>(n.mother));
                ImGui::Text("father uid %llu", static_cast<unsigned long long>(n.father));
                if (!n.alive)
                    ImGui::Text("died yr %.1f", static_cast<double>(n.deathTick) / kHoursPerYear);
                ImGui::EndTooltip();
            }
        }
        ImGui::Unindent();
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Click any individual to open its card and re-root the tree there.");
}

}  // namespace

void drawPhylogeny(Simulation& sim, UiState& ui) {
    ImGui::SetNextWindowSize(ImVec2(860, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Phylogeny", &ui.showPhylogeny)) { ImGui::End(); return; }

    if (ImGui::BeginTabBar("phylotabs")) {
        if (ImGui::BeginTabItem("Species tree")) {
            drawSpeciesTree(sim, ui);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Individual lineage")) {
            drawLineageTree(sim, ui);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

}  // namespace gen
