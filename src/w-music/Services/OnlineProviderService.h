#pragma once

/// Loads online-music-source adapters and runs them.
///
/// Two kinds of adapters coexist:
///
///   1. Built-ins for freely licensed sources (see BuiltinProviders.h) that
///      make the online tab work out of the box.
///   2. User supplied plain JSON files describing how to search a site and how
///      to get a playable / downloadable URL. They are never compiled into the
///      app: they are read at runtime from
///
///        1. %WMUSIC_PROVIDER_DIR%   (semicolon separated list)
///        2. %LOCALAPPDATA%\w-music\providers
///        3. <exe folder>\adapters
///
/// An external adapter with the same "id" replaces a built-in one. See
/// adapters/README.md next to the solution for the file format.

#include "pch.h"

// IHttpRequestHeaderCollection / IHttpContentHeaderCollection live in the
// Headers namespace, and winrt/Windows.Web.Http.h only forward-declares the
// collection types -- without this the members are "auto-returning functions
// that cannot be used before they are defined" (C3779).
#include <winrt/Windows.Web.Http.Headers.h>

#include "Models/OnlineTrackItem.h"
#include "Models/TrackItem.h"

#include <wm/core/ProviderAdapter.h>

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace wm::app
{
    struct LoadedAdapter
    {
        wm::core::ProviderAdapter adapter;
        std::wstring path;
        bool builtin = false;
    };

    class OnlineProviderService
    {
    public:
        OnlineProviderService();

        /// (Re)scans the external directories. Cheap; call it before searching.
        void Reload();

        std::size_t Count() const noexcept { return m_adapters.size(); }
        /// "Name (id)" entries, ready to drop into a ComboBox. Built-ins carry
        /// a "·内置" marker.
        std::vector<hstring> AdapterNames() const;
        hstring AdapterIdAt(int32_t index) const;
        /// Free-form description of the adapter at |index| (search hints,
        /// licensing), shown under the search box.
        hstring AdapterNoteAt(int32_t index) const;
        /// One line describing what was loaded, for the discover page.
        hstring StatusText() const;
        std::vector<std::wstring> SearchPaths() const noexcept { return m_searchPaths; }

        /// Runs the adapter's search step. Returns an empty vector on failure.
        winrt::Windows::Foundation::IAsyncOperation<
            winrt::Windows::Foundation::Collections::IVector<winrt::w_music::OnlineTrackItem>>
            SearchAsync(hstring adapterId, hstring query);

        /// Fills play/download URLs of |item| through the adapter's detail step
        /// when they are still empty. Returns the item unchanged otherwise.
        winrt::Windows::Foundation::IAsyncOperation<winrt::w_music::OnlineTrackItem>
            ResolveAsync(winrt::w_music::OnlineTrackItem item);

        /// Whether previews of this source must be fetched by the app first
        /// (adapter "preview": "cache") because the host rejects players.
        bool CachesPreview(hstring const& adapterId) const;

        /// Downloads the file of |item| into the app's PreviewCache and returns
        /// the local path, or an empty string on failure. Already cached files
        /// are returned as-is.
        winrt::Windows::Foundation::IAsyncOperation<hstring>
            CachePreviewAsync(winrt::w_music::OnlineTrackItem item);

        /// Fetches the file into the app's Downloads folder and adds it to the
        /// local library. Returns nullptr when the download or the import fails.
        winrt::Windows::Foundation::IAsyncOperation<winrt::w_music::TrackItem>
            DownloadAsync(winrt::w_music::OnlineTrackItem item);

        /// Streams the remote url through the player without downloading it.
        void PlayOnline(winrt::w_music::OnlineTrackItem const& item);

        // ---- helpers for the built-in a-music replica sources ----

        /// HTTP transport for wm::core::QqSource / Net24Source, which the
        /// online page drives directly (mirroring how a-music's UI calls its
        /// QqOnlineApi / Net24Api objects).
        wm::core::FetchFn Transport() const;

        /// The download target directory (created on demand):
        /// <LocalState>\Downloads.
        std::wstring DownloadsDirectory() const;

        /// Streams |url| to |filePath| with |headers| (e.g. the browser-ish
        /// UA/Referer pairs the sources require). Reports progress as a 0..1
        /// fraction from any worker thread; |expectedBytes| is the fallback
        /// denominator when the server sends no Content-Length. Returns false
        /// on any failure (the file is removed again).
        bool DownloadUrlToFile(std::wstring const& filePath,
                               std::string const& url,
                               std::map<std::string, std::string> const& headers,
                               std::uint64_t expectedBytes,
                               std::function<void(double)> progress) const;

    private:
        wm::core::ProviderAdapter const* Find(hstring const& adapterId) const;
        /// Blocking WinRT HttpClient round trip used by the core engine.
        wm::core::HttpResponse Fetch(wm::core::HttpRequest const& request) const;
        /// GET |url| (with the adapter's headers when present) into |file|.
        bool DownloadTo(winrt::Windows::Storage::StorageFile const& file,
                        std::string const& url,
                        wm::core::ProviderAdapter const* adapter) const;

        winrt::Windows::Web::Http::HttpClient m_http{ nullptr };
        std::vector<LoadedAdapter> m_adapters;
        std::vector<std::wstring> m_searchPaths;
        std::wstring m_status;
    };
} // namespace wm::app
