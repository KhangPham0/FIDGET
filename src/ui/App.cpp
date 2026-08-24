#include "App.h"

#include "core/ActivityLog.h"
#include "core/ApplicationStorage.h"
#include "core/GuidedWorkflow.h"
#include "core/TunerSnapshot.h"
#include "presentation/GuiPresentation.h"
#include "ui/GuiShell.h"
#include "ui/WorkflowRail.h"
#include "ui/fonts/IconsFontAwesome5.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder API, used for the default layout
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include "nfd.h"

#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>

// Shortcut labels use the platform's conventional modifier name. ImGui
// maps the Ctrl modifier to Command on macOS.
#ifdef __APPLE__
#define FIDGET_MOD "Cmd"
#else
#define FIDGET_MOD "Ctrl"
#endif

namespace fidget {

namespace {

// Panel window titles. The default layout docks them by these names.
constexpr const char* kWorkflowTitle = "Workflow";
constexpr const char* kStageTitle = "Stage";
constexpr const char* kActivityLogTitle = "Activity Log";
// Bump this whenever a dockable window is added, removed, or retitled.
constexpr int kLayoutVersion = 1;

constexpr std::array<ActivityLogCategory, 10U> ActivityCategories{
    ActivityLogCategory::Session,
    ActivityLogCategory::Audit,
    ActivityLogCategory::Capture,
    ActivityLogCategory::Apply,
    ActivityLogCategory::Startup,
    ActivityLogCategory::Acquisition,
    ActivityLogCategory::Source,
    ActivityLogCategory::Preview,
    ActivityLogCategory::Recovery,
    ActivityLogCategory::Export,
};

constexpr std::array<TunerStatusLevel, 4U> ActivitySeverities{
    TunerStatusLevel::Information,
    TunerStatusLevel::Success,
    TunerStatusLevel::Warning,
    TunerStatusLevel::Error,
};

std::optional<ActivityLogCategory> FocusedActivityCategory(
    const GuidedTunerOperation operation)
{
    switch (operation)
    {
    case GuidedTunerOperation::Audit:
        return ActivityLogCategory::Audit;
    case GuidedTunerOperation::ConfigurationCapture:
        return ActivityLogCategory::Capture;
    case GuidedTunerOperation::ProfileApplication:
        return ActivityLogCategory::Apply;
    case GuidedTunerOperation::StartupPreparation:
        return ActivityLogCategory::Startup;
    case GuidedTunerOperation::Acquisition:
        return ActivityLogCategory::Acquisition;
    case GuidedTunerOperation::None:
    case GuidedTunerOperation::Other:
        return std::nullopt;
    }
    return std::nullopt;
}

std::size_t ActivityCategoryIndex(const ActivityLogCategory category)
{
    const auto found = std::find(
        ActivityCategories.begin(), ActivityCategories.end(), category);
    return static_cast<std::size_t>(
        std::distance(ActivityCategories.begin(), found));
}

std::size_t ActivitySeverityIndex(const TunerStatusLevel severity)
{
    const auto found = std::find(
        ActivitySeverities.begin(), ActivitySeverities.end(), severity);
    return static_cast<std::size_t>(
        std::distance(ActivitySeverities.begin(), found));
}

ImVec4 ActivitySeverityColor(
    const TunerStatusLevel severity,
    const Theme& theme)
{
    switch (severity)
    {
    case TunerStatusLevel::Information:
        return theme.textPrimary;
    case TunerStatusLevel::Success:
        return theme.statusGood;
    case TunerStatusLevel::Warning:
        return theme.statusWarning;
    case TunerStatusLevel::Error:
        return theme.statusError;
    }
    return theme.textPrimary;
}

void DrawActivityFilterChip(
    const char* label,
    bool& selected,
    const Theme& theme)
{
    ImGui::PushStyleColor(
        ImGuiCol_Button, selected ? theme.accentActive : theme.frame);
    ImGui::PushStyleColor(
        ImGuiCol_ButtonHovered,
        selected ? theme.accentHover : theme.frameHover);
    ImGui::PushStyleColor(
        ImGuiCol_Text,
        selected ? theme.textOnAccent : theme.textPrimary);
    if (ImGui::SmallButton(label))
    {
        selected = !selected;
    }
    ImGui::PopStyleColor(3);
}

void GlfwErrorCallback(int error, const char* description)
{
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

int ReadLayoutVersion(const std::string& path)
{
    std::ifstream file(path);
    int version = 0;
    if (!(file >> version))
    {
        return 0;
    }

    file >> std::ws;
    return file.eof() ? version : 0;
}

void WriteLayoutVersion(const std::string& path)
{
    std::ofstream file(path, std::ios::trunc);
    if (file)
    {
        file << kLayoutVersion << '\n';
    }
}

} // namespace

App::App(ITunerControl& tunerControl)
    : m_tunerControl(tunerControl),
      m_dialogs(m_errorMessage)
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

