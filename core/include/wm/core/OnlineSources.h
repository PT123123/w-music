#pragma once

/// One-to-one replicas of the two online sources behind a-music's 发现 tab
/// (https://github.com/PT123123/a-music, data/online/QqOnlineApi.kt and
/// Net24Api.kt): the same endpoints, the same request shapes, the same
/// parsing quirks, and the same guard rails.
///
/// Like ProviderEngine the sources are synchronous and free of platform
/// APIs -- the host injects the HTTP transport (FetchFn) and decides which
/// thread to run on. Site specific behaviour intentionally lives in compiled
/// code here (the pluggable adapter layer cannot express merged catalogues,
/// quota markers or name-checking) while the transport stays swappable so the
/// whole thing can be tested offline.

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <wm/core/ProviderEngine.h>

namespace wm::core
{
    // =====================================================================
    // == QQ 音乐 (replica of QqOnlineApi.kt)                              ==
    // =====================================================================
    //
    // Two calls make the whole source work:
    //   1. Search    -> https://c.y.qq.com/soso/fcgi-bin/client_search_cp
    //   2. DirectUrl -> https://u.y.qq.com/cgi-bin/musicu.fcg (vkey lookup)
    //
    // Gotchas replicated from a-music:
    //   - The endpoints reject requests without a browser-ish User-Agent and
    //     a "Referer: https://y.qq.com/".
    //   - A VIP-only track resolves to an empty purl; callers surface that
    //     instead of attempting a download that would 403.

    /// One search result row (a-music: OnlineSong).
    struct QqSong
    {
        std::string mid;        // QQ songmid -- the stable key used to resolve a play URL
        std::string title;
        std::string artist;
        std::string album;
        std::string albumMid;
        int durationSec = 0;
        std::int64_t sizeBytes = 0;  // advertised 128k file size; 0 when unknown
        bool vipOnly = false;        // pay.play == 1 -> needs a VIP account

        /// QQ serves album art from a predictable URL keyed by albummid.
        std::string CoverUrl() const;
        std::string DurationText() const;
        std::string SizeText() const;
        /// "歌手 - 歌名" shaped label used for file names.
        std::string DisplayName() const;
    };

    class QqSource
    {
    public:
        /// Arbitrary but stable; QQ only echoes it back in the signed URL.
        static constexpr char const* kGuid = "3982823384";
        static char const* UserAgent() noexcept { return kUa; }
        static char const* Referer() noexcept { return kReferer; }

        explicit QqSource(FetchFn fetch);

        /// Public search API: keyword -> songs (a-music defaults page 1, 20 rows).
        std::vector<QqSong> Search(std::string const& keyword, int page = 1, int pageSize = 20) const;

        /// Resolve a playable/downloadable URL for |mid|. Returns "" when the
        /// track is not freely available (VIP-only, taken down...).
        std::string DirectUrl(std::string const& mid) const;

        /// Fetches the LRC lyric text for |mid| (already de-base64'd by the
        /// endpoint when nobase64=1). Returns "" when the song has no lyric.
        std::string Lyric(std::string const& mid) const;

        /// Installs the login cookie set produced by QqLoginFlow (a
        /// "k1=v1; k2=v2" string) together with the logged-in QQ number. All
        /// subsequent requests carry the cookie; |uin| is forwarded into the
        /// vkey lookup so the CDN can honour the account's entitlements.
        void SetSession(std::string const& cookie, std::string const& uin = {})
        {
            m_session = cookie;
            m_uin = uin;
        }
        std::string const& Session() const noexcept { return m_session; }
        std::string const& Uin() const noexcept { return m_uin; }

        /// Headers the downloader must send when pulling audio off QQ's CDN.
        static std::map<std::string, std::string> DownloadHeaders();

        /// Suggestions shown before the user types anything.
        static std::vector<std::string> const& HotWords();

    private:
        static constexpr char const* kUa =
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
            "(KHTML, like Gecko) Chrome/124.0 Safari/537.36";
        static constexpr char const* kReferer = "https://y.qq.com/";
        static constexpr char const* kSearchEndpoint =
            "https://c.y.qq.com/soso/fcgi-bin/client_search_cp";
        static constexpr char const* kMusicuEndpoint =
            "https://u.y.qq.com/cgi-bin/musicu.fcg";
        static constexpr char const* kLyricEndpoint =
            "https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg";

        std::string HttpGet(std::string const& url, std::string const& referer) const;

