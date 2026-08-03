#include "ui/app.h"

#include <windows.h>
#include <GL/gl.h>

#include <cstdio>

#include "backends/imgui_impl_opengl2.h"
#include "backends/imgui_impl_win32.h"
#include "core/config.h"
#include "imgui.h"
#include "imgui_internal.h"

// Forward declaration from imgui_impl_win32.cpp.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

namespace gen {

namespace {
constexpr const wchar_t* kWindowClass = L"GenesisWindowClass";
App* g_app = nullptr;

LRESULT WINAPI wndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return 1;
    switch (msg) {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                glViewport(0, 0, LOWORD(lParam), HIWORD(lParam));
            }
            return 0;
        case WM_SYSCOMMAND:
            // Swallow the Alt-key menu activation, which otherwise steals focus
            // every time a shortcut uses Alt.
            if ((wParam & 0xFFF0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

typedef BOOL(WINAPI* PFNWGLSWAPINTERVALEXTPROC)(int);
PFNWGLSWAPINTERVALEXTPROC g_wglSwapInterval = nullptr;
}  // namespace

void UiState::setStatus(const std::string& s) {
    statusMessage = s;
    statusMessageTime = ImGui::GetTime();
}

void helpMarker(const char* text) {
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// ---------------------------------------------------------------------------

App::~App() {
    m_sim.stop();
    shutdownImGui();
    destroyWindow();
}

bool App::createWindow() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    if (!::RegisterClassExW(&wc)) return false;

    HWND hwnd = ::CreateWindowExW(
        0, kWindowClass, L"GENESIS", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1600, 950,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return false;
    m_hwnd = hwnd;

    HDC hdc = ::GetDC(hwnd);
    m_hdc = hdc;

    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;

    const int pf = ::ChoosePixelFormat(hdc, &pfd);
    if (pf == 0 || !::SetPixelFormat(hdc, pf, &pfd)) return false;

    HGLRC rc = ::wglCreateContext(hdc);
    if (!rc || !::wglMakeCurrent(hdc, rc)) return false;
    m_hglrc = rc;

    // Optional vsync. Absent on some drivers, in which case the toggle simply
    // has no effect and the UI says so.
    g_wglSwapInterval = reinterpret_cast<PFNWGLSWAPINTERVALEXTPROC>(
        reinterpret_cast<void*>(::wglGetProcAddress("wglSwapIntervalEXT")));
    if (g_wglSwapInterval) g_wglSwapInterval(cfg().getBool("render.vsync", true) ? 1 : 0);

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);
    return true;
}

void App::destroyWindow() {
    if (m_hglrc) {
        ::wglMakeCurrent(nullptr, nullptr);
        ::wglDeleteContext(static_cast<HGLRC>(m_hglrc));
        m_hglrc = nullptr;
    }
    if (m_hdc && m_hwnd) {
        ::ReleaseDC(static_cast<HWND>(m_hwnd), static_cast<HDC>(m_hdc));
        m_hdc = nullptr;
    }
    if (m_hwnd) {
        ::DestroyWindow(static_cast<HWND>(m_hwnd));
        m_hwnd = nullptr;
    }
    ::UnregisterClassW(kWindowClass, ::GetModuleHandleW(nullptr));
}

bool App::initImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Multi-viewport is deliberately NOT enabled: it needs a per-window HGLRC,
    // which would be the only complicated thing in an otherwise trivial GL
    // setup. Docking alone gives dockable, rearrangeable, savable panels.
    io.IniFilename = "genesis_layout.ini";

    applyDarkTheme();

    if (!ImGui_ImplWin32_Init(m_hwnd)) return false;
    if (!ImGui_ImplOpenGL2_Init()) return false;
    return true;
}

void App::shutdownImGui() {
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplOpenGL2_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
}

void App::applyDarkTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();

