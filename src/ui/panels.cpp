// ui/panels.cpp — every dockable panel except the world viewport.
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "core/config.h"
#include "imgui.h"
#include "ui/app.h"

namespace gen {

namespace {

void textRow(const char* label, const char* fmt, ...) {
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(190.0f);
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}

// A compact time-series plot drawn straight into the window draw list: axes,
// gridlines, min/max/last readouts and a hover cursor. ImGui::PlotLines would
// have been one line of code but gives none of that.
void drawSeriesPlot(const Series& s, float height) {
    const size_t n = s.values.size();
    ImGui::PushID(s.key.c_str());

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float w = std::max(80.0f, avail.x);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 br(origin.x + w, origin.y + height);

    dl->AddRectFilled(origin, br, IM_COL32(16, 18, 22, 255), 3.0f);
    dl->AddRect(origin, br, IM_COL32(52, 56, 66, 255), 3.0f);

    if (n < 2) {
        dl->AddText(ImVec2(origin.x + 8.0f, origin.y + height * 0.5f - 7.0f),
                    IM_COL32(140, 145, 155, 255), "collecting samples...");
        ImGui::Dummy(ImVec2(w, height));
        ImGui::PopID();
        return;
    }

    float lo = s.values[0], hi = s.values[0];
    for (size_t i = 1; i < n; ++i) {
        lo = std::min(lo, s.values[i]);
        hi = std::max(hi, s.values[i]);
    }
    // A flat series would otherwise divide by zero and draw at the top edge.
    if (hi - lo < 1e-9f) { hi = lo + 1.0f; lo -= 1.0f; }
    const float pad = (hi - lo) * 0.08f;
    lo -= pad;
    hi += pad;

    for (int g = 1; g < 4; ++g) {
        const float y = origin.y + height * static_cast<float>(g) / 4.0f;
        dl->AddLine(ImVec2(origin.x + 1.0f, y), ImVec2(br.x - 1.0f, y), IM_COL32(38, 42, 50, 255));
    }

    const float invRange = 1.0f / (hi - lo);
    ImVec2 prev;
    for (size_t i = 0; i < n; ++i) {
        const float x = origin.x + w * static_cast<float>(i) / static_cast<float>(n - 1);
        const float y = br.y - (s.values[i] - lo) * invRange * height;
        const ImVec2 p(x, y);
        if (i > 0) dl->AddLine(prev, p, IM_COL32(107, 179, 240, 255), 1.4f);
        prev = p;
    }

    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s  [%.4g .. %.4g]  last %.4g%s%s",
                  s.label.c_str(), static_cast<double>(lo + pad), static_cast<double>(hi - pad),
                  static_cast<double>(s.values.back()),
                  s.unit.empty() ? "" : " ", s.unit.c_str());
    dl->AddText(ImVec2(origin.x + 6.0f, origin.y + 4.0f), IM_COL32(200, 205, 215, 255), buf);

    ImGui::InvisibleButton("##plot", ImVec2(w, height));
    if (ImGui::IsItemHovered()) {
        const float mx = ImGui::GetIO().MousePos.x;
        const float t = std::min(1.0f, std::max(0.0f, (mx - origin.x) / w));
        const size_t idx = static_cast<size_t>(t * static_cast<float>(n - 1) + 0.5f);
        const float x = origin.x + w * static_cast<float>(idx) / static_cast<float>(n - 1);
        dl->AddLine(ImVec2(x, origin.y + 1.0f), ImVec2(x, br.y - 1.0f), IM_COL32(255, 210, 60, 160));
        const DateTime d = tickToDate(s.ticks[idx]);
        ImGui::SetTooltip("%s\n%.6g %s", d.toString().c_str(),
                          static_cast<double>(s.values[idx]), s.unit.c_str());
    }
    ImGui::PopID();
}

}  // namespace

// ---------------------------------------------------------------------------
// Menu bar
// ---------------------------------------------------------------------------