        FetchFn m_fetch;
        /// Login cookie set ("k=v; k=v"), installed by QqLoginFlow.
        std::string m_session;
        /// Logged-in QQ account number (empty when not logged in).
        std::string m_uin;
    };

    // =====================================================================
    // == QQ 音乐 网页版扫码登录                                           ==
    // =====================================================================
    //
    // Reproduces the QR-code login flow the QQ Music web player drives through
    // ssl.ptlogin2.qq.com (there is no official / qq-music-api login module,
    // so this mirrors what the site itself does). It is a pure state machine:
    // the host feeds it an HTTP transport, calls FetchQr() to obtain the
    // QR image, displays it, then polls CheckStatus() until the scan succeeds
    // and hands back the login cookie set to install on a QqSource.

    /// State of a QR login exchange after FetchQr() succeeds.
    /// The |qrImage| is the bytes of the QR picture (jpg/png); |qrsig| is the
    /// opaque session marker from the Set-Cookie response; |ptqrtoken| is the
    /// derived token required by the polling call.
    struct QqLoginContext
    {
        bool ok = false;
        std::string qrImage;
        std::string qrsig;
        std::string ptqrtoken;
        std::string reason;   // human readable failure description
    };

    /// Result of one CheckStatus() poll.
    enum class QqLoginStatus
    {
        Waiting,    // 65: QR shown, not scanned yet
        Scanned,    // 66: scanned on the phone, waiting for confirmation
        Success,    // 0: confirmed, cookie set is available
        Failed,     // expired (67) / network error / unknown
    };

    struct QqLoginResult
    {
        QqLoginStatus status = QqLoginStatus::Waiting;
        std::string cookie;   // "k=v; k=v" -- valid when status == Success
        std::string uin;      // logged-in QQ number when status == Success
        std::string reason;
    };

    class QqLoginFlow
    {
    public:
        explicit QqLoginFlow(FetchFn fetch);

        /// Requests a fresh QR image and prepares |context| for later polls.
        /// The caller shows |ctx.qrImage| to the user and keeps |ctx| to poll.
        QqLoginContext FetchQr() const;

        /// Polls the login result for the given context. Call repeatedly every
        /// ~2s after showing the QR. On Success the returned cookie is ready to
        /// install on a QqSource via QqSource::SetSession.
        QqLoginResult CheckStatus(QqLoginContext const& context) const;

        /// hash33 algorithm (the web player derives ptqrtoken from qrsig).
        static std::string Hash33(std::string const& qrsig);

        /// Browser-ish header block every ptlogin request needs.
        static std::map<std::string, std::string> RequestHeaders();

    private:
        std::string HttpGet(std::string const& url) const;

        static constexpr char const* kUa =
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
            "(KHTML, like Gecko) Chrome/124.0 Safari/537.36";
        static constexpr char const* kQrUrl =
            "https://ssl.ptlogin2.qq.com/ptqrshow"
            "?appid=716027609&e=2&l=M&s=3&d=72&v=4&t=0.1"
            "&daid=383&pt_3rd_aid=100497308"
            "&u1=https%3A%2F%2Fy.qq.com%2Fportal%2Fwplayer.html";
        static constexpr char const* kLoginBase =
            "https://ssl.ptlogin2.qq.com/ptqrlogin";

        FetchFn m_fetch;
        /// Accumulated Set-Cookie values while a login exchange is in flight.
        mutable std::string m_cookies;
    };

    // =====================================================================
    // == 无损站 (replica of Net24Api.kt / Net24Song.kt)                   ==
    // =====================================================================

    /// Which of the source site's two catalogues a song id belongs to.
    enum class Net24Origin
    {
        /// searchOnlineMusicOne -- its ids resolve for MASTER / SURROUND.
        Master,
        /// searchOnlineMusicTwo -- its ids resolve for LOSSLESS.
        Lossless,
    };

    /// Text shown in the confirm dialog for one origin (a-music listLabel).
    char const* Net24OriginLabel(Net24Origin origin) noexcept;

    /// The three download tiers the source site exposes. Type() is the URL
    /// path segment. The site keys the id space per source, so an id from the
    /// 母带源 list resolves to a completely unrelated song under "b" and vice
    /// versa -- every resolve therefore goes through Net24Song::QualityId and
    /// the answer is name-checked before it is offered.
    enum class Net24Quality
    {
        Master,    // "a" 至臻母带
        Surround,  // "c" 高清环绕声
        Lossless,  // "b" 无损音质
    };

