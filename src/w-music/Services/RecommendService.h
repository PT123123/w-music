#pragma once

/// Bridge to the local music recommendation engine.
///
/// The engine is a separate project kept in its own git repository:
///
///     music-recommend -- https://github.com/PT123123/music-recommend
///     (git@github.com:PT123123/music-recommend.git)
///
/// It extracts MIR features (MFCC / chroma / rhythm / harmony / ...) from the
/// local audio files into its own SQLite + FAISS store and serves a FastAPI
/// HTTP interface on 127.0.0.1 (see its scripts/run_server.py and
/// src/music_recommender/api/server.py). This service:
///
///   1. resolves the engine checkout (env %WMUSIC_RECOMMEND_DIR% > the
///      settings.json entry > <Desktop>\music-recommend),
///   2. spawns `<repo>\.venv\Scripts\python.exe scripts\run_server.py
///      --port <p>` as a hidden child process, bound to a kill-on-close job
///      object so it can never outlive w-music (an already-healthy engine on
///      the same port is reused instead),
///   3. translates the /v1 endpoints into WinRT-friendly async calls whose
///      results are ready for XAML (RecommendItem / CategoryItem vectors).
///
/// Responses are parsed with wm::core::json. Display names fall back to the
/// file name when a track carries no tags (the engine never invents titles).

#include "pch.h"

// IHttpRequestHeaderCollection lives in the Headers namespace; see
// OnlineProviderService.h for why this include is needed.
#include <winrt/Windows.Web.Http.Headers.h>

#include "Models/CategoryItem.h"
#include "Models/RecommendItem.h"

