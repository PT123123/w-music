#include "pch.h"

#include "MainWindow.h"
#include "MainWindow.g.cpp"

#include "Services/DiscoverSettings.h"
#include "Services/Services.h"

#include <cstdint>
#include "ViewModels/LibraryViewModel.h"
#include "ViewModels/PlayerViewModel.h"
#include "Views/DiscoverPage.h"
#include "Views/LibraryPage.h"
#include "Views/NowPlayingPage.h"
#include "Views/OnlinePage.h"

using namespace winrt;
using namespace Microsoft::UI::Dispatching;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Controls::Primitives;

namespace
{
    // The window/tray icons are loaded from app.ico, which the build deploys
    // next to the exe (an unpackaged app has no packaged assets to read).
    std::filesystem::path ExeDirectory()
    {
        wchar_t buffer[MAX_PATH]{};
        const DWORD written = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        if (written == 0 || written >= MAX_PATH)
        {
            return std::filesystem::current_path();
        }
        return std::filesystem::path{ buffer }.parent_path();
    }

    // -----------------------------------------------------------------------
    // 界面主题
    // -----------------------------------------------------------------------
    // 一个主题 = 一支渐变画刷（App.xaml 里定义）+ 一组强调色。渐变统一是"垂直
    // 上→下、上端色相暗哑版、下端统一 #0A2442 深海蓝青"的低亮度暗渐变。渐变挂在
    // 窗口根节点上（页面/导航栏/底栏全部透明，于是整窗共用一层连续渐变），强调色则
    // 要改写 ThemeDictionaries 里那批 SolidColorBrush —— 它们是共享实例，改
    // Color 之后所有引用它的控件（导航选中指示条、滑块、主按钮文字色）立刻重绘，
    // 不需要重建任何页面。
    struct ThemeInfo
    {
        wchar_t const* id;
        wchar_t const* gradientKey;
        winrt::Windows::UI::Color accent;
        /// 副色：频谱柱渐变的那一端。
        winrt::Windows::UI::Color accentAlt;
    };

    // 顺序与界面里色块按钮的顺序无关（按 id 查表），但中文名与之一一对应。
    ThemeInfo const kThemes[]{
        { L"qq",     L"WmThemeQqGradient",     { 0xFF, 0x31, 0xC2, 0x7C }, { 0xFF, 0x2D, 0xD4, 0xBF } },  // QQ 绿
        { L"ocean",  L"WmThemeOceanGradient",  { 0xFF, 0x2E, 0x8B, 0xF0 }, { 0xFF, 0x22, 0xD3, 0xEE } },  // 海洋蓝
        { L"sunset", L"WmThemeSunsetGradient", { 0xFF, 0xF0, 0x75, 0x26 }, { 0xFF, 0xFB, 0xBF, 0x24 } },  // 日落橙
        { L"galaxy", L"WmThemeGalaxyGradient", { 0xFF, 0x7A, 0x52, 0xF0 }, { 0xFF, 0xC0, 0x84, 0xFC } },  // 星空紫
        { L"sakura", L"WmThemeSakuraGradient", { 0xFF, 0xE8, 0x5A, 0x93 }, { 0xFF, 0xF4, 0x72, 0xB6 } },  // 樱粉
    };

    ThemeInfo const& ThemeById(std::wstring const& id)
    {
        for (auto const& theme : kThemes)
        {
            if (id == theme.id)
            {
                return theme;
            }
        }
        return kThemes[0];  // 未知 id（老 settings.json / 手改坏了）回落到默认绿
    }

    std::uint8_t ScaleChannel(std::uint8_t value, double factor)
    {
        double const scaled = static_cast<double>(value) * factor;
        double const clamped = scaled < 0.0 ? 0.0 : (scaled > 255.0 ? 255.0 : scaled);
        return static_cast<std::uint8_t>(clamped + 0.5);
    }

    winrt::Windows::UI::Color Lighten(winrt::Windows::UI::Color const& color)
    {
        return { color.A, ScaleChannel(color.R, 1.16), ScaleChannel(color.G, 1.16), ScaleChannel(color.B, 1.16) };
    }

