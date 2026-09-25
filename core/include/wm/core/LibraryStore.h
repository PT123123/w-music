#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace wm::core {

enum class PlaylistKind : int {
    User = 0,     // regular user playlist
    Favorites = 1, // built-in "My Favorites"
    Recent = 2,    // built-in "Recently Played"
};

struct TrackRecord {
    std::string id;
    std::string title;
    std::string artist;
    std::string album;
    std::string filePath;
    std::string lyricPath;
    std::int64_t durationMs = 0;
    std::int64_t dateAdded = 0;
    std::int64_t lastPlayed = 0;
    int playCount = 0;
    bool favorite = false;
    std::uint32_t bitrateKbps = 0;
    std::uint64_t fileSize = 0;
};

struct PlaylistRecord {
    std::string id;
    std::string name;
    PlaylistKind kind = PlaylistKind::User;
    std::vector<std::string> trackIds;
    std::int64_t createdAt = 0;
};

struct LibraryData {
    int version = 1;
    std::vector<TrackRecord> tracks;
    std::vector<PlaylistRecord> playlists;
    /// Folders chosen by the user (paths, or FutureAccessList tokens).
    std::vector<std::string> scanFolders;
};

/// The music library: tracks, playlists, persistence.
///
/// Storage is a single JSON file at %LOCALAPPDATA%/w-music/library.json.
/// A few thousand tracks is well within budget; swap for SQLite later if the
/// library grows past ~50k entries.
class LibraryStore {
public:
    bool Load(const std::string& path, std::string* error = nullptr);
    bool Save(const std::string& path, std::string* error = nullptr) const;

    LibraryData& Data() noexcept { return data_; }
    const LibraryData& Data() const noexcept { return data_; }

    // ---- tracks ----
    /// Stable id derived from the file path (FNV-1a 64 over lowercased path).
    static std::string MakeTrackId(const std::string& filePath);

    TrackRecord* FindTrack(const std::string& id);
    const TrackRecord* FindTrack(const std::string& id) const;
    TrackRecord* FindTrackByPath(const std::string& path);
    /// Insert or merge; statistics (playCount, favorite) survive re-scans.
    void UpsertTrack(const TrackRecord& track);
    void RemoveTrack(const std::string& id);
    bool ToggleFavorite(const std::string& id);
    bool SetFavorite(const std::string& id, bool value);
    void MarkPlayed(const std::string& id, std::int64_t whenMs);

    // ---- playlists ----
    PlaylistRecord& EnsureBuiltin(PlaylistKind kind, const std::string& name);
    PlaylistRecord& CreatePlaylist(const std::string& name);
    bool DeletePlaylist(const std::string& id);
    bool RenamePlaylist(const std::string& id, const std::string& name);
    bool AddToPlaylist(const std::string& playlistId, const std::string& trackId);
    bool RemoveFromPlaylist(const std::string& playlistId, const std::string& trackId);
    bool MoveInPlaylist(const std::string& playlistId, std::size_t from, std::size_t to);
    PlaylistRecord* FindPlaylist(const std::string& id);
    const PlaylistRecord* FindPlaylist(const std::string& id) const;

    /// Tracks sorted by play count (desc) then last played — used by Discover.
    std::vector<const TrackRecord*> TopPlayed(std::size_t count) const;
    std::vector<const TrackRecord*> RecentlyAdded(std::size_t count) const;

private:
    static std::string NewId();
    void RebuildTrackIndex();

    LibraryData data_;
    std::unordered_map<std::string, std::size_t> m_trackIndex;
};

} // namespace wm::core
