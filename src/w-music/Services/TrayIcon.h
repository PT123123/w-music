#pragma once

#include <windows.h>

#include <string_view>

namespace wm::app
{
    /// System-tray icon (Shell_NotifyIcon) for w-music.
    ///
    /// Owns a message-only window that receives the tray callback messages:
    /// left-click restores the main window, right-click shows a small menu
    /// (open / exit). Call Destroy() before the process exits -- or simply let
    /// the destructor run -- so the icon is removed from the tray.
    class TrayIcon
    {
    public:
        TrayIcon() = default;
        ~TrayIcon();
        TrayIcon(TrayIcon const&) = delete;
        TrayIcon& operator=(TrayIcon const&) = delete;

        /// Creates the hidden message window and adds the tray icon. Returns
        /// false if the icon could not be registered (tray unavailable, etc.).
        bool Initialize(HWND mainWindow, std::wstring_view iconPath);

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
    };
}
