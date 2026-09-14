#pragma once

#include <winrt/base.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace wm::app
{
    /// Where w-music keeps its data: %LOCALAPPDATA%\Packages\<pkg>\LocalState
    std::filesystem::path DataDirectory();
    std::filesystem::path LibraryFilePath();
    std::filesystem::path SettingsFilePath();
    void EnsureDataDirectory();

    // ---- encoding helpers (WinRT is UTF-16, the core layer is UTF-8) ----
    std::wstring Utf16(std::string_view utf8);
    std::string Utf8(std::wstring_view utf16);

    /// Formats milliseconds as m:ss (or h:mm:ss when long enough).
    std::wstring FormatDuration(long long milliseconds);
}
