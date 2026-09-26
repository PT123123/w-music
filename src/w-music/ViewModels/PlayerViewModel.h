#pragma once

#include "PlayerViewModel.g.h"

#include "Audio/WasapiLoopback.h"

#include <wm/core/Equalizer.h>
#include <wm/core/Lyric.h>
#include <wm/core/PlayQueue.h>
#include <wm/core/SpectrumAnalyzer.h>

#include <array>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace wm::app
{
    class EqualizedSource;
}

namespace winrt::w_music::implementation
{
    struct PlayerViewModel : PlayerViewModelT<PlayerViewModel>
    {
        PlayerViewModel();
        ~PlayerViewModel();

        // ---- player ----
        winrt::Windows::Media::Playback::MediaPlayer Player() const noexcept { return m_player; }

        winrt::w_music::TrackItem CurrentTrack() const noexcept { return m_currentTrack; }
        bool HasTrack() const noexcept { return m_currentTrack != nullptr; }
        hstring Title() const;
        hstring Artist() const;
        hstring Album() const;

        bool IsPlaying() const noexcept { return m_isPlaying; }
        double PositionSeconds() const noexcept { return m_positionSeconds; }
        void PositionSeconds(double value);
        double DurationSeconds() const noexcept { return m_durationSeconds; }
        hstring PositionText() const;
        hstring DurationText() const;

        double Volume() const noexcept { return m_volume; }
        void Volume(double value);
        bool IsMuted() const noexcept { return m_isMuted; }
        void IsMuted(bool value);

        winrt::w_music::PlayMode Mode() const noexcept { return m_mode; }
        void Mode(winrt::w_music::PlayMode value);
        hstring ModeText() const;
        bool IsFavorite() const noexcept;
        void IsFavorite(bool value);
        /// 手势/推荐流的一行轻提示（几秒后自动清空；空 = 不显示）。
        hstring FlowStatusText() const noexcept { return m_flowStatusText; }

        // ---- 推荐流「接下来」卡片 ----
        /// 当前是否处于推荐流模式（Radio）。
        bool IsFlowMode() const noexcept;
        /// 接下来要播的预告（推荐流 = 预取缓冲前 3；顺序模式 = 队列预览 1 行）。
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::w_music::RecommendItem>
            FlowUpNext() const noexcept { return m_flowUpNext; }
        /// 卡片标题：「接下来 · 推荐流（晚间）」/「接下来 · 顺序播放」。
        hstring FlowHeaderText() const noexcept { return m_flowHeaderText; }
        /// 卡片底部的诚实说明：已预载几首 / 引擎预热中 / 随机不预告。
        hstring FlowStreamText() const noexcept { return m_flowStreamText; }
        /// 「换一个」只在推荐流模式显示。
        winrt::Microsoft::UI::Xaml::Visibility FlowRefreshVisibility() const noexcept;
        /// 点击预告行：直接播放这首（前面的预告一并作废）。
        void PlayFlowUpNext(int32_t index);
        /// 「换一个」：跳过缓冲头部的预告，后台补一首新的。
        void SkipFlowSlot();
        /// 曲库（重）加载完成：缓存指纹就位，值得再试一次离线种子。
        void NotifyLibraryReady();

        // ---- lyrics ----
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::w_music::LyricLineItem> Lyrics() const noexcept { return m_lyrics; }
        bool HasLyrics() const noexcept { return m_lyric.valid && !m_lyric.empty(); }
        int32_t ActiveLyricIndex() const noexcept { return m_activeLyricIndex; }
        int64_t LyricOffsetMs() const noexcept { return m_lyric.offsetMs; }
        void LyricOffsetMs(int64_t value);

        // ---- commands ----
        void TogglePlayPause();
        void Next();
        void Previous();
        void Seek(double seconds);
        void CycleMode();
        void ToggleFavorite();
        /// 点赞当前曲：回传引擎加权，播放继续（不打断）。
        void LikeCurrent();
        /// 差评当前曲：回传引擎 + 从当前播放队列移除 + 立即流向下一首。
        void DislikeCurrent();
        void SetQueue(winrt::Windows::Foundation::Collections::IVector<winrt::w_music::TrackItem> const& tracks, int32_t startIndex);
        void PlayTrack(winrt::w_music::TrackItem const& track);
        void PlayTrackById(hstring const& trackId);
        void SeekToLyric(int32_t index);
        void AdjustLyricOffset(int64_t deltaMs);