    // Compact and information-dense, as specified: small padding, tight rows,
    // square corners so tables and graphs align to the pixel.
    s.WindowPadding    = ImVec2(8, 6);
    s.FramePadding     = ImVec2(6, 3);
    s.CellPadding      = ImVec2(5, 2);
    s.ItemSpacing      = ImVec2(6, 4);
    s.ItemInnerSpacing = ImVec2(5, 4);
    s.ScrollbarSize    = 12;
    s.GrabMinSize      = 9;
    s.WindowRounding   = 3;
    s.ChildRounding    = 3;
    s.FrameRounding    = 3;
    s.GrabRounding     = 3;
    s.TabRounding      = 3;
    s.WindowBorderSize = 1;
    s.FrameBorderSize  = 0;
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]            = ImVec4(0.086f, 0.090f, 0.102f, 1.00f);
    c[ImGuiCol_ChildBg]             = ImVec4(0.098f, 0.102f, 0.114f, 1.00f);
    c[ImGuiCol_PopupBg]             = ImVec4(0.071f, 0.075f, 0.086f, 0.98f);
    c[ImGuiCol_Border]              = ImVec4(0.200f, 0.212f, 0.235f, 0.75f);
    c[ImGuiCol_FrameBg]             = ImVec4(0.145f, 0.153f, 0.173f, 1.00f);
    c[ImGuiCol_FrameBgHovered]      = ImVec4(0.200f, 0.212f, 0.243f, 1.00f);
    c[ImGuiCol_FrameBgActive]       = ImVec4(0.243f, 0.259f, 0.298f, 1.00f);
    c[ImGuiCol_TitleBg]             = ImVec4(0.071f, 0.075f, 0.086f, 1.00f);
    c[ImGuiCol_TitleBgActive]       = ImVec4(0.129f, 0.196f, 0.263f, 1.00f);
    c[ImGuiCol_MenuBarBg]           = ImVec4(0.106f, 0.110f, 0.125f, 1.00f);
    c[ImGuiCol_Header]              = ImVec4(0.157f, 0.220f, 0.290f, 1.00f);
    c[ImGuiCol_HeaderHovered]       = ImVec4(0.196f, 0.286f, 0.376f, 1.00f);
    c[ImGuiCol_HeaderActive]        = ImVec4(0.235f, 0.345f, 0.451f, 1.00f);
    c[ImGuiCol_Button]              = ImVec4(0.169f, 0.192f, 0.227f, 1.00f);
    c[ImGuiCol_ButtonHovered]       = ImVec4(0.235f, 0.310f, 0.400f, 1.00f);
    c[ImGuiCol_ButtonActive]        = ImVec4(0.290f, 0.400f, 0.510f, 1.00f);
    c[ImGuiCol_CheckMark]           = ImVec4(0.420f, 0.702f, 0.941f, 1.00f);
    c[ImGuiCol_SliderGrab]          = ImVec4(0.325f, 0.549f, 0.741f, 1.00f);
    c[ImGuiCol_SliderGrabActive]    = ImVec4(0.420f, 0.702f, 0.941f, 1.00f);
    c[ImGuiCol_Separator]           = ImVec4(0.200f, 0.212f, 0.235f, 1.00f);
    c[ImGuiCol_Tab]                 = ImVec4(0.118f, 0.141f, 0.176f, 1.00f);
    c[ImGuiCol_TabHovered]          = ImVec4(0.220f, 0.322f, 0.424f, 1.00f);
    c[ImGuiCol_TableHeaderBg]       = ImVec4(0.137f, 0.153f, 0.184f, 1.00f);
    c[ImGuiCol_TableBorderStrong]   = ImVec4(0.220f, 0.235f, 0.267f, 1.00f);
    c[ImGuiCol_TableBorderLight]    = ImVec4(0.157f, 0.169f, 0.192f, 1.00f);
    c[ImGuiCol_TableRowBgAlt]       = ImVec4(1.000f, 1.000f, 1.000f, 0.025f);
    c[ImGuiCol_PlotLines]           = ImVec4(0.420f, 0.702f, 0.941f, 1.00f);
    c[ImGuiCol_PlotHistogram]       = ImVec4(0.900f, 0.700f, 0.300f, 1.00f);
    c[ImGuiCol_DockingPreview]      = ImVec4(0.235f, 0.345f, 0.451f, 0.70f);

