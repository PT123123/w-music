#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wm::core {

struct LyricLine {
    std::int64_t timeMs = 0;
    std::string text;
};

struct LyricMetadata {
    std::string title;
    std::string artist;
    std::string album;
    std::string by;
};

struct LyricDocument {
    LyricMetadata meta;
    std::vector<LyricLine> lines;
    /// Global shift applied to every line (from [offset:] tag or user tweak).
    std::int64_t offsetMs = 0;
    bool valid = false;

    bool empty() const noexcept { return lines.empty(); }
    std::size_t size() const noexcept { return lines.size(); }

    /// Time of a line with the global offset applied.
    std::int64_t EffectiveTime(std::size_t index) const noexcept;

    /// Index of the line that should be highlighted at |positionMs|.
    /// Returns -1 when the position is before the first line.
    std::ptrdiff_t IndexAt(std::int64_t positionMs) const noexcept;
};

} // namespace wm::core
