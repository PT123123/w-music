// Standalone proof for the "add music folder" UI freeze fix.
//
// The app called IFileOpenDialog::Show straight from the XAML thread. Show only
// pumps messages once its dialog is up: everything before that (COM create,
// restoring the last location, enumerating the shell namespace, injected shell
// extensions) runs with the UI thread blocked and no pump, which is what WER
// reports as AppHangTransient for w-music.exe.
//
// Arms, in order:
//   1. control-stall-on-main   -- a non-pumping stall on the owner thread:
//                                 IsHungAppWindow flips and the heartbeat timer stops.
//   2. stall-on-worker-thread  -- the same stall on a separate thread: owner keeps
//                                 pumping and the window is never flagged hung.
//   3. dialog-on-main          -- the real dialog on the owner thread; proves the
//                                 scripted OK driver really returns a path.
//   4. dialog-on-worker-thread -- the real dialog from a worker STA thread with a
//                                 cross-thread owner: it must appear, return the
//                                 requested folder, and the owner thread must keep
//                                 pumping the whole time.
//
// Arms 3 and 4 print the thread id that created the dialog window, so they cannot
// silently collapse into the same configuration.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "user32.lib")

namespace
{
    constexpr UINT_PTR kHeartbeatTimer = 0xE1;
    constexpr DWORD kStallMs = 4000;
    constexpr DWORD kArmTimeoutMs = 30000;

    std::atomic_long g_heartbeats{ 0 };
    std::atomic_bool g_ownerLoopStopping{ false };
    HWND g_owner{};
    std::wstring g_target;

    int g_failures = 0;

    void Check(char const* name, bool ok, std::string const& detail)
    {
        std::printf("ASSERT %-32s %s | %s\n", name, ok ? "pass" : "FAIL", detail.c_str());
        std::fflush(stdout);
        if (!ok)
        {
            ++g_failures;
        }
    }

