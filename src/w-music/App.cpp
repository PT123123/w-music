#include "pch.h"

#include "App.h"
#include "App.g.cpp"

#include "MainWindow.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::w_music::implementation
{
    App::App()
    {
        InitializeComponent();

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
        m_window = make<MainWindow>();
        m_window.Activate();
    }
}
