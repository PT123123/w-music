#include "pch.h"

#include "Services/LibraryService.h"
#include "Services/AppPaths.h"

#include "Models/LyricLineItem.h"
#include "Models/PlaylistItem.h"
#include "Models/TrackItem.h"

#include <shobjidl_core.h>

#include <algorithm>
#include <random>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Storage;
using namespace Windows::Storage::AccessCache;
using namespace Windows::Storage::FileProperties;
using namespace Windows::Storage::Pickers;
using namespace Windows::Storage::Streams;

namespace wm::app
{
    namespace
    {
        constexpr wchar_t kGlyphPlaylist[] = L"\uE8D6";   // List
        constexpr wchar_t kGlyphFavorite[] = L"\uEB51";   // Heart
        constexpr wchar_t kGlyphRecent[] = L"\uE823";     // Clock

        bool IsAudioExtension(std::wstring_view extension)
        {
            static const std::vector<std::wstring> kExtensions{
                L".mp3", L".flac", L".wav", L".m4a", L".aac", L".ogg", L".opus", L".wma", L".aiff", L".alac"
            };
            std::wstring lower(extension);
            std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) {
                return static_cast<wchar_t>(towlower(c));
            });
            return std::find(kExtensions.begin(), kExtensions.end(), lower) != kExtensions.end();
        }

        std::wstring FindLyricFile(std::wstring const& audioPath)
        {
            std::error_code ec;
            const std::filesystem::path original(audioPath);

            auto sameFolder = original;
            sameFolder.replace_extension(L".lrc");
            if (std::filesystem::exists(sameFolder, ec))
            {
                return sameFolder.wstring();
            }

            auto subFolder = original.parent_path() / L"lyrics" / (original.stem().wstring() + L".lrc");
            if (std::filesystem::exists(subFolder, ec))
            {
                return subFolder.wstring();
            }

            auto songFolder = original.parent_path() / original.stem() / (original.stem().wstring() + L".lrc");
            if (std::filesystem::exists(songFolder, ec))
            {
                return songFolder.wstring();
            }
            return {};
        }

        std::int64_t NowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch()).count();
        }

        std::string IdOf(hstring const& value)
        {
            return Utf8(std::wstring_view{ value.c_str(), value.size() });
        }
    } // namespace

    LibraryService::LibraryService()
    {
        m_tracks = winrt::single_threaded_observable_vector<winrt::w_music::TrackItem>();
        m_playlists = winrt::single_threaded_observable_vector<winrt::w_music::PlaylistItem>();
    }

    void LibraryService::Load()
    {
        EnsureDataDirectory();
        std::string error;
        m_store.Load(LibraryFilePath().string(), &error);
        m_store.EnsureBuiltin(wm::core::PlaylistKind::Favorites, "我喜欢的音乐");
        m_store.EnsureBuiltin(wm::core::PlaylistKind::Recent, "最近播放");
        RefreshTracks();
        RefreshPlaylists();
    }

    void LibraryService::Save()
    {
        EnsureDataDirectory();
        std::string error;
        m_store.Save(LibraryFilePath().string(), &error);
    }

    winrt::w_music::TrackItem LibraryService::EnsureTrackItem(wm::core::TrackRecord const& record)
    {
        auto item = winrt::make<winrt::w_music::implementation::TrackItem>();
        item.Id(hstring{ Utf16(record.id) });
        item.Title(hstring{ Utf16(record.title) });
        item.Artist(hstring{ Utf16(record.artist) });
        item.Album(hstring{ Utf16(record.album) });
        item.FilePath(hstring{ Utf16(record.filePath) });
        item.DurationMs(record.durationMs);
        item.PlayCount(record.playCount);
        item.IsFavorite(record.favorite);
        return item;
    }

    void LibraryService::RefreshTracks()
    {
        m_tracks.Clear();
        m_trackIndex.clear();

        for (const auto& record : m_store.Data().tracks)
        {
            auto item = EnsureTrackItem(record);
            m_tracks.Append(item);
            m_trackIndex[Utf16(record.id)] = item;
        }
    }

    void LibraryService::RefreshPlaylists()
    {
        m_playlists.Clear();
        m_playlistIndex.clear();

        for (const auto& record : m_store.Data().playlists)
        {
            auto item = winrt::make<winrt::w_music::implementation::PlaylistItem>();
            item.Id(hstring{ Utf16(record.id) });
            item.Name(hstring{ Utf16(record.name) });
            item.TrackCount(static_cast<int32_t>(record.trackIds.size()));
            item.IsBuiltIn(record.kind != wm::core::PlaylistKind::User);

            switch (record.kind)
            {
                case wm::core::PlaylistKind::Favorites: item.Glyph(kGlyphFavorite); break;
                case wm::core::PlaylistKind::Recent: item.Glyph(kGlyphRecent); break;
                default: item.Glyph(kGlyphPlaylist); break;
            }

            m_playlists.Append(item);
            m_playlistIndex[Utf16(record.id)] = item;
        }
    }

    void LibraryService::IngestFile(StorageFile const& file, MusicProperties const& props)
    {
        const std::wstring path{ file.Path().c_str() };
        if (path.empty())
        {
            return;
        }

        const std::string utf8Path = Utf8(path);

        wm::core::TrackRecord record;
        record.id = wm::core::LibraryStore::MakeTrackId(utf8Path);
        record.filePath = utf8Path;

        std::wstring title{ props.Title().c_str() };
        if (title.empty())
        {
            title = std::filesystem::path(path).stem().wstring();
        }
        record.title = Utf8(title);
        record.artist = Utf8(std::wstring_view{ props.Artist().c_str(), props.Artist().size() });
        record.album = Utf8(std::wstring_view{ props.Album().c_str(), props.Album().size() });
        record.durationMs = props.Duration().count() / 10000; // 100ns ticks -> ms
        record.bitrateKbps = props.Bitrate();
        record.dateAdded = NowMs();
        record.lyricPath = Utf8(FindLyricFile(path));

        m_store.UpsertTrack(record);
    }

    IAsyncOperation<winrt::w_music::TrackItem> LibraryService::ImportFileAsync(StorageFile const& file)
    {
        if (file == nullptr)
        {
            co_return nullptr;
        }
        const std::wstring path{ file.Path().c_str() };
        if (path.empty())
        {
            co_return nullptr;
        }
        try
        {
            auto props = co_await file.Properties().GetMusicPropertiesAsync();
            IngestFile(file, props);
            RefreshTracks();
            Save();
            co_return FindTrack(hstring{ Utf16(wm::core::LibraryStore::MakeTrackId(Utf8(path))) });
        }
        catch (...)
        {
            co_return nullptr;
        }
    }

    IAsyncOperation<int> LibraryService::ScanFolderAsync(StorageFolder folder)
    {
        int scanned = 0;
        if (folder == nullptr)
        {
            co_return scanned;
        }

        auto items = co_await folder.GetItemsAsync();
        for (auto const& item : items)
        {
            if (item.IsOfType(StorageItemTypes::Folder))
            {
                scanned += co_await ScanFolderAsync(item.as<StorageFolder>());
            }
            else if (item.IsOfType(StorageItemTypes::File))
            {
                auto file = item.as<StorageFile>();
                if (!IsAudioExtension(std::wstring_view{ file.FileType().c_str(), file.FileType().size() }))
                {
                    continue;
                }

                try
                {
                    auto props = co_await file.Properties().GetMusicPropertiesAsync();
                    IngestFile(file, props);
                    ++scanned;
                }
                catch (...)
                {
                    // A single unreadable file must not abort the whole scan.
                }
            }
        }
        co_return scanned;
    }

    IAsyncOperation<int> LibraryService::PickAndAddFolderAsync(winrt::Microsoft::UI::WindowId windowId)
    {
        FolderPicker picker;
        const HWND hwnd = winrt::Microsoft::UI::GetWindowFromWindowId(windowId);
        picker.as<::IInitializeWithWindow>()->Initialize(hwnd);
        picker.FileTypeFilter().Append(L"*");
        picker.SuggestedStartLocation(PickerLocationId::MusicLibrary);

        auto folder = co_await picker.PickSingleFolderAsync();
        if (folder == nullptr)
        {
            co_return 0;
        }

        const hstring token = StorageApplicationPermissions::FutureAccessList().Add(folder);
        m_store.Data().scanFolders.push_back(Utf8(std::wstring_view{ token.c_str(), token.size() }));

        const int scanned = co_await ScanFolderAsync(folder);
        RefreshTracks();
        RefreshPlaylists();
        Save();
        co_return scanned;
    }

    IAsyncOperation<int> LibraryService::RescanAsync()
    {
        int scanned = 0;
        const auto access = StorageApplicationPermissions::FutureAccessList();

        for (const auto& tokenUtf8 : m_store.Data().scanFolders)
        {
            try
            {
                auto folder = co_await access.GetFolderAsync(hstring{ Utf16(tokenUtf8) });
                if (folder != nullptr)
                {
                    scanned += co_await ScanFolderAsync(folder);
                }
            }
            catch (...)
            {
                // Token expired or permission revoked: skip that folder.
            }
        }

        PruneMissing();
        RefreshTracks();
        RefreshPlaylists();
        Save();
        co_return scanned;
    }

    int LibraryService::PruneMissing()
    {
        auto& tracks = m_store.Data().tracks;
        const auto removed = std::stable_partition(tracks.begin(), tracks.end(), [](const wm::core::TrackRecord& t) {
            return t.filePath.empty() || std::filesystem::exists(Utf16(t.filePath));
        });

        const int count = static_cast<int>(std::distance(removed, tracks.end()));
        if (count > 0)
        {
            for (auto it = removed; it != tracks.end(); ++it)
            {
                for (auto& playlist : m_store.Data().playlists)
                {
                    playlist.trackIds.erase(
                        std::remove(playlist.trackIds.begin(), playlist.trackIds.end(), it->id),
                        playlist.trackIds.end());
                }
            }
            tracks.erase(removed, tracks.end());
        }
        return count;
    }

    std::size_t LibraryService::FolderCount() const noexcept
    {
        return m_store.Data().scanFolders.size();
    }

    winrt::w_music::TrackItem LibraryService::FindTrack(hstring const& trackId) const
    {
        const auto it = m_trackIndex.find(std::wstring{ trackId.c_str() });
        return it == m_trackIndex.end() ? winrt::w_music::TrackItem{ nullptr } : it->second;
    }

    std::vector<hstring> LibraryService::TrackIdsOfPlaylist(hstring const& playlistId) const
    {
        std::vector<hstring> ids;
        const auto* playlist = m_store.FindPlaylist(IdOf(playlistId));
        if (playlist == nullptr)
        {
            return ids;
        }
        ids.reserve(playlist->trackIds.size());
        for (const auto& id : playlist->trackIds)
        {
            ids.push_back(hstring{ Utf16(id) });
        }
        return ids;
    }

    bool LibraryService::ToggleFavorite(hstring const& trackId)
    {
        const bool favorite = m_store.ToggleFavorite(IdOf(trackId));
        if (auto item = FindTrack(trackId))
        {
            item.IsFavorite(favorite);
        }

        // Keep the built-in favourites playlist count in sync.
        if (const auto* favorites = m_store.FindPlaylist("builtin:1"))
        {
            if (const auto it = m_playlistIndex.find(Utf16(favorites->id)); it != m_playlistIndex.end())
            {
                it->second.TrackCount(static_cast<int32_t>(favorites->trackIds.size()));
            }
        }

        Save();
        return favorite;
    }

    void LibraryService::MarkPlayed(hstring const& trackId)
    {
        m_store.MarkPlayed(IdOf(trackId), NowMs());
        if (auto item = FindTrack(trackId))
        {
            const auto* record = m_store.FindTrack(IdOf(trackId));
            if (record != nullptr)
            {
                item.PlayCount(record->playCount);
            }
        }

        if (const auto* recent = m_store.FindPlaylist("builtin:2"))
        {
            if (const auto it = m_playlistIndex.find(Utf16(recent->id)); it != m_playlistIndex.end())
            {
                it->second.TrackCount(static_cast<int32_t>(recent->trackIds.size()));
            }
        }
        Save();
    }

    IAsyncOperation<hstring> LibraryService::LoadLyricTextAsync(hstring const& trackId)
    {
        const auto* record = m_store.FindTrack(IdOf(trackId));
        if (record == nullptr || record->lyricPath.empty())
        {
            co_return hstring{};
        }

        const std::wstring path = Utf16(record->lyricPath);
        try
        {
            auto file = co_await StorageFile::GetFileFromPathAsync(path);
            if (file == nullptr)
            {
                co_return hstring{};
            }

            auto buffer = co_await FileIO::ReadBufferAsync(file);
            std::string raw(static_cast<std::size_t>(buffer.Length()), '\0');
            if (!raw.empty())
            {
                auto reader = DataReader::FromBuffer(buffer);
                reader.ReadBytes(winrt::array_view<std::uint8_t>(
                    reinterpret_cast<std::uint8_t*>(raw.data()), raw.size()));
            }
            co_return hstring{ Utf16(wm::core::LyricParser::DecodeToUtf8(raw)) };
        }
        catch (...)
        {
            co_return hstring{};
        }
    }

    winrt::w_music::PlaylistItem LibraryService::CreatePlaylist(hstring const& name)
    {
        const auto& record = m_store.CreatePlaylist(Utf8(std::wstring_view{ name.c_str(), name.size() }));

        auto item = winrt::make<winrt::w_music::implementation::PlaylistItem>();
        item.Id(hstring{ Utf16(record.id) });
        item.Name(hstring{ Utf16(record.name) });
        item.TrackCount(0);
        item.IsBuiltIn(false);
        item.Glyph(kGlyphPlaylist);
        m_playlists.Append(item);
        m_playlistIndex[Utf16(record.id)] = item;
        Save();
        return item;
    }

    void LibraryService::DeletePlaylist(hstring const& playlistId)
    {
        const std::string id = IdOf(playlistId);
        const auto it = m_playlistIndex.find(Utf16(id));
        std::uint32_t position = 0;
        bool found = false;

        for (std::uint32_t i = 0; i < m_playlists.Size(); ++i)
        {
            if (m_playlists.GetAt(i).Id() == playlistId)
            {
                position = i;
                found = true;
                break;
            }
        }

        if (!m_store.DeletePlaylist(id))
        {
            return; // built-ins are protected
        }
        if (found)
        {
            m_playlists.RemoveAt(position);
        }
        if (it != m_playlistIndex.end())
        {
            m_playlistIndex.erase(it);
        }
        Save();
    }

    void LibraryService::RenamePlaylist(hstring const& playlistId, hstring const& name)
    {
        const std::string id = IdOf(playlistId);
        if (!m_store.RenamePlaylist(id, Utf8(std::wstring_view{ name.c_str(), name.size() })))
        {
            return;
        }
        if (const auto it = m_playlistIndex.find(Utf16(id)); it != m_playlistIndex.end())
        {
            it->second.Name(name);
        }
        Save();
    }

    bool LibraryService::AddToPlaylist(hstring const& playlistId, hstring const& trackId)
    {
        const std::string id = IdOf(playlistId);
        if (!m_store.AddToPlaylist(id, IdOf(trackId)))
        {
            return false;
        }
        if (const auto* playlist = m_store.FindPlaylist(id))
        {
            if (const auto it = m_playlistIndex.find(Utf16(id)); it != m_playlistIndex.end())
            {
                it->second.TrackCount(static_cast<int32_t>(playlist->trackIds.size()));
            }
        }
        Save();
        return true;
    }

    void LibraryService::RemoveFromPlaylist(hstring const& playlistId, hstring const& trackId)
    {
        const std::string id = IdOf(playlistId);
        if (!m_store.RemoveFromPlaylist(id, IdOf(trackId)))
        {
            return;
        }
        if (const auto* playlist = m_store.FindPlaylist(id))
        {
            if (const auto it = m_playlistIndex.find(Utf16(id)); it != m_playlistIndex.end())
            {
                it->second.TrackCount(static_cast<int32_t>(playlist->trackIds.size()));
            }
        }
        Save();
    }

    hstring LibraryService::PlaylistName(hstring const& playlistId) const
    {
        const auto* playlist = m_store.FindPlaylist(IdOf(playlistId));
        return playlist == nullptr ? hstring{} : hstring{ Utf16(playlist->name) };
    }

    std::vector<winrt::w_music::TrackItem> LibraryService::RecentlyAdded(std::size_t count) const
    {
        std::vector<winrt::w_music::TrackItem> result;
        for (const auto* record : m_store.RecentlyAdded(count))
        {
            if (const auto it = m_trackIndex.find(Utf16(record->id)); it != m_trackIndex.end())
            {
                result.push_back(it->second);
            }
        }
        return result;
    }

    std::vector<winrt::w_music::TrackItem> LibraryService::TopPlayed(std::size_t count) const
    {
        std::vector<winrt::w_music::TrackItem> result;
        for (const auto* record : m_store.TopPlayed(count))
        {
            if (const auto it = m_trackIndex.find(Utf16(record->id)); it != m_trackIndex.end())
            {
                result.push_back(it->second);
            }
        }
        return result;
    }

    std::vector<winrt::w_music::TrackItem> LibraryService::ShufflePick(std::size_t count) const
    {
        std::vector<winrt::w_music::TrackItem> pool;
        pool.reserve(m_trackIndex.size());
        for (const auto& [id, item] : m_trackIndex)
        {
            pool.push_back(item);
        }

        static std::mt19937 rng{ std::random_device{}() };
        std::shuffle(pool.begin(), pool.end(), rng);
        if (pool.size() > count)
        {
            pool.resize(count);
        }
        return pool;
    }
}
