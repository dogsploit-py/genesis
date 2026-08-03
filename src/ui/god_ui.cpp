// ui/god_ui.cpp — the god toolbar and everything divine it drives.
//
// Nothing here mutates simulation state directly. Every control queues a
// GodAction, an agent edit, or a Lua chunk, all of which are applied on the sim
// thread at a tick boundary with the world lock held exclusively, then logged
// and pushed onto the undo stack.
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

const char* kRockNames[] = {"Granite", "Basalt", "Limestone", "Sandstone", "Shale",
                            "Gneiss", "Marble", "Coal", "Alluvium", "Ice"};

const char* kOreNames[] = {"(remove ore)", "Hematite", "Magnetite", "Malachite", "Chalcopyrite",
                           "Cassiterite", "Galena", "Sphalerite", "Bauxite", "Native gold",
                           "Native copper", "Pyrolusite", "Chromite", "Wolframite", "Uraninite",
                           "Cinnabar", "Rock salt", "Native sulfur", "Niter", "Clay"};

struct BrushDef {
    GodActionKind kind;
    const char*   label;
    const char*   tooltip;
};

const BrushDef kBrushes[] = {
    {GodActionKind::BrushElevation, "Terrain",
     "Raise or lower the ground. Water follows the land: push a seabed above sea level and it "
     "drains; sink a plain and it floods."},
    {GodActionKind::BrushWater, "Water",
     "Add or remove standing water. Deeper than 1.5 m is impassable, so this is also how you "
     "draw a barrier that isolates two populations."},
    {GodActionKind::BrushPlants, "Vegetation",
     "Plant or clear biomass. Cleared ground regrows on the ecology's own terms; it is not "
     "pinned to what you painted."},
    {GodActionKind::BrushSoil, "Soil nutrients",
     "Enrich or strip N, P and K together. Nutrients are the usual limiting factor on plant "
     "growth, so this is the strongest lever on carrying capacity."},
    {GodActionKind::BrushRock, "Surface rock",
     "Replace the topmost stratum. Determines what can be quarried and what ore the tile can "
     "plausibly host."},
    {GodActionKind::BrushOre, "Ore",
     "Place or remove a deposit. Placement is patchy rather than a uniform disc, so a painted "
     "vein still looks prospected."},
    {GodActionKind::BrushTemperature, "Temperature",
     "Warm or cool a region directly. The thermal model will pull it back toward equilibrium, "
     "so this is a shock, not a permanent setting."},
};

struct DisasterDef {
    GodActionKind kind;
    const char*   label;
    const char*   tooltip;
    bool          global;      // can be applied worldwide
    const char*   magnitudeLabel;
    float         defaultMagnitude;
};

const DisasterDef kDisasters[] = {
    {GodActionKind::Drought, "Drought",
     "Strips soil moisture and suppresses rainfall for a period. The ecology responds on its own "
     "terms: biomass falls where the drought bites and recovers when it lifts.",
     true, "Years", 3.0f},
    {GodActionKind::Flood, "Flood",
     "Raises standing water, saturates the soil and drowns vegetation and agents.",
     false, "Metres of water", 3.0f},
    {GodActionKind::Storm, "Storm",
     "Heavy rain, flattened vegetation and some mortality.", false, "(unused)", 0.0f},
    {GodActionKind::Wildfire, "Wildfire",
     "Burns the standing crop and spreads outward for two days, stopping at water and bare "
     "ground. Burnt ground is briefly MORE fertile: the ash returns nutrients.",
     false, "(unused)", 0.0f},
    {GodActionKind::Earthquake, "Earthquake",
     "Displaces the ground along a noise-defined fault trace rather than lifting it uniformly.",
     false, "Max displacement (m)", 60.0f},
    {GodActionKind::Volcano, "Volcanic eruption",
     "Builds a basalt cone, sterilises the area, and triggers a three-year volcanic winter "
     "worldwide as aerosols cool the planet.",
     false, "Cone height (m)", 900.0f},
    {GodActionKind::Meteor, "Meteor impact",
     "Excavates a crater with an uplifted rim, kills almost everything in the blast radius, and "
     "starts an eight-year impact winter.",
     false, "Crater depth (m)", 400.0f},
    {GodActionKind::IceAge, "Ice age",
     "Drops the global temperature for a long period. Ice sheets advance from the poles on their "
     "own; nothing about the ice is painted directly.",
     true, "Years", 200.0f},
    {GodActionKind::Plague, "Plague",
     "An imposed mortality event. Resistance comes from the ImmuneStrength trait, so a population "
     "with a healthy immune profile genuinely fares better. NOTE: until pathogens are their own "
     "evolving entities this is not transmissible and cannot coevolve.",
     true, "(unused)", 0.0f},
};