void drawMainMenuBar(Simulation& sim, UiState& ui, Viewport& vp, bool& quitRequested) {
    if (!ImGui::BeginMenuBar()) return;

    if (ImGui::BeginMenu("World")) {
        if (ImGui::MenuItem("Regenerate...")) ui.showRegenerate = true;
        ImGui::Separator();
        ImGui::InputText("Save path", ui.savePath, sizeof(ui.savePath));
        if (ImGui::MenuItem("Save snapshot", "Ctrl+S")) {
            Command c;
            c.type = CommandType::SaveSnapshot;
            c.text = ui.savePath;
            sim.push(c);
            ui.setStatus(std::string("Saving to ") + ui.savePath);
        }
        ImGui::InputText("Load path", ui.loadPath, sizeof(ui.loadPath));
        if (ImGui::MenuItem("Load snapshot", "Ctrl+O")) {
            Command c;
            c.type = CommandType::LoadSnapshot;
            c.text = ui.loadPath;
            sim.push(c);
            ui.setStatus(std::string("Loading ") + ui.loadPath);
        }
        ImGui::Separator();
        ImGui::InputText("CSV path", ui.csvPath, sizeof(ui.csvPath));
        if (ImGui::MenuItem("Export telemetry to CSV")) {
            const bool ok = sim.exportTelemetryCsv(ui.csvPath);
            ui.setStatus(ok ? (std::string("Exported ") + ui.csvPath)
                            : std::string("Nothing to export yet"));
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) quitRequested = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("World", nullptr, &ui.showWorld);
        ImGui::MenuItem("Time and speed", nullptr, &ui.showTimeBar);
        ImGui::MenuItem("God toolbar", nullptr, &ui.showGodToolbar);
        ImGui::MenuItem("Tile inspector", nullptr, &ui.showTileInspector);
        ImGui::MenuItem("Population", nullptr, &ui.showPopulation);
        ImGui::MenuItem("Brain inspector", nullptr, &ui.showBrainInspector);
        ImGui::MenuItem("Genome browser", nullptr, &ui.showGenomeBrowser);
        ImGui::MenuItem("Event feed", nullptr, &ui.showEventFeed);
        ImGui::MenuItem("Charts", nullptr, &ui.showCharts);
        ImGui::MenuItem("Statistics", nullptr, &ui.showStatistics);
        ImGui::MenuItem("Phylogeny", "F6", &ui.showPhylogeny);
        ImGui::MenuItem("Colour agents by lineage", nullptr, &vp.colourBySpecies);
        ImGui::MenuItem("Profiler", "F9", &ui.showProfiler);
        ImGui::MenuItem("Chemistry Lab", "F7", &ui.showChemistryLab);
        ImGui::MenuItem("Knowledge & Culture", "F8", &ui.showKnowledge);
        ImGui::MenuItem("Economy", nullptr, &ui.showEconomy);
        ImGui::MenuItem("Intervention log", nullptr, &ui.showInterventionLog);
        ImGui::MenuItem("Bookmarks", nullptr, &ui.showBookmarks);
        ImGui::Separator();
        ImGui::MenuItem("Settings", nullptr, &ui.showSettings);
        ImGui::Separator();
        ImGui::MenuItem("Show tile grid", "G", &vp.showGrid);
        ImGui::MenuItem("Hide render (throughput)", "H", &vp.hideRender);
        if (ImGui::MenuItem("Reset layout")) ui.layoutResetRequested = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Overlay")) {
        for (int i = 0; i < static_cast<int>(Overlay::Count); ++i) {
            const Overlay o = static_cast<Overlay>(i);
            const char* req = overlayRequires(o);
            const bool available = (req == nullptr);
            if (ImGui::MenuItem(overlayName(o), nullptr, vp.overlay() == o, available))
                vp.setOverlay(o);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(420.0f);
                ImGui::TextUnformatted(overlayDescription(o));
                if (!available) {
                    ImGui::Separator();
                    // Not "needs milestone N" any more: the two remaining
                    // unavailable overlays are waiting on a model that was never
                    // built, and the tooltip says which and why.
                    ImGui::TextDisabled("Unavailable, and it is waiting on %s", req);
                }
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        ImGui::MenuItem("About GENESIS", nullptr, &ui.showAbout);
        ImGui::MenuItem("Milestone status", nullptr, &ui.showMilestones);
        ImGui::MenuItem("Dear ImGui demo", nullptr, &ui.showImGuiDemo);
        ImGui::EndMenu();
    }

    // Right-aligned live status.
    const SimSnapshot s = sim.snapshot();
    char right[256];
    std::snprintf(right, sizeof(right), "%s  |  %.0f ticks/s  |  %.1f FPS",
                  s.date.toShortString().c_str(), s.ticksPerSecond,
                  static_cast<double>(ImGui::GetIO().Framerate));
    const float tw = ImGui::CalcTextSize(right).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - tw - 16.0f);
    ImGui::TextDisabled("%s", right);

    ImGui::EndMenuBar();
}

// ---------------------------------------------------------------------------
// Time and speed
// ---------------------------------------------------------------------------

