#pragma once

#include "MainWindow.g.h"

#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <array>
#include <string>
#include <vector>

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

        // --- 均衡器（EQ） ----------------------------------------------------
        /// EQ 面板的滑条全部由代码生成（11 根：前置 + 十段），XAML 只留容器。
        /// 事件都在代码里挂，所以处理器放 private 也没问题。
        void BuildEqPanel();
        void SelectEqPresetItem(std::wstring const& presetId);
        void ReadEqSliders(double& preampDb, std::array<double, 10>& gainsDb);
        void ShowEqValues(double preampDb, std::array<double, 10> const& gainsDb);
        void SaveEqState(); ///< 把面板当前状态写进 settings.json

        // WinUI 3 这批委托的 sender 都是 IInspectable（见 LibraryPage 同款写法）。
        void OnEqSliderChanged(winrt::Windows::Foundation::IInspectable const& sender,
                               winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args);
        void OnEqPresetSelected(winrt::Windows::Foundation::IInspectable const& sender,
                                winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
        void OnEqToggleChanged(winrt::Windows::Foundation::IInspectable const& sender,
                               winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnEqResetClicked(winrt::Windows::Foundation::IInspectable const& sender,
                              winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);

        wm::app::SpectrumView m_spectrumView;
        wm::app::TrayIcon m_trayIcon;
        bool m_updatingSlider = false;
        std::wstring m_themeId{ L"qq" };

        std::vector<winrt::Microsoft::UI::Xaml::Controls::Slider> m_eqSliders;
        std::vector<winrt::Microsoft::UI::Xaml::Controls::TextBlock> m_eqValueLabels;
        /// 代码改滑条/预设下拉框时置位，避免 ValueChanged/SelectionChanged 回环。
        bool m_eqApplying = false;
    };
}

namespace winrt::w_music::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
