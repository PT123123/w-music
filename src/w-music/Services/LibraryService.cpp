#include "pch.h"

#include "Services/LibraryService.h"
#include "Services/AppPaths.h"

#include "Models/LyricLineItem.h"
#include "Models/PlaylistItem.h"
#include "Models/TrackItem.h"

#include <shobjidl_core.h>

#include <algorithm>
#include <random>
#include <unordered_set>

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
            if (!lower.empty() && lower.front() != L'.')
            {
                lower.insert(lower.begin(), L'.');
            }
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

        wm::core::TrackRecord MakeTrackRecord(std::wstring const& path, MusicProperties const* props, bool findLyrics = true)
        {
            wm::core::TrackRecord record;
            const std::string utf8Path = Utf8(path);
            record.id = wm::core::LibraryStore::MakeTrackId(utf8Path);
            record.filePath = utf8Path;

            const auto stem = std::filesystem::path(path).stem().wstring();
            const auto separator = stem.find(L" - ");
            if (separator != std::wstring::npos && separator > 0 && separator + 3 < stem.size())
            {
                record.artist = Utf8(stem.substr(0, separator));
                record.title = Utf8(stem.substr(separator + 3));
            }
            else
            {
                record.title = Utf8(stem);
            }

            if (props != nullptr)
            {
                const auto metadataTitle = props->Title();
                if (!metadataTitle.empty())
                {
                    record.title = Utf8(std::wstring_view{ metadataTitle.c_str(), metadataTitle.size() });
                }
                const auto artist = props->Artist();
                const auto album = props->Album();
                record.artist = Utf8(std::wstring_view{ artist.c_str(), artist.size() });
                record.album = Utf8(std::wstring_view{ album.c_str(), album.size() });
                record.durationMs = props->Duration().count() / 10000;
                record.bitrateKbps = props->Bitrate();
            }

            std::error_code sizeError;
            record.fileSize = std::filesystem::file_size(path, sizeError);
            if (sizeError)
            {
                record.fileSize = 0;
            }
            record.dateAdded = NowMs();
            if (findLyrics)
            {
                record.lyricPath = Utf8(FindLyricFile(path));
            }
            return record;
        }

        std::string IdOf(hstring const& value)
        {
            return Utf8(std::wstring_view{ value.c_str(), value.size() });
        }

        class ScanGuard
        {
        public:
            explicit ScanGuard(std::atomic_bool& active) : m_active(active) {}
            ~ScanGuard() { m_active.store(false); }
            ScanGuard(const ScanGuard&) = delete;
            ScanGuard& operator=(const ScanGuard&) = delete;

        private:
            std::atomic_bool& m_active;
        };
    } // namespace

    LibraryService::LibraryService()
    {
        m_dispatcher = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        m_tracks = winrt::single_threaded_observable_vector<winrt::w_music::TrackItem>();
        m_playlists = winrt::single_threaded_observable_vector<winrt::w_music::PlaylistItem>();
    }

    void LibraryService::Load()
    {
        if (m_dispatcher == nullptr)
        {
            m_dispatcher = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        }
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

    void LibraryService::ApplyTrackBatch(std::vector<wm::core::TrackRecord> batch,
                                         int scanned,
                                         std::function<void(int)> const& progress)
    {
        for (const auto& record : batch)
        {
            m_store.UpsertTrack(record);
            const auto key = Utf16(record.id);
            if (const auto it = m_trackIndex.find(key); it != m_trackIndex.end())
            {
                auto item = it->second;
                item.Title(hstring{ Utf16(record.title) });
                item.Artist(hstring{ Utf16(record.artist) });
                item.Album(hstring{ Utf16(record.album) });
                item.FilePath(hstring{ Utf16(record.filePath) });
                item.DurationMs(record.durationMs);
                item.PlayCount(record.playCount);
                item.IsFavorite(record.favorite);
            }
            else
            {
                auto item = EnsureTrackItem(record);
                m_tracks.Append(item);
                m_trackIndex.emplace(key, item);
            }
        }
        // Persist incrementally so a crash mid-scan does not lose the batch.
        if (!batch.empty())
        {
            wm::app::Diag("batch applied n=" + std::to_string(batch.size()));
            const auto now = NowMs();
            if (now - m_lastScanSaveMs >= 1000)
            {
                m_lastScanSaveMs = now;
                Save();
            }
        }
        if (progress)
        {
            try
            {
                progress(scanned);
            }
            catch (...)
            {
            }
        }
    }

    bool LibraryService::IngestFile(StorageFile const& file, MusicProperties const* props)
    {
        const std::wstring path{ file.Path().c_str() };
        if (path.empty())
        {
            return false;
        }
        m_store.UpsertTrack(MakeTrackRecord(path, props));
        return true;
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
            if (!IngestFile(file, &props))
            {
                co_return nullptr;
            }
        }
        catch (...)
        {
            if (!IngestFile(file, nullptr))
            {
                co_return nullptr;
            }
        }
        // The property await above resumes on a background thread;
        // RefreshTracks mutates the bound observable vector, so hop back first.
        co_await wm::app::ResumeOnUi();
        RefreshTracks();
        Save();
        co_return FindTrack(hstring{ Utf16(wm::core::LibraryStore::MakeTrackId(Utf8(path))) });
    }

    IAsyncOperation<int> LibraryService::ScanFolderAsync(StorageFolder folder)
    {
        int scanned = 0;
        if (folder == nullptr)
        {
            co_return scanned;
        }

        try
        {
            auto items = co_await folder.GetItemsAsync();
            co_await wm::app::ResumeOnUi();
            for (auto const& item : items)
            {
                if (item.IsOfType(StorageItemTypes::Folder))
                {
                    try
                    {
                        const int child = co_await ScanFolderAsync(item.as<StorageFolder>());
                        co_await wm::app::ResumeOnUi();
                        scanned += child;
                    }
                    catch (...)
                    {
                    }
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
                        if (IngestFile(file, nullptr))
                        {
                            ++scanned;
                        }
                    }
                    catch (...)
                    {
                    }
                }
            }
        }
        catch (...)
        {
        }
        co_return scanned;
    }

    IAsyncOperation<int> LibraryService::ScanPathAsync(std::wstring const& path, std::function<void(int)> progress)
    {
        const auto dispatcher = m_dispatcher;
        if (dispatcher == nullptr)
        {
            co_return 0;
        }

        co_await winrt::resume_background();

        int scanned = 0;
        std::vector<std::wstring> pending{ path };
        std::unordered_set<std::wstring> visited;
        std::vector<wm::core::TrackRecord> batch;
        auto pendingBatches = std::make_shared<std::atomic_int>(0);

        auto enqueueBatch = [this, dispatcher, progress, pendingBatches](
                                std::vector<wm::core::TrackRecord> records,
                                int count) {
            pendingBatches->fetch_add(1, std::memory_order_release);
            const bool queued = dispatcher.TryEnqueue(
                [this, records = std::move(records), count, progress, pendingBatches]() mutable {
                    try
                    {
                        ApplyTrackBatch(std::move(records), count, progress);
                    }
                    catch (...)
                    {
                    }
                    pendingBatches->fetch_sub(1, std::memory_order_release);
                });
            if (!queued)
            {
                pendingBatches->fetch_sub(1, std::memory_order_release);
                throw hresult_error(E_FAIL);
            }
        };

        for (std::size_t index = 0; index < pending.size(); ++index)
        {
            const std::filesystem::path current(pending[index]);
            std::error_code keyError;
            auto keyPath = std::filesystem::weakly_canonical(current, keyError);
            if (keyError)
            {
                keyPath = current;
            }
            auto key = keyPath.wstring();
            std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) {
                return static_cast<wchar_t>(towlower(c));
            });
            if (!visited.insert(std::move(key)).second)
            {
                continue;
            }

            std::error_code directoryError;
            std::filesystem::directory_iterator it(
                current,
                std::filesystem::directory_options::skip_permission_denied,
                directoryError);
            const std::filesystem::directory_iterator end;
            if (directoryError)
            {
                continue;
            }

            while (it != end)
            {
                const auto entry = *it;
                std::error_code kindError;
                if (entry.is_directory(kindError) && !kindError)
                {
                    pending.push_back(entry.path().wstring());
                }
                else
                {
                    kindError.clear();
                    if (entry.is_regular_file(kindError) && !kindError)
                    {
                        const auto extension = entry.path().extension().wstring();
                        if (IsAudioExtension(std::wstring_view{ extension.c_str(), extension.size() }))
                        {
                            batch.push_back(MakeTrackRecord(entry.path().wstring(), nullptr, false));
                            ++scanned;
                            if (batch.size() >= 100)
                            {
                                enqueueBatch(std::move(batch), scanned);
                                batch.clear();
                            }
                        }
                    }
                }

                it.increment(directoryError);
                if (directoryError)
                {
                    break;
                }
            }
        }

        enqueueBatch(std::move(batch), scanned);
        while (pendingBatches->load(std::memory_order_acquire) != 0)
        {
            co_await winrt::resume_after(std::chrono::milliseconds{ 1 });
        }
        co_return scanned;
    }

    IAsyncOperation<int> LibraryService::PickAndAddFolderAsync(
        winrt::Microsoft::UI::WindowId windowId,
        std::function<void(int)> progress)
    {
        if (m_scanInProgress.exchange(true))
        {
            co_return 0;
        }
        ScanGuard guard(m_scanInProgress);

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

        hstring token;
        try
        {
            token = StorageApplicationPermissions::FutureAccessList().Add(folder);
        }
        catch (...)
        {
        }

        const auto path = folder.Path();
        std::string reference;
        if (!path.empty())
        {
            reference = Utf8(std::wstring_view{ path.c_str(), path.size() });
        }
        else if (!token.empty())
        {
            reference = Utf8(std::wstring_view{ token.c_str(), token.size() });
        }

        // Remember (and persist) the folder before scanning so that even a
        // crash mid-scan leaves enough state to recover on the next launch.
        if (!reference.empty())
        {
            const auto& folders = m_store.Data().scanFolders;
            if (std::find(folders.begin(), folders.end(), reference) == folders.end())
            {
                m_store.Data().scanFolders.push_back(reference);
            }
        }
        Save();

        int scanned = 0;
        const bool pathScan = !path.empty();
        if (pathScan)
        {
            scanned = co_await ScanPathAsync(std::wstring{ path.c_str(), path.size() }, progress);
        }
        else
        {
            scanned = co_await ScanFolderAsync(folder);
        }
        co_await wm::app::ResumeOnUi();
        if (!pathScan)
        {
            RefreshTracks();
        }
        RefreshPlaylists();
        Save();
        co_return scanned;
    }

    IAsyncOperation<int> LibraryService::RescanAsync(std::function<void(int)> progress)
    {
        if (m_scanInProgress.exchange(true))
        {
            co_return 0;
        }
        ScanGuard guard(m_scanInProgress);

        int scanned = 0;
        int completed = 0;
        bool needsRefresh = false;
        auto report = [&progress, &completed](int count) {
            if (progress)
            {
                progress(completed + count);
            }
        };

        for (const auto& folderReference : m_store.Data().scanFolders)
        {
            const std::wstring path = Utf16(folderReference);
            std::error_code pathError;
            if (std::filesystem::is_directory(path, pathError) && !pathError)
            {
                try
                {
                    const int part = co_await ScanPathAsync(path, report);
                    co_await wm::app::ResumeOnUi();
                    scanned += part;
                    completed += part;
                }
                catch (...)
                {
                }
                continue;
            }

            StorageFolder folder = nullptr;
            try
            {
                folder = co_await StorageFolder::GetFolderFromPathAsync(hstring{ Utf16(folderReference) });
            }
            catch (...)
            {
            }

            if (folder == nullptr)
            {
                try
                {
                    const auto access = StorageApplicationPermissions::FutureAccessList();
                    folder = co_await access.GetFolderAsync(hstring{ Utf16(folderReference) });
                }
                catch (...)
                {
                }
            }

            if (folder != nullptr)
            {
                try
                {
                    const int part = co_await ScanFolderAsync(folder);
                    co_await wm::app::ResumeOnUi();
                    scanned += part;
                    completed += part;
                    needsRefresh = true;
                }
                catch (...)
                {
                }
            }
        }

        co_await wm::app::ResumeOnUi();
        const int removed = PruneMissing();
        if (needsRefresh || removed > 0)
        {
            RefreshTracks();
        }
        RefreshPlaylists();
        Save();
        co_return scanned;
    }

    int LibraryService::PruneMissing()
    {
        std::vector<std::string> missing;
        for (const auto& track : m_store.Data().tracks)
        {
            if (track.filePath.empty())
            {
                missing.push_back(track.id);
                continue;
            }
            std::error_code ec;
            const bool exists = std::filesystem::exists(Utf16(track.filePath), ec);
            if (!ec && !exists)
            {
                missing.push_back(track.id);
            }
        }
        for (const auto& id : missing)
        {
            m_store.RemoveTrack(id);
        }
        return static_cast<int>(missing.size());
    }

    std::size_t LibraryService::FolderCount() const noexcept
    {
        return m_store.Data().scanFolders.size();
    }

    std::vector<std::wstring> LibraryService::FolderPaths() const
    {
        // Only entries that still resolve to a real directory: the store may
        // also carry FutureAccessList tokens (packaged-mode leftovers), which
        // mean nothing to the recommendation engine.
        std::vector<std::wstring> paths;
        for (const auto& reference : m_store.Data().scanFolders)
        {
            const std::wstring path = Utf16(reference);
            std::error_code ec;
            if (std::filesystem::is_directory(path, ec) && !ec)
            {
                paths.push_back(path);
            }
        }
        return paths;
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