    s.ScaleAllSizes(cfg().getF("render.ui_scale", 1.0f));
    ImGui::GetIO().FontGlobalScale = cfg().getF("render.ui_scale", 1.0f);
}

void App::buildDefaultLayout(unsigned dockspaceId) {
    ImGuiID root = dockspaceId;
    ImGui::DockBuilderRemoveNode(root);
    ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(root, ImGui::GetMainViewport()->WorkSize);

    ImGuiID top = 0, bottom = 0, left = 0, right = 0, centre = root;
    top    = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Up,   0.075f, nullptr, &centre);
    bottom = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.300f, nullptr, &centre);
    left   = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.180f, nullptr, &centre);
    right  = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right,0.260f, nullptr, &centre);

    ImGui::DockBuilderDockWindow("Time and speed", top);
    ImGui::DockBuilderDockWindow("God toolbar", left);
    ImGui::DockBuilderDockWindow("World", centre);
    ImGui::DockBuilderDockWindow("Tile inspector", right);
    ImGui::DockBuilderDockWindow("Statistics", right);
    ImGui::DockBuilderDockWindow("Economy", right);
    ImGui::DockBuilderDockWindow("Brain inspector", right);
    ImGui::DockBuilderDockWindow("Genome browser", right);
    ImGui::DockBuilderDockWindow("Population", bottom);
    ImGui::DockBuilderDockWindow("Event feed", bottom);
    ImGui::DockBuilderDockWindow("Charts", bottom);
    ImGui::DockBuilderDockWindow("Intervention log", bottom);
    ImGui::DockBuilderDockWindow("Lua console", bottom);
    ImGui::DockBuilderFinish(root);
}

void App::handleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureKeyboard && io.WantTextInput) return;

    const bool ctrl = io.KeyCtrl;

    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        const SimSnapshot s = m_sim.snapshot();
        m_sim.pushPause(!s.paused);
    }
    for (int i = 0; i < kSpeedPresetCount; ++i) {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + i), false))
            m_sim.pushSpeed(kSpeedPresets[i]);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_M, false)) m_sim.push(CommandType::SetMaxSpeed);
    if (ImGui::IsKeyPressed(ImGuiKey_Period, false)) m_sim.pushStep(1);
    if (ImGui::IsKeyPressed(ImGuiKey_G, false)) m_viewport.showGrid = !m_viewport.showGrid;
    if (ImGui::IsKeyPressed(ImGuiKey_H, false)) m_viewport.hideRender = !m_viewport.hideRender;

    // Undo / redo, and Ctrl+0..9 to cast a bound miracle.
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) m_sim.push(CommandType::Undo);
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) m_sim.push(CommandType::Redo);
    for (int k = 0; k <= 9; ++k) {
        if (!ctrl || !ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_0 + k), false)) continue;
        int index = -1;
        m_sim.readGod([&](const GodMode& g) {
            for (size_t i = 0; i < g.miracles().size(); ++i)
                if (g.miracles()[i].hotkey == k) { index = static_cast<int>(i); break; }
        });
        if (index >= 0) {
            Command c;
            c.type = CommandType::CastMiracle;
            c.ix = index;
            m_sim.push(c);
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F6, false))
        m_ui.showPhylogeny = !m_ui.showPhylogeny;
    if (ImGui::IsKeyPressed(ImGuiKey_F9, false))
        m_ui.showProfiler = !m_ui.showProfiler;
    if (ImGui::IsKeyPressed(ImGuiKey_F7, false))
        m_ui.showChemistryLab = !m_ui.showChemistryLab;
    if (ImGui::IsKeyPressed(ImGuiKey_F8, false))
        m_ui.showKnowledge = !m_ui.showKnowledge;

    // Escape puts the brush down.
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) m_ui.brushActive = false;

    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        Command c;
        c.type = CommandType::SaveSnapshot;
        c.text = m_ui.savePath;
        m_sim.push(c);
        m_ui.setStatus(std::string("Saving to ") + m_ui.savePath);
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) {
        Command c;
        c.type = CommandType::LoadSnapshot;
        c.text = m_ui.loadPath;
        m_sim.push(c);
        m_ui.setStatus(std::string("Loading ") + m_ui.loadPath);
    }

    // Camera: arrows pan by a tenth of the view, +/- zoom about the centre.
    Camera& cam = m_viewport.camera();
    const double panStep = 40.0 / cam.pixelsPerTile;
    bool moved = false;
    if (ImGui::IsKeyDown(ImGuiKey_LeftArrow))  { cam.centreX -= panStep; moved = true; }
    if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) { cam.centreX += panStep; moved = true; }
    if (ImGui::IsKeyDown(ImGuiKey_UpArrow))    { cam.centreY -= panStep; moved = true; }
    if (ImGui::IsKeyDown(ImGuiKey_DownArrow))  { cam.centreY += panStep; moved = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_Equal, true) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, true)) {
        cam.pixelsPerTile *= 1.25;
        moved = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Minus, true) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, true)) {
        cam.pixelsPerTile /= 1.25;
        moved = true;
    }
    if (moved) m_viewport.setOverlay(m_viewport.overlay());  // marks the raster dirty
}

