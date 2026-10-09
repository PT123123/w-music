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
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace wm::app
{
    /// One section of a track, as the engine grouped it. `label` is a heuristic
    /// estimate and is often "unknown" -- `group` (A/B/C) is the part that does
    /// not need a name, it just says "this repeats".
    struct TimelineSegment
    {
        double start = 0.0;
        double end = 0.0;
        std::wstring label;
        std::wstring group;
        double boundaryConfidence = 0.0;
        double labelConfidence = 0.0;
    };

    /// A turning point the engine found on the loudness curve: it builds, peaks,
    /// pulls back, drops or releases. `confidence` is the engine's own uncalibrated
    /// model score -- the page prints it rather than letting it read as a fact.
    struct DynamicsEvent
    {
        double time = 0.0;
        std::wstring type;
        double confidence = 0.0;
        /// The engine's reason strings ("rms_rise:+14.1dB_over_18s"), joined.
        std::wstring evidence;
    };

    /// The scalar columns of the same analysis row. They come from
    /// GET /v1/tracks/{id} because /v1/analysis only carries the curves and the
    /// sections, and they are measured on the loudest window of the track, not on
    /// the whole file -- |windowStart| / |windowEnd| say which slice, and the page
    /// prints that instead of letting "161 BPM" read as a whole-song statement.
    struct TrackAnalysis
    {
        bool ok = false;
        std::wstring engineTrackId;
        /// "full_track" when the sections and the curve cover the whole file.
        bool fullTrack = false;
        std::wstring scope;

        // rhythm (window scope)
        double bpm = 0.0;
        double beatConsistency = 0.0;
        double danceability = 0.0;
        double onsetDensity = 0.0;
        // tonality and harmony (window scope, estimated)
        std::wstring key;
        std::wstring mode;
        std::wstring chordSequence;
        std::wstring chorusChordSequence;
        double harmonicRhythm = 0.0;
        double dissonance = 0.0;
        // loudness distribution
        double dynamicRange = 0.0;
        double crestFactor = 0.0;
        double rmsMean = 0.0;
        // voice and instrumentation (window scope, heuristics)
        bool hasVocal = false;
        double vocalRatio = 0.0;
        std::wstring vocalGender;
        /// 20–250 Hz 能量占比（引擎注明：不等于 bass 乐器）。
        double lowBandRatio = 0.0;
        /// HPSS 打击性成分占比（引擎注明：不是鼓组轨道数）。
        double drumRatio = 0.0;
        double lowEnergyRatio = 0.0;
        double midEnergyRatio = 0.0;
        double highEnergyRatio = 0.0;
        double spectralCentroid = 0.0;
        // structure summary
        std::wstring segmentTypeSequence;
        std::wstring groupSequence;
        int chorusRepeatCount = 0;
        // honesty payload: which numbers are estimates, and what each proxy measures
        std::wstring estimateFlags;
        std::vector<std::wstring> fieldNotes;

        double windowStart = 0.0;
        double windowEnd = 0.0;
    };

    /// 一条可以切到时间轴上显示的分析曲线（电平 / 起音率 / 低频比例 / 亮度 /
    /// 谱变化…）。通用结构而不是每种曲线一组成员：引擎加曲线时这边不用跟着改。
    /// 缺失的曲线根本不会出现在列表里——缺失不是零，零有可能是真实测量结果。
    struct AnalysisCurve
    {
        std::wstring id;
        std::wstring unit;
        std::wstring meaning;
        std::wstring scope;
        std::vector<double> times;
        std::vector<double> values;
        /// 对应点位是否可信；不可信的点画图时跳过，不补零。
        std::vector<bool> valid;
    };

    /// The full-track dynamics curve plus its sections, for the now-playing
    /// timeline. Plain C++ on purpose: a few thousand buckets boxed into WinRT
    /// vectors would cost more than the drawing.
    struct TrackTimeline
    {
        bool ok = false;
        /// Analysed by an older algorithm version: still shown, labelled as such.
        bool stale = false;
        std::wstring filePath;
        double duration = 0.0;
        /// Bucket centers in seconds (the engine's own time grid) and the
        /// normalized loudness at each -- never a 64-value blob without an axis.
        std::vector<double> curveTimes;
        std::vector<double> curve;
        /// 引擎声明可切换的全部曲线（level_db 在第一位时是默认显示）；
        /// 旧引擎回答没有 curves 块时由 ParseTimeline 从 legacy 字段合成。
        std::vector<AnalysisCurve> curves;
        std::vector<TimelineSegment> segments;
        std::vector<DynamicsEvent> events;
        /// What the curve's numbers mean: the engine's own dBFS scale, its average
        /// level and level range, plus the notes on what each proxy is not.
        double levelMeanDb = 0.0;
        double levelRangeDb = 0.0;
        double curveInterval = 0.0;
        std::wstring curveUnit;
        std::vector<std::wstring> curveNotes;
        /// 引擎描述层的确定性中文描述（纹理/空间/律动/轮廓逐句可追溯）；
        /// 空模块时为空串。v5 引擎回答才有。
        std::wstring profileText;
        /// The scalar columns of the same row (GET /v1/tracks/{id}).
        TrackAnalysis analysis;
        /// What the answer is worth: engine unavailable, file not indexed,
        /// older analysis. The page prints it instead of guessing.
        std::wstring note;
    };

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

        /// Per-track library analysis: for every track the caller lists (mp3 /
        /// wav only; the rest are filtered out), ask the engine what it already
        /// has (GET /v1/analysis) and index the rest one by one
        /// (POST /v1/tracks/index), so the UI can show "第 i / N 首：文件名".
        /// Afterwards one incremental /v1/library/scan per folder rebuilds the
        /// embeddings + FAISS the recommendation side needs (every file is
        /// already analysed by then, so the scan itself is cheap). |progress|
        /// (optional) is called from a background thread at each step; the
        /// callee is responsible for marshaling to the UI. Returns a summary.
        winrt::Windows::Foundation::IAsyncOperation<hstring>
            AnalyzeLibraryAsync(std::vector<std::wstring> trackPaths,
                                std::vector<std::wstring> folders,
                                std::function<void(hstring const&)> progress = {});

        /// Analyze exactly one track for the structure card: start the engine
        /// on demand, leave tracks that already carry a full curve untouched,
        /// and POST /v1/tracks/index for the rest. The structure view only
        /// ever needs the song that is playing, so it never scans the whole
        /// folder here -- the full-library pass lives on 个性推荐. Returns an
        /// error text; empty on success.
        winrt::Windows::Foundation::IAsyncOperation<hstring> AnalyzeTrackAsync(std::wstring filePath);

        /// Drop the session-cached timeline for one file so the next
        /// PeekTimeline re-asks the engine (after a fresh per-track analysis).
        void ForgetTimeline(std::wstring const& filePath);

        /// GET /v1/feed/state -> one-line short/medium/long interest summary
        /// for the status strip. Returns an error text on failure.
        winrt::Windows::Foundation::IAsyncOperation<hstring> FeedStateTextAsync();

        // ---- per-track structure timeline (GET /v1/analysis) ----
        /// Timeline already fetched for this file in the current session. Cheap
        /// and disk-free: navigating back to the now-playing page never waits on
        /// the network for the same track.
        bool PeekTimeline(std::wstring_view filePath, TrackTimeline& out) const;

        /// GET /v1/analysis?file_path=... plus, for the same row, GET
        /// /v1/tracks/{track_id} for the scalar columns. |done| runs on the
        /// thread that called this (the UI thread), always, including on failure
        /// -- |TrackTimeline.note| carries the reason.
        winrt::fire_and_forget RequestTimelineAsync(std::wstring filePath,
                                                    std::function<void(TrackTimeline)> done);

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

        /// GET /v1/analysis body -> TrackTimeline. Shared by the live call and
        /// the session memo, so a remembered answer reads exactly as it did live.
        /// |engineError| is RecommendService::LastError() of that request.
        static TrackTimeline ParseTimeline(std::wstring_view jsonText, std::wstring filePath,
                                           hstring const& engineError);

        /// Fills |tl.analysis| from GET /v1/tracks/{track_id} (the row's scalar
        /// columns + the extras hidden in features_json). No-op on an unreadable
        /// answer: the curve already on |tl| stays painted either way.
        static void MergeAnalysisRow(TrackTimeline& tl, std::wstring_view rowJson,
                                     hstring const& engineError);

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

        /// Session memo of the timelines already fetched (cap keeps a long
        /// listening session from growing it without bound).
        std::map<std::wstring, TrackTimeline> m_timelines;
        mutable std::mutex m_timelineMutex;
        static constexpr std::size_t kTimelineMax = 40;
    };
} // namespace wm::app
