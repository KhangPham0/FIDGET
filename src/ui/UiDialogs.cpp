#include "ui/UiDialogs.h"

#include "nfd.h"

#include <utility>

namespace fidget {
namespace {

const char* OptionalPath(const std::string& path)
{
    return path.empty() ? nullptr : path.c_str();
}

} // namespace

UiDialogs::UiDialogs(std::string& errorMessage)
    : m_errorMessage(errorMessage)
{
}

std::optional<std::string> UiDialogs::OpenFile(
    const UiDialogFilter& filter)
{
    nfdu8char_t* selectedPath = nullptr;
    const nfdu8filteritem_t nfdFilter{
        filter.name,
        filter.extension,
    };
    const nfdresult_t outcome = NFD_OpenDialogU8(
        &selectedPath, &nfdFilter, 1U, nullptr);
    if (outcome == NFD_OKAY)
    {
        std::string path(selectedPath);
        NFD_FreePathU8(selectedPath);
        return path;
    }
    if (outcome == NFD_ERROR)
    {
        const char* error = NFD_GetError();
        ShowError(error == nullptr
                ? "The native file dialog failed."
                : error);
    }
    // NFD_CANCEL means the user changed their mind; nothing to report.
    return std::nullopt;
}

std::optional<std::string> UiDialogs::SaveFile(
    const UiDialogFilter& filter,
    const std::string& defaultDirectory,
    const std::string& defaultName)
{
    nfdu8char_t* selectedPath = nullptr;
    const nfdu8filteritem_t nfdFilter{
        filter.name,
        filter.extension,
    };
    const nfdresult_t outcome = NFD_SaveDialogU8(
        &selectedPath,
        &nfdFilter,
        1U,
        OptionalPath(defaultDirectory),
        OptionalPath(defaultName));
    if (outcome == NFD_OKAY)
    {
        std::string path(selectedPath);
        NFD_FreePathU8(selectedPath);
        return path;
    }
    if (outcome == NFD_ERROR)
    {
        const char* error = NFD_GetError();
        ShowError(error == nullptr
                ? "The native file dialog failed."
                : error);
    }
    // NFD_CANCEL means the user changed their mind; nothing to report.
    return std::nullopt;
}

void UiDialogs::ShowError(std::string message)
{
    m_errorMessage = std::move(message);
}

} // namespace fidget
