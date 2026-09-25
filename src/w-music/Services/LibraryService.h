#pragma once

#include "pch.h"

#include "Models/PlaylistItem.h"
#include "Models/TrackItem.h"

#include <wm/core/LibraryStore.h>
#include <wm/core/Lyric.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace wm::app
{
    /// Owns the local music library: persistence, folder scanning and playlist
    /// management. Not a WinRT class itself -- it feeds plain C++ data to the
    /// view models, which expose it to XAML.
    class LibraryService
    {
    public:
        LibraryService();

        void Load();
        void Save();

        // ---- collections bound to XAML ----
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::w_music::TrackItem> Tracks() const noexcept { return m_tracks; }
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::w_music::PlaylistItem> Playlists() const noexcept { return m_playlists; }

        // ---- folders ----
        /// Shows the folder picker, remembers the folder, and scans it.
        winrt::Windows::Foundation::IAsyncOperation<int> PickAndAddFolderAsync(
            winrt::Microsoft::UI::WindowId windowId,
            std::function<void(int)> progress = {});
        /// Re-scans every remembered folder.
        winrt::Windows::Foundation::IAsyncOperation<int> RescanAsync(std::function<void(int)> progress = {});
        /// Drops files that no longer exist on disk.
        int PruneMissing();
        std::size_t FolderCount() const noexcept;
        /// The remembered scan folders that still exist on disk (used by the
        /// recommendation engine's library analysis, which walks the same
        /// folders on its own).
        std::vector<std::wstring> FolderPaths() const;

        // ---- tracks ----
        winrt::w_music::TrackItem FindTrack(hstring const& trackId) const;
        std::vector<hstring> TrackIdsOfPlaylist(hstring const& playlistId) const;
        bool ToggleFavorite(hstring const& trackId);
        void MarkPlayed(hstring const& trackId);
        /// Returns the raw lyric text (empty when no .lrc sits next to the file).
        winrt::Windows::Foundation::IAsyncOperation<hstring> LoadLyricTextAsync(hstring const& trackId);

        /// Adds a single file (used by the online-source downloader) and returns
        /// the resulting library entry, or nullptr when it could not be read.
        winrt::Windows::Foundation::IAsyncOperation<winrt::w_music::TrackItem> ImportFileAsync(
            winrt::Windows::Storage::StorageFile const& file);

        // ---- playlists ----
        winrt::w_music::PlaylistItem CreatePlaylist(hstring const& name);
        void DeletePlaylist(hstring const& playlistId);
        void RenamePlaylist(hstring const& playlistId, hstring const& name);
        bool AddToPlaylist(hstring const& playlistId, hstring const& trackId);
        void RemoveFromPlaylist(hstring const& playlistId, hstring const& trackId);
        hstring PlaylistName(hstring const& playlistId) const;

        // ---- discover page ----
        std::vector<winrt::w_music::TrackItem> RecentlyAdded(std::size_t count) const;
        std::vector<winrt::w_music::TrackItem> TopPlayed(std::size_t count) const;
        std::vector<winrt::w_music::TrackItem> ShufflePick(std::size_t count) const;

    private:
        void RefreshTracks();
        void RefreshPlaylists();
        void SyncPlaylistCounts();
        void ApplyTrackBatch(std::vector<wm::core::TrackRecord> batch,
                             int scanned,
                             std::function<void(int)> const& progress);
        winrt::Windows::Foundation::IAsyncOperation<int> ScanPathAsync(
            std::wstring const& path,
            std::function<void(int)> progress);
        bool IngestFile(winrt::Windows::Storage::StorageFile const& file,
                        winrt::Windows::Storage::FileProperties::MusicProperties const* props);
        winrt::w_music::TrackItem EnsureTrackItem(wm::core::TrackRecord const& record);

        wm::core::LibraryStore m_store;
        std::atomic_bool m_scanInProgress{ false };
        std::int64_t m_lastScanSaveMs = 0;
        winrt::Microsoft::UI::Dispatching::DispatcherQueue m_dispatcher{ nullptr };
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::w_music::TrackItem> m_tracks{ nullptr };
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::w_music::PlaylistItem> m_playlists{ nullptr };
        std::map<std::wstring, winrt::w_music::TrackItem> m_trackIndex;   // id -> item
        std::map<std::wstring, winrt::w_music::PlaylistItem> m_playlistIndex;
    };
}
