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
        FlowUpNextList().ItemClick({ this, &NowPlayingPage::OnFlowUpNextItemClick });
        FlowRefreshBtn().Click({ this, &NowPlayingPage::OnFlowRefreshClicked });
        ProgressSlider().ValueChanged({ this, &NowPlayingPage::OnProgressChanged });
        PrevBtn().Click({ this, &NowPlayingPage::OnPrevClicked });
        PlayPauseBtn().Click({ this, &NowPlayingPage::OnPlayPauseClicked });
        NextBtn().Click({ this, &NowPlayingPage::OnNextClicked });
        ModeBtn().Click({ this, &NowPlayingPage::OnModeClicked });
        FavBtn().Click({ this, &NowPlayingPage::OnFavoriteClicked });
        SlowerBtn().Click({ this, &NowPlayingPage::OnSlowerClicked });
        FasterBtn().Click({ this, &NowPlayingPage::OnFasterClicked });
        ResetBtn().Click({ this, &NowPlayingPage::OnResetOffsetClicked });

        // 封面区手势（见 xaml 注释）：左右滑 = 点赞/差评，长按 = 差评。
        Cover().PointerPressed({ this, &NowPlayingPage::OnCoverPointerPressed });
        Cover().PointerReleased({ this, &NowPlayingPage::OnCoverPointerReleased });
        Cover().ManipulationDelta({ this, &NowPlayingPage::OnCoverManipulationDelta });
        Cover().ManipulationCompleted({ this, &NowPlayingPage::OnCoverManipulationCompleted });

        wm::app::Player().PropertyChanged({ this, &NowPlayingPage::OnPlayerPropertyChanged });
    }

    winrt::w_music::PlayerViewModel NowPlayingPage::Player() const
    {
        return wm::app::Player();
    }

    void NowPlayingPage::OnLoaded(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (m_holdTimer == nullptr)
        {
            m_holdTimer = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();
            m_holdTimer.Interval(std::chrono::milliseconds{ 550 });
            m_holdTimer.IsRepeating(false);
            m_holdTimer.Tick({ this, &NowPlayingPage::OnHoldTimerTick });
        }

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

    void NowPlayingPage::OnFlowRefreshClicked(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        wm::app::Player().SkipFlowSlot();
    }

    void NowPlayingPage::OnFlowUpNextItemClick(Windows::Foundation::IInspectable const&,
                                               winrt::Microsoft::UI::Xaml::Controls::ItemClickEventArgs const& args)
    {
        auto player = wm::app::Player();
        // 推荐流：点谁播谁（它前面的预告一并出队）。顺序类模式：卡片里那
        // 一行就是队列的下一首，等价于点「下一曲」。
        if (!player.IsFlowMode())
        {
            player.Next();
            return;
        }
        uint32_t index = 0;
        if (!FlowUpNextList().Items().IndexOf(args.ClickedItem(), index))
        {
            return;
        }
        player.PlayFlowUpNext(static_cast<int32_t>(index));
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

    // ------------------------------------------------------------ 封面区手势

    void NowPlayingPage::OnCoverPointerPressed(winrt::Windows::Foundation::IInspectable const&,
                                               winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        m_pointerDown = true;
        m_dragging = false;
        Cover().CapturePointer(args.Pointer());
        if (m_holdTimer != nullptr)
        {
            m_holdTimer.Start();
        }
    }

    void NowPlayingPage::OnCoverPointerReleased(winrt::Windows::Foundation::IInspectable const&,
                                                winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
    {
        m_pointerDown = false;
        if (m_holdTimer != nullptr)
        {
            m_holdTimer.Stop();
        }
        Cover().ReleasePointerCapture(args.Pointer());
    }

    void NowPlayingPage::OnCoverManipulationDelta(winrt::Windows::Foundation::IInspectable const&,
                                                  winrt::Microsoft::UI::Xaml::Input::ManipulationDeltaRoutedEventArgs const& args)
    {
        const double x = args.Cumulative().Translation.X;
        if (std::fabs(x) > 12)
        {
            m_dragging = true;
            if (m_holdTimer != nullptr)
            {
                m_holdTimer.Stop();
            }
        }
        // 橡皮筋：跟手但限幅，视觉上明确"这是一个手势，不是拖窗口"。
        CoverShift().X(std::clamp(x, -110.0, 110.0) * 0.6);
    }

    void NowPlayingPage::OnCoverManipulationCompleted(winrt::Windows::Foundation::IInspectable const&,
                                                      winrt::Microsoft::UI::Xaml::Input::ManipulationCompletedRoutedEventArgs const& args)
    {
        CoverShift().X(0);
        if (m_holdTimer != nullptr)
        {
            m_holdTimer.Stop();
        }
        const double x = args.Cumulative().Translation.X;
        if (x < -70)
        {
            wm::app::Player().DislikeCurrent();
        }
        else if (x > 70)
        {
            wm::app::Player().LikeCurrent();
        }
    }

    void NowPlayingPage::OnHoldTimerTick(winrt::Windows::Foundation::IInspectable const&,
                                         winrt::Windows::Foundation::IInspectable const&)
    {
        if (m_holdTimer != nullptr)
        {
            m_holdTimer.Stop();
        }
        // 定时器到点而指针没拖动也没抬起 = 长按 = 差评。
        if (m_pointerDown && !m_dragging)
        {
            wm::app::Player().DislikeCurrent();
        }
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
        // 电台/手势的轻提示：有内容就浮现，清空（2.6s 后）就淡出。
        FlowToast().Opacity(player.FlowStatusText().empty() ? 0.0 : 0.92);

        const int32_t active = player.ActiveLyricIndex();
        if (active != m_lastScrolled)
        {
            m_lastScrolled = active;
            ScrollLyricIntoView(active);
        }
    }
}
