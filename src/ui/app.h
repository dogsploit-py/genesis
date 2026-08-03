// ui/app.h — the application shell and the shared UI state.
//
// The shell is a raw Win32 window with a WGL OpenGL context driving Dear ImGui
// through the win32 + opengl2 backends. No SDL, no GLFW, no GL loader, no
// prebuilt binaries: everything it links is either an OS DLL or compiled from
// vendored source. See ARCHITECTURE.md §0.
#pragma once

#include <string>
#include <vector>

#include "god/god.h"
#include "sim/simulation.h"
#include "ui/viewport.h"

namespace gen {

// Which panels are open, plus the scratch state the panels need between
// frames. Kept in one struct so panel functions stay free functions rather
// than methods on a god-object.
struct UiState {
    bool showWorld = true;
    bool showTimeBar = true;
    bool showGodToolbar = true;
    bool showTileInspector = true;
    bool showEventFeed = true;
    bool showCharts = true;
    bool showStatistics = true;
    bool showSettings = false;
    bool showInterventionLog = false;
    bool showBookmarks = false;
    bool showEconomy = false;
    bool showAbout = false;
    bool showMilestones = false;
    bool showImGuiDemo = false;
    bool showBrainInspector = false;
    bool showGenomeBrowser = false;
    bool showPopulation = false;
    bool showChemistryLab = false;
    bool showKnowledge = false;
    bool showProfiler = false;
    bool showPhylogeny = false;

    // Individual Cards: non-modal, dockable, several open at once. Each entry
    // is an agent uid; the card follows the individual, not the slot, so a card
    // stays correct even after slots are recycled around it.
    std::vector<uint64_t> openCards;
    uint64_t focusAgentUid = 0;     // which card the inspectors follow
    uint64_t compareAgentUid = 0;   // pinned for side-by-side comparison

    // Population search filters.
    char  popNameFilter[64] = {0};
    int   popStageFilter = -1;      // -1 = any
    float popMinAge = 0.0f, popMaxAge = 500.0f;
    int   popTraitFilter = -1;      // trait index, -1 = none
    float popTraitMin = -1e9f, popTraitMax = 1e9f;
    bool  popOnlyTagged = false;
    int   popSortColumn = 0;
    bool  popSortAscending = false;

    // Genome browser state.
    int   genomeChromosome = -1;    // -1 = all
    int   genomeLocusFilter = -1;   // LocusType, -1 = all
    bool  genomeShowHeatmap = true;

    // Brain inspector state.
    bool  brainShowWeights = true;
    int   brainSelectedConn = -1;
    float brainNodeScale = 1.0f;

    // Event feed.
    bool  eventFilter[static_cast<int>(EventKind::Count)];
    char  eventSearch[128] = {0};
    bool  eventAutoScroll = true;

    // Charts: which series are plotted, and over what window.
    std::vector<std::string> chartSeries;
    float chartHeight = 110.0f;
    int   chartWindowYears = 0;   // 0 = all history

    // Save / load.
    char savePath[260] = "world.gen";
    char loadPath[260] = "world.gen";
    char csvPath[260]  = "telemetry.csv";
    std::string statusMessage;
    double statusMessageTime = 0.0;

    // World regeneration dialog.
    bool     showRegenerate = false;
    long long regenSeed = 1;
    int      regenWidth = 1024;
    int      regenHeight = 1024;
    bool     regenRandomSeed = false;

    // Run-until.
    int    runUntilKind = 1;      // index into RunUntilKind
    double runUntilValue = 100.0;

    char bookmarkName[128] = {0};

    bool  layoutResetRequested = false;

    // -- god mode (M5) ------------------------------------------------------
    bool          showLuaConsole = false;
    bool          brushActive = false;
    GodActionKind brushKind = GodActionKind::BrushElevation;
    float brushRadius = 8.0f, brushIntensity = 1.0f;
    float brushElevation = 40.0f, brushWater = 3.0f, brushPlants = 300.0f;
    float brushSoil = 60.0f, brushTemperature = 5.0f, brushOreGrade = 0.6f;
    int   brushRock = 0, brushOre = 1;

    int   spawnCount = 20;
    float spawnX = 0.0f, spawnY = 0.0f, spawnRadius = 10.0f;

    PopulationFilter godFilter;
    int   godFilterStage = 0;      // combo index; -1 offset applied on use
    int   massTrait = 0, massMode = 0;
    float massValue = 0.0f;
    float selectionStrength = 1.0f;
    int   bottleneckSurvivors = 10;
    float migrateX = 0.0f, migrateY = 0.0f, migrateRadius = 10.0f;

    bool  disasterGlobal = false;
    float disasterX = 0.0f, disasterY = 0.0f, disasterRadius = 40.0f;
    float disasterIntensity = 1.0f;
    float disasterMagnitude[16] = {3.0f, 3.0f, 0.0f, 0.0f, 60.0f, 900.0f, 400.0f, 200.0f, 0.0f};

    bool  miracleRecording = false;
    char  miracleName[64] = {0};
    int   miracleHotkey = -1;
    std::vector<GodAction> miracleDraft;

