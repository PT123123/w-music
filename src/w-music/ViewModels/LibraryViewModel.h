#pragma once

#include "LibraryViewModel.g.h"

namespace winrt::w_music::implementation
{
    struct LibraryViewModel : LibraryViewModelT<LibraryViewModel>
    {
        LibraryViewModel();

        winrt::Windows::Foundation::Collections::IObservableVector<winrt::w_music::TrackItem> Tracks() const noexcept { return m_tracks; }
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::w_music::PlaylistItem> Playlists() const noexcept { return m_playlists; }
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::w_music::TrackItem> DiscoverRecent() const noexcept { return m_discoverRecent; }
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::w_music::TrackItem> DiscoverTop() const noexcept { return m_discoverTop; }
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::w_music::TrackItem> DiscoverShuffle() const noexcept { return m_discoverShuffle; }
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::w_music::TrackItem> PlaylistTracks() const noexcept { return m_playlistTracks; }

        int32_t TrackCount() const noexcept { return m_trackCount; }
        hstring StatusText() const noexcept { return m_statusText; }
        bool IsScanning() const noexcept { return m_isScanning; }
        hstring CurrentPlaylistName() const noexcept { return m_currentPlaylistName; }

        winrt::Windows::Foundation::IAsyncAction InitializeAsync(winrt::Microsoft::UI::WindowId windowId);
        winrt::Windows::Foundation::IAsyncAction AddFolderAsync();
        winrt::Windows::Foundation::IAsyncAction RescanAsync();
        void CreatePlaylist(hstring const& name);
        void ToggleFavorite(winrt::w_music::TrackItem const& track);
        winrt::Windows::Foundation::IAsyncAction OpenPlaylistAsync(hstring const& playlistId);
        void PlayPlaylist(hstring const& playlistId);
        void PlayAll();

        winrt::event_token PropertyChanged(winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler);
        void PropertyChanged(winrt::event_token const& token) noexcept { m_propertyChanged.remove(token); }

    private:
        void RaisePropertyChanged(std::wstring_view const& name);
        void RefreshDiscover();
        void SetStatus(hstring const& text);
        static void Fill(winrt::Windows::Foundation::Collections::IObservableVector<winrt::w_music::TrackItem> const& target,
                         std::vector<winrt::w_music::TrackItem> const& source);

        winrt::Microsoft::UI::WindowId m_windowId{};

        winrt::Windows::Foundation::Collections::IObservableVector<winrt::w_music::TrackItem> m_tracks{ nullptr };
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::w_music::PlaylistItem> m_playlists{ nullptr };
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::w_music::TrackItem> m_discoverRecent{ nullptr };
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::w_music::TrackItem> m_discoverTop{ nullptr };
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::w_music::TrackItem> m_discoverShuffle{ nullptr };
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::w_music::TrackItem> m_playlistTracks{ nullptr };

        int32_t m_trackCount = 0;
        bool m_isScanning = false;
        hstring m_statusText;
        hstring m_currentPlaylistName;

        winrt::event<winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler> m_propertyChanged;
    };
}

namespace winrt::w_music::factory_implementation
{
    struct LibraryViewModel : LibraryViewModelT<LibraryViewModel, implementation::LibraryViewModel>
    {
    };
}
