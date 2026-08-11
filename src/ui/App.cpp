#include "App.h"

#include "core/GuidedWorkflow.h"
#include "core/TunerSnapshot.h"
#include "ui/WorkflowRail.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder API, used for the default layout
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"

#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace fidget {

namespace {

// Panel window titles. The default layout docks them by these names.
constexpr const char* kWorkflowTitle = "Workflow";
constexpr const char* kStageTitle = "Stage";
constexpr const char* kActivityLogTitle = "Activity Log";

void GlfwErrorCallback(int error, const char* description)
{
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

// The directory holding the running executable; the current directory as
// a fallback. Keeps FIDGET's own files (the window layout) next to the
// binary, wherever it is launched from.
std::filesystem::path ExecutableDirectory()
{
#ifdef __APPLE__
    char buffer[4096];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) == 0)
    {
        return std::filesystem::weakly_canonical(buffer).parent_path();
    }
#else
    std::error_code error;
    std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error)
    {
        return self.parent_path();
    }
#endif
    return std::filesystem::current_path();
}

} // namespace

App::App(ITunerControl& tunerControl)
    : m_tunerControl(tunerControl)
{
}

void App::SetFrameLimit(long frames)
{
    m_frameLimit = frames;
}

int App::Run()
{
    if (!Init())
    {
        return 1;
    }

    long frame = 0;
    while (!glfwWindowShouldClose(m_window))
    {
        if (m_frameLimit >= 0 && frame++ >= m_frameLimit)
        {
            break;
        }
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        DrawFrame();

        ImGui::Render();
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(m_window, &width, &height);
        glViewport(0, 0, width, height);
        const ImVec4& clear = m_theme.windowBackground;
        glClearColor(clear.x, clear.y, clear.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(m_window);
    }

    SaveWindowState();
    Shutdown();
    return 0;
}

bool App::Init()
{
    glfwSetErrorCallback(GlfwErrorCallback);
    if (!glfwInit())
    {
        return false;
    }

    // OpenGL 3.2 core profile: the newest version macOS still supports.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

    // First run: 80% of the monitor's work area, centered. Later runs: the
    // size and place the window was left at. Stay hidden until positioned
    // to avoid a jump. The work area is already in screen coordinates, so
    // no DPI scaling is applied here.
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    float scale = ImGui_ImplGlfw_GetContentScaleForMonitor(monitor);
    int workX = 0;
    int workY = 0;
    int workWidth = 1280;
    int workHeight = 800;
    glfwGetMonitorWorkarea(monitor, &workX, &workY, &workWidth, &workHeight);

    int width = static_cast<int>(workWidth * 0.8f);
    int height = static_cast<int>(workHeight * 0.8f);
    int posX = workX + (workWidth - width) / 2;
    int posY = workY + (workHeight - height) / 2;

    m_windowStateFilePath = (ExecutableDirectory() / "window.ini").string();
    LoadWindowState(width, height, posX, posY);

    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    m_window = glfwCreateWindow(width, height, "FIDGET", nullptr, nullptr);
    if (m_window == nullptr)
    {
        glfwTerminate();
        return false;
    }
    glfwSetWindowPos(m_window, posX, posY);
    glfwShowWindow(m_window);
    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(1); // vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigDpiScaleFonts = true;
    io.ConfigDpiScaleViewports = true;

    // The layout file lives next to the executable, and the default layout
    // is built only when no saved layout exists from a previous run.
    m_layoutFilePath = (ExecutableDirectory() / "imgui.ini").string();
    io.IniFilename = m_layoutFilePath.c_str();
    m_needDefaultLayout = !std::filesystem::exists(m_layoutFilePath);

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;

    m_theme = DarkTheme();
    ApplyTheme(m_theme);
    m_fonts = LoadFonts();

    ImGui_ImplGlfw_InitForOpenGL(m_window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    return true;
}

void App::DrawFrame()
{
    HandleShortcuts();

    // The status strip is fixed chrome above the dockspace; everything
    // else docks below it.
    const auto snapshot = m_tunerControl.CurrentSnapshot();
    const float stripHeight = ImGui::GetFrameHeight() + 8.0f;
    DrawStatusStrip(stripHeight, *snapshot);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + stripHeight));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, viewport->WorkSize.y - stripHeight));
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGuiWindowFlags hostFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus
        | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground
        | ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##dockspace_host", nullptr, hostFlags);
    ImGui::PopStyleVar(2);

    ImGuiID dockspaceId = ImGui::GetID("FidgetDockspace");
    ImGui::DockSpace(dockspaceId);
    if (m_needDefaultLayout)
    {
        BuildDefaultLayout(dockspaceId);
        m_needDefaultLayout = false;
    }
    ImGui::End();

    DrawWorkflowPanel(*snapshot);
    DrawStagePanel(*snapshot);
    DrawActivityLogPanel(*snapshot);
}

