#include "pch.h"

#include "ViewModels/PlayerViewModel.h"
#include "ViewModels/PlayerViewModel.g.cpp"

#include "Audio/EqualizedSource.h"
#include "Models/LyricLineItem.h"
#include "Models/RecommendItem.h"
#include "Models/TrackItem.h"
#include "Services/AppPaths.h"
#include "Services/DiscoverSettings.h"
#include "Services/LibraryService.h"
#include "Services/OnlineProviderService.h"
#include "Services/RecommendService.h"
#include "Services/Services.h"

#include <wm/core/OnlineSources.h>

#include <ctime>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Media::Core;
using namespace Windows::Media::Playback;
using namespace Windows::Storage;
using namespace Microsoft::UI::Dispatching;

namespace winrt::w_music::implementation
{
    namespace
    {
        wm::core::PlayMode ToCoreMode(winrt::w_music::PlayMode mode)
        {
            switch (mode)
            {
                case winrt::w_music::PlayMode::Sequential: return wm::core::PlayMode::Sequential;
                case winrt::w_music::PlayMode::Shuffle: return wm::core::PlayMode::Shuffle;
                case winrt::w_music::PlayMode::RepeatOne: return wm::core::PlayMode::RepeatOne;
                default: return wm::core::PlayMode::LoopAll;
            }
        }

        bool IsRemotePath(std::wstring_view path)
        {
            return path.rfind(L"http://", 0) == 0 || path.rfind(L"https://", 0) == 0;
        }

        winrt::Windows::Foundation::TimeSpan ToTimeSpan(double seconds)
        {
            return std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
                std::chrono::duration<double>(seconds));
        }

        double ToSeconds(winrt::Windows::Foundation::TimeSpan const& value)
        {
            return std::chrono::duration<double>(value).count();
        }

        std::string IdOf(hstring const& value)
        {
            return wm::app::Utf8(std::wstring_view{ value.c_str(), value.size() });
        }

        /// 引擎推荐行 -> 可播放的 TrackItem（"rec:" 前缀 id，不在曲库索引里）。
        winrt::w_music::TrackItem MakeFlowTrack(winrt::w_music::RecommendItem const& row)
        {
            auto track = winrt::make<winrt::w_music::implementation::TrackItem>();
            track.Id(hstring{ L"rec:" } + row.TrackId());
            track.Title(row.Title());
            track.Artist(row.Artist());
            track.Album(row.Album());
            track.FilePath(row.FilePath());
            return track;
        }

        bool StartsWith(std::string const& text, char const* prefix)
        {
            std::size_t const n = std::strlen(prefix);
            return text.size() >= n && text.compare(0, n, prefix) == 0;
        }

        /// Awaiter that hops a fire_and_forget body back onto the UI thread
        /// (no-op when the coroutine already runs there).
        struct ResumeToUi
        {
            DispatcherQueue queue;

            bool await_ready() const noexcept { return queue == nullptr || queue.HasThreadAccess(); }

            void await_suspend(std::coroutine_handle<> handle) const
            {
                queue.TryEnqueue([handle] { handle.resume(); });
            }

