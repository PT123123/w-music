#include "pch.h"

#include "Services/TrayIcon.h"

#include <shellapi.h>

namespace
{
    constexpr UINT TRAY_ICON_ID = 1;
    constexpr UINT TRAY_CALLBACK = WM_APP + 1;

    enum : UINT_PTR
    {
        CmdShowMain = 1,
        CmdExitApp = 2,
    };
} // namespace

namespace wm::app
{
    TrayIcon::~TrayIcon()
    {
        Destroy();
    }

    bool TrayIcon::Initialize(HWND mainWindow, std::wstring_view iconPath,
                              std::function<void()> onShow, std::function<void()> onExit)
    {
        if (m_messageWindow != nullptr)
        {
            return true; // already initialized
        }
        m_mainWindow = mainWindow;
        m_onShow = std::move(onShow);
        m_onExit = std::move(onExit);

        WNDCLASSW wc{};
        wc.lpfnWndProc = &TrayIcon::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kTrayWindowClassName;
        RegisterClassW(&wc);

        m_messageWindow = CreateWindowExW(0, kTrayWindowClassName, L"w-music-tray", 0,
                                          0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, this);
        if (m_messageWindow == nullptr)
        {
            return false;
        }

        // Load a 16/32px icon from the .ico deployed next to the exe.
        m_icon = static_cast<HICON>(LoadImageW(nullptr, iconPath.data(), IMAGE_ICON, 0, 0,
                                               LR_LOADFROMFILE | LR_DEFAULTSIZE));
        if (m_icon == nullptr)
        {
            m_icon = LoadIconW(nullptr, IDI_APPLICATION);
        }

        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(NOTIFYICONDATAW);
        nid.hWnd = m_messageWindow;
        nid.uID = TRAY_ICON_ID;
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = TRAY_CALLBACK;
        nid.hIcon = m_icon;
        wcscpy_s(nid.szTip, L"w-music");

        m_added = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
        if (!m_added)
        {
            Destroy();
            return false;
        }
        return true;
    }

    void TrayIcon::Destroy()
    {
        if (m_added)
        {
            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof(NOTIFYICONDATAW);
            nid.hWnd = m_messageWindow;
            nid.uID = TRAY_ICON_ID;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            m_added = false;
        }
        if (m_messageWindow != nullptr)
        {
            DestroyWindow(m_messageWindow);
            m_messageWindow = nullptr;
        }
        if (m_icon != nullptr)
        {
            DestroyIcon(m_icon);
            m_icon = nullptr;
        }
        m_mainWindow = nullptr;
    }

    LRESULT CALLBACK TrayIcon::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        TrayIcon* self = nullptr;
        if (message == WM_NCCREATE)
        {
            const auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<TrayIcon*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        else
        {
            self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (self != nullptr)
        {
            if (message == TRAY_CALLBACK)
            {
                self->OnTrayMessage(static_cast<UINT>(lParam), wParam);
                return 0;
            }
            if (message == WM_TRAY_SHOW_MAIN)
            {
                // 第二个实例请求显示窗口。
                if (self->m_onShow)
                {
                    self->m_onShow();
                }
                return 0;
            }
            if (message == WM_DESTROY)
            {
                self->m_messageWindow = nullptr;
                return 0;
            }
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void TrayIcon::OnTrayMessage(UINT message, WPARAM /*wParam*/)
    {
        switch (message)
        {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            if (m_onShow)
            {
                m_onShow();
            }
            else if (m_mainWindow != nullptr)
            {
                ShowWindow(m_mainWindow, SW_RESTORE);
                SetForegroundWindow(m_mainWindow);
            }
            break;
        case WM_RBUTTONUP:
            ShowContextMenu();
            break;
        default:
            break;
        }
    }

    void TrayIcon::ShowContextMenu()
    {
        HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
        {
            return;
        }
        AppendMenuW(menu, MF_STRING, CmdShowMain, L"打开主界面");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, CmdExitApp, L"退出");

        POINT pt{};
        GetCursorPos(&pt);
        SetForegroundWindow(m_messageWindow);
        const UINT_PTR command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                                pt.x, pt.y, 0, m_messageWindow, nullptr);
        DestroyMenu(menu);
        // Message-only windows never process the menu-dismissal mouse message;
        // posting a null message lets the shell close the menu cleanly.
        PostMessageW(m_messageWindow, WM_NULL, 0, 0);

        if (command == CmdShowMain)
        {
            if (m_onShow)
            {
                m_onShow();
            }
        }
        else if (command == CmdExitApp)
        {
            // 退出必须走回调：直接 Post WM_CLOSE 现在会变成"隐藏到托盘"。
            if (m_onExit)
            {
                m_onExit();
            }
        }
    }
} // namespace wm::app
