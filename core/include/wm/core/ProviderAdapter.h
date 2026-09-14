#pragma once

/// Declarative description of one online music source.
///
/// An adapter is a plain JSON file that tells the engine how to turn a keyword
/// into a list of tracks, and how to turn a track into a playable / downloadable
/// URL. No site specific logic lives in compiled code: everything is data, so a
/// source can be added, edited or removed without rebuilding the app.
///
/// Adapters are intentionally NOT part of the source tree -- they are loaded at
/// runtime from an external directory (see docs/providers.md).

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wm::core
{
    /// How a single value is pulled out of an HTTP response.
    struct ExtractRule
    {
        /// Destination field: id / title / artist / album / duration / cover /
        /// play / download / detail. Anything else lands in OnlineTrack::extra.
        std::string key;
        /// "json"    -> |pattern| is a dotted path (a.b[0].c) into a JSON body
        /// "regex"   -> |pattern| is a regular expression with a capture group
        /// "static"  -> |value| is used verbatim
        std::string source = "regex";
        std::string pattern;
        std::string value;
        /// Capture group for source == "regex". 0 means the whole match.
        int group = 1;
        /// When true a missing value makes the whole record invalid.
        bool required = false;
    };

    /// One HTTP round trip plus the rules that read its response.
    struct RequestStep
    {
        bool enabled = false;
        /// URL template. Relative values are appended to ProviderAdapter::baseUrl.
        /// Placeholders: {query} {query_enc} {id} {title} {artist} {album}
        /// {detail} and any {$extra} captured by an earlier step.
        std::string url;
        std::string method = "GET";
        std::string body;
        /// Extra headers; values are templates as well.
        std::map<std::string, std::string> headers;
        /// HTML: regex whose whole match delimits one record.
        std::string itemPattern;
        /// JSON: dotted path to the array of records. "$" means the document
        /// root itself (use it when the response body is a top-level array).
        std::string listPath;
        /// Optional: inside every record, the array at this dotted path is
        /// expanded into one output record per element. Field lookup reads the
        /// element first and falls back to the parent record (e.g. one album
        /// entry containing a nested "files" array with several formats).
        std::string flattenPath;
        std::vector<ExtractRule> fields;
    };

    struct ProviderAdapter
    {
        std::string id;
        std::string name;
        std::string baseUrl;
        /// Sent with every request (User-Agent, Referer, Cookie, Authorization...).
        std::map<std::string, std::string> headers;
        std::string charset = "utf-8";
        /// Politeness hint for the caller; the engine itself never sleeps.
        int minIntervalMs = 0;
        RequestStep search;
        RequestStep detail;
        RequestStep lyric;
        /// When true, play/download URLs are only known after the detail step.
        bool detailRequired = false;
        /// "stream" (default): preview URLs are handed straight to the player.
        /// "cache": the host downloads the file first and plays it locally
        /// (for hosts that reject requests without browser-like headers).
        std::string preview = "stream";
        /// Free-form description shown in the UI (search hints, licensing...).
        std::string note;
    };

    /// Parses one adapter file. Returns nullopt and fills |error| on failure.
    std::optional<ProviderAdapter> ParseAdapter(const std::string& jsonText, std::string* error = nullptr);

    /// Returns an empty string when the adapter is usable, otherwise the reason.
    std::string ValidateAdapter(ProviderAdapter const& adapter);
} // namespace wm::core
