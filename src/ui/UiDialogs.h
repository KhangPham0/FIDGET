#ifndef FIDGET_UI_UI_DIALOGS_H
#define FIDGET_UI_UI_DIALOGS_H

#include <optional>
#include <string>

namespace fidget {

struct UiDialogFilter
{
    const char* name = "";
    const char* extension = "";
};

class UiDialogs
{
public:
    explicit UiDialogs(std::string& errorMessage);

    [[nodiscard]] std::optional<std::string> OpenFile(
        const UiDialogFilter& filter);
    [[nodiscard]] std::optional<std::string> SaveFile(
        const UiDialogFilter& filter,
        const std::string& defaultDirectory,
        const std::string& defaultName);

    void ShowError(std::string message);

private:
    std::string& m_errorMessage;
};

} // namespace fidget

#endif
