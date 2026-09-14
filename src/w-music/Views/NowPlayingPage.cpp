#include "pch.h"

#include "Views/NowPlayingPage.h"
#include "Views/NowPlayingPage.g.cpp"

#include "Models/LyricLineItem.h"
#include "Services/Services.h"
#include "ViewModels/PlayerViewModel.h"

using namespace winrt;
using namespace Windows::Foundation;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Controls::Primitives;

namespace winrt::w_music::implementation
{
    NowPlayingPage::NowPlayingPage()
    {
        InitializeComponent();
        m_spectrumView.Attach(SpectrumCanvas(), 32);

        Loaded({ this, &NowPlayingPage::OnLoaded });
        Unloaded({ this, &NowPlayingPage::OnUnloaded });

        LyricList().ItemClick({ this, &NowPlayingPage::OnLyricItemClick });
        ProgressSlider().ValueChanged({ this, &NowPlayingPage::OnProgressChanged });
        PrevBtn().Click({ this, &NowPlayingPage::OnPrevClicked });
        PlayPauseBtn().Click({ this, &NowPlayingPage::OnPlayPauseClicked });
        NextBtn().Click({ this, &NowPlayingPage::OnNextClicked });
        ModeBtn().Click({ this, &NowPlayingPage::OnModeClicked });
        FavBtn().Click({ this, &NowPlayingPage::OnFavoriteClicked });
        SlowerBtn().Click({ this, &NowPlayingPage::OnSlowerClicked });
        FasterBtn().Click({ this, &NowPlayingPage::OnFasterClicked });
        ResetBtn().Click({ this, &NowPlayingPage::OnResetOffsetClicked });

        wm::app::Player().PropertyChanged({ this, &NowPlayingPage::OnPlayerPropertyChanged });
    }

    winrt::w_music::PlayerViewModel NowPlayingPage::Player() const
    {
        return wm::app::Player();
    }

    void NowPlayingPage::OnLoaded(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto playerImpl = winrt::get_self<implementation::PlayerViewModel>(wm::app::Player());
        const auto weak = get_weak();

        m_spectrumToken = playerImpl->AddSpectrumSink([weak](std::vector<double> const& bars) {
            if (auto page = weak.get())
            {
                // weak_ref<D> here is weak_ref<implementation::NowPlayingPage>,
                // so get() already hands back the implementation pointer;
                // wrapping it in get_self<> asks for produce<D, D>, which does
                // not exist and fails to instantiate.
                page->m_spectrumView.Update(bars);
            }
        });

        UpdateTransport();
    }

    void NowPlayingPage::OnUnloaded(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (m_spectrumToken == 0)
        {
            return;
        }
        winrt::get_self<implementation::PlayerViewModel>(wm::app::Player())->RemoveSpectrumSink(m_spectrumToken);
        m_spectrumToken = 0;
    }

    void NowPlayingPage::OnProgressChanged(Windows::Foundation::IInspectable const&, RangeBaseValueChangedEventArgs const& args)
    {
        if (m_updatingSlider)
        {
            return;
        }
        wm::app::Player().Seek(args.NewValue());
    }

    void NowPlayingPage::OnPrevClicked(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        wm::app::Player().Previous();
    }

    void NowPlayingPage::OnPlayPauseClicked(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        wm::app::Player().TogglePlayPause();
    }

    void NowPlayingPage::OnNextClicked(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        wm::app::Player().Next();
    }

    void NowPlayingPage::OnModeClicked(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        wm::app::Player().CycleMode();
    }

    void NowPlayingPage::OnFavoriteClicked(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        wm::app::Player().ToggleFavorite();
    }

    void NowPlayingPage::OnSlowerClicked(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        wm::app::Player().AdjustLyricOffset(-500);
        UpdateTransport();
    }

    void NowPlayingPage::OnFasterClicked(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        wm::app::Player().AdjustLyricOffset(500);
        UpdateTransport();
    }

    void NowPlayingPage::OnResetOffsetClicked(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        wm::app::Player().LyricOffsetMs(0);
        UpdateTransport();
    }

    void NowPlayingPage::OnLyricItemClick(Windows::Foundation::IInspectable const&, ItemClickEventArgs const& args)
    {
        auto line = args.ClickedItem().try_as<winrt::w_music::LyricLineItem>();
        if (line == nullptr)
        {
            return;
        }
        wm::app::Player().SeekToLyric(line.Index());
    }

    void NowPlayingPage::OnPlayerPropertyChanged(Windows::Foundation::IInspectable const&,
                                                 winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs const&)
    {
        UpdateTransport();
    }

    void NowPlayingPage::ScrollLyricIntoView(int32_t index)
    {
        if (index < 0)
        {
            return;
        }
        const auto items = LyricList().Items();
        if (static_cast<std::uint32_t>(index) >= items.Size())
        {
            return;
        }
        // ScrollIntoViewAlignment has only Default/Leading -- there is no Center.
        LyricList().ScrollIntoView(items.GetAt(static_cast<std::uint32_t>(index)), ScrollIntoViewAlignment::Leading);
    }

    void NowPlayingPage::UpdateTransport()
    {
        auto player = wm::app::Player();

        const double duration = player.DurationSeconds();
        if (duration > 0.5)
        {
            ProgressSlider().Maximum(duration);
        }

        m_updatingSlider = true;
        ProgressSlider().Value(player.PositionSeconds());
        m_updatingSlider = false;

        PosText().Text(player.PositionText());
        DurText().Text(player.DurationText());
        PlayPauseIcon().Glyph(player.IsPlaying() ? hstring{ L"\uE769" } : hstring{ L"\uE768" });
        OffsetText().Text(hstring{ L"偏移 " + std::to_wstring(player.LyricOffsetMs()) + L" ms" });

        const int32_t active = player.ActiveLyricIndex();
        if (active != m_lastScrolled)
        {
            m_lastScrolled = active;
            ScrollLyricIntoView(active);
        }
    }
}