void App::DrawStatusStrip(float height, const TunerSnapshot& snapshot)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, height));
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings
        | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, m_theme.windowBackground);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 4.0f));
    ImGui::Begin("##status_strip", nullptr, flags);

    ImGui::TextColored(m_theme.accent, "FIDGET");

    ImGui::SameLine(0.0f, 24.0f);
    if (snapshot.projectActive)
    {
        ImGui::Text(
            "%s",
            snapshot.activeModuleName.empty()
                ? "crate project active"
                : snapshot.activeModuleName.c_str());
    }
    else
    {
        ImGui::TextDisabled("no project");
    }

    const auto decision = PlanGuidedTunerWorkflow(
        MakeGuidedTunerInputs(snapshot));
    ImGui::SameLine(0.0f, 24.0f);
    ImGui::Text(
        "step %zu/%zu: %s",
        decision.step,
        decision.totalSteps,
        GuidedTunerStageName(decision.stage));

    const char* version = "v" FIDGET_VERSION;
    float versionWidth = ImGui::CalcTextSize(version).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - versionWidth - 10.0f);
    ImGui::TextDisabled("%s", version);

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void App::DrawWorkflowPanel(const TunerSnapshot& snapshot)
{
    ImGui::Begin(kWorkflowTitle);
    const auto decision = PlanGuidedTunerWorkflow(
        MakeGuidedTunerInputs(snapshot));
    if (DrawWorkflowRail(decision, m_theme))
    {
        ImGui::SetWindowFocus(kStageTitle);
    }
    ImGui::End();
}

void App::DrawStagePanel(const TunerSnapshot& snapshot)
{
    ImGui::Begin(kStageTitle);
    const auto decision = PlanGuidedTunerWorkflow(
        MakeGuidedTunerInputs(snapshot));
    ImGui::TextColored(
        m_theme.highlight, "%s", GuidedTunerStageName(decision.stage));
    ImGui::Separator();
    ImGui::TextWrapped("%s", GuidedTunerStageMessage(decision.stage));
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (decision.step == 1U)
    {
        m_projectStage.Draw(m_tunerControl, snapshot, m_theme);
    }
    else if (decision.step == 2U)
    {
        m_sessionStage.Draw(m_tunerControl, snapshot, m_theme);
    }
    else if (decision.step == 3U)
    {
        m_profileStage.Draw(m_tunerControl, snapshot, m_theme);
    }
    else if (decision.step == 4U)
    {
        m_startupAuditStage.Draw(m_tunerControl, snapshot, m_theme);
    }
    else
    {
        m_configurationStage.Draw(m_tunerControl, snapshot, m_theme);
        if (decision.step > 5U)
        {
            ImGui::Spacing();
            ImGui::TextDisabled(
                "Startup planning and acquisition arrive in later phases.");
        }
    }
    ImGui::End();
}

void App::DrawActivityLogPanel(const TunerSnapshot& snapshot)
{
    ImGui::Begin(kActivityLogTitle);
    if (snapshot.statusMessages.empty())
    {
        ImGui::TextDisabled("No tuner status has been published.");
    }
    else
    {
        for (const auto& message : snapshot.statusMessages)
        {
            ImGui::TextWrapped("%s", message.summary.c_str());
            if (!message.detail.empty())
            {
                ImGui::TextDisabled("%s", message.detail.c_str());
            }
        }
    }
    ImGui::End();
}

