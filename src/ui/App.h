#ifndef FIDGET_UI_APP_H
#define FIDGET_UI_APP_H

#include "core/TunerControl.h"
#include "presentation/GuiPresentation.h"
#include "ui/AcquisitionStage.h"
#include "ui/ConfigurationStage.h"
#include "ui/Fonts.h"
#include "ui/HomePage.h"
#include "ui/ProfileStage.h"
#include "ui/ProjectStage.h"
#include "ui/SessionStage.h"
#include "ui/StartupAuditStage.h"
#include "ui/StartupStage.h"
#include "ui/Theme.h"
#include "ui/UiDialogs.h"

#include <array>
#include <cstddef>
#include <string>

struct GLFWwindow;

namespace fidget {

// The application shell: owns the window, the GUI contexts, the theme and
// fonts, and runs the main loop.
class App
{
public:
    explicit App(ITunerControl& tunerControl);

    // Smoke test mode: render this many frames, then close. A negative
    // count (the default) runs until the user closes the window.
    void SetFrameLimit(long frames);

    // Runs the application. Returns the process exit code.
    int Run();

private:
    bool Init();
    void DrawFrame();
    void DrawMainMenu();
    void DrawAboutWindow();
    void DrawErrorPopup();
    void DrawRedesignedPage(
        const TunerSnapshot& snapshot,
        const GuiViewState& view,
        float shellHeight);
    void DrawLegacyWorkspace(
        const TunerSnapshot& snapshot,
        float shellHeight);
    void DrawWorkflowPanel(const TunerSnapshot& snapshot);
    void DrawStagePanel(const TunerSnapshot& snapshot);
    void DrawActivityLogPanel(const TunerSnapshot& snapshot);
    void HandleShortcuts(const GuiViewState& view);
    void ChangeFontScale(int direction);
    void BuildDefaultLayout(unsigned int dockspaceId);
    // Window geometry persistence: first run opens at a default size, later
    // runs restore the size and place the window was left at.
    void LoadWindowState(int& width, int& height, int& posX, int& posY);
    void SaveWindowState();
    void Shutdown();

    GLFWwindow* m_window = nullptr;
    ITunerControl& m_tunerControl;
    std::string m_errorMessage;
    UiDialogs m_dialogs;
    Theme m_theme;
    Fonts m_fonts;
    HomePage m_homePage;
    ProjectStage m_projectStage;
    SessionStage m_sessionStage;
    ProfileStage m_profileStage;
    StartupAuditStage m_startupAuditStage;
    ConfigurationStage m_configurationStage;
    StartupStage m_startupStage;
    AcquisitionStage m_acquisitionStage;
    GuiPresentationSelection m_guiSelection;

    long m_frameLimit = -1;

    std::array<bool, 10U> m_activityCategoryVisible{
        true, true, true, true, true,
        true, true, true, true, true,
    };
    std::array<bool, 4U> m_activitySeverityVisible{
        true, true, true, true,
    };
    bool m_activityAutoScroll = true;
    std::size_t m_lastActivityEntryCount = 0U;

    // Optional panels retain their docking state while hidden.
    bool m_showWorkflow = true;
    bool m_showActivityLog = true;
    bool m_showAbout = false;
    bool m_showLegacyWorkflow = false;

    // True until the default panel layout has been applied for the current
    // layout version.
    bool m_needDefaultLayout = false;

    // GUI state lives in the self-contained application state directory.
    std::string m_layoutFilePath;
    std::string m_layoutVersionFilePath;
    std::string m_windowStateFilePath;
};

} // namespace fidget

#endif
