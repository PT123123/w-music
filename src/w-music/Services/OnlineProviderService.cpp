#include "pch.h"

#include "Services/OnlineProviderService.h"
#include "Services/AppPaths.h"
#include "Services/BuiltinProviders.h"
#include "Services/LibraryService.h"
#include "Services/Services.h"

#include "Models/OnlineTrackItem.h"
#include "Models/TrackItem.h"

#include <wm/core/ProviderEngine.h>

#include <algorithm>
#include <fstream>
#include <sstream>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Storage;
using namespace Windows::Web::Http;

namespace wm::app
{
    namespace
    {
        std::string Narrow(hstring const& value)
        {
            if (value.empty())
            {
                return {};
            }
            return Utf8(std::wstring_view{ value.c_str(), value.size() });
        }

        hstring Wide(std::string const& value)
        {
            return hstring{ Utf16(value) };
        }

        std::wstring EnvironmentValue(wchar_t const* name)
        {
            wchar_t buffer[4096]{};
            constexpr DWORD kCapacity = 4096;
            const DWORD length = ::GetEnvironmentVariableW(name, buffer, kCapacity);
            if (length == 0 || length >= kCapacity)
            {
                return {};
            }
            return std::wstring(buffer, static_cast<std::size_t>(length));
        }

        std::vector<std::wstring> SplitSemicolon(std::wstring const& value)
        {
            std::vector<std::wstring> out;
            std::wstringstream stream(value);
            std::wstring part;
            while (std::getline(stream, part, L';'))
            {
                if (!part.empty())
                {
                    out.push_back(part);
                }
            }
            return out;
        }

        /// A plain UA so hosts can identify us; adapters override it per source.
        constexpr wchar_t kDefaultUserAgent[] = L"w-music/1.0 (online discovery)";

        void ApplyAdapterHeaders(HttpRequestMessage const& message, wm::core::ProviderAdapter const* adapter)
        {
            bool hasUserAgent = false;
            if (adapter != nullptr)
            {
                for (auto const& [name, value] : adapter->headers)
                {
                    message.Headers().TryAppendWithoutValidation(Wide(name), Wide(value));
                    if (_stricmp(name.c_str(), "user-agent") == 0)
                    {
                        hasUserAgent = true;
                    }
                }
            }
            if (!hasUserAgent)
            {
                message.Headers().TryAppendWithoutValidation(L"User-Agent", kDefaultUserAgent);
            }
        }

        /// Directories that are searched for *.json adapters, in priority order.
        /// None of them live inside the repository.
        std::vector<std::filesystem::path> ProviderSearchPaths()
        {
            std::vector<std::filesystem::path> out;

            for (std::wstring const& dir : SplitSemicolon(EnvironmentValue(L"WMUSIC_PROVIDER_DIR")))
            {
                out.emplace_back(dir);
            }

            const std::wstring localAppData = EnvironmentValue(L"LOCALAPPDATA");
            if (!localAppData.empty())
            {
                out.push_back(std::filesystem::path(localAppData) / L"w-music" / L"providers");
            }

            wchar_t modulePath[MAX_PATH]{};
            if (::GetModuleFileNameW(nullptr, modulePath, MAX_PATH) > 0)
            {
                std::filesystem::path exe(modulePath);
                out.push_back(exe.parent_path() / L"adapters");
                // Developer layout: bin/<arch>/<config> next to the solution.
                out.push_back(exe.parent_path().parent_path().parent_path() / L"adapters");
            }
            return out;
        }

        std::wstring SanitizeFileName(std::wstring const& name)
        {
            static const std::wstring kForbidden = L"\\/:*?\"<>|";
            std::wstring out;
            for (wchar_t c : name)
            {
                out.push_back(kForbidden.find(c) == std::wstring::npos ? c : L'_');
            }
            while (!out.empty() && (out.back() == L' ' || out.back() == L'.'))
            {
                out.pop_back();
            }
            return out.empty() ? std::wstring(L"track") : out;
        }