void drawTimeBar(Simulation& sim, UiState& ui, Viewport& vp) {
    if (!ImGui::Begin("Time and speed", &ui.showTimeBar)) { ImGui::End(); return; }

    const SimSnapshot s = sim.snapshot();

    // --- transport ---
    if (ImGui::Button(s.paused ? "RESUME" : " PAUSE ", ImVec2(76, 0)))
        sim.pushPause(!s.paused);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Space");

    for (int i = 0; i < kSpeedPresetCount; ++i) {
        ImGui::SameLine();
        char label[16];
        std::snprintf(label, sizeof(label), "%gx", kSpeedPresets[i]);
        const bool active = !s.paused && !s.maxSpeed &&
                            std::fabs(s.requestedSpeed - kSpeedPresets[i]) < 1e-9;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.45f, 0.62f, 1.0f));
        if (ImGui::Button(label, ImVec2(44, 0))) sim.pushSpeed(kSpeedPresets[i]);
        if (active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hotkey %d", i + 1);
    }

    ImGui::SameLine();
    if (s.maxSpeed) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.34f, 0.18f, 1.0f));
    if (ImGui::Button("MAX", ImVec2(52, 0))) sim.push(CommandType::SetMaxSpeed);
    if (s.maxSpeed) ImGui::PopStyleColor();
    ImGui::SameLine();
    helpMarker("MAX is uncapped: the simulation thread runs as fast as the CPU allows and the "
               "achieved multiplier is measured, not promised. Every agent's brain still runs "
               "every tick at every speed -- fast-forward is more real ticks per second, never "
               "an approximation. The honest ceiling is shown below.");

    // --- arbitrary speed ---
    static float speedSlider = 1.0f;
    static float speedEntry = 1.0f;
    if (!s.maxSpeed) speedSlider = static_cast<float>(s.requestedSpeed);
    ImGui::SetNextItemWidth(280.0f);
    if (ImGui::SliderFloat("##speedslider", &speedSlider, 0.1f, 500.0f,
                           "%.1f sim hours / real second", ImGuiSliderFlags_Logarithmic))
        sim.pushSpeed(static_cast<double>(speedSlider));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::InputFloat("##speedentry", &speedEntry, 0.0f, 0.0f, "%.3f",
                          ImGuiInputTextFlags_EnterReturnsTrue))
        sim.pushSpeed(static_cast<double>(speedEntry));
    ImGui::SameLine();
    ImGui::TextDisabled("exact speed");

    // --- stepping ---
    ImGui::Separator();
    if (ImGui::Button("+1 tick"))  sim.pushStep(1);
    ImGui::SameLine();
    if (ImGui::Button("+1 day"))   sim.pushStep(kHoursPerDay);
    ImGui::SameLine();
    if (ImGui::Button("+1 month")) sim.pushStep(kHoursPerMonth);
    ImGui::SameLine();
    if (ImGui::Button("+1 year"))  sim.pushStep(kHoursPerYear);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Checkbox("Hide render", &vp.hideRender);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Stop rasterising the world and spend the whole frame budget on\n"
                          "ticks. Charts, the event feed and every readout keep updating. (H)");

    // --- run until ---
    ImGui::Separator();
    ImGui::SetNextItemWidth(180.0f);
    const char* kRunUntilNames[] = {
        "(none)", "Reach year", "Run for N more years",
        "Population below", "Population above", "Extinction",
        "First discovery", "Custom (Lua)"
    };
    ImGui::Combo("##rununtilkind", &ui.runUntilKind, kRunUntilNames,
                 IM_ARRAYSIZE(kRunUntilNames));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputDouble("##rununtilvalue", &ui.runUntilValue, 0.0, 0.0, "%.0f");
    ImGui::SameLine();

    const RunUntilKind kind = static_cast<RunUntilKind>(ui.runUntilKind);
    const bool supported = (kind == RunUntilKind::Year || kind == RunUntilKind::ElapsedYears);
    ImGui::BeginDisabled(!supported || s.runUntilActive);
    if (ImGui::Button("Run until")) {
        Command c;
        c.type = CommandType::SetRunUntil;
        c.ix = ui.runUntilKind;
        c.a = ui.runUntilValue;
        c.text = std::string(kRunUntilNames[ui.runUntilKind]) + " " +
                 std::to_string(static_cast<long long>(ui.runUntilValue));
        sim.push(c);
    }
    ImGui::EndDisabled();
    if (!supported && kind != RunUntilKind::None) {
        ImGui::SameLine();
        ImGui::TextDisabled("(needs agents / chemistry -- M2+)");
    }

    if (s.runUntilActive) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) sim.push(CommandType::CancelRunUntil);
        ImGui::ProgressBar(static_cast<float>(s.runUntilProgress), ImVec2(-1, 0),
                           s.runUntilLabel.c_str());
    }

    // --- readouts ---
    ImGui::Separator();
    if (ImGui::BeginTable("timereadout", 4, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("Date");
        ImGui::Text("%s", s.date.toString().c_str());
        ImGui::TextDisabled("Season");
        ImGui::Text("%s", seasonName(s.season));

        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("Sim ticks / sec");
        ImGui::Text("%.0f", s.ticksPerSecond);
        ImGui::TextDisabled("Effective multiplier");
        if (s.paused) ImGui::TextUnformatted("paused");
        else ImGui::Text("%.1fx", s.effectiveMultiplier);

        ImGui::TableSetColumnIndex(2);
        ImGui::TextDisabled("Render FPS");
        ImGui::Text("%.1f", static_cast<double>(ImGui::GetIO().Framerate));
        ImGui::TextDisabled("Raster cost");
        ImGui::Text("%.2f ms", vp.lastRasterMs());

        ImGui::TableSetColumnIndex(3);
        ImGui::TextDisabled("Sim years elapsed");
        ImGui::Text("%.3f", s.simYears);
        ImGui::TextDisabled("Real time elapsed");
        const int hrs = static_cast<int>(s.realElapsedSeconds / 3600.0);
        const int min = static_cast<int>(s.realElapsedSeconds / 60.0) % 60;
        const int sec = static_cast<int>(s.realElapsedSeconds) % 60;
        ImGui::Text("%02d:%02d:%02d", hrs, min, sec);
        ImGui::EndTable();
    }

    ImGui::TextDisabled("Bottleneck: ");
    ImGui::SameLine();
    ImGui::Text("%s", s.bottleneck);
    ImGui::SameLine();
    ImGui::TextDisabled(" |  batch %llu ticks in %.2f ms  |  %u worker threads",
                        static_cast<unsigned long long>(s.batchSize), s.lastBatchMs,
                        s.workerCount);
    ImGui::SameLine();
    helpMarker("The stage that dominated the most recent tick. Environmental processes run on "
               "their own physical timescales (thermal 6 h, hydrology and ecology daily, "
               "weather monthly, geology yearly) -- every cadence is editable under Settings "
               "-> env. Agents, when they exist, are never scheduled this way: they run every "
               "tick without exception.");

    if (!ui.statusMessage.empty() && ImGui::GetTime() - ui.statusMessageTime < 6.0) {
        ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.4f, 1.0f), "%s", ui.statusMessage.c_str());
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// God toolbar
// ---------------------------------------------------------------------------

