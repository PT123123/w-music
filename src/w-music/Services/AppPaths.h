#pragma once

#include <winrt/base.h>

#include <coroutine>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace wm::app
{
    /// Where w-music keeps its data: %LOCALAPPDATA%\w-music -- outside the build
    /// folder on purpose, so deploying a new build cannot touch a library.
    std::filesystem::path DataDirectory();
    std::filesystem::path LibraryFilePath();
    std::filesystem::path SettingsFilePath();
    /// Last-seen recommend-engine answers, so the page can paint instantly (and
    /// offline) instead of waiting for the Python process to boot.
    std::filesystem::path RecommendCacheFilePath();
    void EnsureDataDirectory();

    // ---- UI thread helpers ----
    /// Records the thread that owns the XAML window (call once from MainWindow).
    void SetUiThread();
    bool UiThread();
    /// Runs |fn| on the UI thread (inline when already there).
    void PostToUi(std::function<void()> fn);

    /// Coroutine awaiter that hops back to the XAML thread via the UI
    /// DispatcherQueue. winrt::apartment_context cannot do that here: the UI
    /// thread is MTA, so background thread-pool threads share its COM context
    /// and apartment_context resumes them inline (off the UI thread).
    struct UiAwaiter
    {
        bool await_ready() const noexcept;
        void await_suspend(std::coroutine_handle<> handle) const;
        void await_resume() const noexcept {}
    };

    /// Usage: co_await wm::app::ResumeOnUi();
    UiAwaiter ResumeOnUi() noexcept;
    /// Appends one breadcrumb line to %LOCALAPPDATA%\w-music\diag.log.
    void Diag(std::string_view msg);

    /// Installs a Win32 unhandled-exception filter that writes the exception
    /// code, the faulting address and every stack word that looks like a
    /// return address inside w-music.exe to diag.log (as RVAs). w-music ships
    /// without a PDB and WER only reports the faulting offset, so this is how a
    /// user-reported crash gets pinned to a call site: resolve the logged RVAs
    /// against build\w-music.map of the very same link.
    void InstallCrashLogger();

    // ---- encoding helpers (WinRT is UTF-16, the core layer is UTF-8) ----
    std::wstring Utf16(std::string_view utf8);
    std::string Utf8(std::wstring_view utf16);

    /// Formats milliseconds as m:ss (or h:mm:ss when long enough).
    std::wstring FormatDuration(long long milliseconds);
}