    const auto storagePaths = DefaultApplicationStoragePaths();
    const auto storageReady =
        EnsureApplicationStorageDirectories(storagePaths);
    if (!storageReady.success)
    {
        std::fprintf(
            stderr, "could not initialize application storage: %s\n",
            storageReady.message.c_str());
        glfwTerminate();
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

    m_windowStateFilePath = storagePaths.windowStateFile.string();
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

    if (NFD_Init() != NFD_OKAY)
    {
        std::fprintf(stderr, "could not initialize the file dialog library\n");
        glfwDestroyWindow(m_window);
        glfwTerminate();
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigDpiScaleFonts = true;
    io.ConfigDpiScaleViewports = true;

    // A missing layout or a version mismatch rebuilds the default before
    // normal persistence resumes in the self-contained state directory.
    m_layoutFilePath = storagePaths.imguiIniFile.string();
    m_layoutVersionFilePath = storagePaths.layoutVersionFile.string();
    io.IniFilename = m_layoutFilePath.c_str();
    const int storedLayoutVersion = ReadLayoutVersion(m_layoutVersionFilePath);
    m_needDefaultLayout = !std::filesystem::exists(m_layoutFilePath)
        || storedLayoutVersion != kLayoutVersion;

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;

    m_theme = LightTheme();
    ApplyTheme(m_theme);
    m_fonts = LoadFonts();

    ImGui_ImplGlfw_InitForOpenGL(m_window, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    return true;
}

void App::DrawFrame()
{
    DrawMainMenu();

    const auto snapshot = m_tunerControl.CurrentSnapshot();
    m_profileStage.ReportFileOperationResult(*snapshot, m_dialogs);

    GuiViewState guiView = PresentGui(*snapshot, m_guiSelection);
    m_guiSelection.drawer = guiView.drawer;
    HandleShortcuts(guiView);
    guiView = PresentGui(*snapshot, m_guiSelection);

    GuiShellResult shellResult;
    const float shellHeight = DrawGuiShellHeader(
        guiView, m_theme, m_fonts, shellResult);
    if (shellResult.requestedDrawer != GuiDrawer::Count)
    {
        m_guiSelection.drawer = shellResult.requestedDrawer;
        guiView = PresentGui(*snapshot, m_guiSelection);
        m_guiSelection.drawer = guiView.drawer;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(
        viewport->WorkPos.x, viewport->WorkPos.y + shellHeight));
    ImGui::SetNextWindowSize(ImVec2(
        viewport->WorkSize.x, viewport->WorkSize.y - shellHeight));
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
        WriteLayoutVersion(m_layoutVersionFilePath);
        m_needDefaultLayout = false;
    }
    ImGui::End();

    if (m_showWorkflow)
    {
        DrawWorkflowPanel(*snapshot);
    }
    DrawStagePanel(*snapshot);
    if (m_showActivityLog)
    {
        DrawActivityLogPanel(*snapshot);
    }
    DrawGuiShellDrawer(
        guiView, shellHeight, m_theme, m_fonts, shellResult);
    if (shellResult.closeDrawer)
        m_guiSelection.drawer = GuiDrawer::None;
    DrawAboutWindow();
    DrawErrorPopup();
}

void App::DrawMainMenu()
{
    if (!ImGui::BeginMainMenuBar())
    {
        return;
    }

    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem(ICON_FA_SIGN_OUT_ALT "  Quit"))
        {
            glfwSetWindowShouldClose(m_window, GLFW_TRUE);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View"))
    {
        ImGui::MenuItem(
            ICON_FA_STREAM "  Workflow",
            FIDGET_MOD "+B",
            &m_showWorkflow);
        ImGui::MenuItem(
            ICON_FA_HISTORY "  Activity Log",
            FIDGET_MOD "+L",
            &m_showActivityLog);
        ImGui::Separator();
        if (ImGui::MenuItem(
                ICON_FA_SEARCH_PLUS "  Larger text", FIDGET_MOD "+="))
        {
            ChangeFontScale(+1);
        }
        if (ImGui::MenuItem(
                ICON_FA_SEARCH_MINUS "  Smaller text", FIDGET_MOD "+-"))
        {
            ChangeFontScale(-1);
        }
        if (ImGui::MenuItem(
                ICON_FA_FONT "  Reset text size", FIDGET_MOD "+0"))
        {
            ChangeFontScale(0);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_WINDOW_RESTORE "  Reset layout"))
        {
            m_showWorkflow = true;
            m_showActivityLog = true;
            m_needDefaultLayout = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("About"))
    {
        if (ImGui::MenuItem(ICON_FA_INFO_CIRCLE "  About FIDGET"))
        {
            m_showAbout = true;
        }
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

void App::DrawAboutWindow()
{
    if (!m_showAbout)
    {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(440.0F, 0.0F), ImGuiCond_Appearing);
    if (ImGui::Begin(
            "About FIDGET",
            &m_showAbout,
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking))
    {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.5F);
        ImGui::TextUnformatted("FIDGET");
        ImGui::PopFont();
        ImGui::PushStyleColor(
            ImGuiCol_Text,
            ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped(
            "Frontend for Interactive Data Graphing and Electronics Tuning");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextWrapped(
            "FIDGET is a tuner for MDPP-32 SCP digitizers behind an MVLC. "
            "It guards parameter writes with ownership checks and exact "
            "readback, and verifies every restore.");
        ImGui::Spacing();
        ImGui::TextDisabled("Built on Dear ImGui, ImPlot, and GLFW.");
    }
    ImGui::End();
}

void App::DrawErrorPopup()
{
    if (!m_errorMessage.empty())
    {
        ImGui::OpenPopup("Error");
    }
    if (ImGui::BeginPopupModal(
            "Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0F);
        ImGui::TextWrapped("%s", m_errorMessage.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(120.0F, 0.0F)))
        {
            m_errorMessage.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
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
        m_projectStage.Draw(
            m_tunerControl, snapshot, m_theme, m_dialogs);
    }
    else if (decision.step == 2U)
    {
        m_sessionStage.Draw(m_tunerControl, snapshot, m_theme);
    }
    else if (decision.step == 3U)
    {
        m_profileStage.Draw(
            m_tunerControl, snapshot, m_theme, m_dialogs);
    }
    else if (decision.step == 4U)
    {
        m_startupAuditStage.Draw(m_tunerControl, snapshot, m_theme);
    }
    else if (decision.step == 5U)
    {
        m_configurationStage.Draw(m_tunerControl, snapshot, m_theme);
    }
    else if (decision.step == 6U)
    {
        m_startupStage.Draw(m_tunerControl, snapshot, m_theme);
    }
    else
    {
        m_acquisitionStage.Draw(m_tunerControl, snapshot, m_theme);
    }
    ImGui::End();
}

void App::DrawActivityLogPanel(const TunerSnapshot& snapshot)
{
    ImGui::Begin(kActivityLogTitle);
    if (!ImGui::CollapsingHeader(
            "Activity history", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Categories");
    for (std::size_t index = 0U; index < ActivityCategories.size(); ++index)
    {
        if (index != 0U)
        {
            ImGui::SameLine();
        }
        ImGui::PushID(static_cast<int>(index));
        DrawActivityFilterChip(
            ActivityLogCategoryName(ActivityCategories[index]),
            m_activityCategoryVisible[index],
            m_theme);
        ImGui::PopID();
    }

    ImGui::TextDisabled("Severity");
    for (std::size_t index = 0U; index < ActivitySeverities.size(); ++index)
    {
        if (index != 0U)
        {
            ImGui::SameLine();
        }
        ImGui::PushID(static_cast<int>(ActivityCategories.size() + index));
        DrawActivityFilterChip(
            TunerStatusLevelName(ActivitySeverities[index]),
            m_activitySeverityVisible[index],
            m_theme);
        ImGui::PopID();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &m_activityAutoScroll);

    if (!snapshot.activityLogPersistenceError.empty())
    {
        ImGui::TextColored(
            m_theme.statusError,
            "Activity persistence error: %s",
            snapshot.activityLogPersistenceError.c_str());
    }

    const auto& entries = snapshot.activityLog.Entries();
    std::size_t focusedEntry = entries.size();
    if (const auto category = FocusedActivityCategory(
            snapshot.activeOperation))
    {
        for (std::size_t index = entries.size(); index > 0U; --index)
        {
            if (entries[index - 1U].category == *category)
            {
                focusedEntry = index - 1U;
                break;
            }
        }
    }

    ImGui::Separator();
    ImGui::BeginChild(
        "activity_entries", ImVec2(0.0F, 0.0F), false,
        ImGuiWindowFlags_HorizontalScrollbar);
    if (entries.empty())
    {
        ImGui::TextDisabled("No activity has been recorded.");
    }
    for (std::size_t index = 0U; index < entries.size(); ++index)
    {
        const auto& entry = entries[index];
        const std::size_t categoryIndex = ActivityCategoryIndex(
            entry.category);
        const std::size_t severityIndex = ActivitySeverityIndex(
            entry.severity);
        if (categoryIndex >= m_activityCategoryVisible.size()
            || severityIndex >= m_activitySeverityVisible.size()
            || !m_activityCategoryVisible[categoryIndex]
            || !m_activitySeverityVisible[severityIndex])
        {
            continue;
        }

        const ImVec4 color = index == focusedEntry
            ? m_theme.highlight
            : ActivitySeverityColor(entry.severity, m_theme);
        const std::string line = FormatActivityLogEntry(entry);
        ImGui::TextColored(color, "%s", line.c_str());
    }
    if (m_activityAutoScroll
        && entries.size() != m_lastActivityEntryCount)
    {
        ImGui::SetScrollHereY(1.0F);
    }
    ImGui::EndChild();
    m_lastActivityEntryCount = entries.size();
    ImGui::End();
}

void App::HandleShortcuts(const GuiViewState& view)
{
    if (view.drawer != GuiDrawer::None
        && Allows(view.allowedActions, GuiAction::CloseDrawer)
        && ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        m_guiSelection.drawer = GuiDrawer::None;
        return;
    }

    // Do not treat shortcuts as commands while the user is editing a field.
    if (ImGui::GetIO().WantTextInput)
    {
        return;
    }

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_B))
    {
        m_showWorkflow = !m_showWorkflow;
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_L))
    {
        m_showActivityLog = !m_showActivityLog;
    }
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
    NFD_Quit();
    glfwDestroyWindow(m_window);
    glfwTerminate();
}

} // namespace fidget