// drawGodToolbar now lives in ui/god_ui.cpp, together with the rest of god
// mode: brushes, disasters, population tools, rules, miracles and the console.

// ---------------------------------------------------------------------------
// Event feed
// ---------------------------------------------------------------------------

void drawEventFeed(Simulation& sim, UiState& ui, Viewport& vp) {
    if (!ImGui::Begin("Event feed", &ui.showEventFeed)) { ImGui::End(); return; }

    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##search", "filter text", ui.eventSearch, sizeof(ui.eventSearch));
    ImGui::SameLine();
    if (ImGui::Button("Kinds")) ImGui::OpenPopup("kindfilter");
    if (ImGui::BeginPopup("kindfilter")) {
        for (int i = 0; i < static_cast<int>(EventKind::Count); ++i)
            ImGui::Checkbox(eventKindName(static_cast<EventKind>(i)), &ui.eventFilter[i]);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &ui.eventAutoScroll);
    ImGui::SameLine();
    ImGui::TextDisabled("%llu events", static_cast<unsigned long long>(sim.eventCount()));

    static std::vector<WorldEvent> events;
    sim.copyEvents(events, 4000);

    if (ImGui::BeginTable("events", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        const size_t searchLen = std::strlen(ui.eventSearch);
        for (size_t i = 0; i < events.size(); ++i) {
            const WorldEvent& e = events[i];
            if (!ui.eventFilter[static_cast<int>(e.kind)]) continue;
            if (searchLen > 0 && e.text.find(ui.eventSearch) == std::string::npos) continue;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", tickToDate(e.tick).toShortString().c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(eventKindName(e.kind));
            ImGui::TableSetColumnIndex(2);
            ImGui::PushID(static_cast<int>(i));
            // Clicking an event with a location jumps the camera to it.
            if (ImGui::Selectable(e.text.c_str(), false, ImGuiSelectableFlags_SpanAllColumns) &&
                e.x >= 0) {
                vp.select(e.x, e.y);
                vp.focusOn(e.x, e.y);
            }
            ImGui::PopID();
        }
        if (ui.eventAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndTable();
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Charts
// ---------------------------------------------------------------------------

void drawCharts(Simulation& sim, UiState& ui) {
    if (!ImGui::Begin("Charts", &ui.showCharts)) { ImGui::End(); return; }

    static std::vector<std::string> keys, labels;
    sim.copySeriesKeys(keys, labels);

    ImGui::SetNextItemWidth(280.0f);
    if (ImGui::BeginCombo("##addseries", "Add a series...")) {
        for (size_t i = 0; i < keys.size(); ++i) {
            const bool already = std::find(ui.chartSeries.begin(), ui.chartSeries.end(),
                                           keys[i]) != ui.chartSeries.end();
            if (ImGui::Selectable(labels[i].c_str(), already) && !already)
                ui.chartSeries.push_back(keys[i]);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Height", &ui.chartHeight, 60.0f, 320.0f, "%.0f px");
    ImGui::SameLine();
    ImGui::TextDisabled("%zu series tracked", keys.size());
    ImGui::SameLine();
    helpMarker("Any scalar the simulation tracks can be plotted. New series appear here "
               "automatically as later milestones start recording them -- population, mean "
               "brain size, allele frequencies, technology index, disease prevalence. Price "
               "and inequality series appear only if a currency ever comes into existence.");

    ImGui::Separator();
    Series s;
    for (size_t i = 0; i < ui.chartSeries.size();) {
        ImGui::PushID(static_cast<int>(i));
        bool remove = false;
        if (ImGui::SmallButton("x")) remove = true;
        ImGui::SameLine();
        if (sim.copySeries(ui.chartSeries[i], s)) {
            drawSeriesPlot(s, ui.chartHeight);
        } else {
            ImGui::TextDisabled("%s (no data)", ui.chartSeries[i].c_str());
        }
        ImGui::PopID();
        if (remove) ui.chartSeries.erase(ui.chartSeries.begin() + static_cast<long>(i));
        else ++i;
    }
    if (ui.chartSeries.empty())
        ImGui::TextDisabled("No series selected. Use the dropdown above.");

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

void drawStatistics(Simulation& sim, UiState& ui) {
    if (!ImGui::Begin("Statistics", &ui.showStatistics)) { ImGui::End(); return; }

    const SimSnapshot s = sim.snapshot();

    if (ImGui::CollapsingHeader("World", ImGuiTreeNodeFlags_DefaultOpen)) {
        textRow("Mean temperature", "%.2f C", s.world.meanTemperature);
        textRow("Mean rainfall", "%.0f mm/yr", s.world.meanRainfall);
        textRow("Land fraction", "%.1f %%", s.world.landFraction * 100.0);
        textRow("Ice cover", "%.1f %%", s.world.iceFraction * 100.0);
        textRow("Mean soil fertility", "%.3f", s.world.meanSoilFertility);
        textRow("Total plant biomass", "%.4g kg", s.world.totalBiomass);
        textRow("Standing water", "%.4g m3", s.world.totalWaterVolume);
    }

    if (ImGui::CollapsingHeader("Simulation", ImGuiTreeNodeFlags_DefaultOpen)) {
        textRow("Tick", "%llu", static_cast<unsigned long long>(s.tick));
        textRow("Simulated years", "%.4f", s.simYears);
        textRow("World seed", "%llu", static_cast<unsigned long long>(s.seed));
        textRow("Worker threads", "%u (+1 caller)", s.workerCount);
        textRow("Interventions", "%llu", static_cast<unsigned long long>(s.interventionCount));
    }

    if (ImGui::CollapsingHeader("Population genetics", ImGuiTreeNodeFlags_DefaultOpen)) {
        const Genetics::PopulationGenetics& g = s.popGenetics;
        if (g.sampleSize < 2) {
            ImGui::TextWrapped(
                "Fewer than two individuals have been sampled, so there is no variation to "
                "measure. This says so rather than showing a column of zeroes that would look "
                "like measurements.");
        } else {
            ImGui::TextDisabled("Over a sample of %u individuals.", g.sampleSize);
            textRow("Observed heterozygosity", "%.4f", g.observedHeterozygosity);
            textRow("Expected heterozygosity", "%.4f", g.expectedHeterozygosity);
            textRow("Mean inbreeding F", "%.4f", g.meanInbreedingF);
            textRow("Tajima's D", "%+.4f", g.tajimasD);
            textRow("Effective population Ne", "%.1f", g.effectivePopulationSize);
            textRow("Linkage disequilibrium", "%.4f", g.linkageDisequilibrium);
            textRow("Segregating loci", "%u", g.segregatingSites);
            textRow("Fixed loci", "%u", g.fixedLoci);
            textRow("Fst between quadrants", "%.4f", s.fst);
            helpMarker(
                "Fst is the share of total genetic variance that sits BETWEEN geographic "
                "subpopulations rather than within them. Climbing away from zero across a "
                "mountain range or a strait is what speciation looks like before it finishes -- "
                "and the species detector on F6 is watching the same divergence from the other "
                "end.");

            ImGui::SeparatorText("Heritability");
            ImGui::TextWrapped(
                "For each trait: how much of the variation in the population is genetic, and how "
                "much of THAT responds to selection.");
            ImGui::TextDisabled(
                "h2 is narrow-sense -- the additive fraction, and the number in the breeder's "
                "equation. H2 is broad-sense and includes the non-additive part: here that is "
                "regulatory genes scaling a coding total, which is epistasis and does not "
                "respond to selection the same way. The gap between them is real and worth "
                "watching. Vp - Vg is developmental noise: the same genotype does not build the "
                "same body twice.");

            static char filter[48] = {0};
            static bool onlyPolygenic = true;
            ImGui::SetNextItemWidth(180.0f);
            ImGui::InputTextWithHint("##h2filter", "filter traits", filter, sizeof filter);
            ImGui::SameLine();
            ImGui::Checkbox("Polygenic only", &onlyPolygenic);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("A trait driven by one coding locus is Mendelian, not "
                                  "polygenic, and its heritability is a much less interesting "
                                  "number.");

            if (ImGui::BeginTable("h2", 7,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                                  ImVec2(0, 260.0f))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Trait");
                ImGui::TableSetupColumn("Loci", ImGuiTableColumnFlags_WidthFixed, 45.0f);
                ImGui::TableSetupColumn("h2", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableSetupColumn("H2", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                ImGui::TableSetupColumn("Va", ImGuiTableColumnFlags_WidthFixed, 75.0f);
                ImGui::TableSetupColumn("Vg", ImGuiTableColumnFlags_WidthFixed, 75.0f);
                ImGui::TableSetupColumn("Vp", ImGuiTableColumnFlags_WidthFixed, 75.0f);
                ImGui::TableHeadersRow();

                for (int t = 0; t < kTraitCount; ++t) {
                    const Genetics::TraitHeritability& h = g.heritability[t];
                    const TraitSpec& spec = traitSpec(static_cast<Trait>(t));
                    if (onlyPolygenic && h.codingLoci < 2) continue;
                    if (filter[0] != '\0') {
                        std::string name(spec.name), needle(filter);
                        std::transform(name.begin(), name.end(), name.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        std::transform(needle.begin(), needle.end(), needle.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        if (name.find(needle) == std::string::npos) continue;
                    }
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(spec.name);
                    if (ImGui::IsItemHovered() && spec.description) {
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(380.0f);
                        ImGui::TextUnformatted(spec.description);
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                    ImGui::TableNextColumn();
                    if (h.codingLoci >= 2) ImGui::Text("%u", h.codingLoci);
                    else ImGui::TextDisabled("%u", h.codingLoci);
                    ImGui::TableNextColumn();
                    {
                        char buf[40];
                        std::snprintf(buf, sizeof buf,
                                      h.rangeLimited ? "%.3f  clamped" : "%.3f", h.narrowH2);
                        ImGui::ProgressBar(static_cast<float>(h.narrowH2), ImVec2(-1, 0), buf);
                    }
                    if (h.rangeLimited && ImGui::IsItemHovered())
                        ImGui::SetTooltip("Additive variance exceeds phenotypic variance, so h2 is "
                                          "capped here. That happens when a trait is pressed "
                                          "against one end of its legal range: the population "
                                          "carries genetic variation the body cannot express.");
                    ImGui::TableNextColumn(); ImGui::Text("%.3f", h.broadH2);
                    ImGui::TableNextColumn(); ImGui::Text("%.4g", h.additiveVariance);
                    ImGui::TableNextColumn(); ImGui::Text("%.4g", h.genotypicVariance);
                    ImGui::TableNextColumn(); ImGui::Text("%.4g", h.phenotypicVariance);
                }
                ImGui::EndTable();
            }
        }
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Settings -- generated from the config registry
// ---------------------------------------------------------------------------

void drawSettings(Simulation& sim, UiState& ui) {
    if (!ImGui::Begin("Settings", &ui.showSettings)) { ImGui::End(); return; }

    ImGui::TextWrapped(
        "Every tunable constant in GENESIS is registered once in core/config.cpp and this "
        "screen is generated by walking that registry. Nothing here is hand-written, which is "
        "why a constant added by a later milestone shows up automatically with its own "
        "tooltip and range.");
    ImGui::Separator();

    if (ImGui::Button("Save to data/config.ini")) {
        const bool ok = cfg().save("data/config.ini");
        ui.setStatus(ok ? "Wrote data/config.ini" : "Could not write data/config.ini");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload from data/config.ini")) {
        const bool ok = cfg().load("data/config.ini");
        ui.setStatus(ok ? "Reloaded data/config.ini" : "data/config.ini not found");
    }
    ImGui::SameLine();
    if (ImGui::Button("Restore all defaults")) {
        cfg().restoreDefaults();
        ui.setStatus("All settings restored to defaults");
    }
    ImGui::Separator();

    static char filter[128] = {0};
    ImGui::SetNextItemWidth(240.0f);
    ImGui::InputTextWithHint("##cfgfilter", "filter by name", filter, sizeof(filter));

    const size_t filterLen = std::strlen(filter);
    for (const std::string& section : cfg().sections()) {
        // Count visible entries first so empty sections are not shown at all.
        int visible = 0;
        for (const CfgEntry& e : cfg().entries())
            if (e.section == section &&
                (filterLen == 0 || e.key.find(filter) != std::string::npos)) ++visible;
        if (visible == 0) continue;

        if (!ImGui::CollapsingHeader(section.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;

        ImGui::PushID(section.c_str());
        if (ImGui::SmallButton("restore section defaults")) cfg().restoreDefaults(section);
        ImGui::PopID();

        for (CfgEntry& e : cfg().entriesMutable()) {
            if (e.section != section) continue;
            if (filterLen > 0 && e.key.find(filter) == std::string::npos) continue;

            ImGui::PushID(e.key.c_str());
            const bool changedByHand = (e.type != CfgType::String)
                                       ? (e.value != e.defaultValue)
                                       : (e.strValue != e.strDefault);
            if (changedByHand) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.45f, 1.0f));

            ImGui::SetNextItemWidth(240.0f);
            switch (e.type) {
                case CfgType::Bool: {
                    bool v = (e.value != 0.0);
                    if (ImGui::Checkbox(e.name.c_str(), &v)) e.value = v ? 1.0 : 0.0;
                    break;
                }
                case CfgType::Int: {
                    int v = static_cast<int>(e.value);
                    if (ImGui::SliderInt(e.name.c_str(), &v, static_cast<int>(e.minValue),
                                         static_cast<int>(e.maxValue), "%d",
                                         e.logarithmic ? ImGuiSliderFlags_Logarithmic : 0))
                        e.value = static_cast<double>(v);
                    break;
                }
                case CfgType::Float: {
                    float v = static_cast<float>(e.value);
                    if (ImGui::SliderFloat(e.name.c_str(), &v, static_cast<float>(e.minValue),
                                           static_cast<float>(e.maxValue), "%.5g",
                                           e.logarithmic ? ImGuiSliderFlags_Logarithmic : 0))
                        e.value = static_cast<double>(v);
                    break;
                }
                case CfgType::String: {
                    char buf[256];
                    std::snprintf(buf, sizeof(buf), "%s", e.strValue.c_str());
                    if (ImGui::InputText(e.name.c_str(), buf, sizeof(buf))) e.strValue = buf;
                    break;
                }
            }
            if (changedByHand) ImGui::PopStyleColor();

            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(440.0f);
                ImGui::TextUnformatted(e.description.c_str());
                ImGui::Separator();
                ImGui::TextDisabled("key: %s", e.key.c_str());
                if (e.type != CfgType::String)
                    ImGui::TextDisabled("default %.6g, range %.6g .. %.6g",
                                        e.defaultValue, e.minValue, e.maxValue);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("reset")) {
                e.value = e.defaultValue;
                e.strValue = e.strDefault;
            }
            ImGui::PopID();
        }
    }

    (void)sim;
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Intervention log and bookmarks
// ---------------------------------------------------------------------------

void drawInterventionLog(Simulation& sim, UiState& ui) {
    if (!ImGui::Begin("Intervention log", &ui.showInterventionLog)) { ImGui::End(); return; }

    ImGui::TextWrapped(
        "Every divine act, in order, with the tick it was applied on. This is simultaneously "
        "the audit trail of how much you cheated and the replay format: the log plus the world "
        "seed reproduces the run exactly.");
    ImGui::Separator();

    static std::vector<Command> log;
    sim.copyInterventionLog(log);

    if (log.empty()) {
        ImGui::TextDisabled("No interventions. This world has run on its own so far.");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTable("ilog", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                          ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 44.0f);
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < log.size(); ++i) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%zu", i + 1);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", tickToDate(log[i].appliedTick).toShortString().c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(commandName(log[i].type));
            ImGui::TableSetColumnIndex(3);
            if (!log[i].text.empty()) ImGui::Text("%s = %.6g", log[i].text.c_str(), log[i].a);
            else ImGui::Text("%.6g", log[i].a);
        }
        ImGui::EndTable();
    }

    ImGui::TextDisabled(
        "Every entry here is undoable with Ctrl+Z, except the three economy acts: undo restores "
        "tiles and whole agents, and enabling barter, introducing a currency and abolishing the "
        "economy touch neither. Their inverse act is the way back, and the Economy panel offers "
        "it.");
    ImGui::End();
}

void drawBookmarks(Simulation& sim, UiState& ui) {
    if (!ImGui::Begin("Bookmarks", &ui.showBookmarks)) { ImGui::End(); return; }

    static std::vector<Bookmark> marks;
    sim.copyBookmarks(marks);

    if (marks.empty()) {
        ImGui::TextDisabled("No bookmarks yet. Use the God toolbar to save the current moment.");
    } else if (ImGui::BeginTable("bookmarks", 4,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < marks.size(); ++i) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(marks[i].name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", tickToDate(marks[i].tick).toShortString().c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("%s", marks[i].filePath.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton("Jump")) {
                Command c;
                c.type = CommandType::JumpToBookmark;
                c.ix = static_cast<int32_t>(i);
                sim.push(c);
                ui.setStatus("Jumping to " + marks[i].name);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Economy -- dormant
// ---------------------------------------------------------------------------

// drawEconomy now lives in ui/econ_ui.cpp. It grew from a placeholder that said
// "no currency exists" into a real panel once M8 gave it something to report.

// ---------------------------------------------------------------------------
// Dialogs and help
// ---------------------------------------------------------------------------

void drawRegenerateDialog(Simulation& sim, UiState& ui, Viewport& vp) {
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("Regenerate world", &ui.showRegenerate,
                      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    ImGui::TextWrapped("A new world is generated from the seed alone. The same seed and the same "
                       "settings always produce exactly the same world.");
    ImGui::Separator();

    ImGui::Checkbox("Random seed", &ui.regenRandomSeed);
    ImGui::BeginDisabled(ui.regenRandomSeed);
    ImGui::InputScalar("Seed", ImGuiDataType_S64, &ui.regenSeed);
    ImGui::EndDisabled();

    ImGui::InputInt("Width", &ui.regenWidth, 128, 512);
    ImGui::InputInt("Height", &ui.regenHeight, 128, 512);
    ui.regenWidth = std::min(4096, std::max(128, ui.regenWidth));
    ui.regenHeight = std::min(4096, std::max(128, ui.regenHeight));

    const double megaTiles = static_cast<double>(ui.regenWidth) * ui.regenHeight / 1.0e6;
    ImGui::TextDisabled("%.2f M tiles, about %.0f MB of tile arrays", megaTiles, megaTiles * 64.0);
    if (megaTiles > 4.5)
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                           "Large world: generation will take a while and memory use is high.");

    ImGui::Separator();
    if (ImGui::Button("Generate", ImVec2(140, 0))) {
        Command c;
        c.type = CommandType::RegenerateWorld;
        if (ui.regenRandomSeed) {
            // Wall-clock is used ONLY here, to pick a seed. Nothing inside the
            // simulation ever reads a clock; once chosen, the seed determines
            // everything (ARCHITECTURE.md §5.4).
            ui.regenSeed = static_cast<long long>(
                static_cast<uint64_t>(ImGui::GetTime() * 1.0e6) ^ 0x9E3779B97F4A7C15ull);
            if (ui.regenSeed < 0) ui.regenSeed = -ui.regenSeed;
        }
        c.a = static_cast<double>(ui.regenSeed);
        c.ix = ui.regenWidth;
        c.iy = ui.regenHeight;
        sim.push(c);
        vp.clearSelection();
        vp.resetCamera(ui.regenWidth, ui.regenHeight);
        ui.showRegenerate = false;
        ui.setStatus("Regenerating world...");
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(140, 0))) ui.showRegenerate = false;

    ImGui::End();
}

void drawAbout(UiState& ui) {
    ImGui::SetNextWindowSize(ImVec2(600, 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("About GENESIS", &ui.showAbout,
                      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::TextUnformatted("GENESIS -- high-fidelity artificial life simulator");
    ImGui::Separator();
    ImGui::TextWrapped(
        "C++17, compiled native, no interpreter and no runtime VM. The interface is Dear ImGui "
        "on a raw Win32 window with a WGL OpenGL context; the world viewport is rasterised on "
        "the CPU and uploaded as a single texture, which is why no shader, no GL loader and no "
        "prebuilt binary dependency is needed anywhere in the build.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Controls");
    ImGui::BulletText("Left drag or middle drag: pan.  Wheel: zoom about the cursor.");
    ImGui::BulletText("Left click: select a tile.  Arrow keys: pan.  + / -: zoom.");
    ImGui::BulletText("Space: pause.  1-7: speed presets.  M: MAX.  . : single tick.");
    ImGui::BulletText("G: tile grid.  H: hide render.  Ctrl+S / Ctrl+O: save / load.");
    ImGui::Spacing();
    ImGui::TextWrapped("See README.md for the model documentation and ARCHITECTURE.md for the "
                       "threading, memory and determinism design.");
    ImGui::End();
}

void drawMilestones(UiState& ui) {
    ImGui::SetNextWindowSize(ImVec2(700, 520), ImGuiCond_Appearing);
    if (!ImGui::Begin("Milestone status", &ui.showMilestones, ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }

    struct Row { const char* id; const char* title; int state; const char* detail; };
    // state: 0 = done, 1 = in progress, 2 = not started
    static const Row kRows[] = {
        {"M1", "World, rendering, time controls, camera, save/load", 0,
         "Tile world with tectonics, erosion, D8 drainage, orographic climate, strata, "
         "process-based ore and Whittaker biomes. Deterministic tick engine with the full "
         "speed control set, stepping, run-until (year targets), bookmarks, chunked binary "
         "snapshots, CSV telemetry export and headless batch mode. Docked dark UI with the "
         "world viewport, 13 working overlays, tile inspector, event feed, charts, statistics "
         "and a settings screen generated from the constant registry."},
        {"M2", "Agents: genetics, metabolism, reproduction, death, Individual Card", 0,
         "Diploid genome with coding, regulatory, brain, junk, sex and MHC loci, crossover "
         "with recombination maps and hotspots, per-locus dominance models, the full mutation "
         "set including gene and whole-chromosome duplication, inbreeding coefficients and "
         "recessive lethals. Full life cycle, metabolism, aging and pedigree, with an "
         "eleven-tab Individual Card in which every allele is editable."},
        {"M3", "Neural brains, drives, reward learning, Brain Inspector", 0,
         "Genome-encoded recurrent networks, 48 inputs to 20 outputs, with NEAT-style "
         "topology mutation and innovation-number alignment, plus lifetime learning through a "
         "reward-prediction-error signal with eligibility traces. Homeostatic drives with "
         "heritable reward weights."},
        {"M4", "Sex expression, orientation, attraction matrix, sexual selection", 0,
         "Continuous sex expression stored separately from chromosomal sex, a 2D orientation "
         "preference space, and relational attractiveness computed per observer-target pair "
         "with mutual acceptance required. Ornaments are costly, so runaway is possible."},
        {"M5", "Full god mode, Lua console, undo", 0,
         "Spawning, terrain and resource brushes, disasters, climate control, rule overrides, "
         "population filters and mass edits, selection-pressure painting, recordable miracles "
         "with hotkeys, full undo/redo, and a sandboxed Lua 5.4 console. Every intervention is "
         "logged and replayable."},
        {"M6", "Chemistry, materials, discovery, knowledge and culture", 0,
         "48 real elements and 82 substances with real thermodynamic data, 45 balanced "
         "reactions gated by dG = dH - T.dS and Arrhenius rates with catalysis and Le "
         "Chatelier. An unbalanced equation is a hard load failure. Materials derive their "
         "properties from composition and process history. Discovery is a SEARCH: agents "
         "combine what they gathered at whatever temperature their techniques reach and the "
         "engine says what happens. Knowledge is taught, degrades in the telling, and is lost "
         "when the last holder dies."},
        {"M7", "Charts, population genetics, phylogeny, telemetry, headless", 0,
         "Charts, telemetry export, CSV, headless batch mode and the full "
         "population-genetics readouts. Species are DETECTED rather than declared: the "
         "population is clustered by gap in neutral genetic distance, measured against its own "
         "nearest-neighbour spacing, and a split has reproductive consequences through a "
         "continuous hybrid-fertility penalty. The phylogeny is drawn against a time axis, plus "
         "an individual ancestry and descent tree from the pedigree."},
        {"M8", "Optional: barter, currency detection, INTRODUCE CURRENCY, markets", 0,
         "Dormant by default and structurally inert: the tick's guard is one bool test at the "
         "call site, so with no economy no function in the module is entered at all, and nothing "
         "in it is allocated. Barter runs on subjective valuations with gains from trade on both "
         "sides. Money is not decreed but DETECTED, from turnover, pass-on rate and breadth of "
         "acceptance -- and it can also be imposed by decree, which the log records as an "
         "imposition. God mode reaches the module through a nullable pointer, so deleting it "
         "leaves M1-M7 whole."},
        {"M9", "Optimisation pass to 10k+ agents, profiling report", 0,
         "Driven by the built-in per-stage profiler, which is always on. At 12,000 agents the "
         "tick went from 90.5 ms to 21.5 ms and throughput from 13 to 48 ticks/s, with no stage "
         "left above 19% -- a flat profile rather than one dominant cost. The largest single "
         "finding was that the job system's serial threshold of 2048 items meant every parallel "
         "agent stage had been running single-threaded at every population the program had ever "
         "reached. Two model bounds came out of it as well: perception and social interaction "
         "per hour are both finite, which is the realistic case and not a shortcut."},
    };

    for (const Row& r : kRows) {
        ImVec4 col;
        const char* tag;
        switch (r.state) {
            case 0:  col = ImVec4(0.45f, 0.85f, 0.50f, 1.0f); tag = "COMPLETE"; break;
            case 1:  col = ImVec4(0.95f, 0.80f, 0.35f, 1.0f); tag = "PARTIAL";  break;
            default: col = ImVec4(0.55f, 0.58f, 0.65f, 1.0f); tag = "NOT STARTED"; break;
        }
        ImGui::TextColored(col, "%s  %-11s", r.id, tag);
        ImGui::SameLine();
        ImGui::TextUnformatted(r.title);
        ImGui::Indent();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.67f, 0.72f, 1.0f));
        ImGui::TextWrapped("%s", r.detail);
        ImGui::PopStyleColor();
        ImGui::Unindent();
        ImGui::Spacing();
    }
    ImGui::End();
}

}  // namespace gen
