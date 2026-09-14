#include "wm/core/PlayQueue.h"

#include <algorithm>

namespace wm::core {

void PlayQueue::SetTracks(std::vector<std::string> ids, std::ptrdiff_t startIndex) {
    ids_ = std::move(ids);
    mode_ = mode_; // unchanged

    if (ids_.empty()) {
        index_ = -1;
        shuffleOrder_.clear();
        shufflePos_ = 0;
        return;
    }

    if (startIndex < 0 || static_cast<std::size_t>(startIndex) >= ids_.size()) {
        startIndex = 0;
    }
    index_ = startIndex;

    if (mode_ == PlayMode::Shuffle) {
        Reshuffle();
    }
}

void PlayQueue::Clear() {
    ids_.clear();
    shuffleOrder_.clear();
    shufflePos_ = 0;
    index_ = -1;
}

void PlayQueue::SetMode(PlayMode mode) noexcept {
    if (mode_ == mode) {
        return;
    }
    mode_ = mode;
    if (mode == PlayMode::Shuffle) {
        Reshuffle();
    }
}

std::optional<std::string> PlayQueue::Current() const {
    if (index_ < 0 || static_cast<std::size_t>(index_) >= ids_.size()) {
        return std::nullopt;
    }
    return ids_[static_cast<std::size_t>(index_)];
}

void PlayQueue::Reshuffle() {
    shuffleOrder_.resize(ids_.size());
    for (std::size_t i = 0; i < ids_.size(); ++i) {
        shuffleOrder_[i] = i;
    }
    std::shuffle(shuffleOrder_.begin(), shuffleOrder_.end(), rng_);

    // Make sure the currently playing track comes first so that "next" does not
    // immediately repeat it.
    if (index_ >= 0 && !shuffleOrder_.empty()) {
        auto it = std::find(shuffleOrder_.begin(), shuffleOrder_.end(), static_cast<std::size_t>(index_));
        if (it != shuffleOrder_.end()) {
            std::iter_swap(shuffleOrder_.begin(), it);
        }
        shufflePos_ = 0;
    } else {
        shufflePos_ = 0;
    }
}

std::ptrdiff_t PlayQueue::ShuffleAdvance(bool forward) {
    if (ids_.empty()) {
        return -1;
    }
    if (shuffleOrder_.size() != ids_.size()) {
        Reshuffle();
    }

    if (!forward) {
        if (shufflePos_ == 0) {
            shufflePos_ = shuffleOrder_.size() - 1;
        } else {
            --shufflePos_;
        }
    } else {
        ++shufflePos_;
        if (shufflePos_ >= shuffleOrder_.size()) {
            Reshuffle();
            shufflePos_ = 0;
        }
    }
    return static_cast<std::ptrdiff_t>(shuffleOrder_[shufflePos_]);
}

std::optional<std::string> PlayQueue::Next(bool autoAdvance) {
    if (ids_.empty()) {
        return std::nullopt;
    }

    if (mode_ == PlayMode::RepeatOne) {
        if (autoAdvance) {
            return Current(); // natural end -> repeat
        }
        // explicit user action -> move on
    }

    if (mode_ == PlayMode::Shuffle) {
        index_ = ShuffleAdvance(true);
        return Current();
    }

    std::ptrdiff_t next = index_ + 1;
    if (next >= static_cast<std::ptrdiff_t>(ids_.size())) {
        if (mode_ == PlayMode::Sequential) {
            index_ = -1;
            return std::nullopt;
        }
        next = 0; // LoopAll / RepeatOne fallback
    }
    index_ = next;
    return Current();
}

std::optional<std::string> PlayQueue::Previous() {
    if (ids_.empty()) {
        return std::nullopt;
    }

    if (mode_ == PlayMode::Shuffle) {
        index_ = ShuffleAdvance(false);
        return Current();
    }

    std::ptrdiff_t prev = index_ - 1;
    if (prev < 0) {
        prev = static_cast<std::ptrdiff_t>(ids_.size()) - 1;
    }
    index_ = prev;
    return Current();
}

std::optional<std::string> PlayQueue::JumpTo(std::size_t index) {
    if (index >= ids_.size()) {
        return std::nullopt;
    }
    index_ = static_cast<std::ptrdiff_t>(index);

    if (mode_ == PlayMode::Shuffle) {
        const auto it = std::find(shuffleOrder_.begin(), shuffleOrder_.end(), index);
        if (it != shuffleOrder_.end()) {
            shufflePos_ = static_cast<std::size_t>(std::distance(shuffleOrder_.begin(), it));
        }
    }
    return Current();
}

std::optional<std::string> PlayQueue::JumpToId(const std::string& id) {
    const auto it = std::find(ids_.begin(), ids_.end(), id);
    if (it == ids_.end()) {
        return std::nullopt;
    }
    return JumpTo(static_cast<std::size_t>(std::distance(ids_.begin(), it)));
}

} // namespace wm::core
