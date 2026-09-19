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

    // -----------------------------------------------------------------------
    // 均衡器预设
    // -----------------------------------------------------------------------
    // 十段增益（dB，31 Hz – 16 kHz），沿用 AIMP/Winamp 一脉的经典曲线。
    // 预设只改频段增益，前置放大器（第 0 根滑条）保持用户当前值。
    struct EqPresetInfo
    {
        wchar_t const* id;
        wchar_t const* name;
        std::array<double, 10> gains;
    };

    EqPresetInfo const kEqPresets[]{
        { L"flat",      L"平直",     { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
        { L"dance",     L"舞曲",     { 7, 6.5, 2, 0, -1.5, -2.5, 0, 3.5, 5.5, 6 } },
        { L"pop",       L"流行",     { -1, 0.5, 2.5, 4.5, 5, 4, 2, 0, -0.5, -1 } },
        { L"rock",      L"摇滚",     { 5.5, 4.5, 3, 1, -1.5, -2.5, -1.5, 3.5, 5, 6 } },
        { L"jazz",      L"爵士",     { 4.5, 3.5, 1.5, 4, -1.5, -1.5, 0.5, 3.5, 4.5, 4.5 } },
        { L"classical", L"古典",     { 6, 5.5, 5, 2.5, 0, -2, -4, -4.5, -2.5, 1.5 } },
        { L"bass",      L"低音增强", { 8, 7.5, 6.5, 4, 1, 0, 0, 0, 0, 0 } },
        { L"vocal",     L"人声",     { -2, -1, 0.5, 3, 4.5, 4.5, 3.5, 2, 1, 0.5 } },
    };

    // 11 根滑条的名字：第 0 根是前置放大器，其余对应上面十段。
    wchar_t const* const kEqColumnLabels[]{
        L"前置", L"31", L"63", L"125", L"250", L"500", L"1k", L"2k", L"4k", L"8k", L"16k"
    };

    hstring FormatEqDb(double value)
    {
        wchar_t buffer[16];
        swprintf(buffer, 16, L"%+.1f", value);
        return hstring{ buffer };
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

        // EQ 面板 + 把存档的均衡器配置推给播放器（此时还没有曲目，
        // SetEqualizerEnabled 只会记下开关，首播时再生效）。
        BuildEqPanel();
        auto& settings = wm::app::Settings();
        auto* playerVm = winrt::get_self<implementation::PlayerViewModel>(player);
        playerVm->ConfigureEqualizer(settings.EqGainsDb(), settings.EqPreampDb());
        playerVm->SetEqualizerEnabled(settings.EqEnabled());

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
        using namespace winrt::w_music::implementation;
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

    // -----------------------------------------------------------------------
    // 均衡器面板
    // -----------------------------------------------------------------------

    void MainWindow::BuildEqPanel()
    {
        auto const& settings = wm::app::Settings();

        for (auto const& preset : kEqPresets)
        {
            ComboBoxItem item;
            item.Content(box_value(hstring{ preset.name }));
            item.Tag(box_value(hstring{ preset.id }));
            EqPresetBox().Items().Append(item);
        }

        // 11 列：第 0 列前置放大器，其余十段；每列 = 数值 + 垂直滑条 + 频点名。
        for (int i = 0; i < 11; ++i)
        {
            StackPanel column;
            column.Spacing(2);
            column.Width(36);

            TextBlock valueLabel;
            valueLabel.FontSize(11);
            valueLabel.TextAlignment(TextAlignment::Center);
            valueLabel.Opacity(0.75);
            column.Children().Append(valueLabel);

            Slider slider;
            slider.Orientation(Orientation::Vertical);
            slider.Height(150);
            slider.Minimum(-12.0);
            slider.Maximum(12.0);
            slider.StepFrequency(0.5);
            // 垂直滑条默认"向下增大"，翻转成向上增大才符合推子直觉。
            slider.IsDirectionReversed(true);
            slider.HorizontalAlignment(HorizontalAlignment::Center);
            column.Children().Append(slider);

            TextBlock bandLabel;
            bandLabel.Text(hstring{ kEqColumnLabels[i] });
            bandLabel.FontSize(11);
            bandLabel.TextAlignment(TextAlignment::Center);
            bandLabel.Opacity(0.7);
            column.Children().Append(bandLabel);

            EqBandsHost().Children().Append(column);
            m_eqSliders.push_back(slider);
            m_eqValueLabels.push_back(valueLabel);
        }

        // 初值从 settings.json 恢复；先赋值、后挂事件，装配期不触发处理逻辑。
        double const preamp = settings.EqPreampDb();
        std::array<double, 10> const& gains = settings.EqGainsDb();
        m_eqSliders[0].Value(preamp);
        for (int i = 0; i < 10; ++i)
        {
            m_eqSliders[i + 1].Value(gains[i]);
        }
        ShowEqValues(preamp, gains);
        SelectEqPresetItem(settings.EqPreset());
        EqToggle().IsOn(settings.EqEnabled());

        EqToggle().Toggled({ this, &MainWindow::OnEqToggleChanged });
        EqPresetBox().SelectionChanged({ this, &MainWindow::OnEqPresetSelected });
        EqResetButton().Click({ this, &MainWindow::OnEqResetClicked });
        for (auto const& slider : m_eqSliders)
        {
            slider.ValueChanged({ this, &MainWindow::OnEqSliderChanged });
        }
    }

    void MainWindow::SelectEqPresetItem(std::wstring const& presetId)
    {
        int index = -1;
        for (std::size_t i = 0; i < std::size(kEqPresets); ++i)
        {
            if (presetId == kEqPresets[i].id)
            {
                index = static_cast<int>(i);
                break;
            }
        }
        // "custom"（或未知值）→ -1，下拉框留空。
        m_eqApplying = true;
        EqPresetBox().SelectedIndex(index);
        m_eqApplying = false;
    }

    void MainWindow::ReadEqSliders(double& preampDb, std::array<double, 10>& gainsDb)
    {
        preampDb = m_eqSliders[0].Value();
        for (int i = 0; i < 10; ++i)
        {
            gainsDb[i] = m_eqSliders[i + 1].Value();
        }
    }

    void MainWindow::ShowEqValues(double preampDb, std::array<double, 10> const& gainsDb)
    {
        m_eqValueLabels[0].Text(FormatEqDb(preampDb));
        for (int i = 0; i < 10; ++i)
        {
            m_eqValueLabels[i + 1].Text(FormatEqDb(gainsDb[i]));
        }
    }

    void MainWindow::SaveEqState()
    {
        double preamp = 0.0;
        std::array<double, 10> gains{};
        ReadEqSliders(preamp, gains);
        auto const index = EqPresetBox().SelectedIndex();
        std::wstring const preset = index >= 0 ? std::wstring{ kEqPresets[index].id } : std::wstring{ L"custom" };
        wm::app::Settings().SetEqualizer(EqToggle().IsOn(), preset, preamp, gains);
    }

    void MainWindow::OnEqSliderChanged(IInspectable const&, RangeBaseValueChangedEventArgs const&)
    {
        if (m_eqApplying)
        {
            return;
        }
        double preamp = 0.0;
        std::array<double, 10> gains{};
        ReadEqSliders(preamp, gains);
        ShowEqValues(preamp, gains);
        SelectEqPresetItem(L"custom");
        SaveEqState();
        // 即时生效：系数在音频线程上加锁换掉，不重启播放。
        winrt::get_self<implementation::PlayerViewModel>(wm::app::Player())->ConfigureEqualizer(gains, preamp);
    }

    void MainWindow::OnEqPresetSelected(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (m_eqApplying)
        {
            return;
        }
        auto const index = EqPresetBox().SelectedIndex();
        if (index < 0 || static_cast<std::size_t>(index) >= std::size(kEqPresets))
        {
            return;
        }
        auto const& preset = kEqPresets[index];
        m_eqApplying = true;
        for (int i = 0; i < 10; ++i)
        {
            m_eqSliders[i + 1].Value(preset.gains[i]);
        }
        m_eqApplying = false;

        double preamp = 0.0;
        std::array<double, 10> gains{};
        ReadEqSliders(preamp, gains);
        ShowEqValues(preamp, gains);
        SaveEqState();
        winrt::get_self<implementation::PlayerViewModel>(wm::app::Player())->ConfigureEqualizer(gains, preamp);
    }

    void MainWindow::OnEqToggleChanged(IInspectable const&, RoutedEventArgs const&)
    {
        // 重新绑定播放源（EQ 代理 <-> 直通），进度 / 播放状态会跨切换保留；
        // 没有曲目时只记下开关，首播再生效。
        winrt::get_self<implementation::PlayerViewModel>(wm::app::Player())
            ->SetEqualizerEnabled(EqToggle().IsOn());
        SaveEqState();
    }

    void MainWindow::OnEqResetClicked(IInspectable const&, RoutedEventArgs const&)
    {
        m_eqApplying = true;
        for (auto const& slider : m_eqSliders)
        {
            slider.Value(0.0);
        }
        m_eqApplying = false;
        SelectEqPresetItem(L"flat");
        ShowEqValues(0.0, std::array<double, 10>{});
        SaveEqState();
        winrt::get_self<implementation::PlayerViewModel>(wm::app::Player())
            ->ConfigureEqualizer(std::array<double, 10>{}, 0.0);
    }
}
