#include "wm/core/LibraryStore.h"

#include "wm/core/Json.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>

namespace wm::core {
namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

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

json::Value ToJson(const TrackRecord& t) {
    json::Object o;
    o["id"] = json::Value(t.id);
    o["title"] = json::Value(t.title);
    o["artist"] = json::Value(t.artist);
    o["album"] = json::Value(t.album);
    o["filePath"] = json::Value(t.filePath);
    o["lyricPath"] = json::Value(t.lyricPath);
    o["durationMs"] = json::Value(t.durationMs);
    o["dateAdded"] = json::Value(t.dateAdded);
    o["lastPlayed"] = json::Value(t.lastPlayed);
    o["playCount"] = json::Value(t.playCount);
    o["favorite"] = json::Value(t.favorite);
    o["bitrateKbps"] = json::Value(static_cast<std::int64_t>(t.bitrateKbps));
    o["fileSize"] = json::Value(static_cast<std::int64_t>(t.fileSize));
    return json::Value(std::move(o));
}

TrackRecord TrackFromJson(const json::Value& v) {
    TrackRecord t;
    t.id = v.Find("id") ? v.Find("id")->asString() : std::string();
    t.title = v.Find("title") ? v.Find("title")->asString() : std::string();
    t.artist = v.Find("artist") ? v.Find("artist")->asString() : std::string();
    t.album = v.Find("album") ? v.Find("album")->asString() : std::string();
    t.filePath = v.Find("filePath") ? v.Find("filePath")->asString() : std::string();
    t.lyricPath = v.Find("lyricPath") ? v.Find("lyricPath")->asString() : std::string();
    t.durationMs = v.Find("durationMs") ? v.Find("durationMs")->asInt() : 0;
    t.dateAdded = v.Find("dateAdded") ? v.Find("dateAdded")->asInt() : 0;
    t.lastPlayed = v.Find("lastPlayed") ? v.Find("lastPlayed")->asInt() : 0;
    t.playCount = static_cast<int>(v.Find("playCount") ? v.Find("playCount")->asInt() : 0);
    t.favorite = v.Find("favorite") ? v.Find("favorite")->asBool() : false;
    t.bitrateKbps = static_cast<std::uint32_t>(v.Find("bitrateKbps") ? v.Find("bitrateKbps")->asInt() : 0);
    t.fileSize = static_cast<std::uint64_t>(v.Find("fileSize") ? v.Find("fileSize")->asInt() : 0);
    return t;
}

json::Value ToJson(const PlaylistRecord& p) {
    json::Array ids;
    ids.reserve(p.trackIds.size());
    for (const auto& id : p.trackIds) {
        ids.push_back(json::Value(id));
    }
    json::Object o;
    o["id"] = json::Value(p.id);
    o["name"] = json::Value(p.name);
    o["kind"] = json::Value(static_cast<std::int64_t>(p.kind));
    o["createdAt"] = json::Value(p.createdAt);
    o["trackIds"] = json::Value(std::move(ids));
    return json::Value(std::move(o));
}

PlaylistRecord PlaylistFromJson(const json::Value& v) {
    PlaylistRecord p;
    p.id = v.Find("id") ? v.Find("id")->asString() : std::string();
    p.name = v.Find("name") ? v.Find("name")->asString() : std::string();
    p.kind = static_cast<PlaylistKind>(static_cast<int>(v.Find("kind") ? v.Find("kind")->asInt() : 0));
    p.createdAt = v.Find("createdAt") ? v.Find("createdAt")->asInt() : 0;
    if (const json::Value* ids = v.Find("trackIds")) {
        for (const auto& id : ids->asArray()) {
            p.trackIds.push_back(id.asString());
        }
    }
    return p;
}

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

std::string LibraryStore::MakeTrackId(const std::string& filePath) {
    std::uint64_t hash = kFnvOffset;
    for (const char ch : filePath) {
        const unsigned char c = static_cast<unsigned char>(
            ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
        hash ^= static_cast<std::uint64_t>(c);
        hash *= kFnvPrime;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(buf);
}

std::string LibraryStore::NewId() {
    static std::mt19937_64 rng{std::random_device{}()};
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(rng()));
    return std::string(buf);
}

bool LibraryStore::Load(const std::string& path, std::string* error) {
    const std::string text = ReadFile(path);
    if (text.empty()) {
        return false; // nothing persisted yet
    }

    std::string parseError;
    const auto root = json::Parse(text, &parseError);
    if (!root) {
        if (error) *error = parseError;
        return false;
    }

    LibraryData loaded;
    loaded.version = static_cast<int>(root->Find("version") ? root->Find("version")->asInt() : 1);

    if (const json::Value* folders = root->Find("scanFolders")) {
        for (const auto& f : folders->asArray()) {
            loaded.scanFolders.push_back(f.asString());
        }
    }
    if (const json::Value* tracks = root->Find("tracks")) {
        for (const auto& t : tracks->asArray()) {
            loaded.tracks.push_back(TrackFromJson(t));
        }
    }
    if (const json::Value* playlists = root->Find("playlists")) {
        for (const auto& p : playlists->asArray()) {
            loaded.playlists.push_back(PlaylistFromJson(p));
        }
    }

    data_ = std::move(loaded);
    return true;
}

bool LibraryStore::Save(const std::string& path, std::string* error) const {
    json::Array folders;
    for (const auto& f : data_.scanFolders) folders.push_back(json::Value(f));

    json::Array tracks;
    tracks.reserve(data_.tracks.size());
    for (const auto& t : data_.tracks) tracks.push_back(ToJson(t));

    json::Array playlists;
    playlists.reserve(data_.playlists.size());
    for (const auto& p : data_.playlists) playlists.push_back(ToJson(p));

    json::Object root;
    root["version"] = json::Value(data_.version);
    root["scanFolders"] = json::Value(std::move(folders));
    root["tracks"] = json::Value(std::move(tracks));
    root["playlists"] = json::Value(std::move(playlists));

    if (!WriteFileAtomic(path, json::Serialize(json::Value(std::move(root)), true))) {
        if (error) *error = "failed to write library file";
        return false;
    }
    return true;
}

TrackRecord* LibraryStore::FindTrack(const std::string& id) {
    const auto it = std::find_if(data_.tracks.begin(), data_.tracks.end(),
                                 [&](const TrackRecord& t) { return t.id == id; });
    return it == data_.tracks.end() ? nullptr : &(*it);
}

const TrackRecord* LibraryStore::FindTrack(const std::string& id) const {
    const auto it = std::find_if(data_.tracks.begin(), data_.tracks.end(),
                                 [&](const TrackRecord& t) { return t.id == id; });
    return it == data_.tracks.end() ? nullptr : &(*it);
}

TrackRecord* LibraryStore::FindTrackByPath(const std::string& path) {
    const std::string id = MakeTrackId(path);
    return FindTrack(id);
}

void LibraryStore::UpsertTrack(const TrackRecord& track) {
    if (TrackRecord* existing = FindTrack(track.id)) {
        // Preserve user state that a rescan must not clobber.
        const bool favorite = existing->favorite;
        const int playCount = existing->playCount;
        const std::int64_t lastPlayed = existing->lastPlayed;
        const std::int64_t dateAdded = existing->dateAdded != 0 ? existing->dateAdded : track.dateAdded;

        *existing = track;
        existing->favorite = favorite;
        existing->playCount = playCount;
        existing->lastPlayed = lastPlayed;
        existing->dateAdded = dateAdded;
        return;
    }
    data_.tracks.push_back(track);
}

void LibraryStore::RemoveTrack(const std::string& id) {
    data_.tracks.erase(std::remove_if(data_.tracks.begin(), data_.tracks.end(),
                                      [&](const TrackRecord& t) { return t.id == id; }),
                       data_.tracks.end());
    for (auto& p : data_.playlists) {
        p.trackIds.erase(std::remove(p.trackIds.begin(), p.trackIds.end(), id), p.trackIds.end());
    }
}

bool LibraryStore::SetFavorite(const std::string& id, bool value) {
    TrackRecord* t = FindTrack(id);
    if (t == nullptr) {
        return false;
    }
    t->favorite = value;

    PlaylistRecord& favorites = EnsureBuiltin(PlaylistKind::Favorites, "我喜欢的音乐");
    if (value) {
        AddToPlaylist(favorites.id, id);
    } else {
        RemoveFromPlaylist(favorites.id, id);
    }
    return true;
}

bool LibraryStore::ToggleFavorite(const std::string& id) {
    const TrackRecord* t = FindTrack(id);
    if (t == nullptr) {
        return false;
    }
    return SetFavorite(id, !t->favorite);
}

void LibraryStore::MarkPlayed(const std::string& id, std::int64_t whenMs) {
    if (TrackRecord* t = FindTrack(id)) {
        t->playCount += 1;
        t->lastPlayed = whenMs;
    }

    PlaylistRecord& recent = EnsureBuiltin(PlaylistKind::Recent, "最近播放");
    RemoveFromPlaylist(recent.id, id);
    recent.trackIds.insert(recent.trackIds.begin(), id);
    constexpr std::size_t kRecentLimit = 200;
    if (recent.trackIds.size() > kRecentLimit) {
        recent.trackIds.resize(kRecentLimit);
    }
}

PlaylistRecord& LibraryStore::EnsureBuiltin(PlaylistKind kind, const std::string& name) {
    const auto it = std::find_if(data_.playlists.begin(), data_.playlists.end(),
                                 [&](const PlaylistRecord& p) { return p.kind == kind; });
    if (it != data_.playlists.end()) {
        if (!name.empty() && it->name != name) {
            it->name = name;
        }
        return *it;
    }

    PlaylistRecord p;
    p.id = "builtin:" + std::to_string(static_cast<int>(kind));
    p.name = name;
    p.kind = kind;
    p.createdAt = NowMs();
    data_.playlists.push_back(std::move(p));
    return data_.playlists.back();
}

PlaylistRecord& LibraryStore::CreatePlaylist(const std::string& name) {
    PlaylistRecord p;
    p.id = "pl:" + NewId();
    p.name = name.empty() ? "新建歌单" : name;
    p.kind = PlaylistKind::User;
    p.createdAt = NowMs();
    data_.playlists.push_back(std::move(p));
    return data_.playlists.back();
}

bool LibraryStore::DeletePlaylist(const std::string& id) {
    const auto it = std::find_if(data_.playlists.begin(), data_.playlists.end(),
                                 [&](const PlaylistRecord& p) { return p.id == id; });
    if (it == data_.playlists.end() || it->kind != PlaylistKind::User) {
        return false; // built-ins cannot be deleted
    }
    data_.playlists.erase(it);
    return true;
}

bool LibraryStore::RenamePlaylist(const std::string& id, const std::string& name) {
    PlaylistRecord* p = FindPlaylist(id);
    if (p == nullptr || name.empty()) {
        return false;
    }
    p->name = name;
    return true;
}

bool LibraryStore::AddToPlaylist(const std::string& playlistId, const std::string& trackId) {
    PlaylistRecord* p = FindPlaylist(playlistId);
    if (p == nullptr) {
        return false;
    }
    if (std::find(p->trackIds.begin(), p->trackIds.end(), trackId) != p->trackIds.end()) {
        return false;
    }
    p->trackIds.push_back(trackId);
    return true;
}

bool LibraryStore::RemoveFromPlaylist(const std::string& playlistId, const std::string& trackId) {
    PlaylistRecord* p = FindPlaylist(playlistId);
    if (p == nullptr) {
        return false;
    }
    const auto it = std::remove(p->trackIds.begin(), p->trackIds.end(), trackId);
    if (it == p->trackIds.end()) {
        return false;
    }
    p->trackIds.erase(it, p->trackIds.end());
    return true;
}

bool LibraryStore::MoveInPlaylist(const std::string& playlistId, std::size_t from, std::size_t to) {
    PlaylistRecord* p = FindPlaylist(playlistId);
    if (p == nullptr || from >= p->trackIds.size() || to >= p->trackIds.size() || from == to) {
        return false;
    }
    const std::string id = p->trackIds[from];
    p->trackIds.erase(p->trackIds.begin() + static_cast<std::ptrdiff_t>(from));
    p->trackIds.insert(p->trackIds.begin() + static_cast<std::ptrdiff_t>(to), id);
    return true;
}

PlaylistRecord* LibraryStore::FindPlaylist(const std::string& id) {
    const auto it = std::find_if(data_.playlists.begin(), data_.playlists.end(),
                                 [&](const PlaylistRecord& p) { return p.id == id; });
    return it == data_.playlists.end() ? nullptr : &(*it);
}

const PlaylistRecord* LibraryStore::FindPlaylist(const std::string& id) const {
    const auto it = std::find_if(data_.playlists.begin(), data_.playlists.end(),
                                 [&](const PlaylistRecord& p) { return p.id == id; });
    return it == data_.playlists.end() ? nullptr : &(*it);
}

std::vector<const TrackRecord*> LibraryStore::TopPlayed(std::size_t count) const {
    std::vector<const TrackRecord*> all;
    all.reserve(data_.tracks.size());
    for (const auto& t : data_.tracks) {
        if (t.playCount > 0) {
            all.push_back(&t);
        }
    }
    std::sort(all.begin(), all.end(), [](const TrackRecord* a, const TrackRecord* b) {
        if (a->playCount != b->playCount) return a->playCount > b->playCount;
        return a->lastPlayed > b->lastPlayed;
    });
    if (all.size() > count) {
        all.resize(count);
    }
    return all;
}

std::vector<const TrackRecord*> LibraryStore::RecentlyAdded(std::size_t count) const {
    std::vector<const TrackRecord*> all;
    all.reserve(data_.tracks.size());
    for (const auto& t : data_.tracks) {
        all.push_back(&t);
    }
    std::sort(all.begin(), all.end(), [](const TrackRecord* a, const TrackRecord* b) {
        return a->dateAdded > b->dateAdded;
    });
    if (all.size() > count) {
        all.resize(count);
    }
    return all;
}

} // namespace wm::core
