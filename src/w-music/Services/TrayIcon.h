#pragma once

#include <windows.h>

#include <functional>
#include <string_view>

namespace wm::app
{
    /// 托盘消息窗口的类名。单实例服务（SingleInstance.cpp）用它在
    /// HWND_MESSAGE 下面找到已运行实例的窗口，两处必须一致。
    inline constexpr wchar_t kTrayWindowClassName[] = L"w-music-tray-window";

    /// 跨进程"显示主窗口"消息：第二个实例投给上面那个托盘窗口。
    /// WM_APP 段（0x8000-0xBFFF）是应用自定义消息，可跨进程投递。
    inline constexpr UINT WM_TRAY_SHOW_MAIN = WM_APP + 2;

    /// System-tray icon (Shell_NotifyIcon) for w-music.
    ///
    /// Owns a message-only window that receives the tray callback messages:
    /// left-click restores the main window, right-click shows a small menu
    /// (open / exit). Call Destroy() before the process exits -- or simply let
    /// the destructor run -- so the icon is removed from the tray.
    ///
    /// 该消息窗口创建在 UI 线程上，所以回调也在 UI 线程被调用，可以直接
    /// 操作 XAML（隐藏/显示/退出主窗口）。
    class TrayIcon
    {
    public:
        TrayIcon() = default;
        ~TrayIcon();
        TrayIcon(TrayIcon const&) = delete;
        TrayIcon& operator=(TrayIcon const&) = delete;

        /// Creates the hidden message window and adds the tray icon. Returns
        /// false if the icon could not be registered (tray unavailable, etc.).
        /// |onShow| 显示/恢复主窗口，|onExit| 彻底退出应用（不是隐藏到托盘）。
        bool Initialize(HWND mainWindow, std::wstring_view iconPath,
                        std::function<void()> onShow, std::function<void()> onExit);

        /// Removes the tray icon and destroys the message window. Safe to call
        /// more than once; the destructor calls this as well.
        void Destroy();

    private:
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
        void OnTrayMessage(UINT message, WPARAM wParam);
        void ShowContextMenu();

        HWND m_messageWindow = nullptr;
        HWND m_mainWindow = nullptr;
        HICON m_icon = nullptr;
        bool m_added = false;
        std::function<void()> m_onShow;
        std::function<void()> m_onExit;
    };
}