void App::drawFrame() {
    // Full-window dockspace under the menu bar.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##dockhost", nullptr,
                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_MenuBar);
    ImGui::PopStyleVar(3);

    const ImGuiID dockspaceId = ImGui::GetID("GenesisDockspace");
    if (!m_layoutBuilt || m_ui.layoutResetRequested) {
        if (m_ui.layoutResetRequested || ImGui::DockBuilderGetNode(dockspaceId) == nullptr)
            buildDefaultLayout(dockspaceId);
        m_layoutBuilt = true;
        m_ui.layoutResetRequested = false;
    }
    ImGui::DockSpace(dockspaceId, ImVec2(0, 0), ImGuiDockNodeFlags_None);

    bool quitRequested = false;
    drawMainMenuBar(m_sim, m_ui, m_viewport, quitRequested);
    if (quitRequested) m_quit = true;
    ImGui::End();

    handleShortcuts();

    if (m_ui.showTimeBar)         drawTimeBar(m_sim, m_ui, m_viewport);
    if (m_ui.showGodToolbar)      drawGodToolbar(m_sim, m_ui, m_viewport);
    if (m_ui.showWorld) {
        m_viewport.brushActive = m_ui.brushActive;
        m_viewport.brushRadius = m_ui.brushRadius;
        m_viewport.draw(m_sim);
        // Clicking an agent in the world opens its Individual Card -- unless a
        // brush is active, in which case the left button belongs to the brush.
        if (const uint64_t activated = m_viewport.takeAgentActivation())
            if (!m_ui.brushActive) openIndividualCard(m_ui, activated);

        float bx = 0.0f, by = 0.0f;
        if (m_ui.brushActive && m_viewport.takeBrushStroke(bx, by)) {
            GodAction a;
            a.kind = m_ui.brushKind;
            a.x = bx;
            a.y = by;
            a.radius = m_ui.brushRadius;
            a.intensity = m_ui.brushIntensity;
            switch (m_ui.brushKind) {
                case GodActionKind::BrushElevation:   a.f0 = m_ui.brushElevation; break;
                case GodActionKind::BrushWater:       a.f0 = m_ui.brushWater; break;
                case GodActionKind::BrushPlants:      a.f0 = m_ui.brushPlants; break;
                case GodActionKind::BrushSoil:        a.f0 = m_ui.brushSoil; break;
                case GodActionKind::BrushTemperature: a.f0 = m_ui.brushTemperature; break;
                case GodActionKind::BrushRock:        a.i0 = m_ui.brushRock; break;
                case GodActionKind::BrushOre:
                    a.i0 = m_ui.brushOre;
                    a.f0 = m_ui.brushOreGrade;
                    break;
                default: break;
            }
            castGodAction(m_sim, m_ui, a);
        }
    }
    if (m_ui.showTileInspector)   m_viewport.drawTileInspector(m_sim, &m_ui.showTileInspector);
    if (m_ui.showEventFeed)       drawEventFeed(m_sim, m_ui, m_viewport);
    if (m_ui.showCharts)          drawCharts(m_sim, m_ui);
    if (m_ui.showStatistics)      drawStatistics(m_sim, m_ui);
    if (m_ui.showSettings)        drawSettings(m_sim, m_ui);
    if (m_ui.showInterventionLog) drawInterventionLog(m_sim, m_ui);
    if (m_ui.showBookmarks)       drawBookmarks(m_sim, m_ui);
    if (m_ui.showProfiler)        drawProfiler(m_sim, m_ui);
    if (m_ui.showPhylogeny)       drawPhylogeny(m_sim, m_ui);
    if (m_ui.showChemistryLab)    drawChemistryLab(m_sim, m_ui);
    if (m_ui.showKnowledge)       drawKnowledgePanel(m_sim, m_ui);
    if (m_ui.showEconomy)         drawEconomy(m_sim, m_ui);
    if (m_ui.showLuaConsole)      drawLuaConsole(m_sim, m_ui);
    if (m_ui.showPopulation)      drawPopulationSearch(m_sim, m_ui, m_viewport);
    if (m_ui.showBrainInspector)  drawBrainInspector(m_sim, m_ui);
    if (m_ui.showGenomeBrowser)   drawGenomeBrowser(m_sim, m_ui);
    drawIndividualCards(m_sim, m_ui, m_viewport);
    if (m_ui.showAbout)           drawAbout(m_ui);
    if (m_ui.showMilestones)      drawMilestones(m_ui);
    if (m_ui.showRegenerate)      drawRegenerateDialog(m_sim, m_ui, m_viewport);
    if (m_ui.showImGuiDemo)       ImGui::ShowDemoWindow(&m_ui.showImGuiDemo);
}