    winrt::Windows::UI::Color Darken(winrt::Windows::UI::Color const& color)
    {
        return { color.A, ScaleChannel(color.R, 0.86), ScaleChannel(color.G, 0.86), ScaleChannel(color.B, 0.86) };
    }

    /// 浅色主题下的"柔和底色"：强调色往白里兑。
    winrt::Windows::UI::Color TintTowardWhite(winrt::Windows::UI::Color const& color, double amount)
    {
        auto const mix = [amount](std::uint8_t channel) {
            double const value = static_cast<double>(channel);
            return static_cast<std::uint8_t>(value + (255.0 - value) * amount + 0.5);
        };
        return { color.A, mix(color.R), mix(color.G), mix(color.B) };
    }

    /// 只换 alpha 的强调色（深色主题下的柔和底色）。
    winrt::Windows::UI::Color Translucent(winrt::Windows::UI::Color const& color, std::uint8_t alpha)
    {
        return { alpha, color.R, color.G, color.B };
    }

    void SetThemeColor(ResourceDictionary const& dictionary, wchar_t const* key,
                       winrt::Windows::UI::Color const& color)
    {
        // 找不到的 key 直接跳过：Light 字典里没有少数几个 key（导航指示条、
        // 进度条），漏一个不该让整次切主题失败。
        try
        {
            if (auto brush = dictionary.Lookup(box_value(hstring{ key })).try_as<Media::SolidColorBrush>())
            {
                brush.Color(color);
            }
        }
        catch (...)
        {
        }
    }
} // namespace

namespace winrt::w_music::implementation
{
    MainWindow::MainWindow()
    {
        InitializeComponent();

        ExtendsContentIntoTitleBar(true);
        SetTitleBar(AppTitleBar());

        auto player = wm::app::Player();
        // Initialize is a non-WinRT helper on the implementation type (see
        // PlayerViewModel.h), so it has to be reached through winrt::get_self --
        // the projected PlayerViewModel does not expose it.
        winrt::get_self<implementation::PlayerViewModel>(player)->Initialize(DispatcherQueue::GetForCurrentThread());
        player.Volume(VolumeSlider().Value());

        // The sink is invoked from the UI timer, so touching XAML here is safe.
        winrt::get_self<implementation::PlayerViewModel>(player)->AddSpectrumSink(
            [this](std::vector<double> const& bars) { UpdateSpectrum(bars); });

        // Microsoft.UI.Xaml.Window has no Loaded event (it is not a
        // FrameworkElement); the root content is, so hook it there.
        if (auto root = Content().try_as<FrameworkElement>())
        {
            // 主题渐变是饱和色，浅色主题的近黑正文压在上面读不出来，所以整套
            // 界面锁深色（渐变本身与主题无关，深色下玻璃面板也才有层次）。
            root.RequestedTheme(ElementTheme::Dark);
            root.Loaded({ this, &MainWindow::OnLoaded });
        }
        NavView().SelectionChanged({ this, &MainWindow::OnNavigationSelectionChanged });
        PlayPauseButton().Click({ this, &MainWindow::OnPlayPauseClicked });
        PrevButton().Click({ this, &MainWindow::OnPrevClicked });
        NextButton().Click({ this, &MainWindow::OnNextClicked });
        ModeButton().Click({ this, &MainWindow::OnModeClicked });
        FavoriteBarButton().Click({ this, &MainWindow::OnFavoriteClicked });
        LyricButton().Click({ this, &MainWindow::OnLyricClicked });
        ProgressSlider().ValueChanged({ this, &MainWindow::OnProgressChanged });
        VolumeSlider().ValueChanged({ this, &MainWindow::OnVolumeChanged });
        player.PropertyChanged({ this, &MainWindow::OnPlayerPropertyChanged });

        m_spectrumView.Attach(SpectrumCanvas(), 28);
        ThemeButton().Click({ this, &MainWindow::OnThemeClick });
        ApplyTheme(wm::app::Settings().UiTheme());
        UpdateTransport();
    }

    winrt::w_music::LibraryViewModel MainWindow::Library() const
    {
        return wm::app::LibraryVm();
    }

    winrt::w_music::PlayerViewModel MainWindow::Player() const
    {
        return wm::app::Player();
    }

