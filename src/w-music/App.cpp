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

#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e)
        {
            if (IsDebuggerPresent())
            {
                const auto message = e.Message();
                (void)message;
                __debugbreak();
            }
        });
#endif
    }

    void App::OnLaunched(LaunchActivatedEventArgs const&)
    {
        // 单实例：已有实例在跑就把它的窗口唤到前台，然后本进程直接退出。
        if (!wm::app::AcquireSingleInstance())
        {
            wm::app::SignalExistingInstance();
            ExitProcess(0);
        }

        m_window = make<MainWindow>();
        m_window.Activate();
    }
}
