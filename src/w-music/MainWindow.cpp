#include "pch.h"

#include "MainWindow.h"
#include "MainWindow.g.cpp"

#include "Services/Services.h"
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
}
