#include "pch.h"

#include "App.h"
#include "App.g.cpp"

#include "MainWindow.h"
#include "Services/AppPaths.h"
#include "Services/SingleInstance.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::w_music::implementation
{
    App::App()
    {
        InitializeComponent();

        // Crash first, explain later: without this a fault is only visible as a
        // WER record with a bare offset (see wm::app::InstallCrashLogger).
        wm::app::InstallCrashLogger();

        // 整套界面是围绕饱和的主题渐变设计的（深色玻璃面板 + 白字），所以应用级
        // 也锁深色。窗口根节点还会再设一次 ElementTheme.Dark，这里这一次是为了让
        // Flyout / ContentDialog 这类挂在自己 XamlRoot 上的弹出层也走深色资源。
        try
        {
            RequestedTheme(ApplicationTheme::Dark);
        }
        catch (...)
        {
            // 某些版本上这个 setter 只在窗口创建前有效，失败就靠根节点兜底。
        }

        // Stowed exceptions (WER 0xC000027B) never reach the VEH crash logger:
        // XAML catches the exception crossing an event callback and fails fast,
        // leaving diag.log without a trace. This handler is the only place the
        // failing HRESULT + message can be named, so it stays in release too.
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e)
        {
            char hr[16]{};
            snprintf(hr, sizeof(hr), "0x%08X", static_cast<uint32_t>(e.Exception()));
            wm::app::Diag(std::string{ "unhandled exception hr=" } + hr +
                          " msg=" + wm::app::Utf8(e.Message()));
            if (IsDebuggerPresent())
            {
                __debugbreak();
            }
        });
    }

    void App::OnLaunched(LaunchActivatedEventArgs const&)
    {
        // 单实例：已有实例在跑时按 exe 路径分流。同一路径（双击第二次）把它
        // 唤到前台后本进程退出；路径不同（部署新版本后启动的典型场景，每个
        // 版本一个 workshop 目录）请旧实例优雅退出、等它退干净后本进程接管。
        if (!wm::app::AcquireSingleInstance())
        {
            if (!wm::app::TakeOverFromExistingInstance())
            {
                ExitProcess(0);
            }
        }

        m_window = make<MainWindow>();
        m_window.Activate();
    }
}
