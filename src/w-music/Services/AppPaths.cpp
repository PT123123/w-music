#include "pch.h"

#include "Services/AppPaths.h"

#include <knownfolders.h>
#include <shlobj_core.h>

#include <winrt/Windows.Storage.h>

namespace wm::app
{
    namespace
    {
        std::filesystem::path LocalState()
        {
            try
            {
                const auto folder = winrt::Windows::Storage::ApplicationData::Current().LocalFolder();
                return std::filesystem::path{ folder.Path().c_str() };
            }
            catch (...)
            {
                // Unpackaged / no ApplicationData: fall back to the temp folder.
                wchar_t buffer[MAX_PATH]{};
                if (GetTempPathW(MAX_PATH, buffer) > 0)
                {
                    return std::filesystem::path{ buffer } / L"w-music";
                }
                return std::filesystem::path{ L"w-music" };
            }
        }
    } // namespace

    std::filesystem::path DataDirectory()
    {
        static const std::filesystem::path path = [] {
            wchar_t* known = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &known)) && known)
            {
                std::filesystem::path result{ known };
                CoTaskMemFree(known);
                return result / L"w-music";
            }
            if (known) CoTaskMemFree(known);
            return LocalState();
        }();
        return path;
    }

    std::filesystem::path LibraryFilePath()
    {
        return DataDirectory() / L"library.json";
    }

    std::filesystem::path SettingsFilePath()
    {
        return DataDirectory() / L"settings.json";
    }

    void EnsureDataDirectory()
    {
        std::error_code ec;
        std::filesystem::create_directories(DataDirectory(), ec);
    }

    std::wstring Utf16(std::string_view utf8)
    {
        if (utf8.empty())
        {
            return {};
        }
        const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (needed <= 0)
        {
            return {};
        }
        std::wstring out(static_cast<std::size_t>(needed), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
        return out;
    }

    std::string Utf8(std::wstring_view utf16)
    {
        if (utf16.empty())
        {
            return {};
        }
        const int needed = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), nullptr, 0, nullptr, nullptr);
        if (needed <= 0)
        {
            return {};
        }
        std::string out(static_cast<std::size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), out.data(), needed, nullptr, nullptr);
        return out;
    }

    std::wstring FormatDuration(long long milliseconds)
    {
        const long long totalSeconds = milliseconds > 0 ? milliseconds / 1000 : 0;
        const long long hours = totalSeconds / 3600;
        const long long minutes = (totalSeconds % 3600) / 60;
        const long long seconds = totalSeconds % 60;

        wchar_t buffer[32]{};
        if (hours > 0)
        {
            swprintf_s(buffer, L"%lld:%02lld:%02lld", hours, minutes, seconds);
        }
        else
        {
            swprintf_s(buffer, L"%lld:%02lld", minutes, seconds);
        }
        return std::wstring{ buffer };
    }
}
