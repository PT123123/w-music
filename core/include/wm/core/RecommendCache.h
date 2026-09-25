#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace wm::core {

/// One stored recommend-engine answer: the raw JSON body the app already knows
/// how to parse, plus the library fingerprint it was computed for.
///
/// The fingerprint is what keeps a cache honest: the engine's categories and
/// rankings are a function of the analyzed library, so an entry written before
/// the library changed must never be served as if it were current. A mismatch
/// reads as a miss.
struct RecommendCacheEntry {
    std::string payload;        // engine response, verbatim
    std::string fingerprint;    // library identity this answer belongs to
    std::int64_t fetchedAt = 0; // unix milliseconds
};

/// Disk cache for the 个性推荐 page (%LOCALAPPDATA%/w-music/recommend-cache.json).
///
/// Pure data + file handling: the app layer decides what a key means and when
/// to refresh. Never throws -- a missing, corrupt or unwritable file just means
/// an empty cache / a failed save, because a recommendation cache must not be
/// able to break the page.
class RecommendCache {
public:
    /// Beyond this an answer is not worth painting even as an explicit
    /// fallback. How long one counts as *fresh* is the caller's business:
    /// Find() takes the ttl, because a chip row and a feed page do not decay
    /// at the same rate.
    static constexpr std::int64_t kMaxUsableMs = 14 * 24 * 60 * 60 * 1000;
    /// Entries beyond this are dropped from the payload (a runaway page query
    /// string must not be able to grow the file without bound).
    static constexpr std::size_t kMaxPayloadBytes = 512 * 1024;
    static constexpr std::size_t kMaxEntries = 32;

    bool Load(const std::string& path, std::string* error = nullptr);
    bool Save(const std::string& path, std::string* error = nullptr) const;

    /// Fresh hit: |key| present, fingerprint matches, and age <= |ttlMs|.
    const RecommendCacheEntry* Find(std::string_view key, std::string_view fingerprint,
                                    std::int64_t now, std::int64_t ttlMs) const;
    /// Same key + fingerprint, any age: what to paint immediately while a live
    /// request runs (or when the engine cannot be reached). Returns nullptr once
    /// the entry is older than kMaxUsableMs.
    const RecommendCacheEntry* FindStale(std::string_view key, std::string_view fingerprint,
                                         std::int64_t now) const;

    void Put(std::string key, std::string fingerprint, std::string payload, std::int64_t now);

    std::size_t Size() const noexcept { return entries_.size(); }
    void Clear() noexcept { entries_.clear(); }

private:
    std::map<std::string, RecommendCacheEntry, std::less<> > entries_;
};

} // namespace wm::core
