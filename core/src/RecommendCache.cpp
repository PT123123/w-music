#include "wm/core/RecommendCache.h"

#include "wm/core/Json.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace wm::core {
namespace {

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool WriteFileAtomic(const std::string& path, const std::string& content) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out) {
            return false;
        }
    }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

} // namespace

bool RecommendCache::Load(const std::string& path, std::string* error) {
    entries_.clear();
    const std::string text = ReadFile(path);
    if (text.empty()) {
        return false; // nothing persisted yet -- an empty cache, not a failure
    }

    std::string parseError;
    const auto root = json::Parse(text, &parseError);
    if (!root || !root->isObject()) {
        // A half-written file is repaired by the next successful save.
        if (error) *error = parseError.empty() ? std::string{ "cache root is not an object" } : parseError;
        return false;
    }

    const json::Value* items = root->Find("entries");
    if (items == nullptr || !items->isArray()) {
        return false;
    }
    for (const auto& row : items->asArray()) {
        if (!row.isObject()) {
            continue;
        }
        RecommendCacheEntry entry;
        const json::Value* key = row.Find("key");
        if (key == nullptr || !key->isString() || key->asString().empty()) {
            continue;
        }
        if (const json::Value* body = row.Find("payload"); body != nullptr && body->isString()) {
            entry.payload = body->asString();
        }
        if (const json::Value* fp = row.Find("fingerprint"); fp != nullptr && fp->isString()) {
            entry.fingerprint = fp->asString();
        }
        if (const json::Value* at = row.Find("fetchedAt"); at != nullptr && at->isNumber()) {
            entry.fetchedAt = at->asInt();
        }
        if (entry.payload.empty()) {
            continue;
        }
        entries_.insert_or_assign(key->asString(), std::move(entry));
    }
    return true;
}

bool RecommendCache::Save(const std::string& path, std::string* error) const {
    json::Array items;
    items.reserve(entries_.size());
    for (const auto& [key, entry] : entries_) {
        json::Object row;
        row["key"] = json::Value(key);
        row["fingerprint"] = json::Value(entry.fingerprint);
        row["fetchedAt"] = json::Value(entry.fetchedAt);
        row["payload"] = json::Value(entry.payload);
        items.push_back(json::Value(std::move(row)));
    }

    json::Object root;
    root["version"] = json::Value(1);
    root["entries"] = json::Value(std::move(items));
    if (!WriteFileAtomic(path, json::Serialize(json::Value(std::move(root)), true))) {
        if (error) *error = "failed to write recommend cache file";
        return false;
    }
    return true;
}

const RecommendCacheEntry* RecommendCache::Find(std::string_view key, std::string_view fingerprint,
                                                std::int64_t now, std::int64_t ttlMs) const {
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        return nullptr;
    }
    if (it->second.fingerprint != fingerprint) {
        return nullptr;
    }
    if (it->second.fetchedAt <= 0 || now - it->second.fetchedAt > ttlMs) {
        return nullptr;
    }
    return &it->second;
}

const RecommendCacheEntry* RecommendCache::FindStale(std::string_view key, std::string_view fingerprint,
                                                     std::int64_t now) const {
    const auto it = entries_.find(key);
    if (it == entries_.end() || it->second.fingerprint != fingerprint) {
        return nullptr;
    }
    if (it->second.fetchedAt <= 0 || now - it->second.fetchedAt > kMaxUsableMs) {
        return nullptr;
    }
    return &it->second;
}

void RecommendCache::Put(std::string key, std::string fingerprint, std::string payload, std::int64_t now) {
    if (key.empty() || payload.empty() || payload.size() > kMaxPayloadBytes) {
        return;
    }
    std::string const insertedKey = key;
    entries_.insert_or_assign(std::move(key),
                              RecommendCacheEntry{ std::move(payload), std::move(fingerprint), now });

    // Bound the file: drop the oldest fetches (a repeated free-text query would
    // otherwise accumulate one entry per distinct sentence forever). The entry
    // just written is never the victim -- replacing an answer must not discard
    // the answer.
    while (entries_.size() > kMaxEntries) {
        auto oldest = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->first == insertedKey) {
                continue;
            }
            if (oldest == entries_.end() || it->second.fetchedAt < oldest->second.fetchedAt) {
                oldest = it;
            }
        }
        if (oldest == entries_.end()) {
            break;
        }
        entries_.erase(oldest);
    }
}

} // namespace wm::core
