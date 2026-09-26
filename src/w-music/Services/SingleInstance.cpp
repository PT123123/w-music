#include "pch.h"

#include "Services/SingleInstance.h"

#include "Services/AppPaths.h"
#include "Services/TrayIcon.h"

namespace
{
    constexpr wchar_t kMutexName[] = L"Local\\w-music-single-instance";
    HANDLE g_instanceMutex = nullptr;

    // 交接等待上限。旧实例要做的只是走一遍托盘"退出"（关窗、移托盘图标、
    // 停引擎子进程），正常不到一秒；这里给到 6 秒，等不到就算失败。
    constexpr DWORD kHandoffTimeoutMs = 6000;

    // 找旧实例的托盘窗口，带重试：托盘窗口在主窗口 Loaded 后才创建，
    // 旧实例可能还在启动中。返回所有窗口和它们所属的进程 id。
    bool FindExistingTrayWindows(std::vector<HWND>& windows, std::vector<DWORD>& pids)
    {
        for (int attempt = 0; attempt < 40; ++attempt)
        {
            windows.clear();
            pids.clear();
            HWND tray = nullptr;
            while ((tray = FindWindowExW(HWND_MESSAGE, tray, wm::app::kTrayWindowClassName, nullptr)) != nullptr)
            {
                DWORD pid = 0;
                GetWindowThreadProcessId(tray, &pid);
                if (pid != 0)
                {
                    windows.push_back(tray);
                    pids.push_back(pid);
                }
            }
            if (!windows.empty())
            {
                return true;
            }
            Sleep(50);
        }
        return false;
    }
} // namespace

namespace wm::app
{
    bool AcquireSingleInstance()
    {
        // 第一实例真正持有所有权（bInitialOwner=TRUE）：交接靠"属主死亡时
        // 所有权被遗弃"来确认旧实例退场，没有所有权就没有遗弃信号。
        g_instanceMutex = CreateMutexW(nullptr, TRUE, kMutexName);
        if (g_instanceMutex == nullptr)
        {
            // 拿不到互斥量（罕见）时按"第一个实例"处理：宁可多开也不要起不来。
            return true;
        }
        return GetLastError() != ERROR_ALREADY_EXISTS;
    }

    void SignalExistingInstance()
    {
        // 老版本实例也可能注册了同名托盘窗口（它不认识 WM_TRAY_SHOW_MAIN，
        // 收到会忽略），所以把消息发给所有匹配的窗口，让新实例一定能收到。
        for (int attempt = 0; attempt < 20; ++attempt)
        {
            bool found = false;
            HWND tray = nullptr;
            while ((tray = FindWindowExW(HWND_MESSAGE, tray, kTrayWindowClassName, nullptr)) != nullptr)
            {
                found = true;
                PostMessageW(tray, WM_TRAY_SHOW_MAIN, 0, 0);
            }
            if (found)
            {
                Diag("second instance -> signal existing");
                return;
            }
            Sleep(50);
        }
        Diag("second instance -> no tray window found, exit");
    }

    bool TakeOverFromExistingInstance()
    {
        std::vector<HWND> windows;
        std::vector<DWORD> pids;
        if (!FindExistingTrayWindows(windows, pids))
        {
            Diag("handoff -> no existing tray window, exit");
            SignalExistingInstance();
            return false;
        }

        // 看旧实例是谁：exe 路径相同 = 同一个构建（双击第二次），路径不同 =
        // 另一个构建（部署新版本后的典型场景——每个版本一个 workshop 目录）。
        // 跨版本协议只依赖 TrayIcon 里写死的 WM_APP 常量，不需要版本号协商。
        wchar_t selfPath[4096]{};
        GetModuleFileNameW(nullptr, selfPath, 4096);

        std::vector<HANDLE> waitHandles;
        bool allDifferent = true;
        for (DWORD pid : pids)
        {
            // 查路径用受限权限：对提权中的旧实例也放行。
            HANDLE query = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (query == nullptr)
            {
                allDifferent = false;
                continue;
            }
            std::wstring path(4096, L'\0');
            DWORD size = static_cast<DWORD>(path.size());
            const BOOL ok = QueryFullProcessImageNameW(query, 0, path.data(), &size);
            CloseHandle(query);
            if (!ok)
            {
                allDifferent = false;
                continue;
            }
            path.resize(size);
            if (CompareStringOrdinal(path.c_str(), -1, selfPath, -1, TRUE) == CSTR_EQUAL)
            {
                allDifferent = false;
                continue;
            }
            // 确认是另一个构建。同步句柄只给 0.1.21 那条兜底路用（见下），
            // 提权中的旧实例开不了就开不了，主信号不依赖它。
            if (HANDLE waitHandle = OpenProcess(SYNCHRONIZE, FALSE, pid))
            {
                waitHandles.push_back(waitHandle);
            }
        }

        if (!allDifferent)
        {
            // 同一个 exe，或者认不出对方：维持原行为，唤到前台。
            Diag("handoff -> same/unknown build, bring to front");
            SignalExistingInstance();
            for (HANDLE handle : waitHandles)
            {
                CloseHandle(handle);
            }
            return false;
        }

        Diag("handoff -> different build running, requesting graceful exit");
        for (HWND window : windows)
        {
            PostMessageW(window, WM_TRAY_GRACEFUL_EXIT, 0, 0);
        }

        // 等旧实例真正退干净再启动：主窗口、托盘都会自然腾出来，晚一点
        // 启动没有副作用，抢先启动才是灾难（双实例抢音频、抢设置文件）。
        //
        // 主信号是互斥量所有权被遗弃（WAIT_ABANDONED）：旧属主进程死亡时
        // 必然遗弃，且不依赖任何进程句柄——桌面上留着提权的旧僵尸实例也
        // 不影响接管。0.1.21 及更早的旧构建从不持有所有权（那时还是
        // bInitialOwner=FALSE），互斥量会立刻以"空闲"返回，那种情况改用
        // 进程句柄判断死活——而它不认识退出消息，正常都会等到超时退回。
        bool exited = false;
        const ULONGLONG deadline = GetTickCount64() + kHandoffTimeoutMs;
        const DWORD mutexResult = WaitForSingleObject(g_instanceMutex, kHandoffTimeoutMs);
        if (mutexResult == WAIT_ABANDONED)
        {
            exited = true; // 所有权已遗弃给本进程，不用再取
        }
        else if (mutexResult == WAIT_OBJECT_0 && !waitHandles.empty())
        {
            while (GetTickCount64() < deadline)
            {
                const ULONGLONG remaining = deadline - GetTickCount64();
                if (WaitForMultipleObjects(static_cast<DWORD>(waitHandles.size()), waitHandles.data(),
                                           TRUE, static_cast<DWORD>(remaining)) == WAIT_OBJECT_0)
                {
                    exited = true;
                    // 互斥量本来就没人持有，把所有权接过来。
                    WaitForSingleObject(g_instanceMutex, 0);
                    break;
                }
            }
        }
        for (HANDLE handle : waitHandles)
        {
            CloseHandle(handle);
        }

        if (!exited)
        {
            // 旧实例是 0.1.21 及更早的版本（不认识退出消息）或者卡住了：
            // 不杀进程、也不并行双开，退回老行为。
            Diag("handoff -> old instance did not exit in time, bring to front");
            SignalExistingInstance();
            return false;
        }

        Diag("handoff -> old instance exited, taking over");
        return true;
    }
}