    void MainWindow::OnLoaded(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        wm::app::LibraryVm().InitializeAsync(AppWindow().Id());
        NavView().SelectedItem(DiscoverNavItem());
        NavigateTo(hstring{ L"discover" });

        // Kick off the WASAPI loopback capture for the spectrum.
        winrt::get_self<implementation::PlayerViewModel>(wm::app::Player())->StartSpectrum();

        // Window icon (taskbar / alt-tab) and the system-tray icon, both from
        // the green-note app.ico deployed next to the exe.
        const auto iconPath = (ExeDirectory() / L"app.ico").wstring();
        try
        {
            AppWindow().SetIcon(hstring{ iconPath });
        }
        catch (...)
        {
            // Icon is cosmetic; never fail startup over it.
        }
        const HWND hwnd = winrt::Microsoft::UI::GetWindowFromWindowId(AppWindow().Id());
        if (m_trayIcon.Initialize(hwnd, iconPath))
        {
            Closed({ this, &MainWindow::OnWindowClosed });
        }
    }

    void MainWindow::OnWindowClosed(Windows::Foundation::IInspectable const&, WindowEventArgs const&)
    {
        m_trayIcon.Destroy();
    }

    void MainWindow::NavigateTo(hstring const& tag)
    {
        if (tag == L"discover")
        {
            ContentFrame().Navigate(winrt::xaml_typename<w_music::DiscoverPage>());
        }
        else if (tag == L"online")
        {
            ContentFrame().Navigate(winrt::xaml_typename<w_music::OnlinePage>());
        }
        else if (tag == L"library")
        {
            ContentFrame().Navigate(winrt::xaml_typename<w_music::LibraryPage>());
        }
        else if (tag == L"nowplaying")
        {
            ContentFrame().Navigate(winrt::xaml_typename<w_music::NowPlayingPage>());
        }
    }

    void MainWindow::OnNavigationSelectionChanged(NavigationView const&, NavigationViewSelectionChangedEventArgs const& args)
    {
        if (args.IsSettingsSelected())
        {
            return;
        }
        auto item = args.SelectedItem().try_as<NavigationViewItem>();
        if (item == nullptr)
        {
            return;
        }
        auto tag = item.Tag();
        if (tag == nullptr)
        {
            return;
        }
        NavigateTo(winrt::unbox_value<hstring>(tag));
    }

    void MainWindow::OnPlayPauseClicked(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        wm::app::Player().TogglePlayPause();
    }

    void MainWindow::OnPrevClicked(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        wm::app::Player().Previous();
    }

    void MainWindow::OnNextClicked(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        wm::app::Player().Next();
    }

    void MainWindow::OnModeClicked(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        wm::app::Player().CycleMode();
    }

    void MainWindow::OnFavoriteClicked(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        wm::app::Player().ToggleFavorite();
    }

    void MainWindow::OnLyricClicked(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        NavView().SelectedItem(NowPlayingNavItem());
        NavigateTo(hstring{ L"nowplaying" });
    }

    void MainWindow::OnProgressChanged(Windows::Foundation::IInspectable const&, RangeBaseValueChangedEventArgs const& args)
    {
        if (m_updatingSlider)
        {
            return;
        }
        wm::app::Player().Seek(args.NewValue());
    }

    void MainWindow::OnVolumeChanged(Windows::Foundation::IInspectable const&, RangeBaseValueChangedEventArgs const& args)
    {
        wm::app::Player().Volume(args.NewValue());
    }

    void MainWindow::OnPlayerPropertyChanged(Windows::Foundation::IInspectable const&,
                                             winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs const&)
    {
        UpdateTransport();
    }

    void MainWindow::UpdateTransport()
    {
        auto player = wm::app::Player();

        NowPlayingTitle().Text(player.Title());
        NowPlayingArtist().Text(player.Artist());

        const double duration = player.DurationSeconds();
        if (duration > 0.5)
        {
            ProgressSlider().Maximum(duration);
        }

        m_updatingSlider = true;
        ProgressSlider().Value(player.PositionSeconds());
        m_updatingSlider = false;

        TimeText().Text(player.PositionText() + hstring{ L" / " } + player.DurationText());
        PlayPauseIcon().Glyph(player.IsPlaying() ? hstring{ L"\uE769" } : hstring{ L"\uE768" });
    }