void App::HandleShortcuts()
{
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Equal))
    {
        ChangeFontScale(+1);
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Minus))
    {
        ChangeFontScale(-1);
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_0))
    {
        ChangeFontScale(0);
    }
}

void App::ChangeFontScale(int direction)
{
    ImGuiStyle& style = ImGui::GetStyle();
    if (direction == 0)
    {
        style.FontScaleMain = 1.0f;
        return;
    }
    float scale = style.FontScaleMain + 0.1f * direction;
    style.FontScaleMain = std::min(std::max(scale, 0.7f), 2.0f);
}

void App::BuildDefaultLayout(unsigned int dockspaceId)
{
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

    ImGuiID center = dockspaceId;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.16f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.22f, nullptr, &center);

    ImGui::DockBuilderDockWindow(kWorkflowTitle, left);
    ImGui::DockBuilderDockWindow(kStageTitle, center);
    ImGui::DockBuilderDockWindow(kActivityLogTitle, bottom);

    ImGui::DockBuilderFinish(dockspaceId);
}

// Replaces the passed-in defaults with the saved geometry, when a valid
// window.ini exists. A bad or missing file leaves the defaults untouched.
//
// The saved position is validated against the monitors actually connected
// now: if it was saved on an external display that is no longer attached,
// the window would otherwise open off-screen and be unreachable. In that
// case the same size is re-centered on an available monitor instead.
void App::LoadWindowState(int& width, int& height, int& posX, int& posY)
{
    std::ifstream file(m_windowStateFilePath);
    int w = 0;
    int h = 0;
    int x = 0;
    int y = 0;
    if (!(file >> w >> h >> x >> y) || w < 400 || h < 300 || w > 20000 || h > 20000)
    {
        return; // missing or garbage: keep the centered 80% default
    }

    // Find the connected monitor whose work area the saved window overlaps
    // most. glfwGetMonitors lists the primary first, so a window that
    // overlaps nothing falls back to being centered on the primary.
    int monitorCount = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
    if (monitorCount == 0)
    {
        return;
    }

    int bestOverlap = -1;
    int areaX = 0;
    int areaY = 0;
    int areaW = 0;
    int areaH = 0;
    for (int i = 0; i < monitorCount; ++i)
    {
        int mx = 0;
        int my = 0;
        int mw = 0;
        int mh = 0;
        glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);
        int overlapX = std::min(x + w, mx + mw) - std::max(x, mx);
        int overlapY = std::min(y + h, my + mh) - std::max(y, my);
        int overlap = (overlapX > 0 && overlapY > 0) ? overlapX * overlapY : 0;
        if (overlap > bestOverlap)
        {
            bestOverlap = overlap;
            areaX = mx;
            areaY = my;
            areaW = mw;
            areaH = mh;
        }
    }

    // Never larger than the chosen monitor.
    width = std::min(w, areaW);
    height = std::min(h, areaH);

    if (bestOverlap > 0)
    {
        // Visible on this monitor: keep the place, nudged fully on-screen.
        posX = std::min(std::max(x, areaX), areaX + areaW - width);
        posY = std::min(std::max(y, areaY), areaY + areaH - height);
    }
    else
    {
        // The monitor the window lived on is gone: same size, re-centered.
        posX = areaX + (areaW - width) / 2;
        posY = areaY + (areaH - height) / 2;
    }
}

void App::SaveWindowState()
{
    if (m_window == nullptr)
    {
        return;
    }
    int width = 0;
    int height = 0;
    int posX = 0;
    int posY = 0;
    glfwGetWindowSize(m_window, &width, &height);
    glfwGetWindowPos(m_window, &posX, &posY);

    std::ofstream file(m_windowStateFilePath);
    if (file)
    {
        file << width << " " << height << " " << posX << " " << posY << "\n";
    }
}

void App::Shutdown()
{
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(m_window);
    glfwTerminate();
}

} // namespace fidget
