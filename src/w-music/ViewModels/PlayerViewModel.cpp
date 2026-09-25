#include "pch.h"

#include "ViewModels/PlayerViewModel.h"
#include "ViewModels/PlayerViewModel.g.cpp"

#include "Audio/EqualizedSource.h"
#include "Models/LyricLineItem.h"
#include "Services/AppPaths.h"
#include "Services/DiscoverSettings.h"
#include "Services/LibraryService.h"
#include "Services/OnlineProviderService.h"
#include "Services/Services.h"

#include <wm/core/OnlineSources.h>

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
            const auto next = m_queue.Next(true);
            if (next.has_value())
            {
                PlayTrackById(hstring{ wm::app::Utf16(*next) });
                return;
            }

            m_isPlaying = false;
            RaisePropertyChanged(L"IsPlaying");
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

    void PlayerViewModel::Next()
    {
        const auto next = m_queue.Next(false);
        if (next.has_value())
        {
            PlayTrackById(hstring{ wm::app::Utf16(*next) });
        }
    }

    void PlayerViewModel::Previous()
    {
        const auto previous = m_queue.Previous();
        if (previous.has_value())
        {
            PlayTrackById(hstring{ wm::app::Utf16(*previous) });
        }
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
            ids.push_back(IdOf(tracks.GetAt(i).Id()));
        }
        m_queue.SetTracks(std::move(ids), startIndex);
    }

    void PlayerViewModel::PlayTrack(winrt::w_music::TrackItem const& track)
    {
        if (track == nullptr)
        {
            return;
        }
        m_queue.JumpToId(IdOf(track.Id()));
        StartPlaybackAsync(track);
    }

    void PlayerViewModel::PlayTrackById(hstring const& trackId)
    {
        if (auto track = wm::app::Library().FindTrack(trackId))
        {
            StartPlaybackAsync(track);
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
            default: return hstring{ L"列表循环" };
        }
    }

    void PlayerViewModel::CycleMode()
    {
        const auto next = static_cast<int>(m_mode) + 1;
        Mode(static_cast<winrt::w_music::PlayMode>(next % 4));
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