void helpTip(const char* text) {
    if (!ImGui::IsItemHovered()) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(420.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// The population filter widget, shared by every mass operation.
void filterEditor(UiState& ui, const SimSnapshot& snap) {
    PopulationFilter& f = ui.godFilter;
    ImGui::Checkbox("Region only", &f.useRegion);
    helpTip("Restrict the operation to a circle. Uncheck to apply worldwide.");
    if (f.useRegion) {
        ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat3("##region", &f.x, 1.0f);
        ImGui::TextDisabled("centre x, centre y, radius");
    }
    ImGui::SetNextItemWidth(150.0f);
    ImGui::Combo("Stage", &ui.godFilterStage,
                 "Any\0Embryo\0Juvenile\0Adolescent\0Adult\0Senescent\0");
    f.stage = ui.godFilterStage - 1;
    ImGui::SetNextItemWidth(150.0f);
    ImGui::Combo("Sex", &f.sexMode, "Any\0Female-expressed\0Male-expressed\0Intersex\0");
    ImGui::SetNextItemWidth(90.0f);
    ImGui::DragFloat("min age", &f.minAge, 0.25f, 0.0f, 500.0f, "%.1f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::DragFloat("max age", &f.maxAge, 0.25f, 0.0f, 1.0e9f, "%.1f");

    std::string names = "(any trait)";
    names.push_back('\0');
    for (int t = 0; t < kTraitCount; ++t) {
        names += traitSpec(static_cast<Trait>(t)).name;
        names.push_back('\0');
    }
    names.push_back('\0');
    int sel = f.traitIndex + 1;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("Trait filter", &sel, names.c_str())) f.traitIndex = sel - 1;
    if (f.traitIndex >= 0) {
        ImGui::SetNextItemWidth(90.0f);
        ImGui::DragFloat("min##tf", &f.traitMin, 0.01f);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::DragFloat("max##tf", &f.traitMax, 0.01f);
    }
    ImGui::Checkbox("Tagged only", &f.taggedOnly);
    ImGui::SameLine();
    ImGui::Checkbox("Spare immortals", &f.excludeImmortal);
    ImGui::TextDisabled("Selects: %s", f.describe().c_str());
    ImGui::TextDisabled("Population: %llu", static_cast<unsigned long long>(snap.agentCount));
}

}  // namespace

void castGodAction(Simulation& sim, UiState& ui, const GodAction& a) {
    // Recording captures the act as it is cast, so a miracle is exactly the
    // sequence you performed rather than a description of it.
    if (ui.miracleRecording) ui.miracleDraft.push_back(a);
    sim.pushGodAction(a);
}

// ---------------------------------------------------------------------------
// The god toolbar
// ---------------------------------------------------------------------------

void drawGodToolbar(Simulation& sim, UiState& ui, Viewport& vp) {
    if (!ImGui::Begin("God toolbar", &ui.showGodToolbar)) { ImGui::End(); return; }

    const SimSnapshot s = sim.snapshot();

    // -- undo / redo, always at the top ------------------------------------
    {
        ImGui::BeginDisabled(s.undoDepth == 0);
        if (ImGui::Button("Undo", ImVec2(70, 0))) sim.push(CommandType::Undo);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ctrl+Z  (%u available)", s.undoDepth);
        ImGui::SameLine();
        ImGui::BeginDisabled(s.redoDepth == 0);
        if (ImGui::Button("Redo", ImVec2(70, 0))) sim.push(CommandType::Redo);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ctrl+Y  (%u available)", s.redoDepth);
        ImGui::SameLine();
        ImGui::TextDisabled("%u / %u  (%.1f MB)", s.undoDepth, s.redoDepth, s.undoMemoryMb);
        ImGui::SameLine();
        helpMarker("Undo restores the recorded before-state; redo restores the recorded "
                   "after-state. Redo does NOT re-run the action, because re-running would "
                   "consume randomness and produce a different world.");
    }

    ImGui::Separator();

    if (ImGui::BeginTabBar("godtabs", ImGuiTabBarFlags_FittingPolicyScroll)) {

        // ------------------------------------------------------------------
        if (ImGui::BeginTabItem("Brushes")) {
            ImGui::TextWrapped("Pick a brush, then paint in the world view with the left mouse "
                               "button. Every stroke is one undoable act.");
            ImGui::Separator();

            for (const BrushDef& b : kBrushes) {
                const bool active = ui.brushActive && ui.brushKind == b.kind;
                if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.45f, 0.62f, 1.0f));
                if (ImGui::Button(b.label, ImVec2(-1, 0))) {
                    ui.brushKind = b.kind;
                    ui.brushActive = true;
                }
                if (active) ImGui::PopStyleColor();
                helpTip(b.tooltip);
            }
            if (ImGui::Button("Stop painting", ImVec2(-1, 0))) ui.brushActive = false;

            ImGui::Separator();
            ImGui::SetNextItemWidth(-100.0f);
            ImGui::SliderFloat("Radius", &ui.brushRadius, 0.5f, 200.0f, "%.1f tiles");
            ImGui::SetNextItemWidth(-100.0f);
            ImGui::SliderFloat("Intensity", &ui.brushIntensity, 0.0f, 4.0f, "%.2f");
            helpTip("Brushes fall off smoothly to zero at the rim, so strokes blend instead of "
                    "stamping a hard disc.");

            switch (ui.brushKind) {
                case GodActionKind::BrushElevation:
                    ImGui::SetNextItemWidth(-100.0f);
                    ImGui::SliderFloat("Metres", &ui.brushElevation, -500.0f, 500.0f, "%.0f");
                    break;
                case GodActionKind::BrushWater:
                    ImGui::SetNextItemWidth(-100.0f);
                    ImGui::SliderFloat("Depth", &ui.brushWater, -50.0f, 50.0f, "%.1f m");
                    break;
                case GodActionKind::BrushPlants:
                    ImGui::SetNextItemWidth(-100.0f);
                    ImGui::SliderFloat("Biomass", &ui.brushPlants, -800.0f, 800.0f, "%.0f kg");
                    break;
                case GodActionKind::BrushSoil:
                    ImGui::SetNextItemWidth(-100.0f);
                    ImGui::SliderFloat("Nutrients", &ui.brushSoil, -255.0f, 255.0f, "%.0f");
                    break;
                case GodActionKind::BrushRock:
                    ImGui::SetNextItemWidth(-100.0f);
                    ImGui::Combo("Rock", &ui.brushRock, kRockNames, IM_ARRAYSIZE(kRockNames));
                    break;
                case GodActionKind::BrushOre:
                    ImGui::SetNextItemWidth(-100.0f);
                    ImGui::Combo("Ore", &ui.brushOre, kOreNames, IM_ARRAYSIZE(kOreNames));
                    ImGui::SetNextItemWidth(-100.0f);
                    ImGui::SliderFloat("Grade", &ui.brushOreGrade, 0.0f, 1.0f, "%.2f");
                    break;
                case GodActionKind::BrushTemperature:
                    ImGui::SetNextItemWidth(-100.0f);
                    ImGui::SliderFloat("Delta C", &ui.brushTemperature, -60.0f, 60.0f, "%.1f");
                    break;
                default:
                    break;
            }
            ImGui::EndTabItem();
        }

        // ------------------------------------------------------------------
        if (ImGui::BeginTabItem("Create")) {
            ImGui::TextDisabled("SPAWN");
            ImGui::SetNextItemWidth(-120.0f);
            ImGui::DragInt("Count", &ui.spawnCount, 1.0f, 1, 5000);
            ImGui::SetNextItemWidth(-120.0f);
            ImGui::DragFloat2("Position", &ui.spawnX, 1.0f);
            ImGui::SetNextItemWidth(-120.0f);
            ImGui::DragFloat("Scatter", &ui.spawnRadius, 0.5f, 0.0f, 500.0f, "%.0f tiles");
            if (vp.hasSelection()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("at cursor")) {
                    ui.spawnX = static_cast<float>(vp.selectedX());
                    ui.spawnY = static_cast<float>(vp.selectedY());
                }
            }
            if (ImGui::Button("Create founders", ImVec2(-1, 0))) {
                GodAction a;
                a.kind = GodActionKind::SpawnAgents;
                a.i0 = ui.spawnCount;
                a.x = ui.spawnX;
                a.y = ui.spawnY;
                a.radius = ui.spawnRadius;
                castGodAction(sim, ui, a);
            }
            helpTip("Creates adults with fresh random genomes, alternating the heterogametic sex "
                    "so the group is not accidentally all one type. They land only on walkable "
                    "ground.");

            ImGui::Separator();
            ImGui::TextDisabled("SPECIES AND PATHOGENS");
            ImGui::TextWrapped("Defining a whole species, or a pathogen with its own "
                               "transmissibility, virulence, latency and MHC specificity, needs "
                               "subsystems that do not exist yet. The Plague disaster is an "
                               "imposed mortality event, not an evolving organism, and says so.");
            ImGui::EndTabItem();
        }

        // ------------------------------------------------------------------
        if (ImGui::BeginTabItem("Population")) {
            filterEditor(ui, s);
            ImGui::Separator();

            auto massAction = [&](GodActionKind kind) {
                GodAction a;
                a.kind = kind;
                a.filter = ui.godFilter;
                a.i0 = ui.massTrait;
                a.i1 = ui.massMode;
                a.f0 = ui.massValue;
                a.f1 = ui.selectionStrength;
                a.f2 = ui.migrateRadius;
                if (kind == GodActionKind::Migrate || kind == GodActionKind::Teleport) {
                    a.f0 = ui.migrateX;
                    a.f1 = ui.migrateY;
                }
                if (kind == GodActionKind::Bottleneck) a.i0 = ui.bottleneckSurvivors;
                castGodAction(sim, ui, a);
            };

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.16f, 0.16f, 1.0f));
            if (ImGui::Button("Mass kill", ImVec2(-1, 0))) massAction(GodActionKind::MassKill);
            ImGui::PopStyleColor();
            helpTip("Every selected individual dies. They are stored whole on the undo stack, so "
                    "this is reversible -- including their relationships and pedigree links.");

            ImGui::Separator();
            ImGui::TextDisabled("MASS EDIT");
            {
                std::string names;
                for (int t = 0; t < kTraitCount; ++t) {
                    names += traitSpec(static_cast<Trait>(t)).name;
                    names.push_back('\0');
                }
                names.push_back('\0');
                ImGui::SetNextItemWidth(-1);
                ImGui::Combo("##masstrait", &ui.massTrait, names.c_str());
            }
            ImGui::SetNextItemWidth(140.0f);
            ImGui::Combo("Mode", &ui.massMode, "Add\0Set\0Scale\0");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-100.0f);
            ImGui::DragFloat("Value", &ui.massValue, 0.01f);
            if (ImGui::Button("Apply to selection", ImVec2(-1, 0)))
                massAction(GodActionKind::MassEdit);
            helpTip("Edits the expressed PHENOTYPE, not the genome, so the change is not "
                    "inherited. To change what descendants get, edit alleles in the Genome "
                    "Browser or apply selection pressure below.");

            ImGui::Separator();
            ImGui::TextDisabled("SELECTION PRESSURE");
            ImGui::SetNextItemWidth(-140.0f);
            ImGui::DragFloat("Favoured value", &ui.massValue, 0.01f);
            ImGui::SetNextItemWidth(-140.0f);
            ImGui::SliderFloat("Strength", &ui.selectionStrength, 0.0f, 4.0f, "%.2f");
            if (ImGui::Button("Apply selection", ImVec2(-1, 0)))
                massAction(GodActionKind::SelectionPressure);
            helpTip("Individuals far from the favoured value die with a probability set by the "
                    "strength. This is SELECTION, not engineering: the survivors are a biased "
                    "sample of what was already there, so the trait shifts across generations "
                    "rather than instantly, and only if the variation exists to select on.");

            ImGui::Separator();
            ImGui::TextDisabled("OTHER");
            ImGui::SetNextItemWidth(-140.0f);
            ImGui::DragInt("Survivors", &ui.bottleneckSurvivors, 1.0f, 0, 100000);
            if (ImGui::Button("Forced bottleneck", ImVec2(-1, 0)))
                massAction(GodActionKind::Bottleneck);
            helpTip("Keeps this many of the selection and removes the rest, indifferent to "
                    "fitness. That indifference is the point: a bottleneck is drift, not "
                    "selection, and it shows up afterwards as a collapse in heterozygosity.");

            ImGui::SetNextItemWidth(-140.0f);
            ImGui::DragFloat2("Destination", &ui.migrateX, 1.0f);
            ImGui::SetNextItemWidth(-140.0f);
            ImGui::DragFloat("Scatter##mig", &ui.migrateRadius, 0.5f, 0.0f, 500.0f, "%.0f");
            if (ImGui::Button("Forced migration", ImVec2(-1, 0)))
                massAction(GodActionKind::Migrate);

            if (ImGui::Button("Mass sterilise", ImVec2(-1, 0)))
                massAction(GodActionKind::Sterilise);
            if (ImGui::Button("Mass fertilise", ImVec2(-1, 0)))
                massAction(GodActionKind::Fertilise);
            ImGui::EndTabItem();
        }

        // ------------------------------------------------------------------
        if (ImGui::BeginTabItem("Disasters")) {
            ImGui::Checkbox("Worldwide", &ui.disasterGlobal);
            helpTip("A worldwide disaster ignores the centre and radius below.");
            if (!ui.disasterGlobal) {
                ImGui::SetNextItemWidth(-120.0f);
                ImGui::DragFloat2("Centre", &ui.disasterX, 1.0f);
                if (vp.hasSelection()) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("cursor##dis")) {
                        ui.disasterX = static_cast<float>(vp.selectedX());
                        ui.disasterY = static_cast<float>(vp.selectedY());
                    }
                }
                ImGui::SetNextItemWidth(-120.0f);
                ImGui::DragFloat("Radius##dis", &ui.disasterRadius, 1.0f, 1.0f, 2000.0f, "%.0f");
            }
            ImGui::SetNextItemWidth(-120.0f);
            ImGui::SliderFloat("Severity", &ui.disasterIntensity, 0.05f, 2.0f, "%.2f");
            ImGui::Separator();

            for (const DisasterDef& d : kDisasters) {
                const bool canBeGlobal = d.global;
                if (ImGui::Button(d.label, ImVec2(-1, 0))) {
                    GodAction a;
                    a.kind = d.kind;
                    a.intensity = ui.disasterIntensity;
                    if (ui.disasterGlobal && canBeGlobal) {
                        a.radius = 0.0f;
                    } else {
                        a.x = ui.disasterX;
                        a.y = ui.disasterY;
                        a.radius = ui.disasterRadius;
                    }
                    a.f0 = ui.disasterMagnitude[static_cast<int>(&d - kDisasters)];
                    castGodAction(sim, ui, a);
                }
                helpTip(d.tooltip);
                if (std::strcmp(d.magnitudeLabel, "(unused)") != 0) {
                    ImGui::PushID(d.label);
                    ImGui::SetNextItemWidth(-120.0f);
                    ImGui::DragFloat(d.magnitudeLabel,
                                     &ui.disasterMagnitude[static_cast<int>(&d - kDisasters)],
                                     1.0f, 0.0f, 5000.0f, "%.0f");
                    ImGui::PopID();
                }
            }

            ImGui::Separator();
            ImGui::TextDisabled("IN PROGRESS");
            if (s.activeDisasters == 0) {
                ImGui::TextDisabled("Nothing ongoing.");
            } else {
                sim.readGod([&](const GodMode& g) {
                    for (const ActiveDisaster& d : g.disasters())
                        ImGui::BulletText("%s -- %.1f years left", d.label.c_str(),
                                          static_cast<double>(d.ticksRemaining) /
                                          static_cast<double>(kHoursPerYear));
                });
            }
            ImGui::Text("Global forcing: %+.1f C, rainfall x%.2f",
                        static_cast<double>(s.temperatureOffset),
                        static_cast<double>(s.rainfallMultiplier));
            ImGui::EndTabItem();
        }

        // ------------------------------------------------------------------
        if (ImGui::BeginTabItem("Rules")) {
            ImGui::TextWrapped("Physics and rule overrides. Every one is a live config value: it "
                               "takes effect on the next tick and is recorded in the Intervention "
                               "Log.");
            ImGui::Separator();

            static const char* kRuleKeys[] = {
                "rules.metabolism_multiplier", "rules.mutation_rate_multiplier",
                "rules.lifespan_multiplier", "rules.gestation_multiplier",
                "rules.learning_rate_multiplier", "rules.reaction_rate_multiplier",
                "rules.gravity", "rules.disable_aging", "rules.disable_violence",
                "rules.disable_death",
            };
            for (const char* key : kRuleKeys) {
                for (const CfgEntry& e : cfg().entries()) {
                    if (e.key != key) continue;
                    ImGui::PushID(key);
                    if (e.type == CfgType::Bool) {
                        bool v = cfg().getBool(key);
                        if (ImGui::Checkbox(e.name.c_str(), &v)) {
                            GodAction a;
                            a.kind = GodActionKind::SetConfigValue;
                            a.text = key;
                            a.f0 = v ? 1.0f : 0.0f;
                            castGodAction(sim, ui, a);
                        }
                    } else {
                        float v = cfg().getF(key);
                        ImGui::SetNextItemWidth(-190.0f);
                        if (ImGui::SliderFloat(e.name.c_str(), &v,
                                               static_cast<float>(e.minValue),
                                               static_cast<float>(e.maxValue), "%.4g",
                                               e.logarithmic ? ImGuiSliderFlags_Logarithmic : 0)) {
                            GodAction a;
                            a.kind = GodActionKind::SetConfigValue;
                            a.text = key;
                            a.f0 = v;
                            castGodAction(sim, ui, a);
                        }
                    }
                    helpTip(e.description.c_str());
                    ImGui::PopID();
                    break;
                }
            }
            ImGui::Separator();
            if (ImGui::Button("All settings...", ImVec2(-1, 0))) ui.showSettings = true;
            ImGui::EndTabItem();
        }

        // ------------------------------------------------------------------
        if (ImGui::BeginTabItem("Miracles")) {
            ImGui::TextWrapped("A miracle is a recorded sequence of divine acts, replayable on "
                               "demand and bindable to a hotkey. Each step lands on the undo "
                               "stack in its own right, so a miracle can be unwound one act at a "
                               "time.");
            ImGui::Separator();

            ImGui::Checkbox("Recording", &ui.miracleRecording);
            helpTip("While recording, every divine act you perform is appended to the miracle "
                    "being built.");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160.0f);
            ImGui::InputTextWithHint("##mname", "miracle name", ui.miracleName,
                                     sizeof(ui.miracleName));
            ImGui::SameLine();
            ImGui::BeginDisabled(ui.miracleDraft.empty() || ui.miracleName[0] == '\0');
            if (ImGui::Button("Save")) {
                Miracle m;
                m.name = ui.miracleName;
                m.actions = ui.miracleDraft;
                m.hotkey = ui.miracleHotkey;
                sim.editGod([m](GodMode& g) { g.miracles().push_back(m); },
                            std::string("Saved miracle: ") + m.name);
                ui.miracleDraft.clear();
                ui.miracleName[0] = '\0';
                ui.miracleRecording = false;
            }
            ImGui::EndDisabled();
            ImGui::SetNextItemWidth(160.0f);
            ImGui::SliderInt("Hotkey (Ctrl+N)", &ui.miracleHotkey, -1, 9);

            ImGui::Text("Recording buffer: %zu acts", ui.miracleDraft.size());
            ImGui::SameLine();
            if (ImGui::SmallButton("clear")) ui.miracleDraft.clear();

            ImGui::Separator();
            sim.readGod([&](const GodMode& g) {
                if (g.miracles().empty()) {
                    ImGui::TextDisabled("No miracles saved.");
                    return;
                }
                for (size_t i = 0; i < g.miracles().size(); ++i) {
                    const Miracle& m = g.miracles()[i];
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::Button("Cast", ImVec2(60, 0))) {
                        Command c;
                        c.type = CommandType::CastMiracle;
                        c.ix = static_cast<int32_t>(i);
                        sim.push(c);
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("delete")) {
                        const size_t idx = i;
                        sim.editGod([idx](GodMode& g2) {
                            if (idx < g2.miracles().size())
                                g2.miracles().erase(g2.miracles().begin() +
                                                    static_cast<long>(idx));
                        }, "Deleted a miracle");
                    }
                    ImGui::SameLine();
                    if (m.hotkey >= 0) ImGui::Text("%s  [%zu acts, Ctrl+%d]", m.name.c_str(),
                                                   m.actions.size(), m.hotkey);
                    else ImGui::Text("%s  [%zu acts]", m.name.c_str(), m.actions.size());
                    ImGui::PopID();
                }
            });
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::Separator();
    if (ImGui::Button("Lua console", ImVec2(-1, 0))) ui.showLuaConsole = true;
    if (ImGui::Button("Intervention log...", ImVec2(-1, 0))) ui.showInterventionLog = true;
    ImGui::Text("%llu interventions so far",
                static_cast<unsigned long long>(s.interventionCount));

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Lua console
// ---------------------------------------------------------------------------