        std::wstring ExtensionOfUrl(std::string const& url)
        {
            const std::size_t query = url.find_first_of("?#");
            const std::string head = url.substr(0, query);
            const std::size_t dot = head.rfind('.');
            if (dot == std::string::npos || dot + 6 < head.size())
            {
                return L".mp3";
            }
            std::wstring extension = Utf16(head.substr(dot));
            std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) {
                return static_cast<wchar_t>(towlower(c));
            });
            if (extension.size() > 5)
            {
                return L".mp3";
            }
            return extension;
        }

        winrt::w_music::OnlineTrackItem ToItem(wm::core::OnlineTrack const& track)
        {
            auto item = winrt::make<winrt::w_music::implementation::OnlineTrackItem>();
            item.SourceId(Wide(track.sourceId));
            item.Id(Wide(track.id));
            item.Title(Wide(track.title));
            item.Artist(Wide(track.artist));
            item.Album(Wide(track.album));
            item.DurationText(Wide(track.durationText));
            item.DurationMs(static_cast<int64_t>(track.durationSec) * 1000);
            item.PlayUrl(Wide(track.playUrl));
            item.DownloadUrl(Wide(track.downloadUrl));
            item.CoverUrl(Wide(track.coverUrl));
            item.DetailUrl(Wide(track.detailUrl));

            std::string extra;
            for (auto const& [key, value] : track.extra)
            {
                if (!extra.empty())
                {
                    extra += ", ";
                }
                extra += key + "=" + value;
            }
            item.ExtraText(Wide(extra));
            return item;
        }

        wm::core::OnlineTrack ToCore(winrt::w_music::OnlineTrackItem const& item)
        {
            wm::core::OnlineTrack track;
            track.sourceId = Narrow(item.SourceId());
            track.id = Narrow(item.Id());
            track.title = Narrow(item.Title());
            track.artist = Narrow(item.Artist());
            track.album = Narrow(item.Album());
            track.playUrl = Narrow(item.PlayUrl());
            track.downloadUrl = Narrow(item.DownloadUrl());
            track.detailUrl = Narrow(item.DetailUrl());
            return track;
        }
    } // namespace

    OnlineProviderService::OnlineProviderService()
    {
        m_http = HttpClient{};
    }

    void OnlineProviderService::Reload()
    {
        m_adapters.clear();
        m_searchPaths.clear();

        // 1. Built-ins for freely licensed sources (zero configuration).
        for (wm::core::ProviderAdapter& adapter : BuiltinAdapters())
        {
            LoadedAdapter loaded;
            loaded.adapter = std::move(adapter);
            loaded.path = L"<built-in>";
            loaded.builtin = true;
            m_adapters.push_back(std::move(loaded));
        }

        // 2. External adapter files; an id shared with an existing entry
        //    replaces it (so users can override a built-in).
        for (std::filesystem::path const& dir : ProviderSearchPaths())
        {
            m_searchPaths.push_back(dir.wstring());
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec))
            {
                continue;
            }
            for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            {
                std::filesystem::path const& entry = it->path();
                if (!entry.has_extension() || entry.extension() != L".json")
                {
                    continue;
                }
                std::ifstream in(entry, std::ios::binary);
                if (!in)
                {
                    continue;
                }
                const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                std::string error;
                auto parsed = wm::core::ParseAdapter(text, &error);
                if (!parsed)
                {
                    continue;
                }
                std::wstring const path = entry.wstring();
                auto existing = std::find_if(m_adapters.begin(), m_adapters.end(),
                                             [&parsed](LoadedAdapter const& loaded) {
                                                 return loaded.adapter.id == parsed->id;
                                             });
                if (existing != m_adapters.end())
                {
                    existing->adapter = std::move(*parsed);
                    existing->path = path;
                    existing->builtin = false;
                }
                else
                {
                    LoadedAdapter loaded;
                    loaded.adapter = std::move(*parsed);
                    loaded.path = path;
                    loaded.builtin = false;
                    m_adapters.push_back(std::move(loaded));
                }
            }
        }

        std::size_t builtins = 0;
        for (LoadedAdapter const& loaded : m_adapters)
        {
            if (loaded.builtin)
            {
                ++builtins;
            }
        }

        std::wstring status;
        if (m_adapters.empty())
        {
            status = L"未加载任何在线源。把适配器 JSON 放到 %LOCALAPPDATA%\\w-music\\providers 或设置 WMUSIC_PROVIDER_DIR。";
        }
        else
        {
            status = L"已加载 " + std::to_wstring(m_adapters.size()) + L" 个在线源";
            if (builtins > 0 && builtins < m_adapters.size())
            {
                status += L"（内置 " + std::to_wstring(builtins) + L" / 外部 " +
                          std::to_wstring(m_adapters.size() - builtins) + L"）";
            }
            else if (builtins > 0)
            {
                status += L"（内置）";
            }
            status += L"：";
            for (LoadedAdapter const& loaded : m_adapters)
            {
                status += L" " + Utf16(loaded.adapter.name) + L"(" + Utf16(loaded.adapter.id) + L")";
            }
        }
        m_status = status;
    }

    std::vector<hstring> OnlineProviderService::AdapterNames() const
    {
        std::vector<hstring> out;
        for (LoadedAdapter const& loaded : m_adapters)
        {
            std::string name = loaded.adapter.name;
            if (loaded.builtin)
            {
                name += " ·内置";
            }
            out.push_back(hstring{ Utf16(name + " (" + loaded.adapter.id + ")") });
        }
        return out;
    }

    hstring OnlineProviderService::AdapterIdAt(int32_t index) const
    {
        if (index < 0 || static_cast<std::size_t>(index) >= m_adapters.size())
        {
            return {};
        }
        return hstring{ Utf16(m_adapters[static_cast<std::size_t>(index)].adapter.id) };
    }

    hstring OnlineProviderService::AdapterNoteAt(int32_t index) const
    {
        if (index < 0 || static_cast<std::size_t>(index) >= m_adapters.size())
        {
            return {};
        }
        return hstring{ Utf16(m_adapters[static_cast<std::size_t>(index)].adapter.note) };
    }

    hstring OnlineProviderService::StatusText() const
    {
        return hstring{ m_status };
    }

    wm::core::ProviderAdapter const* OnlineProviderService::Find(hstring const& adapterId) const
    {
        const std::string wanted = Narrow(adapterId);
        for (LoadedAdapter const& loaded : m_adapters)
        {
            if (loaded.adapter.id == wanted)
            {
                return &loaded.adapter;
            }
        }
        if (!m_adapters.empty())
        {
            return &m_adapters.front().adapter;
        }
        return nullptr;
    }

    wm::core::HttpResponse OnlineProviderService::Fetch(wm::core::HttpRequest const& request) const
    {
        wm::core::HttpResponse out;
        try
        {
            HttpRequestMessage message{ HttpMethod{ Wide(request.method) }, Uri{ Wide(request.url) } };
            for (auto const& [name, value] : request.headers)
            {
                message.Headers().TryAppendWithoutValidation(Wide(name), Wide(value));
            }
            if (!request.body.empty())
            {
                message.Content(HttpStringContent{ Wide(request.body) });
            }

            auto response = m_http.SendRequestAsync(message).get();
            out.status = static_cast<int>(response.StatusCode());
            if (response.Content() != nullptr)
            {
                out.body = winrt::to_string(response.Content().ReadAsStringAsync().get());
            }
            for (auto const& header : response.Headers())
            {
                out.headers[winrt::to_string(header.Key())] = winrt::to_string(header.Value());
            }
        }
        catch (...)
        {
            out.status = 0;
        }
        return out;
    }

    IAsyncOperation<IVector<winrt::w_music::OnlineTrackItem>> OnlineProviderService::SearchAsync(hstring adapterId, hstring query)
    {
        winrt::apartment_context ui;
        co_await winrt::resume_background();

        std::vector<winrt::w_music::OnlineTrackItem> built;
        if (wm::core::ProviderAdapter const* adapter = Find(adapterId))
        {
            wm::core::ProviderEngine engine(*adapter, [this](wm::core::HttpRequest const& request) {
                return Fetch(request);
            });
            const auto result = engine.Search(Narrow(query), 40);
            for (wm::core::OnlineTrack const& track : result.tracks)
            {
                built.push_back(ToItem(track));
            }
        }

        co_await ui;
        auto vector = winrt::single_threaded_vector<winrt::w_music::OnlineTrackItem>();
        for (auto const& item : built)
        {
            vector.Append(item);
        }
        co_return vector;
    }

    IAsyncOperation<winrt::w_music::OnlineTrackItem> OnlineProviderService::ResolveAsync(
        winrt::w_music::OnlineTrackItem item)
    {
        winrt::apartment_context ui;
        co_await winrt::resume_background();

        if (item != nullptr && Narrow(item.PlayUrl()).empty() && Narrow(item.DownloadUrl()).empty())
        {
            if (wm::core::ProviderAdapter const* adapter = Find(item.SourceId()))
            {
                wm::core::ProviderEngine engine(*adapter, [this](wm::core::HttpRequest const& request) {
                    return Fetch(request);
                });
                const auto resolved = engine.Resolve(ToCore(item));
                if (resolved.ok && !resolved.tracks.empty())
                {
                    wm::core::OnlineTrack const& track = resolved.tracks.front();
                    item.PlayUrl(Wide(track.playUrl));
                    item.DownloadUrl(Wide(track.downloadUrl));
                    if (!track.durationText.empty())
                    {
                        item.DurationText(Wide(track.durationText));
                        item.DurationMs(static_cast<int64_t>(track.durationSec) * 1000);
                    }
                }
            }
        }

        co_await ui;
        co_return item;
    }

    bool OnlineProviderService::CachesPreview(hstring const& adapterId) const
    {
        wm::core::ProviderAdapter const* adapter = Find(adapterId);
        return adapter != nullptr && adapter->preview == "cache";
    }

    IAsyncOperation<hstring> OnlineProviderService::CachePreviewAsync(winrt::w_music::OnlineTrackItem item)
    {
        winrt::apartment_context ui;
        co_await winrt::resume_background();

        hstring result;
        try
        {
            std::string url = Narrow(item.DownloadUrl());
            if (url.empty())
            {
                url = Narrow(item.PlayUrl());
            }
            if (item != nullptr && !url.empty())
            {
                const std::wstring key = std::wstring{ Utf16(Narrow(item.SourceId())) } + L"_" +
                                         SanitizeFileName(item.Id().empty()
                                             ? std::wstring{ item.Title().c_str() }
                                             : std::wstring{ item.Id().c_str() });
                const std::wstring fileName = key + ExtensionOfUrl(url);

                auto folder = ApplicationData::Current().LocalFolder()
                                  .CreateFolderAsync(L"PreviewCache", CreationCollisionOption::OpenIfExists).get();

                auto file = folder.TryGetItemAsync(fileName).get().try_as<StorageFile>();
                if (file == nullptr)
                {
                    file = folder.CreateFileAsync(hstring{ fileName }, CreationCollisionOption::FailIfExists).get();
                    if (!DownloadTo(file, url, Find(item.SourceId())))
                    {
                        file = nullptr;
                    }
                }
                if (file != nullptr)
                {
                    result = hstring{ file.Path() };
                }
            }
        }
        catch (...)
        {
            result = hstring{};
        }

        co_await ui;
        co_return result;
    }

    bool OnlineProviderService::DownloadTo(StorageFile const& file,
                                           std::string const& url,
                                           wm::core::ProviderAdapter const* adapter) const
    {
        try
        {
            HttpRequestMessage message{ HttpMethod{ hstring{ L"GET" } }, Uri{ Wide(url) } };
            ApplyAdapterHeaders(message, adapter);

            auto response = m_http.SendRequestAsync(message, HttpCompletionOption::ResponseHeadersRead).get();
            if (!response.IsSuccessStatusCode() || response.Content() == nullptr)
            {
                return false;
            }
            auto buffer = response.Content().ReadAsBufferAsync().get();
            if (buffer.Length() == 0)
            {
                return false;
            }
            FileIO::WriteBufferAsync(file, buffer).get();
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    IAsyncOperation<winrt::w_music::TrackItem> OnlineProviderService::DownloadAsync(winrt::w_music::OnlineTrackItem item)
    {
        winrt::apartment_context ui;
        co_await winrt::resume_background();

        if (item == nullptr)
        {
            co_return nullptr;
        }

        std::string url = Narrow(item.DownloadUrl());
        const wm::core::ProviderAdapter* adapter = Find(item.SourceId());
        if (adapter != nullptr && (url.empty() || adapter->detailRequired))
        {
            wm::core::ProviderEngine engine(*adapter, [this](wm::core::HttpRequest const& request) {
                return Fetch(request);
            });
            const auto resolved = engine.Resolve(ToCore(item));
            if (resolved.ok && !resolved.tracks.empty())
            {
                url = !resolved.tracks.front().downloadUrl.empty()
                    ? resolved.tracks.front().downloadUrl
                    : resolved.tracks.front().playUrl;
            }
        }
        if (url.empty())
        {
            url = Narrow(item.PlayUrl());
        }
        if (url.empty())
        {
            co_return nullptr;
        }

        try
        {
            const std::wstring fileName = SanitizeFileName(Utf16(Narrow(item.Title()))) + ExtensionOfUrl(url);

            auto folder = ApplicationData::Current().LocalFolder()
                              .CreateFolderAsync(L"Downloads", CreationCollisionOption::OpenIfExists).get();
            auto file = folder.CreateFileAsync(hstring{ fileName }, CreationCollisionOption::ReplaceExisting).get();
            if (!DownloadTo(file, url, adapter))
            {
                co_return nullptr;
            }

            // Save the lyric next to the audio so the scanner picks it up.
            if (adapter != nullptr && adapter->lyric.enabled)
            {
                wm::core::ProviderEngine engine(*adapter, [this](wm::core::HttpRequest const& request) {
                    return Fetch(request);
                });
                const auto lyric = engine.Lyric(ToCore(item));
                if (lyric.ok && !lyric.text.empty())
                {
                    auto lyricFile = folder.CreateFileAsync(
                        hstring{ std::filesystem::path(fileName).stem().wstring() + L".lrc" },
                        CreationCollisionOption::ReplaceExisting).get();
                    FileIO::WriteTextAsync(lyricFile, Wide(lyric.text)).get();
                }
            }

            auto track = wm::app::Library().ImportFileAsync(file).get();
            co_await ui;
            co_return track;
        }
        catch (...)
        {
            co_return nullptr;
        }
    }

    void OnlineProviderService::PlayOnline(winrt::w_music::OnlineTrackItem const& item)
    {
        if (item == nullptr)
        {
            return;
        }
        std::string url = Narrow(item.PlayUrl());
        if (url.empty())
        {
            url = Narrow(item.DownloadUrl());
        }
        if (url.empty())
        {
            return;
        }

        auto track = winrt::make<winrt::w_music::implementation::TrackItem>();
        track.Id(hstring{ L"online:" + Utf16(Narrow(item.SourceId())) + L":" + Utf16(Narrow(item.Id())) });
        track.Title(item.Title().empty() ? hstring{ L"在线播放" } : item.Title());
        track.Artist(item.Artist());
        track.Album(item.Album());
        track.DurationMs(item.DurationMs());
        track.FilePath(Wide(url));

        wm::app::Player().PlayTrack(track);
    }

    // ---- helpers for the built-in a-music replica sources -----------------

    wm::core::FetchFn OnlineProviderService::Transport() const
    {
        return [this](wm::core::HttpRequest const& request) {
            return Fetch(request);
        };
    }

    std::wstring OnlineProviderService::DownloadsDirectory() const
    {
        try
        {
            auto folder = ApplicationData::Current().LocalFolder()
                              .CreateFolderAsync(L"Downloads", CreationCollisionOption::OpenIfExists).get();
            return std::wstring{ folder.Path().c_str() };
        }
        catch (...)
        {
            return {};
        }
    }

    bool OnlineProviderService::DownloadUrlToFile(std::wstring const& filePath,
                                                  std::string const& url,
                                                  std::map<std::string, std::string> const& headers,
                                                  std::uint64_t expectedBytes,
                                                  std::function<void(double)> progress) const
    {
        try
        {
            HttpRequestMessage message{ HttpMethod{ hstring{ L"GET" } }, Uri{ Wide(url) } };
            bool hasUserAgent = false;
            for (auto const& [name, value] : headers)
            {
                message.Headers().TryAppendWithoutValidation(Wide(name), Wide(value));
                if (_stricmp(name.c_str(), "user-agent") == 0)
                {
                    hasUserAgent = true;
                }
            }
            if (!hasUserAgent)
            {
                message.Headers().TryAppendWithoutValidation(L"User-Agent", kDefaultUserAgent);
            }

            auto response = m_http.SendRequestAsync(message, HttpCompletionOption::ResponseHeadersRead).get();
            if (!response.IsSuccessStatusCode() || response.Content() == nullptr)
            {
                return false;
            }

            std::uint64_t total = expectedBytes;
            try
            {
                // ContentLength is an IReference<uint64_t>, not a plain number.
                auto const lengthRef = response.Content().Headers().ContentLength();
                std::uint64_t const length = lengthRef ? lengthRef.Value() : 0;
                if (length > 0)
                {
                    total = length;
                }
            }
            catch (...)
            {
            }

            auto report = [&progress, total](std::uint64_t done) {
                if (progress != nullptr)
                {
                    progress(total > 0
                        ? static_cast<double>(done) / static_cast<double>(total)
                        : 0.0);
                }
            };

            auto input = response.Content().ReadAsInputStreamAsync().get();
            auto file = StorageFile::GetFileFromPathAsync(filePath).get();
            auto target = file.OpenAsync(FileAccessMode::ReadWrite).get();
            auto output = target.GetOutputStreamAt(0);

            winrt::Windows::Storage::Streams::Buffer buffer{ 256 * 1024 };
            std::uint64_t done = 0;
            while (true)
            {
                auto read = input.ReadAsync(buffer, buffer.Capacity(),
                                            winrt::Windows::Storage::Streams::InputStreamOptions::None).get();
                if (read.Length() == 0)
                {
                    break;
                }
                output.WriteAsync(read).get();
                done += read.Length();
                report(done);
            }
            output.FlushAsync().get();
            return true;
        }
        catch (...)
        {
            // Remove a partial file so a failed download is never imported.
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path{ filePath }, ec);
            return false;
        }
    }
} // namespace wm::app