int App::run(const WorldParams& initialWorld) {
    g_app = this;

    if (!createWindow()) {
        ::MessageBoxW(nullptr, L"Failed to create the OpenGL window.", L"GENESIS", MB_ICONERROR);
        return 1;
    }
    if (!initImGui()) {
        ::MessageBoxW(nullptr, L"Failed to initialise the UI backend.", L"GENESIS", MB_ICONERROR);
        return 1;
    }
    m_viewport.init();

    m_ui.regenSeed = static_cast<long long>(initialWorld.seed);
    m_ui.regenWidth = initialWorld.width;
    m_ui.regenHeight = initialWorld.height;

    m_sim.start(initialWorld);

    while (!m_quit) {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) m_quit = true;
        }
        if (m_quit) break;

        if (::IsIconic(static_cast<HWND>(m_hwnd))) {
            ::Sleep(10);
            continue;
        }

        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        drawFrame();

        ImGui::Render();
        RECT rc;
        ::GetClientRect(static_cast<HWND>(m_hwnd), &rc);
        glViewport(0, 0, rc.right - rc.left, rc.bottom - rc.top);
        glClearColor(0.04f, 0.045f, 0.055f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        ::SwapBuffers(static_cast<HDC>(m_hdc));
    }

    m_sim.stop();
    m_viewport.shutdown();
    shutdownImGui();
    destroyWindow();
    g_app = nullptr;
    return 0;
}

}  // namespace gen