void drawLuaConsole(Simulation& sim, UiState& ui) {
    ImGui::SetNextWindowSize(ImVec2(760, 460), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Lua console", &ui.showLuaConsole)) { ImGui::End(); return; }

    if (ImGui::BeginTabBar("luatabs")) {
        if (ImGui::BeginTabItem("Console")) {
            const float inputHeight = ImGui::GetFrameHeightWithSpacing() * 2.4f;
            ImGui::BeginChild("output", ImVec2(0, -inputHeight), ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_HorizontalScrollbar);
            sim.readLua([&](const LuaConsole& lua) {
                for (const LuaLine& l : lua.lines()) {
                    ImVec4 col(0.85f, 0.86f, 0.9f, 1.0f);
                    if (l.kind == 0) col = ImVec4(0.55f, 0.60f, 0.70f, 1.0f);
                    else if (l.kind == 2) col = ImVec4(1.0f, 0.45f, 0.40f, 1.0f);
                    else if (l.kind == 3) col = ImVec4(0.60f, 0.85f, 0.95f, 1.0f);
                    ImGui::PushStyleColor(ImGuiCol_Text, col);
                    ImGui::TextUnformatted(l.text.c_str());
                    ImGui::PopStyleColor();
                }
            });
            if (ui.luaScrollToBottom) {
                ImGui::SetScrollHereY(1.0f);
                ui.luaScrollToBottom = false;
            }
            ImGui::EndChild();

            // Autocomplete on the token under the caret.
            static std::vector<std::string> matches;
            {
                const char* buf = ui.luaInput;
                const size_t len = std::strlen(buf);
                size_t start = len;
                while (start > 0) {
                    const char c = buf[start - 1];
                    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_')) break;
                    --start;
                }
                const std::string token(buf + start, buf + len);
                sim.readLua([&](const LuaConsole& lua) { lua.complete(token, matches); });
                if (token.empty()) matches.clear();
            }

            ImGui::SetNextItemWidth(-90.0f);
            const bool submitted = ImGui::InputText("##luainput", ui.luaInput, sizeof(ui.luaInput),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
            const bool focused = ImGui::IsItemActive();
            ImGui::SameLine();
            const bool runPressed = ImGui::Button("Run", ImVec2(-1, 0));

            if (!matches.empty() && focused) {
                ImGui::TextDisabled("Tab completes:");
                for (size_t i = 0; i < matches.size() && i < 8; ++i) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.0f, 1.0f), "%s", matches[i].c_str());
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
                    const char* buf = ui.luaInput;
                    const size_t len = std::strlen(buf);
                    size_t start = len;
                    while (start > 0) {
                        const char c = buf[start - 1];
                        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_'))
                            break;
                        --start;
                    }
                    std::string replaced(buf, buf + start);
                    replaced += matches[0];
                    std::snprintf(ui.luaInput, sizeof(ui.luaInput), "%s", replaced.c_str());
                }
            }

            if (submitted || runPressed) {
                const std::string src = ui.luaInput;
                if (!src.empty()) {
                    sim.pushScript(src);
                    ui.luaHistory.push_back(src);
                    ui.luaHistoryPos = -1;
                    ui.luaInput[0] = '\0';
                    ui.luaScrollToBottom = true;
                }
                ImGui::SetKeyboardFocusHere(-1);
            }

            // History with the up/down arrows.
            if (focused && !ui.luaHistory.empty()) {
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)) {
                    if (ui.luaHistoryPos < 0)
                        ui.luaHistoryPos = static_cast<int>(ui.luaHistory.size()) - 1;
                    else if (ui.luaHistoryPos > 0) --ui.luaHistoryPos;
                    std::snprintf(ui.luaInput, sizeof(ui.luaInput), "%s",
                                  ui.luaHistory[static_cast<size_t>(ui.luaHistoryPos)].c_str());
                } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) && ui.luaHistoryPos >= 0) {
                    ++ui.luaHistoryPos;
                    if (ui.luaHistoryPos >= static_cast<int>(ui.luaHistory.size())) {
                        ui.luaHistoryPos = -1;
                        ui.luaInput[0] = '\0';
                    } else {
                        std::snprintf(ui.luaInput, sizeof(ui.luaInput), "%s",
                                      ui.luaHistory[static_cast<size_t>(ui.luaHistoryPos)].c_str());
                    }
                }
            }

            ImGui::TextDisabled("Enter runs. Tab completes. Up/Down walks history. Scripts run on "
                                "the sim thread at a tick boundary.");
            ImGui::SameLine();
            if (ImGui::SmallButton("clear")) sim.clearLuaOutput();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Scratchpad")) {
            ImGui::TextWrapped("A multi-line buffer for longer scripts. Nothing runs until you "
                               "press Run.");
            ImGui::InputTextMultiline("##scratch", ui.luaScratch, sizeof(ui.luaScratch),
                                      ImVec2(-1, -ImGui::GetFrameHeightWithSpacing() * 1.4f));
            if (ImGui::Button("Run scratchpad", ImVec2(160, 0))) {
                const std::string src = ui.luaScratch;
                if (!src.empty()) {
                    sim.pushScript(src);
                    ui.luaScrollToBottom = true;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear", ImVec2(100, 0))) ui.luaScratch[0] = '\0';
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("API")) {
            ImGui::TextWrapped("Everything the console exposes. Type help() to print this into "
                               "the console, or help(\"agent\") to filter it.");
            ImGui::Separator();
            if (ImGui::BeginTable("api", 1, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
                sim.readLua([&](const LuaConsole& lua) {
                    for (const std::string& name : lua.completions()) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        const char* h = lua.helpFor(name);
                        ImGui::TextUnformatted(name.c_str());
                        if (h) {
                            ImGui::SameLine(220.0f);
                            ImGui::TextDisabled("%s", h);
                        }
                    }
                });
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

}  // namespace gen