    char const* Net24QualityType(Net24Quality quality) noexcept;   // "a" / "c" / "b"
    char const* Net24QualityLabel(Net24Quality quality) noexcept;  // full name
    char const* Net24QualityShort(Net24Quality quality) noexcept;  // chip label
    Net24Origin Net24QualityOrigin(Net24Quality quality) noexcept;

    /// One song, merged from the two search endpoints (a-music: Net24Song).
    struct Net24Song
    {
        std::string title;
        std::string artist;
        std::string album;
        std::string coverUrl;
        /// Id as understood by the 母带源 ("a" / "c"), empty when not listed there.
        std::string masterId;
        /// Id as understood by the 无损源 ("b"), empty when not listed there.
        std::string losslessId;

        /// Best available id -- used as the compose key.
        std::string Id() const;
        std::string DisplayName() const;
        /// Tiers this song can actually serve, in display order.
        std::vector<Net24Quality> Qualities() const;
        std::string QualityId(Net24Quality quality) const;
        /// Stable per-quality key used by the UI's progress tracking.
        std::string Key(Net24Quality quality) const;
        /// "母带源" / "无损源" / "母带源 + 无损源" -- where the row came from.
        std::string SourceLabel() const;

        /// Folds another listing of the same song into this one.
        void Merge(Net24Song const& other);
    };

    /// A resolved download target scraped from a source-site detail page.
    struct Net24Download
    {
        std::string url;
        std::string fileName;
        std::string ext;
        std::string quality;
        /// Raw size text exactly as the site prints it, e.g. "198.47MB".
        std::string sizeText;
        /// Same size parsed to bytes, or 0 when unparseable.
        std::int64_t sizeBytes = 0;
    };

    /// Result of asking the site for one tier (a-music: Net24Resolve).
    struct Net24Resolve
    {
        bool ok = false;
        /// Human readable failure reason (empty when ok).
        std::string reason;
        /// The tier we actually resolved -- for preview it is the lightest one.
        Net24Quality tier = Net24Quality::Lossless;
        Net24Download download;
    };

    /// "198.47MB" / "21.57 MB" -> bytes. Returns 0 when it cannot be read.
    std::int64_t ParseNet24Size(std::string const& text);

    class Net24Source
    {
    public:
        static char const* UserAgent() noexcept { return kUa; }
        static char const* QuotaMark() noexcept { return "今日访问已达限额"; }

        /// |baseUrl| is machine-local (a-music injects it via local.properties
        /// and BuildConfig.NET24_BASE_URL); never hard-code one here. Requests
        /// against an empty base URL fail with a configuration message.
        explicit Net24Source(std::string baseUrl, FetchFn fetch);

        std::string const& BaseUrl() const noexcept { return m_baseUrl; }
        void SetBaseUrl(std::string baseUrl);

        /// POSTs both search endpoints and folds their listings into one row
        /// per song (a-music: Net24Api.search).
        std::vector<Net24Song> Search(std::string const& keyword, int page = 1) const;

        /// Resolve one tier for |song|. Never silently returns a different
        /// song: the site's answer is compared with the row first, so a bad id
        /// pairing surfaces as a failed Net24Resolve.
        Net24Resolve Resolve(Net24Song const& song, Net24Quality quality) const;

        /// Preview streams the *smallest* tier the row can serve so it starts
        /// fast -- 无损 (b) is roughly a quarter of the 母带 (a) size.
        Net24Resolve Preview(Net24Song const& song) const;

        /// Extra headers the downloader must reuse -- the CDNs care about UA,
        /// the site about Referer.
        std::map<std::string, std::string> DownloadHeaders() const;

        /// Suggestions shown before the user types anything.
        static std::vector<std::string> const& HotWords();

    private:
        struct Detail
        {
            std::string url;
            std::string name;
            std::string player;
            std::string quality;
            std::string sizeText;
            std::string ext;
            /// Set instead of |url| when the lookup failed; not cached.
            std::string reason;
        };

        /// Fetch + parse one detail page. Successful lookups are cached for
        /// the session (each one costs a quota slot).
        Detail FetchDetail(std::string const& type, std::string const& id) const;

        std::string HttpPost(std::string const& url, std::string const& body) const;

        static constexpr char const* kUa =
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
            "(KHTML, like Gecko) Chrome/124.0 Safari/537.36";

        std::string m_baseUrl;
        FetchFn m_fetch;
        /// Resolved detail payloads, keyed by "<type>:<id>".
        mutable std::mutex m_cacheMutex;
        mutable std::map<std::string, Detail> m_cache;
    };
} // namespace wm::core