            void await_resume() const noexcept {}
        };
    } // namespace

    PlayerViewModel::PlayerViewModel()
    {
        m_lyrics = winrt::single_threaded_observable_vector<winrt::w_music::LyricLineItem>();
        m_flowUpNext = winrt::single_threaded_observable_vector<winrt::w_music::RecommendItem>();
        m_flowWindow = FlowWindowLabel();

        wm::core::SpectrumConfig config;
        config.fftSize = 2048;
        config.barCount = 28;
        config.sampleRate = 48000;
        m_spectrum.Configure(config);
    }

    PlayerViewModel::~PlayerViewModel()
    {
        StopSpectrum();
        if (m_timer != nullptr)
        {
            m_timer.Stop();
        }
    }

    void PlayerViewModel::RaisePropertyChanged(std::wstring_view const& name)
    {
        if (!wm::app::UiThread())
        {
            wm::app::Diag("PV Raise off-ui: " + wm::app::Utf8(name));
            wm::app::PostToUi([weak = get_weak(), text = std::wstring{ name }] {
                if (auto self = weak.get())
                {
                    self->RaisePropertyChanged(text);
                }
            });
            return;
        }
        m_propertyChanged(*this, winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs{ hstring{ name } });
    }

    winrt::event_token PlayerViewModel::PropertyChanged(winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler)
    {
        return m_propertyChanged.add(handler);
    }

    // ---------------------------------------------------------------- player

    void PlayerViewModel::Initialize(DispatcherQueue const& queue)
    {
        m_dispatcher = queue;
        if (m_timer != nullptr)
        {
            return;
        }

        m_timer = queue.CreateTimer();
        m_timer.Interval(std::chrono::milliseconds{ 66 });
        m_timer.IsRepeating(true);
        m_timer.Tick([weak = get_weak()](auto&&, auto&&) {
            if (auto self = weak.get())
            {
                self->OnTick();
            }
        });
        m_timer.Start();
    }

    void PlayerViewModel::EnsurePlayer()
    {
        if (m_player != nullptr)
        {
            return;
        }

        if (m_dispatcher == nullptr)
        {
            m_dispatcher = DispatcherQueue::GetForCurrentThread();
        }

        m_player = MediaPlayer();
        m_player.AudioCategory(MediaPlayerAudioCategory::Media);
        m_player.Volume(m_volume);
        m_player.IsMuted(m_isMuted);

        m_mediaEndedToken = m_player.MediaEnded([weak = get_weak(), dispatcher = m_dispatcher](MediaPlayer const&, IInspectable const&) {
            // MediaPlayer raises events on an arbitrary thread; XAML property
            // updates are only legal on the UI thread.
            try
            {
                auto self = weak.get();
                if (!self)
                {
                    return;
                }
                if (dispatcher == nullptr || dispatcher.HasThreadAccess())
                {
                    self->HandleMediaEnded();
                    return;
                }
                dispatcher.TryEnqueue([self = std::move(self)]() { self->HandleMediaEnded(); });
            }
            catch (...)
            {
            }
        });

        m_player.PlaybackSession().PlaybackStateChanged([weak = get_weak(), dispatcher = m_dispatcher](MediaPlaybackSession const&, auto&&) {
            try
            {
                auto self = weak.get();
                if (!self)
                {
                    return;
                }
                if (dispatcher == nullptr || dispatcher.HasThreadAccess())
                {
                    self->SyncPlayingState();
                    return;
                }
                dispatcher.TryEnqueue([self = std::move(self)]() { self->SyncPlayingState(); });
            }
            catch (...)
            {
            }
        });
    }

    void PlayerViewModel::HandleMediaEnded()
    {
        wm::app::Diag("media ended");
        try
        {
            if (m_mode == winrt::w_music::PlayMode::Radio)
            {
                // 推荐流自然续播：同一套出队路径（缓冲空时内部降级为队列）。
                FlowNext(true);
                return;
            }
            FallbackQueueNext(true);
        }
        catch (...)
        {
        }
    }

    void PlayerViewModel::SyncPlayingState()
    {
        wm::app::Diag("state sync");
        try
        {
            if (m_player == nullptr)
            {
                return;
            }
            const bool playing = m_player.PlaybackSession().PlaybackState() == MediaPlaybackState::Playing;
            if (m_isPlaying != playing)
            {
                m_isPlaying = playing;
                RaisePropertyChanged(L"IsPlaying");
            }
        }
        catch (...)
        {
        }
    }

    void PlayerViewModel::TogglePlayPause()
    {
        EnsurePlayer();

        if (m_player.PlaybackSession().PlaybackState() == MediaPlaybackState::Playing)
        {
            m_player.Pause();
        }
        else
        {
            if (m_player.Source() == nullptr && m_currentTrack != nullptr)
            {
                StartPlaybackAsync(m_currentTrack);
                return;
            }
            m_player.Play();
        }
    }

    void PlayerViewModel::FallbackQueueNext(bool autoAdvance)
    {
        const auto next = m_queue.Next(autoAdvance);
        if (next.has_value())
        {
            PlayTrackById(hstring{ wm::app::Utf16(*next) });
            return;
        }
        if (autoAdvance)
        {
            m_isPlaying = false;
            RaisePropertyChanged(L"IsPlaying");
        }
    }

    void PlayerViewModel::Next()
    {
        if (m_mode == winrt::w_music::PlayMode::Radio)
        {
            // 推荐流：下一曲就是从预取缓冲里出队（同步、零等待）。
            FlowNext(false);
            return;
        }
        FallbackQueueNext(false);
    }

    void PlayerViewModel::Previous()
    {
        const auto previous = m_queue.Previous();
        if (previous.has_value())
        {
            PlayTrackById(hstring{ wm::app::Utf16(*previous) });
        }
    }

    // ------------------------------------------------------------------ flow
    // 推荐流（抖音式）与旧电台的本质区别：「问引擎」不在切歌路径上。
    // 切歌 = 预取缓冲同步出队，零等待；引擎的全部工作（/v1/feed/next、
    // 冷启动、特征提取）都在后台补货协程里完成，且磁盘缓存里的上一份
    // feed 先行垫底 —— 缓冲空时才降级为顺序队列，并诚实标注。

    hstring PlayerViewModel::FlowWindowLabel()
    {
        std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        const int hour = local.tm_hour;
        if (hour >= 5 && hour < 8) return hstring{ L"清晨" };
        if (hour >= 8 && hour < 11) return hstring{ L"上午" };
        if (hour >= 11 && hour < 14) return hstring{ L"午间" };
        if (hour >= 14 && hour < 18) return hstring{ L"下午" };
        if (hour >= 18 && hour < 21) return hstring{ L"傍晚" };
        if (hour >= 21) return hstring{ L"夜晚" };
        return hstring{ L"深夜" }; // 0-5 点
    }

    void PlayerViewModel::SyncFlowWindow()
    {
        const hstring window = FlowWindowLabel();
        if (window == m_flowWindow)
        {
            return;
        }
        m_flowWindow = window;
        // 新窗口要一条新流；旧窗口的余量撑到新流就绪（绝不空窗）。
        m_flowReplacePending = true;
        ShowFlowStatus(hstring{ L"已进入「" + window + L"」时间窗，推荐流正在重新生成" });
    }

    void PlayerViewModel::FlowNext(bool autoAdvance)
    {
        // 出队/入队/PlayTrack 全是 UI 亲和操作：万一有后台路径走进来，
        // 取证 + 重新投递，不裸跑（与 RefreshFlowUi 同一套护栏）。
        if (!wm::app::UiThread())
        {
            wm::app::Diag("PV flow next OFF-THREAD");
            wm::app::PostToUi([weak = get_weak(), autoAdvance] {
                if (auto self = weak.get())
                {
                    self->FlowNext(autoAdvance);
                }
            });
            return;
        }
        SyncFlowWindow();

        if (!m_flowBuffer.empty())
        {
            auto const row = m_flowBuffer.front();
            m_flowBuffer.pop_front();

            // 追加进队列再播：上一曲/下一曲按键在推荐流里也能自然回退/前进。
            auto track = MakeFlowTrack(row);
            m_queue.Append({ IdOf(track.Id()) });
            wm::app::Diag("flow -> buffered next: " + IdOf(track.Id()));
            PlayTrack(track);
            // 播放事件喂给引擎的口味画像（时间衰减权重用它）。
            wm::app::Recommend().SendFeedbackAsync(row.TrackId(), hstring{ L"play" });
            RefreshFlowUi();
            EnsureFlowBufferAsync(false);
            return;
        }

        // 缓冲空（冷启动 / 曲库太小 / 引擎没就绪）：顺序顶上，绝不阻塞。
        wm::app::Diag("flow -> buffer empty, queue fallback");
        ShowFlowStatus(hstring{ L"推荐流还没就绪，先按顺序播放" });
        FallbackQueueNext(autoAdvance);
        RefreshFlowUi();
        EnsureFlowBufferAsync(false);
    }

    void PlayerViewModel::RememberRejectedPath(std::string path)
    {
        if (path.empty())
        {
            return;
        }
        m_rejectedPaths.push_back(std::move(path));
        while (m_rejectedPaths.size() > kRejectedMax)
        {
            m_rejectedPaths.pop_front();
        }
    }

    bool PlayerViewModel::MergeFlowRows(
        winrt::Windows::Foundation::Collections::IVectorView<winrt::w_music::RecommendItem> const& rows,
        bool replace)
    {
        if (rows == nullptr || rows.Size() == 0)
        {
            return false; // replace 失败时保留旧余量：绝不空窗
        }

        std::wstring currentPath;
        if (m_currentTrack != nullptr && !IsRemotePath(std::wstring_view{ m_currentTrack.FilePath().c_str() }))
        {
            currentPath.assign(m_currentTrack.FilePath().c_str(), m_currentTrack.FilePath().size());
        }

        auto const seen = [this](std::wstring const& path) {
            std::string const utf8 = wm::app::Utf8(path);
            if (std::find(m_recentPaths.begin(), m_recentPaths.end(), utf8) != m_recentPaths.end())
            {
                return true;
            }
            return std::find(m_rejectedPaths.begin(), m_rejectedPaths.end(), utf8) != m_rejectedPaths.end();
        };

        std::vector<winrt::w_music::RecommendItem> fresh;
        fresh.reserve(rows.Size());
        for (std::uint32_t i = 0; i < rows.Size() && fresh.size() < kFlowBufferMax; ++i)
        {
            auto row = rows.GetAt(i);
            if (row == nullptr || row.FilePath().empty())
            {
                continue;
            }
            std::wstring const path{ row.FilePath().c_str() };
            if (!currentPath.empty() && path == currentPath)
            {
                continue; // 正在播的这首不进预告
            }
            if (seen(path))
            {
                continue; // 最近听过的 / 本会话跳过或差评过的
            }
            bool dup = false;
            for (auto const& existing : fresh)
            {
                if (std::wstring{ existing.FilePath().c_str() } == path)
                {
                    dup = true;
                    break;
                }
            }
            if (!dup && !replace)
            {
                for (auto const& existing : m_flowBuffer)
                {
                    if (std::wstring{ existing.FilePath().c_str() } == path)
                    {
                        dup = true;
                        break;
                    }
                }
            }
            if (dup)
            {
                continue;
            }
            fresh.push_back(row);
        }

        if (fresh.empty())
        {
            return false;
        }
        if (replace)
        {
            m_flowBuffer.assign(fresh.begin(), fresh.end());
        }
        else
        {
            m_flowBuffer.insert(m_flowBuffer.end(), fresh.begin(), fresh.end());
            while (m_flowBuffer.size() > kFlowBufferMax)
            {
                m_flowBuffer.pop_back();
            }
        }
        return true;
    }

    winrt::fire_and_forget PlayerViewModel::EnsureFlowBufferAsync(bool replace)
    {
        auto lifetime = get_strong();
        // 整个补货体只碰 UI 亲和状态（缓冲/卡片/缓冲互斥量）：先把线程归一
        // 到 UI，后续全部段落无需再各自担心调用方从哪来。
        co_await ResumeToUi{ m_dispatcher };
        if (replace)
        {
            m_flowReplacePending = true;
        }
        if (m_flowRefillBusy)
        {
            co_return; // 已有一轮在途：结束后会按需续跑
        }
        if (!m_flowReplacePending && m_flowBuffer.size() >= kFlowBufferTarget)
        {
            co_return; // 够用就不动：缓冲是「下一首」，不是整个曲库
        }
        m_flowRefillBusy = true;

        // 离线种子：磁盘缓存里的上一份 feed（曲库指纹一致才可用）。引擎
        // 冷启动要几秒到几分钟，这段时间「下一曲」也必须有货 —— 这一步
        // 纯读内存里的缓存文件，不同步任何网络。
        if (!m_flowCacheSeeded)
        {
            m_flowCacheSeeded = true;
            auto cached = wm::app::Recommend().CachedRows(
                wm::app::RecommendService::Answer::Feed, hstring{},
                static_cast<int32_t>(kFlowCandidates));
            MergeFlowRows(cached, false);
            RefreshFlowUi();
        }

        auto& engine = wm::app::Recommend();
        if (!engine.IsReady() || engine.CurrentStage() != wm::app::RecommendService::Stage::Ready)
        {
            // 从没预热过（没用过个性推荐页却直接听推荐流的人）就兜底 kick
            // 一次：纯后台启动，这里绝不等待。
            if (!m_flowPrewarmKicked)
            {
                m_flowPrewarmKicked = true;
                wm::app::Diag("flow -> kick engine prewarm");
                engine.PrewarmAsync();
            }
            m_flowRefillBusy = false;
            RefreshFlowUi(); // 说明文案反映「预热中」
            co_return;
        }

        // 排除列表：缓冲里已有的引擎曲目，避免引擎反复端上同一批。
        std::vector<hstring> excludes;
        excludes.reserve(m_flowBuffer.size());
        for (auto const& row : m_flowBuffer)
        {
            if (!row.TrackId().empty())
            {
                excludes.push_back(row.TrackId());
            }
        }

        const hstring windowAtRequest = m_flowWindow;
        IVectorView<winrt::w_music::RecommendItem> rows{ nullptr };
        try
        {
            rows = co_await engine.GetFeedAsync(static_cast<int32_t>(kFlowCandidates), std::move(excludes));
        }
        catch (...)
        {
            rows = nullptr;
        }
        co_await ResumeToUi{ m_dispatcher };
        m_flowRefillBusy = false;

        // 在途期间跨了时间窗：这份是旧窗口的答案。已挂整流重算就丢弃，
        // 立刻按新窗口重取；否则照样并入（推荐仍是可用的）。
        bool changed = false;
        if (windowAtRequest == m_flowWindow)
        {
            const bool replaceNow = m_flowReplacePending;
            m_flowReplacePending = false;
            changed = MergeFlowRows(rows, replaceNow);
        }
        RefreshFlowUi();

        // 变化过但还不够填（被最近播放过滤掉太多）就接着补；没变化说明
        // 引擎端上来的都被过滤了，再问也是同一批，停住等下一次切歌。
        if (changed && (m_flowReplacePending || m_flowBuffer.size() < kFlowBufferTarget))
        {
            EnsureFlowBufferAsync(false);
        }
    }

    void PlayerViewModel::PlayFlowUpNext(int32_t index)
    {
        if (!IsFlowMode() || index < 0 || static_cast<std::size_t>(index) >= m_flowBuffer.size())
        {
            return;
        }
        auto const row = m_flowBuffer[static_cast<std::size_t>(index)];
        // 点播第 index 行：它前面的预告一并出队（用户明确不听）。
        m_flowBuffer.erase(m_flowBuffer.begin(), m_flowBuffer.begin() + index + 1);

        auto track = MakeFlowTrack(row);
        m_queue.Append({ IdOf(track.Id()) });
        PlayTrack(track);
        wm::app::Recommend().SendFeedbackAsync(row.TrackId(), hstring{ L"play" });
        RefreshFlowUi();
        EnsureFlowBufferAsync(false);
    }

    void PlayerViewModel::SkipFlowSlot()
    {
        if (!IsFlowMode())
        {
            return;
        }
        if (!m_flowBuffer.empty())
        {
            RememberRejectedPath(wm::app::Utf8(m_flowBuffer.front().FilePath()));
            m_flowBuffer.pop_front();
            ShowFlowStatus(hstring{ L"已跳过这首预告，换下一首" });
        }
        RefreshFlowUi();
        EnsureFlowBufferAsync(false);
    }

    void PlayerViewModel::NotifyLibraryReady()
    {
        // 曲库（重新）加载 = 缓存指纹可能换了：离线种子值得重试一次。
        m_flowCacheSeeded = false;
        EnsureFlowBufferAsync(false);
    }

    bool PlayerViewModel::IsFlowMode() const noexcept
    {
        return m_mode == winrt::w_music::PlayMode::Radio;
    }

    winrt::Microsoft::UI::Xaml::Visibility PlayerViewModel::FlowRefreshVisibility() const noexcept
    {
        return IsFlowMode() ? winrt::Microsoft::UI::Xaml::Visibility::Visible
                            : winrt::Microsoft::UI::Xaml::Visibility::Collapsed;
    }

    void PlayerViewModel::RefreshFlowUi()
    {
        // 「接下来」卡片动的是非 agile 的 observable vector（线程亲和）：
        // 任何路径漏回 UI 线程都会 8001010e → 0xC000027B（0.1.21/0.1.24 各崩过
        // 一次）。这里统一设卡：不在 UI 线程就取证并重新投递，绝不再裸跑。
        if (!wm::app::UiThread())
        {
            wm::app::Diag("PV flow ui OFF-THREAD");
            wm::app::PostToUi([weak = get_weak()] {
                if (auto self = weak.get())
                {
                    self->RefreshFlowUi();
                }
            });
            return;
        }

        if (m_flowUpNext == nullptr)
        {
            m_flowUpNext = winrt::single_threaded_observable_vector<winrt::w_music::RecommendItem>();
        }

        const bool flow = IsFlowMode();
        m_flowHeaderText = flow
            ? hstring{ L"接下来 · 推荐流（" + m_flowWindow + L"）" }
            : hstring{ L"接下来 · " + ModeText() };

        // 就地重建行列表：observable vector 的 VectorChanged 自己驱动
        // ListView，整卡不重绑。
        m_flowUpNext.Clear();
        if (flow)
        {
            const std::size_t shown = std::min<std::size_t>(m_flowBuffer.size(), 3);
            for (std::size_t i = 0; i < shown; ++i)
            {
                m_flowUpNext.Append(m_flowBuffer[i]);
            }
        }
        else if (auto nextId = m_queue.PeekNext(false))
        {
            // 顺序类模式：非破坏性预览队列的下一首，明确标「顺序」。
            winrt::w_music::TrackItem track{ nullptr };
            if (auto libraryTrack = wm::app::Library().FindTrack(hstring{ wm::app::Utf16(*nextId) }))
            {
                track = libraryTrack;
            }
            else if (const auto it = m_sideTracks.find(*nextId); it != m_sideTracks.end())
            {
                track = it->second;
            }
            if (track != nullptr)
            {
                auto row = winrt::make<winrt::w_music::implementation::RecommendItem>();
                row.TrackId(track.Id());
                row.Title(track.Title());
                row.Artist(track.Artist());
                row.Album(track.Album());
                row.FilePath(track.FilePath());
                row.ScoreText(hstring{ L"顺序" });
                m_flowUpNext.Append(row);
            }
        }

        if (flow)
        {
            if (!m_flowBuffer.empty())
            {
                m_flowStreamText = hstring{ L"已预载 " + std::to_wstring(m_flowBuffer.size())
                    + L" 首，点「下一曲」立即播放（本地引擎后台补货）" };
            }
            else
            {
                auto& engine = wm::app::Recommend();
                m_flowStreamText = !engine.IsReady() ||
                                           engine.CurrentStage() != wm::app::RecommendService::Stage::Ready
                    ? hstring{ L"本地推荐引擎预热中：先用顺序播放，就绪后自动回到推荐流" }
                    : hstring{ L"推荐流暂时没有新歌：先按顺序播放，稍后自动补上" };
            }
        }
        else
        {
            switch (m_mode)
            {
                case winrt::w_music::PlayMode::Shuffle:
                    m_flowStreamText = hstring{ L"随机播放不预告下一首" };
                    break;
                case winrt::w_music::PlayMode::RepeatOne:
                    m_flowStreamText = hstring{ L"单曲循环中：手动点「下一曲」才会切歌" };
                    break;
                case winrt::w_music::PlayMode::Sequential:
                    m_flowStreamText = hstring{ L"按文件顺序切歌（传统模式），到列表末尾就停" };
                    break;
                default:
                    m_flowStreamText = hstring{ L"按文件顺序循环切歌（传统模式）" };
                    break;
            }
        }

        RaisePropertyChanged(L"FlowHeaderText");
        RaisePropertyChanged(L"FlowStreamText");
        RaisePropertyChanged(L"IsFlowMode");
        RaisePropertyChanged(L"FlowRefreshVisibility");
    }

    void PlayerViewModel::LikeCurrent()
    {
        auto track = m_currentTrack;
        if (track == nullptr)
        {
            return;
        }
        SendCurrentFeedback(track, L"like");
        ShowFlowStatus(hstring{ L"已点赞，这类听感会加权" });
    }

    void PlayerViewModel::DislikeCurrent()
    {
        auto track = m_currentTrack;
        if (track == nullptr)
        {
            return;
        }
        SendCurrentFeedback(track, L"dislike");
        // 差评：从当前播放队列去掉、清掉缓冲里同曲的预告，立即流向下一首。
        m_queue.RemoveAll(IdOf(track.Id()));
        std::wstring const path{ track.FilePath().c_str() };
        for (auto it = m_flowBuffer.begin(); it != m_flowBuffer.end();)
        {
            if (std::wstring{ it->FilePath().c_str() } == path)
            {
                it = m_flowBuffer.erase(it);
            }
            else
            {
                ++it;
            }
        }
        if (!IsRemotePath(std::wstring_view{ track.FilePath().c_str() }))
        {
            RememberRejectedPath(wm::app::Utf8(path));
        }
        ShowFlowStatus(hstring{ L"已减少这类歌曲，换一首" });
        if (IsFlowMode())
        {
            FlowNext(false);
        }
        else
        {
            FallbackQueueNext(false);
        }
        RefreshFlowUi();
    }

    void PlayerViewModel::SendCurrentFeedback(winrt::w_music::TrackItem const& track, wchar_t const* event)
    {
        auto& engine = wm::app::Recommend();
        if (!engine.IsReady())
        {
            return;
        }
        const std::string id = IdOf(track.Id());
        if (StartsWith(id, "rec:"))
        {
            engine.SendFeedbackAsync(hstring{ wm::app::Utf16(id.substr(4)) }, hstring{ event });
        }
        else
        {
            // 曲库曲没有引擎的内容 hash，让引擎按文件路径自己解析。
            engine.SendFeedbackForPathAsync(track.FilePath(), hstring{ event });
        }
    }

    void PlayerViewModel::ShowFlowStatus(hstring text)
    {
        m_flowStatusText = std::move(text);
        RaisePropertyChanged(L"FlowStatusText");
        if (m_flowStatusTimer == nullptr && m_dispatcher != nullptr)
        {
            m_flowStatusTimer = m_dispatcher.CreateTimer();
            m_flowStatusTimer.Interval(std::chrono::milliseconds{ 2600 });
            m_flowStatusTimer.IsRepeating(false);
            m_flowStatusTimer.Tick([weak = get_weak()](auto&&, auto&&) {
                if (auto self = weak.get())
                {
                    self->m_flowStatusText = hstring{};
                    self->RaisePropertyChanged(L"FlowStatusText");
                }
            });
        }
        if (m_flowStatusTimer != nullptr)
        {
            m_flowStatusTimer.Start();
        }
    }

    void PlayerViewModel::RememberSideTrack(winrt::w_music::TrackItem const& track)
    {
        if (track == nullptr || wm::app::Library().FindTrack(track.Id()) != nullptr)
        {
            return; // 曲库曲走 Library().FindTrack，不需要旁路
        }
        // insert_or_assign, not operator[]: subscripting a missing key
        // default-constructs the mapped projected TrackItem, and C++/WinRT
        // builds one by *activating* w_music.TrackItem -- an unpackaged process
        // cannot resolve that, so it throws 0x80040154 (没有注册类). Only "rec:"
        // ids reach this line (library ids early-return above), which is why
        // every recommendation-list click crashed while library plays never did.
        m_sideTracks.insert_or_assign(IdOf(track.Id()), track);
    }

    void PlayerViewModel::Seek(double seconds)
    {
        if (m_player == nullptr || m_player.PlaybackSession() == nullptr)
        {
            return;
        }
        m_player.PlaybackSession().Position(ToTimeSpan(seconds));
        m_positionSeconds = seconds;
        RaisePropertyChanged(L"PositionSeconds");
        RaisePropertyChanged(L"PositionText");
    }

    void PlayerViewModel::SetQueue(IVector<winrt::w_music::TrackItem> const& tracks, int32_t startIndex)
    {
        std::vector<std::string> ids;
        ids.reserve(tracks.Size());
        for (std::uint32_t i = 0; i < tracks.Size(); ++i)
        {
            auto const& item = tracks.GetAt(i);
            ids.push_back(IdOf(item.Id()));
            RememberSideTrack(item);
        }
        m_queue.SetTracks(std::move(ids), startIndex);
        // 「接下来」卡片的队列预览要跟着新队列走。
        RefreshFlowUi();
    }

    void PlayerViewModel::PlayTrack(winrt::w_music::TrackItem const& track)
    {
        if (track == nullptr)
        {
            return;
        }
        RememberSideTrack(track);
        m_queue.JumpToId(IdOf(track.Id()));
        StartPlaybackAsync(track);
    }

    void PlayerViewModel::PlayTrackById(hstring const& trackId)
    {
        if (auto track = wm::app::Library().FindTrack(trackId))
        {
            StartPlaybackAsync(track);
            return;
        }
        // rec: 行（引擎建议）不在曲库索引里，从旁路表取回对象。
        // 修掉的老 bug：推荐列表放到歌尾时 Next/MediaEnded 解析不出 id，
        // 播放会直接停在每首的结尾。
        if (const auto it = m_sideTracks.find(IdOf(trackId)); it != m_sideTracks.end())
        {
            StartPlaybackAsync(it->second);
        }
    }

    winrt::fire_and_forget PlayerViewModel::StartPlaybackAsync(winrt::w_music::TrackItem track)
    {
        auto lifetime = get_strong();
        // Callers may arrive from background completion callbacks (downloads,
        // media events); all state below is XAML-bound and UI-only.
        wm::app::Diag("play enter");
        co_await ResumeToUi{ m_dispatcher };
        try
        {
            EnsurePlayer();

            if (m_currentTrack != nullptr)
            {
                m_currentTrack.IsPlaying(false);
            }

            m_currentTrack = track;
            m_positionSeconds = 0.0;
            m_durationSeconds = 0.0;
            track.IsPlaying(true);

            // 推荐流微调窗口：记住最近 30 首的文件路径（在线流不进窗口）。
            if (!IsRemotePath(std::wstring_view{ track.FilePath() }))
            {
                m_recentPaths.push_back(wm::app::Utf8(track.FilePath()));
                while (m_recentPaths.size() > kRecentWindow)
                {
                    m_recentPaths.pop_front();
                }
                // 手动点播的曲子可能正躺在预取缓冲里当预告：把它清出去，
                // 否则流模式下「下一曲」会原曲重播。
                std::wstring const path{ track.FilePath().c_str() };
                const auto bufferBefore = m_flowBuffer.size();
                for (auto it = m_flowBuffer.begin(); it != m_flowBuffer.end();)
                {
                    if (std::wstring{ it->FilePath().c_str() } == path)
                    {
                        it = m_flowBuffer.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
                m_flowPurgedCurrent = m_flowBuffer.size() != bufferBefore;
            }
            RememberSideTrack(track);

            RaisePropertyChanged(L"CurrentTrack");
            RaisePropertyChanged(L"HasTrack");
            RaisePropertyChanged(L"Title");
            RaisePropertyChanged(L"Artist");
            RaisePropertyChanged(L"Album");
            RaisePropertyChanged(L"IsFavorite");
            RaisePropertyChanged(L"PositionSeconds");
            RaisePropertyChanged(L"PositionText");
            RaisePropertyChanged(L"DurationSeconds");
            RaisePropertyChanged(L"DurationText");
            // 队列预览随当前曲变化；推荐流的预告与当前曲无关（缓冲没变）
            // 就不重建，免得预告行每次切歌都闪一下。当前曲占用的预告位
            // 被清掉时还是要刷。
            if (!IsFlowMode() || m_flowPurgedCurrent)
            {
                RefreshFlowUi();
            }

            std::wstring path{ track.FilePath().c_str() };
            const bool remote = path.rfind(L"http://", 0) == 0 || path.rfind(L"https://", 0) == 0;

            // EQ active: decode -> filter -> feed back through a MediaStreamSource
            // proxy. Opening blocks (network!), so hop off the UI thread first; on
            // any failure we simply continue with the plain source below.
            bool eqBound = false;
            if (m_eqEnabled)
            {
                auto eq = std::make_shared<wm::app::EqualizedSource>(m_equalizer, std::move(path));
                co_await winrt::resume_background();
                bool opened = false;
                try
                {
                    opened = eq->Open();
                }
                catch (...)
                {
                    opened = false;
                }
                co_await ResumeToUi{ m_dispatcher };
                if (opened)
                {
                    try
                    {
                        m_eqSource = std::move(eq);
                        m_player.Source(MediaSource::CreateFromMediaStreamSource(m_eqSource->Source()));
                        m_player.Play();
                        eqBound = true;
                        wm::app::Diag("play eq source set");
                    }
                    catch (...)
                    {
                        eqBound = false;
                        m_eqSource.reset();
                    }
                }
            }

            if (!eqBound)
            {
                m_eqSource.reset();
                try
                {
                    if (remote)
                    {
                        // Streamed straight from an online source (see OnlineProviderService).
                        m_player.Source(MediaSource::CreateFromUri(winrt::Windows::Foundation::Uri{ track.FilePath() }));
                        m_player.Play();
                    }
                    else
                    {
                        auto file = co_await StorageFile::GetFileFromPathAsync(track.FilePath());
                        co_await ResumeToUi{ m_dispatcher };
                        m_player.Source(MediaSource::CreateFromStorageFile(file));
                        m_player.Play();
                    }
                    wm::app::Diag("play source set");
                }
                catch (...)
                {
                    // Unreadable / missing file: fall through so the next track can play.
                }
            }

            // Lyrics / play-count / pending-resume all touch UI-bound state.
            co_await ResumeToUi{ m_dispatcher };
            ApplyPendingResume();

            if (!remote)
            {
                co_await LoadLyricAsync(track.Id());
                co_await ResumeToUi{ m_dispatcher };
                // "rec:" rows are engine suggestions, not library entries:
                // MarkPlayed would write unresolvable ids into the recent-played
                // playlist inside library.json. Keep the recommend namespace out
                // of the library's stats entirely.
                if (!StartsWith(IdOf(track.Id()), "rec:"))
                {
                    wm::app::Library().MarkPlayed(track.Id());
                }
            }
            else
            {
                // QQ online previews have a resolvable mid baked into their id; pull
                // the LRC from QQ when logged in (or through the public endpoint) so
                // the now-playing lyrics highlight works without a local file.
                std::string const mid = QqMidFromTrack(track.Id());
                if (!mid.empty())
                {
                    LoadOnlineLyric(mid);
                }
            }
            wm::app::Diag("play done");
        }
        catch (...)
        {
            // Playback failures must never take the app down.
            wm::app::Diag("play exception");
        }
    }

    // --------------------------------------------------------------- getters

    hstring PlayerViewModel::Title() const
    {
        return m_currentTrack == nullptr ? hstring{ L"未在播放" } : m_currentTrack.Title();
    }

    hstring PlayerViewModel::Artist() const
    {
        return m_currentTrack == nullptr ? hstring{} : m_currentTrack.Artist();
    }

    hstring PlayerViewModel::Album() const
    {
        return m_currentTrack == nullptr ? hstring{} : m_currentTrack.Album();
    }

    hstring PlayerViewModel::PositionText() const
    {
        return hstring{ wm::app::FormatDuration(static_cast<long long>(m_positionSeconds * 1000.0)) };
    }

    hstring PlayerViewModel::DurationText() const
    {
        return m_currentTrack == nullptr
            ? hstring{ L"00:00" }
            : m_currentTrack.DurationText();
    }

    bool PlayerViewModel::IsFavorite() const noexcept
    {
        return m_currentTrack != nullptr && m_currentTrack.IsFavorite();
    }

    void PlayerViewModel::IsFavorite(bool value)
    {
        if (m_currentTrack == nullptr)
        {
            return;
        }
        auto& library = wm::app::Library();
        if (m_currentTrack.IsFavorite() == value)
        {
            return;
        }
        const bool actual = library.ToggleFavorite(m_currentTrack.Id());
        m_currentTrack.IsFavorite(actual);
        RaisePropertyChanged(L"IsFavorite");
    }

    void PlayerViewModel::ToggleFavorite()
    {
        if (m_currentTrack == nullptr)
        {
            return;
        }
        const bool favorite = wm::app::Library().ToggleFavorite(m_currentTrack.Id());
        m_currentTrack.IsFavorite(favorite);
        RaisePropertyChanged(L"IsFavorite");
    }

    void PlayerViewModel::SyncFavoriteState()
    {
        RaisePropertyChanged(L"IsFavorite");
    }

    void PlayerViewModel::PositionSeconds(double value)
    {
        Seek(value);
    }

    void PlayerViewModel::Volume(double value)
    {
        m_volume = std::clamp(value, 0.0, 1.0);
        if (m_player != nullptr)
        {
            m_player.Volume(m_volume);
        }
        RaisePropertyChanged(L"Volume");
    }

    void PlayerViewModel::IsMuted(bool value)
    {
        m_isMuted = value;
        if (m_player != nullptr)
        {
            m_player.IsMuted(value);
        }
        RaisePropertyChanged(L"IsMuted");
    }

    // -------------------------------------------------------------- equalizer

    void PlayerViewModel::ConfigureEqualizer(std::array<double, wm::core::Equalizer::BandCount> const& gainsDb,
                                             double preampDb)
    {
        m_equalizer.SetGains(gainsDb, preampDb);
    }

    void PlayerViewModel::SetEqualizerEnabled(bool enabled)
    {
        if (m_eqEnabled == enabled)
        {
            return;
        }
        m_eqEnabled = enabled;

        if (m_currentTrack == nullptr || m_player == nullptr)
        {
            return;
        }

        // Re-bind the source (proxy <-> direct) and resume where we left off.
        // A restart while paused must not start playback.
        m_pendingSeekSeconds = m_positionSeconds;
        m_pendingResumePaused = !m_isPlaying;
        StartPlaybackAsync(m_currentTrack);
    }

    void PlayerViewModel::ApplyPendingResume()
    {
        if (m_pendingSeekSeconds == 0.0 && !m_pendingResumePaused)
        {
            return;
        }
        const double seek = m_pendingSeekSeconds;
        const bool pause = m_pendingResumePaused;
        m_pendingSeekSeconds = 0.0;
        m_pendingResumePaused = false;

        try
        {
            if (seek > 0.0)
            {
                m_player.PlaybackSession().Position(ToTimeSpan(seek));
                m_positionSeconds = seek;
                RaisePropertyChanged(L"PositionSeconds");
                RaisePropertyChanged(L"PositionText");
            }
            if (pause)
            {
                m_player.Pause();
            }
        }
        catch (...)
        {
        }
    }

    void PlayerViewModel::Mode(winrt::w_music::PlayMode value)
    {
        if (m_mode == value)
        {
            return;
        }
        m_mode = value;
        m_coreMode = ToCoreMode(value);
        m_queue.SetMode(m_coreMode);
        if (IsFlowMode())
        {
            // 回到推荐流：对齐时间窗并先把缓冲填上（后台，不挡切歌）。
            SyncFlowWindow();
            EnsureFlowBufferAsync(false);
        }
        RefreshFlowUi();
        RaisePropertyChanged(L"Mode");
        RaisePropertyChanged(L"ModeText");
    }

    hstring PlayerViewModel::ModeText() const
    {
        switch (m_mode)
        {
            case winrt::w_music::PlayMode::Sequential: return hstring{ L"顺序播放" };
            case winrt::w_music::PlayMode::Shuffle: return hstring{ L"随机播放" };
            case winrt::w_music::PlayMode::RepeatOne: return hstring{ L"单曲循环" };
            case winrt::w_music::PlayMode::Radio: return hstring{ L"推荐流" };
            default: return hstring{ L"列表循环" };
        }
    }

    void PlayerViewModel::CycleMode()
    {
        const auto next = static_cast<int>(m_mode) + 1;
        Mode(static_cast<winrt::w_music::PlayMode>(next % 5));
    }

    // ---------------------------------------------------------------- lyrics

    IAsyncAction PlayerViewModel::LoadLyricAsync(hstring trackId)
    {
        const hstring text = co_await wm::app::Library().LoadLyricTextAsync(trackId);
        co_await ResumeToUi{ m_dispatcher };
        ApplyLyricText(wm::app::Utf8(std::wstring_view{ text.c_str(), text.size() }));
    }

    std::string PlayerViewModel::QqMidFromTrack(hstring const& trackId)
    {
        std::string const id = IdOf(trackId);
        constexpr char const* kPrefix = "online:qq:";
        if (!StartsWith(id, kPrefix))
        {
            return {};
        }
        std::string mid = id.substr(std::strlen(kPrefix));
        constexpr char const* kSuffix = ":preview";
        if (mid.size() >= std::strlen(kSuffix) &&
            mid.compare(mid.size() - std::strlen(kSuffix), std::strlen(kSuffix), kSuffix) == 0)
        {
            mid.erase(mid.size() - std::strlen(kSuffix));
        }
        return mid;
    }

    winrt::fire_and_forget PlayerViewModel::LoadOnlineLyric(std::string mid)
    {
        if (mid.empty())
        {
            co_return;
        }
        auto lifetime = get_strong();

        co_await winrt::resume_background();
        std::string lyric;
        try
        {
            wm::core::QqSource source{ wm::app::Online().Transport() };
            auto& settings = wm::app::Settings();
            if (settings.QqLoggedIn())
            {
                source.SetSession(
                    wm::app::Utf8(std::wstring_view{ settings.QqSessionCookie().c_str(), settings.QqSessionCookie().size() }),
                    wm::app::Utf8(std::wstring_view{ settings.QqUin().c_str(), settings.QqUin().size() }));
            }
            lyric = source.Lyric(mid);
        }
        catch (...)
        {
            // Lyrics are decorative: never let a fetch failure break playback.
            co_return;
        }

        if (lyric.empty())
        {
            co_return;
        }
        co_await ResumeToUi{ m_dispatcher };
        ApplyLyricText(lyric);
    }

    void PlayerViewModel::ApplyLyricText(std::string const& text)
    {
        m_lyrics.Clear();
        m_lyric = wm::core::LyricDocument{};
        m_activeLyricIndex = -1;

        if (!text.empty())
        {
            m_lyric = wm::core::LyricParser::Parse(text);

            if (m_lyric.valid)
            {
                int32_t index = 0;
                for (const auto& line : m_lyric.lines)
                {
                    auto item = winrt::make<winrt::w_music::implementation::LyricLineItem>();
                    item.TimeMs(line.timeMs);
                    item.Text(hstring{ wm::app::Utf16(line.text) });
                    item.Index(index);
                    item.IsActive(false);
                    m_lyrics.Append(item);
                    ++index;
                }
            }
        }

        RaisePropertyChanged(L"Lyrics");
        RaisePropertyChanged(L"HasLyrics");
        RaisePropertyChanged(L"LyricOffsetMs");
        RaisePropertyChanged(L"ActiveLyricIndex");
    }

    void PlayerViewModel::LyricOffsetMs(int64_t value)
    {
        if (m_lyric.offsetMs == value)
        {
            return;
        }
        m_lyric.offsetMs = value;
        m_activeLyricIndex = -1; // force the next tick to recompute
        RaisePropertyChanged(L"LyricOffsetMs");
    }

    void PlayerViewModel::AdjustLyricOffset(int64_t deltaMs)
    {
        LyricOffsetMs(m_lyric.offsetMs + deltaMs);
    }

    void PlayerViewModel::SeekToLyric(int32_t index)
    {
        if (!HasLyrics() || index < 0 || static_cast<std::size_t>(index) >= m_lyric.lines.size())
        {
            return;
        }
        Seek(static_cast<double>(m_lyric.EffectiveTime(static_cast<std::size_t>(index))) / 1000.0);
    }

    // -------------------------------------------------------------- spectrum

    std::size_t PlayerViewModel::AddSpectrumSink(std::function<void(std::vector<double> const&)> sink)
    {
        std::lock_guard<std::mutex> lock(m_spectrumMutex);
        const std::size_t token = m_nextSinkId++;
        m_spectrumSinks.emplace(token, std::move(sink));
        return token;
    }

    void PlayerViewModel::RemoveSpectrumSink(std::size_t token)
    {
        std::lock_guard<std::mutex> lock(m_spectrumMutex);
        m_spectrumSinks.erase(token);
    }

    void PlayerViewModel::StartSpectrum()
    {
        if (m_loopback.IsRunning())
        {
            return;
        }

        m_loopback.Start([this](const float* samples, std::size_t frames, int channels, std::uint32_t sampleRate) {
            auto config = m_spectrum.Config();
            if (config.sampleRate != sampleRate)
            {
                config.sampleRate = sampleRate;
                try
                {
                    m_spectrum.Configure(config);
                }
                catch (...)
                {
                    return;
                }
            }

            const auto bars = m_spectrum.Process(samples, frames, channels);
            std::lock_guard<std::mutex> lock(m_spectrumMutex);
            m_pendingBars = bars;
        });
    }

    void PlayerViewModel::StopSpectrum()
    {
        m_loopback.Stop();
    }

    void PlayerViewModel::PushSpectrumToUi()
    {
        std::vector<double> bars;
        {
            std::lock_guard<std::mutex> lock(m_spectrumMutex);
            if (m_pendingBars.empty() || m_spectrumSinks.empty())
            {
                return;
            }
            bars.swap(m_pendingBars);
            for (auto const& [token, sink] : m_spectrumSinks)
            {
                if (sink)
                {
                    sink(bars);
                }
            }
        }
    }

    void PlayerViewModel::OnTick()
    {
        try
        {
        PushSpectrumToUi();

        if (m_player == nullptr || m_player.PlaybackSession() == nullptr)
        {
            return;
        }

        const auto session = m_player.PlaybackSession();
        const double position = ToSeconds(session.Position());
        const double duration = ToSeconds(session.NaturalDuration());

        if (std::fabs(position - m_positionSeconds) > 0.05)
        {
            m_positionSeconds = position;
            RaisePropertyChanged(L"PositionSeconds");
            RaisePropertyChanged(L"PositionText");
        }
        if (std::fabs(duration - m_durationSeconds) > 0.05)
        {
            m_durationSeconds = duration;
            RaisePropertyChanged(L"DurationSeconds");
            RaisePropertyChanged(L"DurationText");
        }

        if (!HasLyrics() || m_lyrics.Size() == 0)
        {
            return;
        }

        const auto found = m_lyric.IndexAt(static_cast<std::int64_t>(position * 1000.0));
        const int32_t next = found < 0 ? -1 : static_cast<int32_t>(found);
        if (next == m_activeLyricIndex)
        {
            return;
        }

        if (m_activeLyricIndex >= 0 && static_cast<std::uint32_t>(m_activeLyricIndex) < m_lyrics.Size())
        {
            m_lyrics.GetAt(static_cast<std::uint32_t>(m_activeLyricIndex)).IsActive(false);
        }
        m_activeLyricIndex = next;
        if (next >= 0 && static_cast<std::uint32_t>(next) < m_lyrics.Size())
        {
            m_lyrics.GetAt(static_cast<std::uint32_t>(next)).IsActive(true);
        }
        RaisePropertyChanged(L"ActiveLyricIndex");
        }
        catch (...)
        {
            wm::app::Diag("OnTick exception");
        }
    }
}