        // ---- non-WinRT helpers (reach these through winrt::get_self) ----
        /// Called on the UI thread with the newest spectrum bars (0..1).
        /// Multiple views (the mini bar and the full now-playing page) can listen.
        /// Returns a token to pass to RemoveSpectrumSink (pages unregister on unload).
        std::size_t AddSpectrumSink(std::function<void(std::vector<double> const&)> sink);
        void RemoveSpectrumSink(std::size_t token);
        void StartSpectrum();
        void StopSpectrum();
        /// Called once from the UI thread so timers bind to the right dispatcher.
        void Initialize(winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue);

        // ---- equalizer ----
        /// Live per-band gains (dB) + preamp (dB); applied to the audio thread
        /// without restarting playback.
        void ConfigureEqualizer(std::array<double, wm::core::Equalizer::BandCount> const& gainsDb, double preampDb);
        /// Toggling re-binds the playback source (proxy vs. direct); the current
        /// position / play state survive the switch.
        void SetEqualizerEnabled(bool enabled);
        bool EqualizerEnabled() const noexcept { return m_eqEnabled; }

        winrt::event_token PropertyChanged(winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler);
        void PropertyChanged(winrt::event_token const& token) noexcept { m_propertyChanged.remove(token); }

    private:
        void RaisePropertyChanged(std::wstring_view const& name);
        void EnsurePlayer();
        winrt::fire_and_forget StartPlaybackAsync(winrt::w_music::TrackItem track);
        winrt::Windows::Foundation::IAsyncAction LoadLyricAsync(hstring trackId);
        /// Parses |text| (LRC) into a target-relative track id, i.e. the mid of
        /// an ongoing QQ online track.
        /// Fetches the LRC on a worker thread, then applies it on the UI thread.
        /// |mid| is by value: this is fire-and-forget, so the caller's own
        /// string can be gone before the worker reads it.
        winrt::fire_and_forget LoadOnlineLyric(std::string mid);
        /// Populates m_lyrics / m_lyric from raw LRC text and raises the UI props.
        void ApplyLyricText(std::string const& text);
        /// Strips a "online:qq:{mid}:preview" TrackItem id down to the mid.
        static std::string QqMidFromTrack(hstring const& trackId);
        void OnTick();
        void HandleMediaEnded();
        void SyncPlayingState();
        void PushSpectrumToUi();
        void SyncFavoriteState();
        /// Restores position / pause state after SetEqualizerEnabled re-bound the source.
        void ApplyPendingResume();

        /// 推荐流（flow，默认）：下一首从预取缓冲同步出队，零等待。缓冲空时
        /// 降级为普通队列推进（诚实提示），绝不阻塞、绝不弹选择框。
        void FlowNext(bool autoAdvance);
        /// 后台补货：磁盘缓存种子先行（离线可用的部分），引擎就绪后走
        /// /v1/feed/next 增量。|replace| = 整流重算（时间窗切换），否则向
        /// 缓冲尾部追加。
        winrt::fire_and_forget EnsureFlowBufferAsync(bool replace);
        /// 把候选行按规则（避开最近播放/已跳过/重复）并入缓冲；replace 时
        /// 整体替换且失败绝不空窗。返回缓冲是否变化。
        bool MergeFlowRows(winrt::Windows::Foundation::Collections::IVectorView<winrt::w_music::RecommendItem> const& rows,
                           bool replace);
        /// 把缓冲/队列预览刷进「接下来」卡片的绑定属性（只可在 UI 线程调）。
        void RefreshFlowUi();
        /// 本地钟 → 时间窗标签（深夜/清晨/上午/午间/下午/傍晚/夜晚）。
        static hstring FlowWindowLabel();
        /// 跨过时间窗就挂整流重算标记，在途的旧窗口答案会被丢弃。
        void SyncFlowWindow();
        /// 队列推进的公共尾：MediaEnded（autoAdvance=true）与 Next 共用。
        void FallbackQueueNext(bool autoAdvance);
        /// 当前曲的引擎反馈：rec: 行带内容 hash，曲库曲带 file_path
        /// （引擎侧做只读解析）。引擎没就绪时静默跳过。
        void SendCurrentFeedback(winrt::w_music::TrackItem const& track, wchar_t const* event);
        /// 显示一行轻提示，2.6 秒后自动清空。
        void ShowFlowStatus(hstring text);
        /// rec: 行不在曲库里，Next/Previous/MediaEnded 按 id 解析时要在这里找。
        void RememberSideTrack(winrt::w_music::TrackItem const& track);
        /// 「换一个」/差评跳过的文件路径（utf8），本会话内不再进推荐流缓冲。
        void RememberRejectedPath(std::string path);