#include <wm/core/Json.h>
#include <wm/core/RecommendCache.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace wm::app
{
    class RecommendService
    {
    public:
        RecommendService();
        ~RecommendService();

        RecommendService(RecommendService const&) = delete;
        RecommendService& operator=(RecommendService const&) = delete;

        // ---- configuration ----
        /// <Desktop>\music-recommend, used when neither the env var nor
        /// settings.json point somewhere else.
        static std::wstring DefaultRepoDir();
        /// env > settings > default; empty result means "not configured and
        /// the default location does not exist either".
        std::wstring RepoDir() const;
        unsigned short Port() const;

        // ---- engine lifecycle ----
        /// Makes sure a healthy engine answers on 127.0.0.1:<Port>, spawning
        /// the Python process when needed and polling /v1/health. Returns an
        /// empty string on success, a user-displayable error otherwise.
        /// Repeated calls while healthy return immediately.
        winrt::Windows::Foundation::IAsyncOperation<hstring> EnsureStartedAsync();
        /// True once EnsureStartedAsync succeeded (cheap, no I/O).
        bool IsReady() const noexcept { return m_ready; }
        /// Terminates the engine we spawned. An engine started outside this
        /// process is left alone; the job object also kills ours if w-music
        /// ever exits without calling this.
        void Shutdown();

        /// Where the engine is, for the UI's waiting hint. Cheap, no I/O.
        enum class Stage : int
        {
            Stopped = 0,   // not running, nobody has asked yet
            Starting = 1,  // spawned or probing; the cold start is the slow one
            Ready = 2,     // /v1/health answered
            Analyzing = 3, // busy extracting features; queries queue behind it
        };
        Stage CurrentStage() const noexcept { return static_cast<Stage>(m_stage.load()); }
        /// Starts the engine without waiting for the page -- the app calls this
        /// in the background once the user has shown they use 个性推荐.
        winrt::Windows::Foundation::IAsyncAction PrewarmAsync();

        // ---- disk cache (%LOCALAPPDATA%\w-music\recommend-cache.json) ----
        // The engine boots in seconds and recomputes its rankings, so the page
        // paints the last answer immediately and replaces it when the live one
        // arrives. These reads never touch the network or a thread switch.
        enum class Answer : int { Feed = 0, Category = 1, Text = 2 };

        /// Cached rows of that answer; empty when there is none, it was computed
        /// for a different library, or it is older than two weeks.
        winrt::Windows::Foundation::Collections::IVectorView<winrt::w_music::RecommendItem>
            CachedRows(Answer kind, hstring const& id, int32_t limit) const;
        /// The honesty note that came with the cached answer (may be empty).
        hstring CachedNote(Answer kind, hstring const& id, int32_t limit) const;
        /// "刚刚 / 12 分钟前 / 3 小时前 / 2 天前"; empty when nothing is cached.
        hstring CachedAgeText(Answer kind, hstring const& id, int32_t limit) const;
        /// Last category chips (preset + auto), for the instant chip row.
        winrt::Windows::Foundation::Collections::IVectorView<winrt::w_music::CategoryItem> CachedChips() const;
        /// Last auto-discovery caption, or empty.
        hstring CachedDiscoveryText() const;

        /// Library size namespaces the cache: an answer computed over another
        /// library is never painted. Call whenever the library is (re)loaded.
        void SetLibrarySize(int32_t trackCount);
        /// Forget every cached answer: after 分析曲库 the old ones describe a
        /// library the engine no longer has.
        void InvalidateCache();

        // ---- /v1 endpoints ----
        /// GET /v1/categories -> preset + auto-discovered category chips, each
        /// carrying source / note / support so the UI can tell a hand-written
        /// preset from a cluster of the current library.
        winrt::Windows::Foundation::IAsyncOperation<
            winrt::Windows::Foundation::Collections::IVectorView<winrt::w_music::CategoryItem>>
            GetCategoriesAsync();

        /// GET /v1/categories/discovery -> one honest line about the
        /// unsupervised pass (library size, chosen k, silhouette, or why the
        /// engine refused to cluster). Used as the auto row's caption.
        winrt::Windows::Foundation::IAsyncOperation<hstring> DiscoveryTextAsync();

        /// POST /v1/feed/next -> the personalized feed. |excludeTrackIds| is
        /// the "换一批" support: rows to keep out of this batch. Coroutine
        /// parameters are taken by value so the frame owns them.
        winrt::Windows::Foundation::IAsyncOperation<
            winrt::Windows::Foundation::Collections::IVectorView<winrt::w_music::RecommendItem>>
            GetFeedAsync(int32_t limit, std::vector<hstring> excludeTrackIds);

        /// POST /v1/recommend/similar with a file path seed (the engine's
        /// content-hash ids are unknown to the w-music library).
        winrt::Windows::Foundation::IAsyncOperation<
            winrt::Windows::Foundation::Collections::IVectorView<winrt::w_music::RecommendItem>>
            GetSimilarByPathAsync(hstring filePath, int32_t limit);

        /// POST /v1/recommend/category with a category_id -> rows of one
        /// category. Whatever the engine says about the answer's trustworthiness
        /// lands in LastCategoryNote().
        winrt::Windows::Foundation::IAsyncOperation<
            winrt::Windows::Foundation::Collections::IVectorView<winrt::w_music::RecommendItem>>
            GetCategoryAsync(hstring categoryId, int32_t limit);

        /// POST /v1/recommend/category with `text` -> the free Chinese entry
        /// ("来点安静又明亮的纯音乐"). Words no local measurement can answer
        /// come back in LastCategoryNote() instead of being mapped onto a
        /// nearby dimension.
        winrt::Windows::Foundation::IAsyncOperation<
            winrt::Windows::Foundation::Collections::IVectorView<winrt::w_music::RecommendItem>>
            SearchByTextAsync(hstring text, int32_t limit);

        /// Honesty note of the most recent category / text answer: support
        /// size, estimated filters, words the engine refused to guess at.
        /// Empty when the answer needed no qualification. Cleared by the next
        /// category call.
        hstring LastCategoryNote() const noexcept { return m_categoryNote; }

        /// POST /v1/feed/feedback ("like" / "dislike" / "play" / ...). Fire
        /// and forget from the UI's perspective; failures only set LastError.
        winrt::Windows::Foundation::IAsyncAction
            SendFeedbackAsync(hstring trackId, hstring eventId);

        /// Same endpoint for tracks the w-music library knows only by path
        /// (engine ids are content hashes w-music cannot compute): the engine
        /// resolves file_path -> track_id itself. Fire and forget.
        winrt::Windows::Foundation::IAsyncAction
            SendFeedbackForPathAsync(hstring filePath, hstring eventId);

        /// POST /v1/feed/reset?scope=all.
        winrt::Windows::Foundation::IAsyncAction ResetTasteAsync();

        /// POST /v1/library/scan for each folder of the w-music library, then
        /// rebuild embeddings + FAISS (the endpoint does that in one shot).
        /// First analysis of a large folder takes minutes; unchanged files are
        /// skipped by the engine on later runs. Returns a one-line summary.
        winrt::Windows::Foundation::IAsyncOperation<hstring>
            AnalyzeFoldersAsync(std::vector<std::wstring> folders);

        /// GET /v1/feed/state -> one-line short/medium/long interest summary
        /// for the status strip. Returns an error text on failure.
        winrt::Windows::Foundation::IAsyncOperation<hstring> FeedStateTextAsync();

        /// Last failure text (empty when everything worked). Cleared by the
        /// next successful call.
        hstring LastError() const noexcept { return m_lastError; }

    private:
        /// True when /v1/health answers {status: ok}.
        winrt::Windows::Foundation::IAsyncOperation<bool> HealthCheckAsync();

        /// Spawns the engine child process. Returns an error text, empty on
        /// success. Runs on a background thread.
        hstring SpawnEngine();
        /// GET/POST plumbing shared by all endpoints; returns the raw JSON
        /// text (empty on failure -- m_lastError carries the reason).
        winrt::Windows::Foundation::IAsyncOperation<hstring>
            RequestJsonAsync(hstring method, std::wstring path, std::string body);
        /// Wraps RequestJsonAsync with the ensure-started handshake the UI
        /// calls before anything else. A non-empty |cacheKey| stores the raw
        /// answer in the disk cache (written from this background thread).
        winrt::Windows::Foundation::IAsyncOperation<hstring>
            CallAfterStartAsync(hstring method, std::wstring path, std::string body,
                                std::wstring cacheKey = {});

        /// Shared POST /v1/recommend/category call; |body| carries either
        /// category_id or text. |cacheKey| names the disk-cache entry.
        winrt::Windows::Foundation::IAsyncOperation<
            winrt::Windows::Foundation::Collections::IVectorView<winrt::w_music::RecommendItem>>
            RequestCategoryAsync(std::string body, std::wstring cacheKey);

        /// Parses one /v1 recommendation row ({track_id, score, reasons,
        /// file_name, display{title, artist, album, genre, language,
        /// file_path, meta_source}}).
        static winrt::w_music::RecommendItem ParseRecommendRow(wm::core::json::Value const& row);

        /// Turns the honesty metadata of a category answer into one caption.
        /// Returns empty when the engine had nothing to qualify.
        static std::wstring CategoryNoteOf(wm::core::json::Value const& answer);

        // Response parsing shared by the live calls and the cache readers, so a
        // cached answer can never be interpreted differently than a live one.
        static winrt::Windows::Foundation::Collections::IVector<winrt::w_music::RecommendItem>
            RowsOf(std::wstring_view jsonText, wchar_t const* listField);
        static winrt::Windows::Foundation::Collections::IVector<winrt::w_music::CategoryItem>
            ChipsOf(std::wstring_view jsonText);
        static hstring DiscoveryNoteOf(std::wstring_view jsonText);
        static hstring NoteOf(std::wstring_view jsonText);

        std::wstring CachePath() const;
        /// Cache key of one answer; must match between the write and the read.
        std::wstring MakeKey(Answer kind, hstring const& id, int32_t limit) const;
        /// What a cached answer has to agree with to be served at all.
        std::string Fingerprint() const;
        void CachePut(std::wstring_view key, std::wstring_view jsonText);
        /// payload + fetchedAt of a usable cached answer.
        bool CacheLookup(std::wstring const& key, std::int64_t maxAgeMs,
                         std::wstring& payloadOut, std::int64_t& fetchedAtOut) const;

        std::wstring BaseUri() const;

        winrt::Windows::Web::Http::HttpClient m_http{ nullptr };
        HANDLE m_job = nullptr;
        HANDLE m_process = nullptr;
        bool m_ready = false;
        bool m_spawnedHere = false;
        hstring m_lastError;
        hstring m_categoryNote;
        std::atomic_int m_stage{ static_cast<int>(Stage::Stopped) };

        /// Filled in the constructor rather than lazily on a page paint: the
        /// cache is one small file and the reads must stay disk-free.
        wm::core::RecommendCache m_cache;
        mutable std::mutex m_cacheMutex;
        /// Written on the UI thread when the library loads, read on the request
        /// thread for every cache touch.
        std::atomic_int32_t m_librarySize{ -1 };
    };
} // namespace wm::app
