#include "pch.h"

#include "ViewModels/PlayerViewModel.h"
#include "ViewModels/PlayerViewModel.g.cpp"

#include "Models/LyricLineItem.h"
#include "Services/AppPaths.h"
#include "Services/LibraryService.h"
#include "Services/Services.h"

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

        m_player = MediaPlayer();
        m_player.AudioCategory(MediaPlayerAudioCategory::Media);
        m_player.Volume(m_volume);
        m_player.IsMuted(m_isMuted);

        m_mediaEndedToken = m_player.MediaEnded({ this, &PlayerViewModel::OnMediaEnded });

        m_player.PlaybackSession().PlaybackStateChanged([weak = get_weak()](auto&&, auto&&) {
            if (auto self = weak.get())
            {
                const bool playing = self->m_player.PlaybackSession().PlaybackState() == MediaPlaybackState::Playing;
                if (self->m_isPlaying != playing)
                {
                    self->m_isPlaying = playing;
                    self->RaisePropertyChanged(L"IsPlaying");
                }
            }
        });
    }

    void PlayerViewModel::OnMediaEnded(MediaPlayer const&, IInspectable const&)
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

        try
        {
            if (remote)
            {
                // Streamed straight from an online source (see OnlineProviderService).
                m_player.Source(MediaSource::CreateFromUri(winrt::Windows::Foundation::Uri{ track.FilePath() }));
            }
            else
            {
                auto file = co_await StorageFile::GetFileFromPathAsync(track.FilePath());
                m_player.Source(MediaSource::CreateFromStorageFile(file));
            }
            m_player.Play();
        }
        catch (...)
        {
            // Unreadable / missing file: fall through so the next track can play.
        }

        if (!remote)
        {
            co_await LoadLyricAsync(track.Id());
            wm::app::Library().MarkPlayed(track.Id());
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

        m_lyrics.Clear();
        m_lyric = wm::core::LyricDocument{};
        m_activeLyricIndex = -1;

        if (!text.empty())
        {
            m_lyric = wm::core::LyricParser::Parse(
                wm::app::Utf8(std::wstring_view{ text.c_str(), text.size() }));

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
}