    std::string WideToUtf8(std::wstring const& value)
    {
        if (value.empty())
        {
            return {};
        }
        int const need = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                                             nullptr, 0, nullptr, nullptr);
        std::vector<char> buffer(static_cast<size_t>(need > 0 ? need : 0) + 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                            buffer.data(), need, nullptr, nullptr);
        return std::string{ buffer.data() };
    }

    bool PathEquals(std::wstring const& a, std::wstring const& b)
    {
        std::wstring left{ a }, right{ b };
        while (!left.empty() && left.back() == L'\\') left.pop_back();
        while (!right.empty() && right.back() == L'\\') right.pop_back();
        if (left.size() != right.size())
        {
            return false;
        }
        for (size_t i = 0; i < left.size(); ++i)
        {
            if (towlower(left[i]) != towlower(right[i]))
            {
                return false;
            }
        }
        return true;
    }

    // ---- owner thread -------------------------------------------------------

    LRESULT CALLBACK OwnerProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_TIMER && wparam == kHeartbeatTimer)
        {
            g_heartbeats.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        if (message == WM_DESTROY)
        {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    void PumpOnce(DWORD waitMs)
    {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, waitMs, QS_ALLINPUT);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                g_ownerLoopStopping.store(true);
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    /// The owner thread idling with a real message pump -- what the XAML thread
    /// does while the app is not blocked.
    void PumpUntil(std::atomic_bool& flag, DWORD timeoutMs)
    {
        DWORD waited = 0;
        while (!flag.load() && waited < timeoutMs && !g_ownerLoopStopping.load())
        {
            PumpOnce(50);
            waited += 50;
        }
    }

    // ---- dialog window driver ----------------------------------------------

    HWND FindOwnDialogWindow()
    {
        HWND found{};
        EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL {
            auto* slot = reinterpret_cast<HWND*>(lparam);
            if (!IsWindowVisible(hwnd))
            {
                return TRUE;
            }
            wchar_t classes[128]{};
            if (GetClassNameW(hwnd, classes, 128) == 0 || std::wcscmp(classes, L"#32770") != 0)
            {
                return TRUE;
            }
            DWORD pid{};
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == GetCurrentProcessId())
            {
                *slot = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
        return found;
    }

    void CloseStaleDialog()
    {
        if (HWND stale = FindOwnDialogWindow())
        {
            DestroyWindow(stale);
            Sleep(300);
        }
    }

    /// Presses the dialog's default button from a thread other than the owner's,
    /// with the folder already set through IFileOpenDialog::SetFolder -- the same
    /// thing the user does by hand.
    void DriveDialogToOk(std::string* note)
    {
        HWND dialog{};
        for (DWORD waited = 0; waited < 12000; waited += 100)
        {
            dialog = FindOwnDialogWindow();
            if (dialog)
            {
                break;
            }
            Sleep(100);
        }
        if (!dialog)
        {
            *note += "[driver] dialog window never appeared; ";
            return;
        }
        *note += "[driver] dialog tid=" +
                 std::to_string(GetWindowThreadProcessId(dialog, nullptr)) + "; ";
        Sleep(1200); // let the shell finish populating the view

        HWND const ok = GetDlgItem(dialog, IDOK);
        *note += ok ? "[driver] ok button found; " : "[driver] no ok button; ";

        for (int attempt = 0; attempt < 6; ++attempt)
        {
            if (ok != nullptr)
            {
                PostMessageW(ok, BM_CLICK, 0, 0);
            }
            else
            {
                PostMessageW(dialog, WM_KEYDOWN, VK_RETURN, 0);
            }
            Sleep(600);
            if (!IsWindow(dialog) || !IsWindowVisible(dialog))
            {
                *note += "[driver] closed on attempt " + std::to_string(attempt + 1) + "; ";
                return;
            }
        }
        *note += "[driver] dialog stayed open; ";
    }

    // ---- the code under test -----------------------------------------------

    struct DialogGuard
    {
        IFileOpenDialog* ptr{};
        explicit DialogGuard(IFileOpenDialog* value) : ptr(value) {}
        ~DialogGuard()
        {
            if (ptr != nullptr)
            {
                ptr->Release();
            }
        }
        DialogGuard(DialogGuard const&) = delete;
        DialogGuard& operator=(DialogGuard const&) = delete;
    };

    struct PickResult
    {
        std::wstring path;
        HRESULT hr{ S_OK };
        std::string stage;
        DWORD threadId{};
    };

    /// Mirrors LibraryService::PickFolder (winrt::com_ptr / check_hresult traded
    /// for raw COM so the probe needs no WinRT projections). Runs the modal dialog
    /// on whichever thread calls it.
    PickResult PickFolderBlocking(HWND owner, wchar_t const* initialFolder)
    {
        PickResult result;
        result.threadId = GetCurrentThreadId();

        IFileOpenDialog* raw{};
        result.hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_IFileOpenDialog, reinterpret_cast<void**>(&raw));
        if (FAILED(result.hr))
        {
            result.stage = "cocreate hr=0x" + [&] {
                char buf[16]{};
                snprintf(buf, sizeof(buf), "%08X", static_cast<uint32_t>(static_cast<int32_t>(result.hr)));
                return std::string{ buf };
            }();
            return result;
        }
        DialogGuard guard{ raw };

        DWORD options{};
        raw->GetOptions(&options);
        raw->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);

        if (initialFolder != nullptr)
        {
            IShellItem* folder{};
            if (SUCCEEDED(SHCreateItemFromParsingName(initialFolder, nullptr, IID_IShellItem,
                                                      reinterpret_cast<void**>(&folder))))
            {
                raw->SetFolder(folder);
                folder->Release();
            }
        }

        const HRESULT shown = raw->Show(owner);
        if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        {
            result.hr = shown;
            result.stage = "cancelled";
            return result;
        }
        if (FAILED(shown))
        {
            result.hr = shown;
            result.stage = "show-failed";
            return result;
        }

        IShellItem* item{};
        result.hr = raw->GetResult(&item);
        result.stage = "getresult-failed";
        if (FAILED(result.hr))
        {
            return result;
        }
        PWSTR display{};
        result.hr = item->GetDisplayName(SIGDN_FILESYSPATH, &display);
        item->Release();
        if (SUCCEEDED(result.hr))
        {
            if (display != nullptr)
            {
                result.path = display;
                CoTaskMemFree(display);
            }
            result.stage = "ok";
        }
        else
        {
            result.stage = "displayname-failed";
        }
        return result;
    }
}

