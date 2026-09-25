#pragma once

#include "PlayerViewModel.g.h"

#include "Audio/WasapiLoopback.h"

#include <wm/core/Equalizer.h>
#include <wm/core/Lyric.h>
#include <wm/core/PlayQueue.h>
#include <wm/core/SpectrumAnalyzer.h>

#include <array>
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

        winrt::Windows::Media::Playback::MediaPlayer m_player{ nullptr };
        winrt::w_music::TrackItem m_currentTrack{ nullptr };

        winrt::Windows::Foundation::Collections::IObservableVector<winrt::w_music::LyricLineItem> m_lyrics{ nullptr };
        wm::core::LyricDocument m_lyric;
        int32_t m_activeLyricIndex = -1;

        wm::core::PlayQueue m_queue;
        wm::core::PlayMode m_coreMode{ wm::core::PlayMode::LoopAll };
        winrt::w_music::PlayMode m_mode{ winrt::w_music::PlayMode::LoopAll };

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
