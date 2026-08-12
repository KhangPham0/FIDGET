#ifndef FIDGET_UI_APP_H
#define FIDGET_UI_APP_H

#include "core/TunerControl.h"
#include "ui/AcquisitionStage.h"
#include "ui/ConfigurationStage.h"
#include "ui/Fonts.h"
#include "ui/ProfileStage.h"
#include "ui/ProjectStage.h"
#include "ui/SessionStage.h"
#include "ui/StartupAuditStage.h"
#include "ui/StartupStage.h"
#include "ui/Theme.h"

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
    void DrawStatusStrip(float height, const TunerSnapshot& snapshot);
    void DrawWorkflowPanel(const TunerSnapshot& snapshot);
    void DrawStagePanel(const TunerSnapshot& snapshot);
    void DrawActivityLogPanel(const TunerSnapshot& snapshot);
    void HandleShortcuts();
    void ChangeFontScale(int direction);
    void BuildDefaultLayout(unsigned int dockspaceId);
    // Window geometry persistence: first run opens at a default size, later
    // runs restore the size and place the window was left at.
    void LoadWindowState(int& width, int& height, int& posX, int& posY);
    void SaveWindowState();
    void Shutdown();

    GLFWwindow* m_window = nullptr;
    ITunerControl& m_tunerControl;
    Theme m_theme;
    Fonts m_fonts;
    ProjectStage m_projectStage;
    SessionStage m_sessionStage;
    ProfileStage m_profileStage;
    StartupAuditStage m_startupAuditStage;
    ConfigurationStage m_configurationStage;
    StartupStage m_startupStage;
    AcquisitionStage m_acquisitionStage;

    long m_frameLimit = -1;

    // True until the default panel layout has been applied. Only used when
    // no saved layout (imgui.ini) exists.
    bool m_needDefaultLayout = false;

    // The layout file lives next to the executable: FIDGET keeps its files
    // with itself instead of scattering them over the user's machine.
    std::string m_layoutFilePath;
    std::string m_windowStateFilePath;
};

} // namespace fidget

#endif