    // -- phylogeny (M7) ------------------------------------------------------
    bool  phyloShowExtinct = true;
    float phyloRowHeight = 22.0f;
    int   lineageDepth = 3;

    // -- economy (M8) --------------------------------------------------------
    int   econCurrencyIndex = 0;
    char  econCurrencyName[48] = "unit";
    float econInitialHolding = 0.0f;
    bool  econShowDecree = false;

    // -- chemistry lab (M6) --------------------------------------------------
    char  chemElementFilter[64] = {0};
    char  chemSubstanceFilter[64] = {0};
    char  chemReactionFilter[64] = {0};
    int   chemClassFilter = 0;         // 0 = all, else ReactionClass + 1
    int   chemSelectedReaction = 0;
    float chemTemperature = 298.15f;
    float chemPressureAtm = 1.0f;
    float chemConcentration = 1.0f;
    bool  chemCatalyst = false;
    bool  chemElectricity = false;
    bool  chemIgnition = false;
    float chemCrossover = 0.0f;        // 0 unset, -1 always, -2 never

    // Materials workbench.
    int   matBaseIndex = 0, matSoluteIndex = 0;
    float matBaseFraction = 0.92f;
    int   matHistory[8] = {0};
    int   matHistoryCount = 0;
    int   matToolKind = 0;
    float matEdgeAngle = 30.0f;
    float matToolMass = 0.5f;

    char  luaInput[1024] = {0};
    char  luaScratch[8192] = {0};
    std::vector<std::string> luaHistory;
    int   luaHistoryPos = -1;
    bool  luaScrollToBottom = false;

    UiState() {
        for (int i = 0; i < static_cast<int>(EventKind::Count); ++i) eventFilter[i] = true;
        chartSeries.push_back("world.mean_temperature");
        chartSeries.push_back("world.total_biomass");
        chartSeries.push_back("world.ice_fraction");
    }

    void setStatus(const std::string& s);
};

class App {
public:
    App() = default;
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Creates the window and runs until the user closes it. Returns a process
    // exit code.
    int run(const WorldParams& initialWorld);

private:
    bool createWindow();
    void destroyWindow();
    bool initImGui();
    void shutdownImGui();
    void applyDarkTheme();
    void buildDefaultLayout(unsigned dockspaceId);
    void handleShortcuts();
    void drawFrame();

    void* m_hwnd = nullptr;      // HWND
    void* m_hdc = nullptr;       // HDC
    void* m_hglrc = nullptr;     // HGLRC
    bool  m_quit = false;

    Simulation m_sim;
    Viewport   m_viewport;
    UiState    m_ui;
    bool       m_layoutBuilt = false;
};

// -- panels (ui/panels.cpp) --------------------------------------------------
void drawMainMenuBar(Simulation& sim, UiState& ui, Viewport& vp, bool& quitRequested);
void drawTimeBar(Simulation& sim, UiState& ui, Viewport& vp);
void drawGodToolbar(Simulation& sim, UiState& ui, Viewport& vp);
void drawLuaConsole(Simulation& sim, UiState& ui);
// Queues a god action, and appends it to the miracle being recorded if one is.
void castGodAction(Simulation& sim, UiState& ui, const GodAction& a);
void drawEventFeed(Simulation& sim, UiState& ui, Viewport& vp);
void drawCharts(Simulation& sim, UiState& ui);
void drawStatistics(Simulation& sim, UiState& ui);
void drawSettings(Simulation& sim, UiState& ui);
void drawInterventionLog(Simulation& sim, UiState& ui);
void drawBookmarks(Simulation& sim, UiState& ui);
// -- economy (ui/econ_ui.cpp) ------------------------------------------------
void drawEconomy(Simulation& sim, UiState& ui);
void drawAbout(UiState& ui);
void drawMilestones(UiState& ui);
void drawRegenerateDialog(Simulation& sim, UiState& ui, Viewport& vp);

// -- individual inspection (ui/cards.cpp) ------------------------------------
void drawIndividualCards(Simulation& sim, UiState& ui, Viewport& vp);
void drawBrainInspector(Simulation& sim, UiState& ui);
void drawGenomeBrowser(Simulation& sim, UiState& ui);
void drawPopulationSearch(Simulation& sim, UiState& ui, Viewport& vp);

// -- chemistry and culture (ui/chem_ui.cpp) ----------------------------------
void drawChemistryLab(Simulation& sim, UiState& ui);
void drawKnowledgePanel(Simulation& sim, UiState& ui);

// -- profiler (ui/profiler_ui.cpp) -------------------------------------------
void drawProfiler(Simulation& sim, UiState& ui);

// -- phylogeny (ui/phylogeny_ui.cpp) -----------------------------------------
void drawPhylogeny(Simulation& sim, UiState& ui);
void openIndividualCard(UiState& ui, uint64_t uid);

// Shared helper: a tooltip marker that explains the model behind a control.
void helpMarker(const char* text);

}  // namespace gen
