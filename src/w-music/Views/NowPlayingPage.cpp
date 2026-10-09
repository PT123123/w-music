#include "pch.h"

#include "Views/NowPlayingPage.h"
#include "Views/NowPlayingPage.g.cpp"

#include "Models/LyricLineItem.h"
#include "Services/AppPaths.h"
#include "Services/LibraryService.h"
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
        StructureAnalyzeBtn().Click({ this, &NowPlayingPage::OnStructureAnalyzeClicked });
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
            // 维度事件与电平无关（哪个维度在 evidence 里），电平平稳的歌
            // 照样可以有这些变化。
            if (type == L"axis-rise") return L"维度上升";
            if (type == L"axis-fall") return L"维度下降";
            if (type == L"axis-drop") return L"维度骤降";
            return type;
        }

        /// 曲线 id 的中文短名；没见过的 id 原样显示，那正是引擎加了新曲线的信号。
        std::wstring CurveLabel(std::wstring const& id)
        {
            if (id == L"level_db") return L"电平(dB)";
            if (id == L"level_norm") return L"电平(相对)";
            if (id == L"onset_activity") return L"起音活动";
            if (id == L"onset_rate_hz") return L"起音率";
            if (id == L"low_band_ratio") return L"低频比例";
            if (id == L"spectral_brightness") return L"亮度";
            if (id == L"spectral_flux") return L"谱变化";
            // v5 引擎新增模块曲线
            if (id == L"spectral_flatness") return L"平坦度";
            if (id == L"spectral_bandwidth") return L"带宽";
            if (id == L"hf_power_ratio") return L"高频占比";
            if (id == L"flux_low") return L"低频变化";
            if (id == L"flux_mid") return L"中频变化";
            if (id == L"flux_high") return L"高频变化";
            if (id == L"lr_balance_db") return L"左右平衡";
            if (id == L"lr_correlation") return L"左右相关";
            if (id == L"side_energy_ratio") return L"侧向能量";
            if (id == L"local_tempo") return L"局部速度";
            return id;
        }

        /// 读一支主题画刷的当前颜色（同 StructureTimelineView：切主题后重画跟上）。
        winrt::Windows::UI::Color BrushColor(wchar_t const* key, winrt::Windows::UI::Color fallback)
        {
            try
            {
                auto value = Application::Current().Resources().Lookup(box_value(hstring{ key }));
                if (auto brush = value.try_as<winrt::Microsoft::UI::Xaml::Media::SolidColorBrush>())
                {
                    return brush.Color();
                }
            }
            catch (...)
            {
            }
            return fallback;
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

        // 引擎冷启动（Python 导入那一套）要十几秒：卡片一出现就后台拉起，
        // 用户真点「生成分析」时多半已经就绪。只踢一次，PrewarmAsync 内部
        // 幂等（引擎已就绪就直接返回），点按钮时 AnalyzeTrackAsync 还会兜底。
        if (!m_prewarmKicked && !wm::app::Recommend().IsReady())
        {
            m_prewarmKicked = true;
            wm::app::Diag("structure -> kick engine prewarm");
            wm::app::Recommend().PrewarmAsync();
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
        CurveChips().Children().Clear();
        CurveUnitText().Text(hstring{});
        // 分析卡片和歌词上的段落标注同理：留着上一首的结论，这一首读起来就像
        // 已经分析过了。
        m_painted = wm::app::TrackTimeline{};
        EventChips().Children().Clear();
        m_eventStarts.clear();
        for (auto const& row : { AnalysisCurveText(), AnalysisRhythmText(), AnalysisHarmonyText(),
                                 AnalysisLoudnessText(), AnalysisTextureText(), AnalysisStructureText(),
                                 AnalysisProfileText(), AnalysisNoteText() })
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

        // 多曲线：默认显示电平（level_db），其余曲线点小字切换；每个尺度按
        // 自身 min-max 显示，真实单位写在 chips 下面那行。旧引擎回答没有
        // curves 时 ParseTimeline 已从 legacy 字段合成，这里几乎总有得切。
        m_activeCurve = 0;
        std::vector<wm::app::StructureTimelineView::Curve> curves;
        curves.reserve(timeline.curves.size());
        for (auto const& c : timeline.curves)
        {
            curves.push_back(wm::app::StructureTimelineView::Curve{ c.id, c.times, c.values, c.valid });
        }
        for (std::size_t i = 0; i < curves.size(); ++i)
        {
            if (curves[i].id == L"level_db")
            {
                m_activeCurve = i;
                break;
            }
        }
        if (!curves.empty())
        {
            m_timeline.SetCurves(timeline.duration, curves, segs);
        }
        else
        {
            m_timeline.SetData(timeline.duration, timeline.curveTimes, timeline.curve, segs);
        }
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
        if (!timeline.curves.empty())
        {
            footer += L" 曲线上方小字可切换：电平 / 起音率 / 低频比例 / 亮度 / 谱变化，每个尺度按自身范围显示（真实单位在下一行）。";
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

        // 引擎描述层：确定性模板生成的制作描述（纹理/空间/律动/轮廓）。
        AnalysisProfileText().Text(hstring{ timeline.profileText });

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

    void NowPlayingPage::BuildCurveChips(wm::app::TrackTimeline const& timeline)
    {
        auto panel = CurveChips().Children();
        panel.Clear();
        if (timeline.curves.empty())
        {
            CurveUnitText().Text(hstring{});
            return;
        }
        for (std::size_t i = 0; i < timeline.curves.size(); ++i)
        {
            auto const& c = timeline.curves[i];
            Button chip;
            chip.Content(box_value(hstring{ CurveLabel(c.id) }));
            chip.FontSize(11);
            chip.Padding(Thickness{ 8, 2, 8, 3 });
            chip.CornerRadius(winrt::Microsoft::UI::Xaml::CornerRadiusHelper::FromUniformRadius(8));
            chip.Tag(box_value(static_cast<int32_t>(i)));
            // 悬停即见这条曲线测的是什么、什么单位：切换不糊里糊涂。
            winrt::Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(
                chip, box_value(hstring{ c.meaning + L"（" + c.unit + L"）" }));
            chip.Click({ this, &NowPlayingPage::OnCurveChipClicked });
            panel.Append(chip);
        }
        HighlightCurveChip();
        UpdateCurveUnitText();
    }

    void NowPlayingPage::HighlightCurveChip()
    {
        auto const children = CurveChips().Children();
        winrt::Windows::UI::Color const accent = BrushColor(
            L"WmAccentBrush", winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x31, 0xC2, 0x7C));
        for (std::uint32_t i = 0; i < children.Size(); ++i)
        {
            if (auto chip = children.GetAt(i).try_as<Button>())
            {
                bool const active = static_cast<std::size_t>(i) == m_activeCurve;
                chip.Foreground(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush{
                    active ? accent : winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x9A, 0x9A, 0x9A) });
                chip.Opacity(active ? 1.0 : 0.75);
            }
        }
    }

    void NowPlayingPage::UpdateCurveUnitText()
    {
        if (m_activeCurve >= m_painted.curves.size())
        {
            CurveUnitText().Text(hstring{});
            return;
        }
        auto const& c = m_painted.curves[m_activeCurve];
        std::wstring text = CurveLabel(c.id) + L"：" + c.meaning;
        if (!c.unit.empty())
        {
            text += L"（单位：" + c.unit + L"）";
        }
        CurveUnitText().Text(hstring{ text });
    }

    void NowPlayingPage::OnCurveChipClicked(Windows::Foundation::IInspectable const& sender,
                                            RoutedEventArgs const&)
    {
        auto const button = sender.try_as<Button>();
        if (button == nullptr)
        {
            return;
        }
        auto const index = unbox_value_or<int32_t>(button.Tag(), -1);
        if (index < 0 || static_cast<std::size_t>(index) >= m_painted.curves.size())
        {
            return;
        }
        m_activeCurve = static_cast<std::size_t>(index);
        m_timeline.SetActiveCurve(m_activeCurve);
        HighlightCurveChip();
        UpdateCurveUnitText();
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

    void NowPlayingPage::OnStructureAnalyzeClicked(Windows::Foundation::IInspectable const&,
                                                   RoutedEventArgs const&)
    {
        AnalyzeForStructureAsync();
    }

    winrt::fire_and_forget NowPlayingPage::AnalyzeForStructureAsync()
    {
        auto lifetime = get_strong();
        // 结构视图只关心正在播的这一首：引擎按文件分析，先把当前这首给扫了，
        // 整个曲库的逐首分析留在「个性推荐」页的「分析曲库」里。
        auto track = wm::app::Player().CurrentTrack();
        std::wstring const path = track ? std::wstring{ track.FilePath() } : std::wstring{};
        if (path.empty())
        {
            StructureNoteText().Text(hstring{ L"当前没有在播本地文件，结构分析只支持本地曲目。" });
            co_return;
        }

        StructureAnalyzeBtn().IsEnabled(false);
        StructureStateText().Text(hstring{ L"分析中…" });
        StructureNoteText().Text(hstring{ L"正在启动本地引擎（首次要拉起 Python 进程，可能要等十几秒）…" });

        hstring error = co_await wm::app::Recommend().AnalyzeTrackAsync(path);
        co_await wm::app::ResumeOnUi();

        StructureAnalyzeBtn().IsEnabled(true);
        if (error.empty() && !m_timelinePending)
        {
            // 分析成了：丢掉这首歌可能留着的「没有数据」会话备忘，重新走一遍
            // SyncTimeline，曲线这次就能出来。
            wm::app::Recommend().ForgetTimeline(path);
            m_timelinePath.clear();
            SyncTimeline(path);
        }
        if (!m_timelinePending)
        {
            StructureStateText().Text(hstring{});
            if (!error.empty())
            {
                StructureNoteText().Text(error);
            }
        }
    }
}
