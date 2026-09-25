#include "pch.h"

#include "Services/AppPaths.h"

#include <knownfolders.h>
#include <shlobj_core.h>
#include <combaseapi.h>

#include <winrt/Windows.Storage.h>
#include <winrt/Microsoft.UI.Dispatching.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <mutex>

namespace wm::app
{
    namespace
    {
        std::atomic<DWORD> g_uiThreadId{ 0 };
        winrt::Microsoft::UI::Dispatching::DispatcherQueue g_dispatcher{ nullptr };
        std::mutex g_diagMutex;

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

    void SetUiThread()
    {
        g_uiThreadId.store(GetCurrentThreadId(), std::memory_order_release);
        g_dispatcher = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();

        // The generated wWinMain calls winrt::init_apartment() which defaults
        // to multi-threaded; record it so diag.log can confirm why
        // apartment_context hops never returned to this thread.
        APTTYPE type{};
        APTTYPEQUALIFIER qualifier{};
        if (SUCCEEDED(CoGetApartmentType(&type, &qualifier)))
        {
            char buf[64]{};
            snprintf(buf, sizeof(buf), "ui apartment type=%d qualifier=%d", static_cast<int>(type),
                     static_cast<int>(qualifier));
            Diag(buf);
        }
    }

    bool UiThread()
    {
        return g_uiThreadId.load(std::memory_order_acquire) == GetCurrentThreadId();
    }

    void PostToUi(std::function<void()> fn)
    {
        if (!fn)
        {
            return;
        }
        const auto dispatcher = g_dispatcher;
        if (UiThread() || dispatcher == nullptr)
        {
            fn();
            return;
        }
        if (!dispatcher.TryEnqueue([fn = std::move(fn)] { fn(); }))
        {
            Diag("PostToUi: dispatcher rejected the work item");
        }
    }

    bool UiAwaiter::await_ready() const noexcept
    {
        return UiThread();
    }

    void UiAwaiter::await_suspend(std::coroutine_handle<> handle) const
    {
        const auto dispatcher = g_dispatcher;
        if (!dispatcher || !dispatcher.TryEnqueue([handle] { handle.resume(); }))
        {
            // Dispatcher unavailable or shutting down: never strand the
            // coroutine - resume inline and let the self-healing property
            // raises deal with the wrong thread.
            Diag("ResumeOnUi: dispatcher rejected, resuming inline");
            handle.resume();
        }
    }

    UiAwaiter ResumeOnUi() noexcept
    {
        return {};
    }

    void Diag(std::string_view msg)
    {
        std::lock_guard<std::mutex> lock(g_diagMutex);
        std::ofstream out(DataDirectory() / L"diag.log", std::ios::app);
        if (!out)
        {
            return;
        }
        out << GetTickCount64()
            << " tid=" << GetCurrentThreadId()
            << " ui=" << (UiThread() ? 1 : 0)
            << ' ' << msg << '\n';
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
