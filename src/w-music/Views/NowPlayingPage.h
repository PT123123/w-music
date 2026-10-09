#pragma once

#include "NowPlayingPage.g.h"

#include "Controls/SpectrumView.h"
#include "Controls/StructureTimelineView.h"
#include "Services/RecommendService.h"

namespace winrt::w_music::implementation
{
    struct NowPlayingPage : NowPlayingPageT<NowPlayingPage>
    {
        NowPlayingPage();

        winrt::w_music::PlayerViewModel Player() const;

    private:
        void OnLoaded(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnUnloaded(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnLyricItemClick(winrt::Windows::Foundation::IInspectable const& sender,
                              winrt::Microsoft::UI::Xaml::Controls::ItemClickEventArgs const& args);
        void OnPlayerPropertyChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                     winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs const& args);
        void OnProgressChanged(winrt::Windows::Foundation::IInspectable const& sender,
                               winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args);
        void OnPrevClicked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnPlayPauseClicked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnNextClicked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnFlowRefreshClicked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnFlowUpNextItemClick(winrt::Windows::Foundation::IInspectable const& sender,
                                   winrt::Microsoft::UI::Xaml::Controls::ItemClickEventArgs const& args);
        void OnModeClicked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);

        // ---- 结构时间轴：全曲响度曲线 + 段落卡片 ----
        void OnStructureTapped(winrt::Windows::Foundation::IInspectable const& sender,
                               winrt::Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const& args);
        /// 曲线切换：电平 / 起音率 / 低频比例 / 亮度 / 谱变化。
        void BuildCurveChips(wm::app::TrackTimeline const& timeline);
        void HighlightCurveChip();
        void UpdateCurveUnitText();
        void OnCurveChipClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        /// 结构卡片上的「生成分析」：让引擎扫描曲库，完成后重取本曲的时间轴。
        void OnStructureAnalyzeClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        winrt::fire_and_forget AnalyzeForStructureAsync();
        void OnSegmentCardClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                  winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        /// 换曲时向本地引擎要这一首的分析（有会话备忘就直接画，不再走网络）。
        void SyncTimeline(std::wstring const& filePath);
        void PaintTimeline(wm::app::TrackTimeline const& timeline);
        void BuildSegmentCards(wm::app::TrackTimeline const& timeline);
        /// 「本曲分析」：曲线尺度 + 动态事件 + 节奏/调性/和声/响度/配器标量。
        void PaintAnalysis(wm::app::TrackTimeline const& timeline);
        void BuildEventChips(wm::app::TrackTimeline const& timeline);
        /// 把段落贴到每一句歌词上（左侧色条 + 段首标签）。歌词比分析晚到、
        /// 或用户改了偏移时都要重贴一遍。
        void AnnotateLyrics();
        void OnEventChipClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnFavoriteClicked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnSlowerClicked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnFasterClicked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnResetOffsetClicked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);

        // ---- 封面区手势：左右滑 = 点赞/差评，长按 = 差评 ----
        void OnCoverPointerPressed(winrt::Windows::Foundation::IInspectable const& sender,
                                   winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnCoverPointerReleased(winrt::Windows::Foundation::IInspectable const& sender,
                                    winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args);
        void OnCoverManipulationDelta(winrt::Windows::Foundation::IInspectable const& sender,
                                      winrt::Microsoft::UI::Xaml::Input::ManipulationDeltaRoutedEventArgs const& args);
        void OnCoverManipulationCompleted(winrt::Windows::Foundation::IInspectable const& sender,
                                          winrt::Microsoft::UI::Xaml::Input::ManipulationCompletedRoutedEventArgs const& args);
        void OnHoldTimerTick(winrt::Windows::Foundation::IInspectable const& sender,
                             winrt::Windows::Foundation::IInspectable const& args);

        void ScrollLyricIntoView(int32_t index);
        void UpdateTransport();

        wm::app::SpectrumView m_spectrumView;
        std::size_t m_spectrumToken = 0;
        winrt::event_token m_propertyToken{};
        bool m_updatingSlider = false;
        int32_t m_lastScrolled = -1;

        // WinUI 3 没有 UWP 的 Holding 事件，长按用指针 + 定时器自己实现。
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_holdTimer{ nullptr };
        bool m_pointerDown = false;
        bool m_dragging = false;

        // ---- 结构时间轴 ----
        wm::app::StructureTimelineView m_timeline;
        /// 已经画过（或正在等答案）的那一首文件路径：只在换曲时重取。
        std::wstring m_timelinePath;
        bool m_timelinePending = false;
        /// 当前显示的曲线序号（m_painted.curves 里）；换曲归零回电平。
        std::size_t m_activeCurve = 0;
        /// 本次页面生命周期里是否已经后台预热过引擎（结构卡片一出现就拉起，
        /// 免得用户点「生成分析」时等十几秒的 Python 冷启动）。
        bool m_prewarmKicked = false;
        /// 卡片序号 -> 段落起点（秒）；按钮 Tag 只装序号。
        std::vector<double> m_segmentStarts;
        /// 事件 chip 同理，序号 -> 事件时间（秒）。
        std::vector<double> m_eventStarts;
        /// 已经画在这一页上的分析：歌词常常比分析晚到一步（在线取词、读盘），
        /// 到了一步就要照同一份段落重贴一次，不能等下一首歌。
        wm::app::TrackTimeline m_painted;
    };
}

namespace winrt::w_music::factory_implementation
{
    struct NowPlayingPage : NowPlayingPageT<NowPlayingPage, implementation::NowPlayingPage>
    {
    };
}
