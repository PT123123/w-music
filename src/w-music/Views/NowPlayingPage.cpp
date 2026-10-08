#include "pch.h"

#include "Views/NowPlayingPage.h"
#include "Views/NowPlayingPage.g.cpp"

#include "Models/LyricLineItem.h"
#include "Services/AppPaths.h"
#include "Services/RecommendService.h"
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

        StructureCanvas().Tapped({ this, &NowPlayingPage::OnStructureTapped });
    }

    winrt::w_music::PlayerViewModel NowPlayingPage::Player() const
    {
        return wm::app::Player();
    }

    void NowPlayingPage::OnLoaded(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        // Player() is a process-lifetime singleton and the handler touches the
        // x:Bind controls: subscribing in the constructor (as it used to) kept
        // firing UpdateTransport() into a page whose XAML content was already
        // torn down after the first navigation away -- 0xC0000005 in
        // NowPlayingPage::UpdateTransport. Same Loaded/Unloaded pairing as
        // RecommendPage.
        if (m_propertyToken.value == 0)
        {
            m_propertyToken = wm::app::Player().PropertyChanged({ this, &NowPlayingPage::OnPlayerPropertyChanged });
        }

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

        // 时间轴的画布随页面走：OnLoaded 挂上、OnUnloaded 摘掉，形状只在这一份
        // Canvas 上活过一遍，不会像频谱那样留到一个已经拆掉的界面里。
        m_timeline.Attach(StructureCanvas());

        UpdateTransport();
    }

    void NowPlayingPage::OnUnloaded(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        wm::app::Player().PropertyChanged(m_propertyToken);
        m_propertyToken = {};
        m_timeline.Detach();
        if (m_holdTimer != nullptr)
        {
            m_holdTimer.Stop();
        }
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
                                                 winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs const& args)
    {
        // 歌词是后到的（读盘、在线取词），偏移也可能被 ±0.5s 改掉：这两种情况下
        // 段落要照着已经拿到的分析重贴一遍，否则色条会停在上一批行上。
        auto const name = args.PropertyName();
        if (name == L"Lyrics" || name == L"LyricOffsetMs")
        {
            AnnotateLyrics();
        }
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

        std::wstring path;
        if (auto track = player.CurrentTrack())
        {
            path = std::wstring{ track.FilePath() };
        }
        // 在途的那次要等它回来再说：中途改口会把两份答案都画歪。
        if (!m_timelinePending)
        {
            SyncTimeline(path);
        }

        const int32_t active = player.ActiveLyricIndex();
        if (active != m_lastScrolled)
        {
            m_lastScrolled = active;
            ScrollLyricIntoView(active);
        }
    }

    // ------------------------------------------------------------ 结构时间轴

    namespace
    {
        /// 段落名称是给玩家看的：引擎只在重复与响度证据都够的时候才敢叫「副歌」，
        /// 认不出的段落一律「未命名」，也不拿一个相近的名字顶替。
        std::wstring SectionName(std::wstring const& label)
        {
            if (label == L"chorus") return L"副歌";
            if (label == L"verse") return L"主歌";
            if (label == L"intro") return L"前奏";
            if (label == L"outro") return L"尾奏";
            if (label == L"pre_chorus") return L"预副歌";
            if (label == L"bridge") return L"桥段";
            return L"未命名";
        }

        std::wstring Fixed2(double value)
        {
            wchar_t buffer[8];
            swprintf(buffer, 8, L"%.2f", std::clamp(value, 0.0, 1.0));
            return std::wstring{ buffer };
        }

        /// 不是 0–1 的分数就用一位小数：BPM、dB 都是这样，2.732226 这种数字
        /// 摆在脸上没人读得下去。
        std::wstring Fixed1(double value)
        {
            wchar_t buffer[12];
            swprintf(buffer, 12, L"%.1f", value);
            return std::wstring{ buffer };
        }

        std::wstring Rounded(double value)
        {
            wchar_t buffer[16];
            swprintf(buffer, 16, L"%.0f", value);
            return std::wstring{ buffer };
        }

        /// 动态事件的中文名。引擎以后加新类型时不硬翻译成别的词，
        /// 原样写出比一个相近的名字更好查。
        std::wstring EventName(std::wstring const& type)
        {
            if (type == L"build") return L"堆叠";
            if (type == L"peak") return L"峰值";
            if (type == L"drop") return L"抽空";
            if (type == L"pull-back") return L"回落";
            if (type == L"release") return L"释放";
            return type;
        }

        std::wstring ModeName(std::wstring const& mode)
        {
            if (mode == L"major") return L"大调";
            if (mode == L"minor") return L"小调";
            return mode;
        }

        /// estimate_flags 里的内部字段名翻成人话；没见过的名字原样留着，
        /// 那正是「引擎新标了一个我们不认识的估计字段」的信号。
        std::wstring FlagName(std::wstring const& flag)
        {
            if (flag == L"chord") return L"和弦";
            if (flag == L"melody_pitch") return L"旋律音高";
            if (flag == L"vocal_presence") return L"人声存在";
            if (flag == L"instrument_energy") return L"乐器能量";
            if (flag == L"structure") return L"段落结构";
            if (flag == L"dynamics_events") return L"动态事件";
            return flag;
        }

        std::wstring JoinCsv(std::wstring const& csv, std::wstring (*name)(std::wstring const&))
        {
            std::wstring out;
            std::size_t pos = 0;
            while (pos < csv.size())
            {
                auto const next = csv.find(L',', pos);
                auto const part = csv.substr(pos, next == std::wstring::npos ? std::wstring::npos : next - pos);
                if (!part.empty())
                {
                    out += (out.empty() ? L"" : L"、") + name(part);
                }
                pos = next == std::wstring::npos ? csv.size() : next + 1;
            }
            return out;
        }

        /// 段落序列里是 intro/chorus/... 这些内部名，界面用结构卡片同一套名字。
        std::wstring SectionSequence(std::wstring const& csv)
        {
            return JoinCsv(csv, &SectionName);
        }
    } // namespace

    void NowPlayingPage::SyncTimeline(std::wstring const& filePath)
    {
        if (filePath == m_timelinePath)
        {
            return;
        }
        m_timelinePath = filePath;

        if (filePath.empty())
        {
            // 在线播放没有本地文件，引擎是按文件分析的，这里没有可画的东西。
            StructureCard().Visibility(Visibility::Collapsed);
            AnalysisCard().Visibility(Visibility::Collapsed);
            m_painted = wm::app::TrackTimeline{};
            AnnotateLyrics();
            return;
        }

        wm::app::TrackTimeline cached;
        if (wm::app::Recommend().PeekTimeline(filePath, cached))
        {
            PaintTimeline(cached);
            return;
        }

        // 先清场：上一首的曲线留在画布上会被读成这一首的形状。
        m_timeline.SetData(0.0, {}, {}, {});
        SegmentCards().Children().Clear();
        m_segmentStarts.clear();
        // 分析卡片和歌词上的段落标注同理：留着上一首的结论，这一首读起来就像
        // 已经分析过了。
        m_painted = wm::app::TrackTimeline{};
        EventChips().Children().Clear();
        m_eventStarts.clear();
        for (auto const& row : { AnalysisCurveText(), AnalysisRhythmText(), AnalysisHarmonyText(),
                                 AnalysisLoudnessText(), AnalysisTextureText(), AnalysisStructureText(),
                                 AnalysisNoteText() })
        {
            row.Text(hstring{});
        }
        AnalysisCard().Visibility(Visibility::Visible);
        AnalysisStateText().Text(hstring{ L"读取中…" });
        AnnotateLyrics();
        m_timelinePending = true;
        StructureCard().Visibility(Visibility::Visible);
        StructureStateText().Text(hstring{ L"读取中…" });
        StructureNoteText().Text(hstring{ L"正在向本地引擎要这首歌的全曲分析；引擎没在跑时不会为一张曲线图新起进程。" });

        auto const weak = get_weak();
        wm::app::Recommend().RequestTimelineAsync(filePath, [weak](wm::app::TrackTimeline timeline)
        {
            auto page = weak.get();
            if (page == nullptr)
            {
                return;
            }
            page->m_timelinePending = false;
            page->PaintTimeline(timeline);
        });
    }

    void NowPlayingPage::PaintTimeline(wm::app::TrackTimeline const& timeline)
    {
        // 迟到的答案属于上一首：切歌快的时候不能把它画到这一首脸上。
        auto player = wm::app::Player();
        std::wstring current;
        if (auto track = player.CurrentTrack())
        {
            current = std::wstring{ track.FilePath() };
        }
        if (current != timeline.filePath)
        {
            return;
        }

        StructureCard().Visibility(Visibility::Visible);

        std::vector<wm::app::StructureTimelineView::Segment> segs;
        segs.reserve(timeline.segments.size());
        for (auto const& seg : timeline.segments)
        {
            segs.push_back(wm::app::StructureTimelineView::Segment{ seg.start, seg.end, seg.group, seg.boundaryConfidence });
        }
        m_timeline.SetData(timeline.duration, timeline.curveTimes, timeline.curve, segs);
        BuildSegmentCards(timeline);
        // 分析卡片和歌词的段落标注吃的是同一份答案：切歌时一起重画，
        // 别让它们中某一个还停在上一首。
        m_painted = timeline;
        PaintAnalysis(timeline);
        AnnotateLyrics();

        StructureStateText().Text(timeline.ok ? hstring{ L"本地引擎 · 全曲" }
                                              : hstring{ L"本地引擎 · 没有数据" });

        std::wstring footer;
        if (timeline.ok)
        {
            std::vector<std::wstring> groups;
            for (auto const& seg : timeline.segments)
            {
                if (!seg.group.empty() && std::find(groups.begin(), groups.end(), seg.group) == groups.end())
                {
                    groups.push_back(seg.group);
                }
            }
            footer = L"共 " + std::to_wstring(timeline.segments.size()) + L" 段";
            if (!groups.empty())
            {
                footer += L" · 重复归为 " + std::to_wstring(groups.size()) + L" 组（同色就是同一段落反复出现）";
            }
            footer += L" · 响度 " + std::to_wstring(timeline.curve.size()) + L" 点。";
        }
        footer += timeline.note;
        if (!timeline.segments.empty())
        {
            footer += L" 点卡片跳到那一段的开头，点曲线的任意位置跳到那个时间。";
        }
        StructureNoteText().Text(hstring{ footer });
    }

    void NowPlayingPage::BuildSegmentCards(wm::app::TrackTimeline const& timeline)
    {
        auto panel = SegmentCards().Children();
        panel.Clear();
        m_segmentStarts.clear();
        if (!timeline.ok || timeline.segments.empty())
        {
            return;
        }

        for (std::size_t i = 0; i < timeline.segments.size(); ++i)
        {
            auto const& seg = timeline.segments[i];
            m_segmentStarts.push_back(seg.start);

            StackPanel box;
            box.Spacing(1);

            TextBlock time;
            time.Text(hstring{ wm::app::FormatDuration(static_cast<long long>(seg.start * 1000.0)) });
            time.FontSize(12.5);

            std::wstring name = SectionName(seg.label);
            if (!seg.group.empty())
            {
                name += L" · " + seg.group + L" 组";
            }
            TextBlock label;
            label.Text(hstring{ name });
            label.FontSize(11.5);
            label.Opacity(0.8);

            // 两个分数摆在卡片脸上：边界是「这里真的换了没有」，命名是「它叫什么」。
            TextBlock scores;
            scores.Text(hstring{ L"边界 " + Fixed2(seg.boundaryConfidence) + L" · 命名 " + Fixed2(seg.labelConfidence) });
            scores.FontSize(10);
            scores.Opacity(0.5);

            box.Children().Append(time);
            box.Children().Append(label);
            box.Children().Append(scores);

            Button card;
            card.Content(box);
            card.Padding(Thickness{ 10, 6, 10, 7 });
            card.CornerRadius(winrt::Microsoft::UI::Xaml::CornerRadiusHelper::FromUniformRadius(10));
            card.Tag(box_value(static_cast<int32_t>(i)));
            card.Click({ this, &NowPlayingPage::OnSegmentCardClicked });
            panel.Append(card);
        }
    }

    void NowPlayingPage::PaintAnalysis(wm::app::TrackTimeline const& timeline)
    {
        auto const& a = timeline.analysis;
        if (!a.ok)
        {
            // 引擎没给出这一首的行：没有数字可写，就不摆一张空卡片。
            AnalysisCard().Visibility(Visibility::Collapsed);
            return;
        }
        AnalysisCard().Visibility(Visibility::Visible);
        AnalysisStateText().Text(a.fullTrack ? hstring{ L"本地引擎 · 全曲 + 最响窗口" }
                                             : hstring{ L"本地引擎 · 最响窗口" });

        // 每个数字都带着它是从哪一段音频测出来的：重新分析之后段落和响度曲线
        // 覆盖全曲，节奏 / 调性 / 和声 / 人声这几项引擎仍然只在最响的窗口上测。
        std::wstring window;
        if (a.windowEnd > a.windowStart)
        {
            window = L"（最响的 " + std::wstring{ wm::app::FormatDuration(static_cast<long long>(a.windowStart * 1000.0)) }
                   + L"–" + std::wstring{ wm::app::FormatDuration(static_cast<long long>(a.windowEnd * 1000.0)) }
                   + L" 窗口）";
        }
        else
        {
            window = L"（引擎选定的分析窗口）";
        }
        std::wstring const fullScope = a.fullTrack ? L"（全曲）" : std::wstring{ window };

        if (!timeline.curve.empty())
        {
            std::wstring unit = timeline.curveUnit.empty() ? std::wstring{ L"dBFS" } : timeline.curveUnit;
            AnalysisCurveText().Text(hstring{ L"全曲响度曲线：平均电平 " + Fixed1(timeline.levelMeanDb) + L" " + unit
                                              + L" · 电平范围 " + Fixed1(timeline.levelRangeDb) + L" dB（p95−p5） · "
                                              + std::to_wstring(timeline.curve.size()) + L" 点 · 每点 "
                                              + Fixed1(timeline.curveInterval) + L" 秒" });
        }
        else
        {
            AnalysisCurveText().Text(hstring{});
        }

        AnalysisRhythmText().Text(hstring{ L"节奏" + window + L"：" + Fixed1(a.bpm) + L" BPM · 拍点稳定 "
                                           + Fixed2(a.beatConsistency) + L" · 舞动度 " + Fixed2(a.danceability)
                                           + L" · 起始密度 " + Fixed1(a.onsetDensity) + L" /秒" });

        std::wstring tonal = a.key.empty() ? std::wstring{ L"没测出调性" } : a.key + L" " + ModeName(a.mode);
        AnalysisHarmonyText().Text(hstring{ L"调性与和声" + window + L"：" + tonal
                                            + L" · 和弦 " + (a.chordSequence.empty() ? std::wstring{ L"没给出" } : a.chordSequence)
                                            + L" · 和声节奏 " + Fixed1(a.harmonicRhythm)
                                            + L" · 不和谐度 " + Fixed2(a.dissonance) });

        AnalysisLoudnessText().Text(hstring{ L"响度分布" + fullScope + L"：动态范围 " + Fixed2(a.dynamicRange)
                                             + L"（p90−p10，线性尺度） · 峰值系数 " + Fixed2(a.crestFactor)
                                             + L" · 平均 RMS " + Fixed2(a.rmsMean) });

        AnalysisTextureText().Text(hstring{ L"人声与配器" + window + L"：" + (a.hasVocal ? std::wstring{ L"有人声" }
                                                                                          : std::wstring{ L"没测出人声" })
                                            + L"（低置信判定） · 人声占比 " + Fixed2(a.vocalRatio)
                                            + L" · 低频 20–250Hz " + Fixed2(a.lowBandRatio)
                                            + L" · 打击成分 " + Fixed2(a.drumRatio)
                                            + L" · 明亮度 " + Rounded(a.spectralCentroid) + L" Hz" });

        if (!a.segmentTypeSequence.empty())
        {
            std::wstring text = L"段落序列" + fullScope + L"：" + SectionSequence(a.segmentTypeSequence);
            if (!a.groupSequence.empty())
            {
                text += L" · 重复分组 " + a.groupSequence;
            }
            if (a.chorusRepeatCount > 0)
            {
                text += L" · 副歌出现 " + std::to_wstring(a.chorusRepeatCount) + L" 次";
            }
            AnalysisStructureText().Text(hstring{ text });
        }
        else
        {
            AnalysisStructureText().Text(hstring{});
        }

        BuildEventChips(timeline);

        std::wstring note = L"这些数字都是本地引擎从音频里测出来的，不是平台标签或文件里的曲风。";
        if (!a.estimateFlags.empty())
        {
            note += L" 引擎声明这些是估计值：" + JoinCsv(a.estimateFlags, &FlagName) + L"。";
        }
        if (!timeline.events.empty() || !timeline.segments.empty())
        {
            note += L" 置信度是未校准的模型分数，不能当概率读。";
        }
        if (!a.fullTrack)
        {
            note += L" 这首歌还是旧版分析，下面的数字测自最响的窗口；重新分析曲库后段落与响度曲线才覆盖全曲。";
        }
        for (auto const& n : timeline.curveNotes)
        {
            note += L" " + n;
        }
        for (auto const& n : a.fieldNotes)
        {
            note += L" " + n + L"。";
        }
        AnalysisNoteText().Text(hstring{ note });
    }

    void NowPlayingPage::BuildEventChips(wm::app::TrackTimeline const& timeline)
    {
        auto panel = EventChips().Children();
        panel.Clear();
        m_eventStarts.clear();
        if (timeline.events.empty())
        {
            return;
        }

        for (std::size_t i = 0; i < timeline.events.size(); ++i)
        {
            auto const& ev = timeline.events[i];
            m_eventStarts.push_back(ev.time);

            StackPanel box;
            box.Spacing(1);

            TextBlock head;
            head.Text(hstring{ std::wstring{ wm::app::FormatDuration(static_cast<long long>(ev.time * 1000.0)) }
                               + L" " + EventName(ev.type) });
            head.FontSize(12.5);

            TextBlock score;
            score.Text(hstring{ L"置信 " + Fixed2(ev.confidence) });
            score.FontSize(10);
            score.Opacity(0.5);

            box.Children().Append(head);
            box.Children().Append(score);

            Button chip;
            chip.Content(box);
            chip.Padding(Thickness{ 10, 5, 10, 6 });
            chip.CornerRadius(winrt::Microsoft::UI::Xaml::CornerRadiusHelper::FromUniformRadius(10));
            chip.Tag(box_value(static_cast<int32_t>(i)));
            // 证据串是引擎自己的记法（rms_rise:+14.1dB_over_18s），原样挂着：
            // 想知道这个峰值凭什么算出来，鼠标停上去就是依据。
            if (!ev.evidence.empty())
            {
                winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
                    chip, winrt::box_value(hstring{ ev.evidence }));
            }
            chip.Click({ this, &NowPlayingPage::OnEventChipClicked });
            panel.Append(chip);
        }
    }

    void NowPlayingPage::OnEventChipClicked(Windows::Foundation::IInspectable const& sender,
                                            RoutedEventArgs const&)
    {
        auto const button = sender.try_as<Button>();
        if (button == nullptr)
        {
            return;
        }
        auto const index = unbox_value_or<int32_t>(button.Tag(), -1);
        if (index < 0 || static_cast<std::size_t>(index) >= m_eventStarts.size())
        {
            return;
        }
        wm::app::Player().Seek(m_eventStarts[static_cast<std::size_t>(index)]);
    }

    void NowPlayingPage::AnnotateLyrics()
    {
        auto const items = LyricList().Items();
        if (items.Size() == 0)
        {
            return;
        }

        // 旧版分析的段落时间是那 45 秒窗口里的相对偏移，贴到歌词上会把
        // 「窗口第 5 秒」读成「全曲第 5 秒」，所以只有全曲段落才往歌词上贴。
        bool const useSegments = m_painted.analysis.fullTrack && !m_painted.segments.empty();
        auto const& segs = m_painted.segments;
        const double offset = static_cast<double>(wm::app::Player().LyricOffsetMs());

        // 同一组一支画刷：两三百行歌词不必每行新建一份。
        std::map<std::wstring, std::pair<winrt::Microsoft::UI::Xaml::Media::SolidColorBrush,
                                         winrt::Microsoft::UI::Xaml::Media::SolidColorBrush>> paints;
        auto paintFor = [&paints](std::wstring const& group)
        {
            auto const found = paints.find(group);
            if (found != paints.end())
            {
                return found->second;
            }
            auto const pair = std::make_pair(
                winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{ wm::app::StructureGroupColor(group, 0xB0) },
                winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{ wm::app::StructureGroupColor(group, 0xFF) });
            paints[group] = pair;
            return pair;
        };

        std::size_t cursor = 0;
        std::size_t tagged = static_cast<std::size_t>(-1);
        std::size_t taggedLines = 0;
        for (std::uint32_t i = 0; i < items.Size(); ++i)
        {
            auto item = items.GetAt(i).try_as<winrt::w_music::LyricLineItem>();
            if (item == nullptr)
            {
                continue;
            }
            if (!useSegments)
            {
                item.SegmentTag(hstring{});
                item.SegmentBar(nullptr);
                item.SegmentTagForeground(nullptr);
                continue;
            }

            const double seconds = (static_cast<double>(item.TimeMs()) + offset) / 1000.0;
            while (cursor + 1 < segs.size() && seconds >= segs[cursor + 1].start)
            {
                ++cursor;
            }
            auto const& seg = segs[cursor];
            if (seconds < seg.start || seconds >= seg.end)
            {
                // 落在两段之间的缝隙里（引擎认为这里没成段）：不硬塞一个相近的段落。
                item.SegmentTag(hstring{});
                item.SegmentBar(nullptr);
                item.SegmentTagForeground(nullptr);
                continue;
            }

            auto const brushes = paintFor(seg.group);
            item.SegmentBar(brushes.first);
            std::wstring tag;
            if (cursor != tagged)
            {
                // 一段只标一次，标在这一段的第一句上。
                tagged = cursor;
                ++taggedLines;
                tag = SectionName(seg.label);
                if (!seg.group.empty())
                {
                    tag += L" · " + seg.group;
                }
            }
            item.SegmentTag(hstring{ tag });
            item.SegmentTagForeground(brushes.second);
        }

        if (useSegments)
        {
            // 界面点不动（桌面自动化被拦），这行就是「段落确实贴到歌词上」的唯一凭据。
            wm::app::Diag("lyric structure lines=" + std::to_string(items.Size())
                 + " sections=" + std::to_string(segs.size())
                 + " tagged=" + std::to_string(taggedLines));
        }
    }

    void NowPlayingPage::OnSegmentCardClicked(Windows::Foundation::IInspectable const& sender,
                                              RoutedEventArgs const&)
    {
        auto const button = sender.try_as<Button>();
        if (button == nullptr)
        {
            return;
        }
        auto const index = unbox_value_or<int32_t>(button.Tag(), -1);
        if (index < 0 || static_cast<std::size_t>(index) >= m_segmentStarts.size())
        {
            return;
        }
        wm::app::Player().Seek(m_segmentStarts[static_cast<std::size_t>(index)]);
    }

    void NowPlayingPage::OnStructureTapped(Windows::Foundation::IInspectable const&,
                                           winrt::Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const& args)
    {
        auto const point = args.GetPosition(StructureCanvas());
        const double seconds = m_timeline.TimeAt(point.X);
        if (seconds >= 0.0)
        {
            wm::app::Player().Seek(seconds);
        }
    }
}