        winrt::Windows::Media::Playback::MediaPlayer m_player{ nullptr };
        winrt::w_music::TrackItem m_currentTrack{ nullptr };

        winrt::Windows::Foundation::Collections::IObservableVector<winrt::w_music::LyricLineItem> m_lyrics{ nullptr };
        wm::core::LyricDocument m_lyric;
        int32_t m_activeLyricIndex = -1;

        wm::core::PlayQueue m_queue;
        wm::core::PlayMode m_coreMode{ wm::core::PlayMode::LoopAll };
        // 推荐流（flow）是默认模式：下一首 = 预取缓冲出队，见 FlowNext。
        winrt::w_music::PlayMode m_mode{ winrt::w_music::PlayMode::Radio };

        // ---- 推荐流（flow）----
        // 最近播放的文件路径（utf8）：选曲微调，候选先避开这 30 首。
        std::deque<std::string> m_recentPaths;
        // rec: 行（引擎建议）不在曲库索引里，按 id 走 Next/Previous 时从这取回
        // TrackItem 对象。
        std::map<std::string, winrt::w_music::TrackItem> m_sideTracks;
        // 预取的推荐流：切歌 = 这里同步出队（顺序即引擎 /v1/feed/next 的排名，
        // 与文件顺序无关）。
        std::deque<winrt::w_music::RecommendItem> m_flowBuffer;
        // 一次只允许一个在途补货；时间窗切换要求整流重算时挂 pending 标记。
        bool m_flowRefillBusy = false;
        bool m_flowReplacePending = false;
        // 离线种子（磁盘缓存里的上一份 feed）本会话只用一次；曲库重载后重试。
        bool m_flowCacheSeeded = false;
        bool m_flowPrewarmKicked = false;
        // 「换一个」/差评跳过的曲子，本会话不再进缓冲（容量上限防膨胀）。
        std::deque<std::string> m_rejectedPaths;
        // StartPlaybackAsync 的临时标记：当前曲占用的预告位刚被清掉，卡片要刷。
        bool m_flowPurgedCurrent = false;
        // 当前时间窗标签；跨窗即触发整流重算（新窗口一条新流）。
        hstring m_flowWindow;
        // 「接下来」卡片：行列表 + 标题/说明两行文案。
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::w_music::RecommendItem>
            m_flowUpNext{ nullptr };
        hstring m_flowHeaderText;
        hstring m_flowStreamText;
        hstring m_flowStatusText;
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_flowStatusTimer{ nullptr };
        static constexpr std::size_t kRecentWindow = 30;
        static constexpr std::uint32_t kFlowCandidates = 30;
        // 缓冲低于 kFlowBufferTarget 就后台补货，上限 kFlowBufferMax；卡片只展示前 3。
        static constexpr std::size_t kFlowBufferTarget = 4;
        static constexpr std::size_t kFlowBufferMax = 6;
        static constexpr std::size_t kRejectedMax = 60;

        double m_positionSeconds = 0.0;
        double m_durationSeconds = 0.0;
        double m_volume = 0.8;
        bool m_isMuted = false;
        bool m_isPlaying = false;

        // Spectrum: produced on the WASAPI thread, consumed on the UI thread.
        wm::app::WasapiLoopback m_loopback;
        wm::core::SpectrumAnalyzer m_spectrum{ wm::core::SpectrumConfig{} };
        std::mutex m_spectrumMutex;
        std::vector<double> m_pendingBars;
        std::map<std::size_t, std::function<void(std::vector<double> const&)>> m_spectrumSinks;
        std::size_t m_nextSinkId = 1;

        // Equalizer: the DSP instance is fed by the UI thread and read by the
        // proxy source's decode thread; m_eqSource keeps the proxy alive while
        // it backs the current playback.
        wm::core::Equalizer m_equalizer;
        bool m_eqEnabled = false;
        std::shared_ptr<wm::app::EqualizedSource> m_eqSource;
        double m_pendingSeekSeconds = 0.0;
        bool m_pendingResumePaused = false;

        winrt::Microsoft::UI::Dispatching::DispatcherQueue m_dispatcher{ nullptr };
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_timer{ nullptr };

        winrt::event<winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler> m_propertyChanged;
        winrt::event_token m_mediaEndedToken{};
        winrt::event_token m_stateChangedToken{};
    };
}

namespace winrt::w_music::factory_implementation
{
    struct PlayerViewModel : PlayerViewModelT<PlayerViewModel, implementation::PlayerViewModel>
    {
    };
}
