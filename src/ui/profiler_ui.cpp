// ui/profiler_ui.cpp — the Profiler panel.
//
// Shows where the tick actually goes, per stage, with a bar per stage scaled to
// the most expensive one. This is the panel that decided what the M9
// optimisation pass worked on; before it existed the bottleneck readout was
// "whichever stage ran last", which named the environment on every sixth tick
// whatever the tick really cost.
#include <algorithm>
#include <cstdio>

#include "core/config.h"
#include "imgui.h"
#include "ui/app.h"

namespace gen {

void drawProfiler(Simulation& sim, UiState& ui) {
    ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Profiler", &ui.showProfiler)) { ImGui::End(); return; }

    const SimSnapshot s = sim.snapshot();

    ImGui::Text("Throughput %.0f ticks/s", s.ticksPerSecond);
    ImGui::SameLine();
    ImGui::TextDisabled("|  %llu agents  |  %lld x %lld tiles  |  %u workers + 1",
                        static_cast<unsigned long long>(s.agentCount),
                        static_cast<long long>(cfg().getInt("world.width", 1024)),
                        static_cast<long long>(cfg().getInt("world.height", 1024)),
                        s.workerCount);

    ImGui::Text("Mean tick %.3f ms", s.tickMeanMs);
    ImGui::SameLine();
    if (s.tickMeanMs > 0.0)
        ImGui::TextDisabled("(a 60 FPS frame budget is 16.7 ms, so %.0f ticks fit in one frame)",
                            16.667 / s.tickMeanMs);
    ImGui::TextDisabled("Measured over %llu ticks. Exponential moving average, so it settles "
                        "within about a second of a change.",
                        static_cast<unsigned long long>(s.profiledTicks));

    ImGui::Separator();

    // The scale for the bars: the largest mean, so the dominant stage fills the
    // width and everything else is read against it.
    double maxMean = 0.0;
    for (int i = 0; i < static_cast<int>(Stage::Count); ++i) {
        if (static_cast<Stage>(i) == Stage::Render) continue;
        maxMean = std::max(maxMean, s.stageMeanMs[i]);
    }
    if (maxMean <= 0.0) maxMean = 1.0;

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("stages", 6, flags, ImVec2(0, -120.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Stage", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Mean ms", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Share", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Peak ms", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Total s", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Relative cost");
        ImGui::TableHeadersRow();

        for (int i = 0; i < static_cast<int>(Stage::Count); ++i) {
            const Stage st = static_cast<Stage>(i);
            const bool isRender = st == Stage::Render;
            const double mean = s.stageMeanMs[i];

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (isRender) {
                ImGui::TextDisabled("%s", stageName(st));
            } else if (mean >= maxMean * 0.999) {
                ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.35f, 1.0f), "%s", stageName(st));
            } else if (mean <= 0.0) {
                ImGui::TextDisabled("%s", stageName(st));
            } else {
                ImGui::TextUnformatted(stageName(st));
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(430.0f);
                ImGui::TextUnformatted(stageNote(st));
                if (stageIsPeriodic(st))
                    ImGui::TextDisabled("\nRuns on a schedule, so the mean is spread over the "
                                        "ticks where it does nothing. Compare the peak.");
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }

            ImGui::TableNextColumn();
            if (mean > 0.0) ImGui::Text("%.4f", mean); else ImGui::TextDisabled("--");

            ImGui::TableNextColumn();
            if (!isRender && s.tickMeanMs > 0.0 && mean > 0.0)
                ImGui::Text("%.1f%%", 100.0 * mean / s.tickMeanMs);
            else
                ImGui::TextDisabled("--");

            ImGui::TableNextColumn();
            if (s.stagePeakMs[i] > 0.0) ImGui::Text("%.3f", s.stagePeakMs[i]);
            else ImGui::TextDisabled("--");

            ImGui::TableNextColumn();
            if (s.stageTotalMs[i] > 0.0) ImGui::Text("%.2f", s.stageTotalMs[i] / 1000.0);
            else ImGui::TextDisabled("--");

            ImGui::TableNextColumn();
            if (!isRender && mean > 0.0) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%.3f ms", mean);
                ImGui::ProgressBar(static_cast<float>(mean / maxMean), ImVec2(-1, 0), buf);
            } else {
                ImGui::TextDisabled(" ");
            }
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::TextWrapped(
        "Serial stages -- act, reproduction, chemistry -- are serial by necessity, not by "
        "oversight: each mutates either the world or another agent, and running them in "
        "parallel would either need locks that cost more than the stage or a deferred buffer "
        "that would change conflict-resolution order and break reproducibility.");
    ImGui::Spacing();
    if (ImGui::Button("Reset counters")) sim.profiler().reset();
    ImGui::SameLine();
    ImGui::TextDisabled("The profiler reads no simulation state and consumes no randomness, so "
                        "it cannot change a run. Its numbers are not saved into snapshots.");

    ImGui::End();
}

}  // namespace gen
