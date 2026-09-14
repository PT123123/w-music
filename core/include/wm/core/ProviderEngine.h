#pragma once

/// Executes a ProviderAdapter: renders request templates, performs the fetches
/// through a caller supplied callback, and extracts OnlineTrack records.
///
/// The engine is synchronous and free of platform APIs -- the host (WinRT,
/// tests, a CLI) provides the HTTP transport and decides which thread to run on.

#include <wm/core/Json.h>
#include <wm/core/ProviderAdapter.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace wm::core
{
    struct HttpRequest
    {
        std::string url;
        std::string method = "GET";
        std::map<std::string, std::string> headers;
        std::string body;
    };

    struct HttpResponse
    {
        int status = 0;
        std::string body;
        std::map<std::string, std::string> headers;

        bool ok() const noexcept { return status >= 200 && status < 300; }
    };

    /// Transport injected by the host. May be called several times per search
    /// (one listing request plus one request per detail lookup).
    using FetchFn = std::function<HttpResponse(HttpRequest const&)>;

    /// One track coming from an online source, before it is downloaded.
    struct OnlineTrack
    {
        std::string sourceId;   // adapter id
        std::string id;
        std::string title;
        std::string artist;
        std::string album;
        std::string durationText;
        int durationSec = 0;
        std::string playUrl;
        std::string downloadUrl;
        std::string coverUrl;
        std::string detailUrl;
        std::map<std::string, std::string> extra;
    };

    struct ProviderResult
    {
        bool ok = false;
        /// Which step failed: "search" / "detail" / "fetch" / "parse".
        std::string stage;
        std::string message;
        std::vector<OnlineTrack> tracks;
    };

    struct ProviderText
    {
        bool ok = false;
        std::string text;
        std::string message;
    };

    class ProviderEngine
    {
    public:
        ProviderEngine(ProviderAdapter adapter, FetchFn fetch);

        ProviderAdapter const& Adapter() const noexcept { return m_adapter; }

        ProviderResult Search(std::string const& query, int limit = 30);

        /// Fills playUrl / downloadUrl using the optional detail step.
        /// Returns the input unchanged when no detail step is configured.
        ProviderResult Resolve(OnlineTrack const& track);

        /// Raw lyric text. Fails when the adapter has no lyric step.
        ProviderText Lyric(OnlineTrack const& track);

    private:
        std::string BuildUrl(RequestStep const& step,
                             std::map<std::string, std::string> const& vars) const;
        std::vector<OnlineTrack> ParseList(RequestStep const& step,
                                           std::string const& body,
                                           std::string const& baseUrl) const;
        /// Returns nullopt when a required field is missing or nothing at all
        /// could be extracted from the response. JSON field lookups try |jsonRoot|
        /// first and fall back to |fallbackRoot| (the parent record when a
        /// flattenPath element is being parsed).
        std::optional<OnlineTrack> ParseObject(RequestStep const& step,
                                              std::string const& body,
                                              std::string const& chunk,
                                              json::Value const* jsonRoot,
                                              std::string const& baseUrl,
                                              json::Value const* fallbackRoot = nullptr) const;

        ProviderAdapter m_adapter;
        FetchFn m_fetch;
    };

    // ---- helpers (exported so they can be unit tested) ----

    /// Percent encodes everything outside the RFC 3986 unreserved set.
    std::string UrlEncode(std::string const& value);

    /// Replaces {name} placeholders. Unknown names expand to an empty string.
    std::string RenderTemplate(std::string const& tpl,
                               std::map<std::string, std::string> const& vars);

    /// Resolves a dotted JSON path (a.b[0].c). "$" (or a leading "$.") addresses
    /// the document root; an otherwise empty path returns |root|.
    json::Value const* JsonAt(json::Value const& root, std::string const& path);

    /// Flattens a JSON value to text (numbers without a trailing .000000).
    std::string JsonText(json::Value const& value);

    /// Every capture of |group| for |pattern|, in order.
    std::vector<std::string> RegexAll(std::string const& text, std::string const& pattern, int group);

    /// First capture of |group|, or an empty string.
    std::string RegexFirst(std::string const& text, std::string const& pattern, int group);

    /// Resolves &amp; &#39; and friends.
    std::string HtmlDecode(std::string const& text);

    /// Turns a possibly relative URL into an absolute one using |base|.
    std::string AbsoluteUrl(std::string const& base, std::string const& maybe);

    /// "03:45" / "225" -> 225.
    int ParseDurationSeconds(std::string const& text);

    /// "225" -> "03:45".
    std::string FormatDuration(int seconds);
} // namespace wm::core