int main()
{
    wchar_t tempPath[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring base{ tempPath };
    while (!base.empty() && base.back() == L'\\') base.pop_back();
    g_target = base + L"\\wmusic-pick-probe";
    CreateDirectoryW(g_target.c_str(), nullptr);

    std::printf("target folder: %S\n", g_target.c_str());
    std::printf("main tid=%lu\n", GetCurrentThreadId());
    std::fflush(stdout);

    WNDCLASSW wc{};
    wc.lpfnWndProc = OwnerProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WmPickProbeOwner";
    RegisterClassW(&wc);
    g_owner = CreateWindowExW(0, wc.lpszClassName, L"w-music pick probe", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 420, 220, nullptr, nullptr,
                              wc.hInstance, nullptr);
    ShowWindow(g_owner, SW_SHOW);
    SetTimer(g_owner, kHeartbeatTimer, 20, nullptr);
    for (int i = 0; i < 4; ++i)
    {
        PumpOnce(20);
    }

    // ---- arm 1: control -- non-pumping stall on the owner thread ------------
    {
        long const before = g_heartbeats.load();
        std::atomic_bool stop{ false };
        std::atomic_bool hung{ false };
        std::thread sampler([&] {
            while (!stop.load())
            {
                if (IsHungAppWindow(g_owner))
                {
                    hung.store(true);
                }
                Sleep(50);
            }
        });
        Sleep(kStallMs); // stands in for the blocked shell dialog init
        stop.store(true);
        sampler.join();
        long const delta = g_heartbeats.load() - before;
        // IsHungAppWindow only flips once the OS hang detector's own threshold
        // passes, so the heartbeat is the signal here: zero ticks for four seconds
        // means the owner thread serviced nothing -- the freeze the user saw.
        Check("control-stall-on-main", delta == 0,
              "heartbeats=" + std::to_string(delta) +
                  " isHungAppWindow=" + (hung.load() ? "true" : "false"));
    }

    // ---- arm 2: same stall on a worker thread ------------------------------
    {
        long const before = g_heartbeats.load();
        std::atomic_bool finished{ false };
        std::atomic_bool hung{ false };
        DWORD workerTid{};
        std::thread worker([&] {
            workerTid = GetCurrentThreadId();
            Sleep(kStallMs);
            finished.store(true);
        });
        std::thread watcher([&] {
            for (DWORD waited = 0; waited < kStallMs + 1500; waited += 50)
            {
                if (IsHungAppWindow(g_owner))
                {
                    hung.store(true);
                }
                Sleep(50);
            }
        });
        PumpUntil(finished, kArmTimeoutMs);
        watcher.join();
        worker.join();
        long const delta = g_heartbeats.load() - before;
        Check("stall-on-worker-thread", delta > 40 && !hung.load(),
              "heartbeats=" + std::to_string(delta) +
                  " isHungAppWindow=" + (hung.load() ? "true" : "false") +
                  " worker tid=" + std::to_string(workerTid));
    }

    // ---- arm 3: control -- the real dialog on the owner thread -------------
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::string note;
        std::thread driver([&] { DriveDialogToOk(&note); });
        PickResult picked = PickFolderBlocking(g_owner, g_target.c_str());
        driver.join();
        Check("dialog-on-main-returns-path", PathEquals(picked.path, g_target),
              "path=" + WideToUtf8(picked.path) + " stage=" + picked.stage +
                  " calling tid=" + std::to_string(picked.threadId) + " " + note);
        CloseStaleDialog();
    }

    // ---- arm 4: the fix -- the real dialog on a worker STA thread ----------
    {
        long const before = g_heartbeats.load();
        std::atomic_bool finished{ false };
        std::atomic_bool hung{ false };
        PickResult picked;
        std::string note;

        std::thread worker([&] {
            const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            picked = PickFolderBlocking(g_owner, g_target.c_str());
            if (SUCCEEDED(init))
            {
                CoUninitialize();
            }
            finished.store(true);
        });
        std::thread driver([&] { DriveDialogToOk(&note); });
        std::thread watcher([&] {
            for (DWORD waited = 0; waited < kArmTimeoutMs + 5000 && !finished.load(); waited += 50)
            {
                if (IsHungAppWindow(g_owner))
                {
                    hung.store(true);
                }
                Sleep(50);
            }
        });

        PumpUntil(finished, kArmTimeoutMs);
        watcher.join();
        driver.join();
        worker.join();

        long const delta = g_heartbeats.load() - before;
        Check("dialog-on-worker-returns-path", PathEquals(picked.path, g_target),
              "path=" + WideToUtf8(picked.path) + " stage=" + picked.stage +
                  " worker tid=" + std::to_string(picked.threadId) + " (main=" +
                  std::to_string(GetCurrentThreadId()) + ") " + note);
        Check("dialog-on-worker-owner-pumped", delta > 40 && !hung.load(),
              "heartbeats=" + std::to_string(delta) +
                  " isHungAppWindow=" + (hung.load() ? "true" : "false"));
        CloseStaleDialog();
    }

    KillTimer(g_owner, kHeartbeatTimer);
    DestroyWindow(g_owner);
    CoUninitialize();

    std::printf("RESULT %s (%d failing assertion%s)\n", g_failures == 0 ? "PASS" : "FAIL",
                g_failures, g_failures == 1 ? "" : "s");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
