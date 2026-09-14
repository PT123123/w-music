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
