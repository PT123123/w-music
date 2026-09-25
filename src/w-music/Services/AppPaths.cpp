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

        // ---- crash logger state (see InstallCrashLogger) ----
        std::wstring g_diagPathWindows;          // cached so the handler needs no allocation
        BYTE* g_imageBase = nullptr;
        DWORD g_codeStart = 0;                   // executable section bounds, as RVAs
        DWORD g_codeEnd = 0;
        DWORD g_imageSize = 0;
        DWORD g_peTimestamp = 0;
        LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;
        std::atomic<bool> g_inFaultHandler{ false };
        std::atomic<int> g_avCount{ 0 };
        std::atomic<bool> g_crashLogged{ false };
        ULONG_PTR g_vectorCookie = 0;

        /// Raw-file write: the CRT streams and the diag mutex may already be the
        /// thing that blew up, so the crash line goes out through Win32 only.
        void CrashAppend(std::string_view line)
        {
            if (g_diagPathWindows.empty())
            {
                return;
            }
            const HANDLE file = CreateFileW(g_diagPathWindows.c_str(), FILE_APPEND_DATA,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                            FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                return;
            }
            SetFilePointer(file, 0, nullptr, FILE_END);
            DWORD written = 0;
            WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
            CloseHandle(file);
        }

        void DescribeImage()
        {
            g_imageBase = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
            if (g_imageBase == nullptr)
            {
                return;
            }
            const auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(g_imageBase);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            {
                return;
            }
            const auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS>(g_imageBase + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE)
            {
                return;
            }
            g_imageSize = nt->OptionalHeader.SizeOfImage;
            g_peTimestamp = nt->FileHeader.TimeDateStamp;
            g_codeStart = nt->OptionalHeader.BaseOfCode;
            g_codeEnd = g_imageSize;  // loose upper bound, tightened by the section walk
            const auto* section = IMAGE_FIRST_SECTION(nt);
            DWORD lowest = MAXDWORD;
            DWORD highest = 0;
            for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
            {
                if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
                {
                    continue;
                }
                lowest = (std::min)(lowest, section->VirtualAddress);
                highest = (std::max)(highest, section->VirtualAddress + section->Misc.VirtualSize);
            }
            if (lowest < highest)
            {
                g_codeStart = lowest;
                g_codeEnd = highest;
            }
        }

        /// Writes "<tag> code=.. rip=+RVA fault=.." plus a scan of the faulting
        /// thread's stack. Win32 file API only, and never re-entered by a fault
        /// inside itself (that would loop until the stack is gone).
        void LogFault(char const* tag, EXCEPTION_POINTERS* info)
        {
            bool expected = false;
            if (!g_inFaultHandler.compare_exchange_strong(expected, true))
            {
                CrashAppend("CRASH reentrant while logging a previous fault\n");
                return;
            }

            char line[1024]{};
            const auto* record = info != nullptr ? info->ExceptionRecord : nullptr;
            const auto* context = info != nullptr ? info->ContextRecord : nullptr;
            const unsigned code = record != nullptr ? static_cast<unsigned>(record->ExceptionCode) : 0u;
            const auto fault = record != nullptr && record->NumberParameters > 1
                ? static_cast<uintptr_t>(static_cast<ULONG_PTR>(record->ExceptionInformation[1]))
                : static_cast<uintptr_t>(0);
            const auto rip = context != nullptr ? static_cast<uintptr_t>(context->Rip) : static_cast<uintptr_t>(0);
            const auto rsp = context != nullptr ? static_cast<uintptr_t>(context->Rsp) : static_cast<uintptr_t>(0);

            int used = snprintf(line, sizeof(line),
                                "%s code=%08lx rip=+%lx fault=%p ts=%08lx tid=%lu ui=%d\n",
                                tag, static_cast<unsigned long>(code),
                                g_imageBase != nullptr
                                    ? static_cast<unsigned long>(rip - reinterpret_cast<uintptr_t>(g_imageBase))
                                    : 0ul,
                                reinterpret_cast<void*>(fault),
                                static_cast<unsigned long>(g_peTimestamp),
                                GetCurrentThreadId(), UiThread() ? 1 : 0);
            if (used > 0)
            {
                CrashAppend({ line, static_cast<std::size_t>(used) });
            }

            // No frame walker here: everything on the faulting thread's stack
            // that points into this image's code section is a possible return
            // address, and the .map of the same link turns those RVAs back into
            // function names.
            char stackText[1024]{};
            int pos = snprintf(stackText, sizeof(stackText), "%s stack", tag);
            int listed = 0;
            if (pos > 0 && g_imageBase != nullptr && rsp != 0)
            {
                MEMORY_BASIC_INFORMATION region{};
                if (VirtualQuery(reinterpret_cast<void*>(rsp), &region, sizeof(region)) != 0 &&
                    region.State == MEM_COMMIT)
                {
                    const uintptr_t begin = rsp;
                    const uintptr_t end = (std::min)(
                        region.BaseAddress ? reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize : 0ull,
                        begin + 0x4000);
                    for (uintptr_t address = begin; address + 8 <= end; address += 8)
                    {
                        const uintptr_t value = *reinterpret_cast<uintptr_t*>(address);
                        const uintptr_t delta = value - reinterpret_cast<uintptr_t>(g_imageBase);
                        if (delta <= g_codeStart || delta >= g_codeEnd)
                        {
                            continue;
                        }
                        // Printed minus one byte so a symbol lookup lands inside
                        // the call instruction rather than on its return target.
                        char item[32]{};
                        const int n = snprintf(item, sizeof(item), " +%lx", static_cast<unsigned long>(delta - 1));
                        if (n <= 0 || pos + n >= static_cast<int>(sizeof(stackText)) - 2)
                        {
                            break;
                        }
                        memcpy(stackText + pos, item, static_cast<std::size_t>(n));
                        pos += n;
                        if (++listed >= 24)
                        {
                            break;
                        }
                    }
                }
            }
            if (pos > 0)
            {
                stackText[pos++] = '\n';
                CrashAppend({ stackText, static_cast<std::size_t>(pos) });
            }

            g_inFaultHandler.store(false, std::memory_order_release);
        }

        /// Vectored, first in line. The top-level filter alone is not enough: in
        /// the first build of this logger the reproduced 0xC0000005 wrote no
        /// CRASH line at all, so something ended the process before our filter
        /// was asked (which component we did not establish). A vectored handler
        /// sees the fault before any of them. Access violations only, and only
        /// the first few: WinRT/XAML legitimately raise and swallow other
        /// first-chance exceptions, and an AV that some frame handles is still
        /// worth one log line because the ones that kill the process are too.
        LONG CALLBACK FaultWatcher(EXCEPTION_POINTERS* info)
        {
            const auto* record = info != nullptr ? info->ExceptionRecord : nullptr;
            if (record != nullptr && record->ExceptionCode == static_cast<DWORD>(STATUS_ACCESS_VIOLATION) &&
                g_avCount.fetch_add(1) < 4)
            {
                LogFault("AV", info);
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }

        LONG CALLBACK CrashFilter(EXCEPTION_POINTERS* info)
        {
            bool first = false;
            if (g_crashLogged.compare_exchange_strong(first, true))
            {
                LogFault("CRASH", info);
            }

            if (g_previousFilter != nullptr)
            {
                return g_previousFilter(info);
            }
            return EXCEPTION_CONTINUE_SEARCH;
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

    std::filesystem::path RecommendCacheFilePath()
    {
        return DataDirectory() / L"recommend-cache.json";
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

    void InstallCrashLogger()
    {
        DescribeImage();
        g_diagPathWindows = (DataDirectory() / L"diag.log").wstring();
        g_previousFilter = SetUnhandledExceptionFilter(CrashFilter);
        if (g_vectorCookie == 0)
        {
            g_vectorCookie = reinterpret_cast<ULONG_PTR>(AddVectoredExceptionHandler(1, FaultWatcher));
        }
        char line[128]{};
        snprintf(line, sizeof(line), "fault logger ts=%08lx code=+%lx..+%lx veh=%d ueh=%d",
                 static_cast<unsigned long>(g_peTimestamp), static_cast<unsigned long>(g_codeStart),
                 static_cast<unsigned long>(g_codeEnd), g_vectorCookie != 0 ? 1 : 0,
                 g_previousFilter != nullptr ? 1 : 0);
        Diag(line);
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
