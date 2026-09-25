#include "pch.h"

#include "Services/SingleInstance.h"

#include "Services/AppPaths.h"
#include "Services/TrayIcon.h"

namespace
{
    constexpr wchar_t kMutexName[] = L"Local\\w-music-single-instance";
    HANDLE g_instanceMutex = nullptr;
} // namespace

namespace wm::app
{
    bool AcquireSingleInstance()
    {
        g_instanceMutex = CreateMutexW(nullptr, FALSE, kMutexName);
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
}
