#pragma once

#include <cstddef>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace wm::core {

enum class PlayMode : int {
    Sequential = 0, // play through once, stop at the end
    LoopAll = 1,    // play through, wrap around
    Shuffle = 2,    // random order, wrap around
    RepeatOne = 3,  // repeat the current track forever
};

/// Owns the ordered list of track ids to play and resolves next/previous
/// according to the current play mode.
class PlayQueue {
public:
    void SetTracks(std::vector<std::string> ids, std::ptrdiff_t startIndex = 0);
    void Clear();

    void SetMode(PlayMode mode) noexcept;
    PlayMode Mode() const noexcept { return mode_; }

    std::size_t Count() const noexcept { return ids_.size(); }
    std::ptrdiff_t Index() const noexcept { return index_; }
    std::optional<std::string> Current() const;

    /// Advance. |autoAdvance| is false when the user explicitly pressed next
    /// (RepeatOne is then skipped).
    std::optional<std::string> Next(bool autoAdvance);
    /// What Next(|autoAdvance|) would return right now, without moving.
    /// Shuffle is the one mode that cannot answer without consuming its
    /// shuffled order, so it peeks nothing and the caller presents that
    /// honestly ("随机播放不预告下一首").
    std::optional<std::string> PeekNext(bool autoAdvance) const;
    std::optional<std::string> Previous();
    std::optional<std::string> JumpTo(std::size_t index);
    std::optional<std::string> JumpToId(const std::string& id);

    /// Adds ids at the tail (radio flow keeps extending the queue). The
    /// current index is untouched; a stale shuffle order is rebuilt lazily.
    void Append(std::vector<std::string> ids);
    /// Drops every occurrence of |id|. When the current track itself goes,
    /// the index lands on whatever follows it (the previous one at the tail).
    /// Returns true when something was removed.
    bool RemoveAll(const std::string& id);

    const std::vector<std::string>& Ids() const noexcept { return ids_; }

private:
    void Reshuffle();
    std::ptrdiff_t ShuffleAdvance(bool forward);

    std::vector<std::string> ids_;
    std::ptrdiff_t index_ = -1;
    PlayMode mode_ = PlayMode::LoopAll;

    std::vector<std::size_t> shuffleOrder_;
    std::size_t shufflePos_ = 0;
    std::mt19937 rng_{std::random_device{}()};
};

} // namespace wm::core
