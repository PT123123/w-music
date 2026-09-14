#include "wm/core/Lyric.h"

namespace wm::core {

std::int64_t LyricDocument::EffectiveTime(std::size_t index) const noexcept {
    if (index >= lines.size()) {
        return 0;
    }
    return lines[index].timeMs + offsetMs;
}

std::ptrdiff_t LyricDocument::IndexAt(std::int64_t positionMs) const noexcept {
    if (lines.empty()) {
        return -1;
    }

    // Last line whose effective time is <= positionMs.
    std::ptrdiff_t lo = 0;
    std::ptrdiff_t hi = static_cast<std::ptrdiff_t>(lines.size()) - 1;
    std::ptrdiff_t found = -1;

    while (lo <= hi) {
        const std::ptrdiff_t mid = lo + (hi - lo) / 2;
        if (EffectiveTime(static_cast<std::size_t>(mid)) <= positionMs) {
            found = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return found;
}

} // namespace wm::core
