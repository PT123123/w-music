#pragma once

#include "MainWindow.g.h"

#include <string>

#include "Controls/SpectrumView.h"
#include "Services/TrayIcon.h"

namespace winrt::w_music::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();

        winrt::w_music::LibraryViewModel Library() const;
        winrt::w_music::PlayerViewModel Player() const;

        // XAML 里直接挂的处理器（Button.Click），必须是 public —— 生成的
        // MainWindowT<MainWindow> 是基类，够不到子类的 private 成员。
        void OnThemeClick(winrt::Windows::Foundation::IInspectable const& sender,
                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);

    private:
        void OnLoaded(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnWindowClosed(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::WindowEventArgs const& args);
        void OnNavigationSelectionChanged(winrt::Microsoft::UI::Xaml::Controls::NavigationView const& sender,
                                          winrt::Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const& args);
        void OnPlayPauseClicked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnPrevClicked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnNextClicked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnModeClicked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnFavoriteClicked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnLyricClicked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnProgressChanged(winrt::Windows::Foundation::IInspectable const& sender,
                               winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args);
        void OnVolumeChanged(winrt::Windows::Foundation::IInspectable const& sender,
                             winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args);
        void OnPlayerPropertyChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                     winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs const& args);

        void NavigateTo(hstring const& tag);
        void UpdateTransport();
        void UpdateSpectrum(std::vector<double> const& bars);

        // --- 界面主题 -------------------------------------------------------
        /// 换主题：把 RootHost 的 Background 换成该主题的渐变，并改写强调色那一组
        /// 主题资源（按钮/滑块/导航指示条会跟着变）。未知 id 回落到 qq。
        void ApplyTheme(std::wstring const& themeId);
        void UpdateThemeMarks();

        wm::app::SpectrumView m_spectrumView;
        wm::app::TrayIcon m_trayIcon;
        bool m_updatingSlider = false;
        std::wstring m_themeId{ L"qq" };
    };
}

namespace winrt::w_music::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
