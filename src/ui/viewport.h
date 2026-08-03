// ui/viewport.h — the world view: camera, CPU rasteriser, overlays, picking.
//
// The viewport is rasterised on the CPU into a single RGBA texture which is
// then drawn by ImGui. That is what lets the whole program run on the
// fixed-function GL2 backend with no shaders and no GL loader (see
// ARCHITECTURE.md §0).
//
// The raster is decoupled from the frame rate. Redrawing 1.4 Mpx every frame
// would cost more than the simulation does; instead the raster is regenerated
// only when the camera moves, the overlay changes, or enough time has passed
// that the world has visibly changed. The cached texture is redrawn at full
// frame rate in between, so the UI stays at 60 FPS regardless.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/jobs.h"

namespace gen {

class Simulation;
class World;

enum class Overlay : int {
    Terrain = 0,     // shaded relief + biome, the default view
    Elevation,
    Temperature,
    Rainfall,
    SoilFertility,
    SoilMoisture,
    WaterTable,
    Biome,
    SurfaceRock,
    Ore,
    Biomass,
    Drainage,
    Plates,
    // --- overlays whose data arrives with a later milestone ---
    PopulationDensity,   // M2
    AlleleFrequency,     // M2
    DiseasePrevalence,   // M5
    Territory,           // M5
    Culture,             // M5
    Pollution,           // M6
    Count
};

const char* overlayName(Overlay o);
const char* overlayDescription(Overlay o);
// Overlays past Drainage/Plates need agents, culture or chemistry to exist.
// Returning the milestone lets the UI grey them out honestly instead of
// showing a control that would silently draw nothing.
const char* overlayRequires(Overlay o);   // nullptr if available now

struct Camera {
    double centreX = 0.0;       // world tile coordinates
    double centreY = 0.0;
    double pixelsPerTile = 1.0;

    void clampTo(int worldW, int worldH, double minPPT, double maxPPT);
};

class Viewport {
public:
    Viewport();
    ~Viewport();

    Viewport(const Viewport&) = delete;
    Viewport& operator=(const Viewport&) = delete;

    void init();
    void shutdown();

    // Builds the ImGui window and draws the world into it.
    void draw(Simulation& sim);

    Overlay overlay() const { return m_overlay; }
    void setOverlay(Overlay o) { if (o != m_overlay) { m_overlay = o; m_dirty = true; } }

    // The agent the user last clicked, as a uid so it survives slot recycling.
    // 0 means nothing selected.
    uint64_t selectedAgentUid() const { return m_selectedAgentUid; }
    void     selectAgent(uint64_t uid) { m_selectedAgentUid = uid; }
    // Set by draw() when a click lands on an agent, so the app can open a card.
    uint64_t takeAgentActivation() { const uint64_t u = m_activatedAgentUid; m_activatedAgentUid = 0; return u; }

    bool hasSelection() const { return m_selX >= 0 && m_selY >= 0; }
    int  selectedX() const { return m_selX; }
    int  selectedY() const { return m_selY; }
    void select(int x, int y) { m_selX = x; m_selY = y; }
    void clearSelection() { m_selX = -1; m_selY = -1; }

    void focusOn(int x, int y);
    void resetCamera(int worldW, int worldH);

    Camera& camera() { return m_camera; }
    const Camera& camera() const { return m_camera; }

    // The tile inspector: every field of the selected tile, all of it visible.
    void drawTileInspector(Simulation& sim, bool* open);

    // Brush painting. The app sets these each frame from the god toolbar; the
    // viewport draws the preview ring and reports strokes back.
    bool  brushActive = false;
    float brushRadius = 8.0f;
    // Returns true once per painted position, with the world coordinates.
    bool takeBrushStroke(float& x, float& y);

    double lastRasterMs() const { return m_lastRasterMs; }
    bool   showGrid = false;
    bool   hideRender = false;   // "Hide render" throughput lever
    // Colour agents by detected lineage instead of by their own inherited
    // pigment. Genetic structure is invisible otherwise: two lineages can look
    // identical and still be reproductively isolated, which is exactly the
    // case worth being able to see.
    bool   colourBySpecies = false;

    // Which gene the allele-frequency overlay maps. 0 means "pick the first
    // neutral locus", which is the one worth watching: a coding locus tracks
    // selection, and a neutral one tracks drift and gene flow.
    int    alleleGeneId = 0;

    // Per-tile fields derived from the population, rebuilt each frame only for
    // the overlay that needs one. The rasteriser is given the World, not the
    // Agents -- deliberately, so it cannot reach into agent state mid-tick --
    // so anything agent-derived has to be reduced to a tile field first, under
    // the read lock, before rasterising.
    std::vector<float>    m_agentField;   // meaning depends on the overlay
    std::vector<uint32_t> m_tileSpecies;  // dominant lineage id per tile
    float                 m_agentFieldMax = 1.0f;
    bool                  m_agentFieldValid = false;
    std::string           m_alleleGeneLabel;

private:
    // Reduces agent state to a per-tile field for the agent-derived overlays.
    // Runs under the agent read lock, before rasterising.
    void buildAgentField(Simulation& sim);
    void rasterise(const World& w, int texW, int texH,
                   int tx0, int ty0, int stride);
    uint32_t sampleColour(const World& w, size_t i) const;

    JobSystem            m_rasterJobs;
    std::vector<uint32_t> m_pixels;
    unsigned             m_texture = 0;
    int                  m_texW = 0, m_texH = 0;

    Camera   m_camera;
    Overlay  m_overlay = Overlay::Terrain;
    bool     m_dirty = true;
    bool     m_cameraInitialised = false;

    int      m_selX = -1, m_selY = -1;
    int      m_hoverX = -1, m_hoverY = -1;
    uint64_t m_selectedAgentUid = 0;
    uint64_t m_activatedAgentUid = 0;
    bool     m_brushStroke = false;
    float    m_brushStrokeX = 0.0f, m_brushStrokeY = 0.0f;

    // Raster inputs from the last frame, used to detect that nothing changed.
    int      m_lastTx0 = -1, m_lastTy0 = -1, m_lastStride = -1;
    uint64_t m_lastRasterTick = ~0ull;
    double   m_rasterAgeSeconds = 0.0;
    double   m_lastRasterMs = 0.0;

    // Screen rect the world was drawn into last frame, for picking.
    float    m_imgX = 0.0f, m_imgY = 0.0f, m_imgW = 0.0f, m_imgH = 0.0f;
    int      m_viewTx0 = 0, m_viewTy0 = 0;
    int      m_viewStride = 1;
};

}  // namespace gen
