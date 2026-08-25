#ifndef FIDGET_UI_HOME_PAGE_H
#define FIDGET_UI_HOME_PAGE_H

#include "core/TunerControl.h"
#include "presentation/GuiPresentation.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"
#include "ui/UiDialogs.h"

#include <optional>
#include <string>

namespace fidget {

// The first redesigned page. It owns only transient editing and workspace
// chooser state; all connection, verification, safety, and action authority
// comes from TunerSnapshot and GuiViewState.
class HomePage
{
public:
    void Draw(
        ITunerControl& tunerControl,
        const TunerSnapshot& snapshot,
        const GuiViewState& view,
        GuiPresentationSelection& selection,
        const Theme& theme,
        const Fonts& fonts,
        UiDialogs& dialogs);

private:
    void SynchronizeDraft(const TunerSnapshot& snapshot);
    void SubmitEdit(ITunerControl& tunerControl);
    void DrawHome(
        ITunerControl& tunerControl,
        const TunerSnapshot& snapshot,
        const GuiViewState& view,
        GuiPresentationSelection& selection,
        const Theme& theme,
        const Fonts& fonts,
        UiDialogs& dialogs);
    void DrawControllerConflict(
        ITunerControl& tunerControl,
        const TunerSnapshot& snapshot,
        const GuiViewState& view,
        const Theme& theme,
        const Fonts& fonts);

    bool initialized_ = false;
    TunerTargetInput draft_;
    std::optional<TunerTargetInput> pendingInput_;
    std::string workspacePath_;
    std::optional<std::string> pendingWorkspacePath_;
};

} // namespace fidget

#endif