    void MainWindow::UpdateSpectrum(std::vector<double> const& bars)
    {
        m_spectrumView.Update(bars);
    }

    void MainWindow::OnThemeClick(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&)
    {
        auto element = sender.try_as<FrameworkElement>();
        if (element == nullptr)
        {
            return;
        }
        auto tag = element.Tag();
        if (tag == nullptr)
        {
            return;
        }
        ApplyTheme(std::wstring{ winrt::unbox_value<hstring>(tag).c_str() });
    }

    void MainWindow::ApplyTheme(std::wstring const& themeId)
    {
        ThemeInfo const& theme = ThemeById(themeId);
        m_themeId = theme.id;

        // 1) 整窗背景渐变。
        auto resources = Application::Current().Resources();
        auto const gradientKey = box_value(hstring{ theme.gradientKey });
        if (resources.HasKey(gradientKey))
        {
            RootHost().Background(resources.Lookup(gradientKey).as<Media::Brush>());
        }

        // 2) 强调色。Light / Dark 两套字典都改：实际生效的是 Dark（根节点锁了
        //    ElementTheme.Dark），Light 一起改只是为了将来放开浅色时不用再补。
        auto dictionaries = resources.ThemeDictionaries();
        for (auto const& name : { L"Light", L"Dark" })
        {
            bool const isLight = name[0] == L'L';
            auto const dictionaryKey = box_value(hstring{ name });
            if (!dictionaries.HasKey(dictionaryKey))
            {
                continue;
            }
            auto dictionary = dictionaries.Lookup(dictionaryKey).try_as<ResourceDictionary>();
            if (dictionary == nullptr)
            {
                continue;
            }

            auto const& accent = theme.accent;
            SetThemeColor(dictionary, L"WmAccentBrush", accent);
            SetThemeColor(dictionary, L"WmAccentSoftBrush",
                          isLight ? TintTowardWhite(accent, 0.88) : Translucent(accent, 0x2E));
            SetThemeColor(dictionary, L"WmAccentAltBrush", theme.accentAlt);
            SetThemeColor(dictionary, L"AccentButtonForeground", accent);
            SetThemeColor(dictionary, L"ToggleButtonForegroundChecked", accent);
            SetThemeColor(dictionary, L"NavigationViewSelectionIndicatorForeground", accent);
            SetThemeColor(dictionary, L"SliderTrackValueFill", accent);
            SetThemeColor(dictionary, L"SliderTrackValueFillPointerOver", accent);
            SetThemeColor(dictionary, L"SliderTrackValueFillPressed", Darken(accent));
            SetThemeColor(dictionary, L"SliderThumbBackground", accent);
            SetThemeColor(dictionary, L"SliderThumbBackgroundPointerOver", Lighten(accent));
            SetThemeColor(dictionary, L"SliderThumbBackgroundPressed", Darken(accent));
            SetThemeColor(dictionary, L"ProgressBarForeground", accent);
            SetThemeColor(dictionary, L"TextControlSelectionHighlightColor", accent);
        }

        // 3) 频谱柱的配色是在 Attach 时烘进每根柱子里的，所以要重建一次。
        m_spectrumView.Attach(SpectrumCanvas(), 28);

        // 4) 记住选择，并把菜单里的对勾挪过去。
        wm::app::Settings().UiTheme(m_themeId);
        UpdateThemeMarks();
    }

    void MainWindow::UpdateThemeMarks()
    {
        auto mark = [this](std::wstring const& id) -> FontIcon {
            if (id == L"ocean")  { return ThemeCheckOcean(); }
            if (id == L"sunset") { return ThemeCheckSunset(); }
            if (id == L"galaxy") { return ThemeCheckGalaxy(); }
            if (id == L"sakura") { return ThemeCheckSakura(); }
            return ThemeCheckQq();
        };
        for (auto const& theme : kThemes)
        {
            auto icon = mark(theme.id);
            if (icon == nullptr)
            {
                continue;
            }
            icon.Visibility(theme.id == m_themeId ? Visibility::Visible : Visibility::Collapsed);
        }
    }
}
