#include "pch.h"

#include "Services/LibraryService.h"
#include "Services/AppPaths.h"

#include "Models/LyricLineItem.h"
#include "Models/PlaylistItem.h"
#include "Models/TrackItem.h"

#include <shobjidl.h>
#include <shobjidl_core.h>

#include <algorithm>
#include <cstdio>
#include <random>
#include <unordered_set>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Storage;
using namespace Windows::Storage::FileProperties;
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

        std::string HrText(winrt::hresult hr)
        {
            char buffer[16]{};
            snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<uint32_t>(static_cast<int32_t>(hr)));
            return buffer;
        }

        /// A step that only re-renders or re-persists state the caller already
        /// produced. Failing it must not undo that work -- and because these run
        /// inside coroutines nobody awaits, the error is named (step + HRESULT)
        /// in diag.log and swallowed here rather than unwinding the caller.
        template <typename Fn>
        void Guarded(char const* step, Fn&& fn)
        {
            try
            {
                fn();
            }
            catch (hresult_error const& exception)
            {
                Diag(std::string{ "scan tail " } + step + " hr=" + HrText(exception.code()) +
                     " msg=" + Utf8(exception.message().c_str()));
            }
            catch (...)
            {
                Diag(std::string{ "scan tail " } + step + " failed");
            }
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

        /// The shell's own folder dialog: IFileOpenDialog with FOS_PICKFOLDERS.
        /// Both it and IShellItem are system-cached COM classes, so they resolve
        /// for an unpackaged process -- unlike the identity-bound WinRT types the
        /// picker used to sit on. Returns an empty string on cancel; throws the
        /// localized COM error when the dialog itself cannot be created.
        std::wstring PickFolder(HWND hwnd)
        {
            IFileOpenDialog* created = nullptr;
            winrt::check_hresult(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                                  IID_PPV_ARGS(&created)));
            winrt::com_ptr<IFileOpenDialog> dialog;
            dialog.attach(created);

            DWORD options = FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM;
            winrt::check_hresult(dialog->GetOptions(&options));
            winrt::check_hresult(dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM));

            const HRESULT shown = dialog->Show(hwnd);
            if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED))
            {
                return {};
            }
            winrt::check_hresult(shown);

            IShellItem* selectedItem = nullptr;
            winrt::check_hresult(dialog->GetResult(&selectedItem));
            winrt::com_ptr<IShellItem> item;
            item.attach(selectedItem);

            wchar_t* rawPath = nullptr;
            winrt::check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath));
            const std::wstring path{ rawPath != nullptr ? rawPath : L"" };
            CoTaskMemFree(rawPath);
            return path;
        }
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
        const std::string file = LibraryFilePath().string();
        std::string error;
        const bool loaded = m_store.Load(file, &error);
        Diag("library load file=" + Utf8(LibraryFilePath().wstring()) + " ok=" + (loaded ? "1" : "0") +
             " error=" + error);
        m_store.EnsureBuiltin(wm::core::PlaylistKind::Favorites, "我喜欢的音乐");
        m_store.EnsureBuiltin(wm::core::PlaylistKind::Recent, "最近播放");
        Diag("library loaded tracks=" + std::to_string(m_store.Data().tracks.size()) +
             " folders=" + std::to_string(m_store.Data().scanFolders.size()));
        Guarded("track list", [&] { RefreshTracks(); });
        Guarded("playlist list", [&] { RefreshPlaylists(); });
        Diag("bound lists tracks=" + std::to_string(m_tracks.Size()) +
             " playlists=" + std::to_string(m_playlists.Size()));
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
            // Not map::operator[]: that default-constructs a TrackItem, and C++/WinRT
            // builds one by *activating* w_music.TrackItem -- which an unpackaged
            // process cannot resolve, so it threw 0x80040154 (没有注册类) per row.
            m_trackIndex.insert_or_assign(Utf16(record.id), item);
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
            // insert_or_assign for the reason given in RefreshTracks.
            m_playlistIndex.insert_or_assign(Utf16(record.id), item);
        }
    }

    void LibraryService::SyncPlaylistCounts()
    {
        // A scan changes which tracks exist, never which playlists do, so the
        // bound items are updated in place instead of rebuilt: Clear()/Append()
        // here would tear down and re-create every container the sidebar shows.
        for (const auto& record : m_store.Data().playlists)
        {
            if (const auto it = m_playlistIndex.find(Utf16(record.id)); it != m_playlistIndex.end())
            {
                it->second.TrackCount(static_cast<int32_t>(record.trackIds.size()));
            }
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
            Diag("add-folder skipped: another scan is running");
            co_return 0;
        }
        ScanGuard guard(m_scanInProgress);

        // A plain shell folder dialog, not Windows.Storage.Pickers.FolderPicker:
        // that one is an application-identity API and the app is unpackaged.
        const HWND hwnd = winrt::Microsoft::UI::GetWindowFromWindowId(windowId);
        const std::wstring path = PickFolder(hwnd);
        if (path.empty())
        {
            co_return 0;
        }

        // Remember (and persist) the folder before scanning so that even a
        // crash mid-scan leaves enough state to recover on the next launch.
        const std::string reference = Utf8(path);
        {
            const auto& folders = m_store.Data().scanFolders;
            if (std::find(folders.begin(), folders.end(), reference) == folders.end())
            {
                m_store.Data().scanFolders.push_back(reference);
            }
        }
        Save();

        // ApplyTrackBatch keeps the bound collection in step with each batch, so
        // there is no rebuild - and no second persist - after the scan.
        const int scanned = co_await ScanPathAsync(path, progress);
        co_await wm::app::ResumeOnUi();
        Diag("add-folder scanned=" + std::to_string(scanned));
        Guarded("playlist counts", [&] { SyncPlaylistCounts(); });
        Guarded("save", [&] { Save(); });
        co_return scanned;
    }

    IAsyncOperation<int> LibraryService::RescanAsync(std::function<void(int)> progress)
    {
        if (m_scanInProgress.exchange(true))
        {
            Diag("rescan skipped: another scan is running");
            co_return 0;
        }
        ScanGuard guard(m_scanInProgress);

        int scanned = 0;
        int completed = 0;
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
            if (!std::filesystem::is_directory(path, pathError) || pathError)
            {
                // An unplugged drive, or a FutureAccessList token left over from
                // the packaged build -- which has no meaning here: the store keys
                // on absolute paths precisely so a reinstall cannot lose a list.
                Diag("rescan skip: " + folderReference);
                continue;
            }

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
        }

        co_await wm::app::ResumeOnUi();
        const int removed = PruneMissing();
        Diag("rescan scanned=" + std::to_string(scanned) + " removed=" + std::to_string(removed));
        Guarded("track list", [&] {
            if (removed > 0)
            {
                RefreshTracks();
            }
        });
        Guarded("playlist counts", [&] { SyncPlaylistCounts(); });
        Guarded("save", [&] { Save(); });
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
        m_playlistIndex.insert_or_assign(Utf16(record.id), item);
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
