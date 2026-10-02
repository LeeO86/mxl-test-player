#include "playlist.hpp"

#include <algorithm>

namespace mtp {

PlaylistLocate locate_playlist(const std::vector<PlaylistEntry>& entries, std::int64_t playhead) {
    PlaylistLocate loc;
    if (entries.empty()) {
        loc.ended = true;
        return loc;
    }
    if (playhead < 0) playhead = 0;
    std::int64_t left = playhead;
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        const auto& e = entries[static_cast<std::size_t>(i)];
        const std::int64_t frames = e.frames > 0 ? e.frames : 1;
        const bool infinite = e.loops <= 0;
        if (infinite) {
            loc.entry = i;
            loc.loop_index = static_cast<int>(left / frames);
            loc.frame_in_item = left % frames;
            return loc;
        }
        const std::int64_t span = frames * static_cast<std::int64_t>(e.loops);
        if (left < span) {
            loc.entry = i;
            loc.loop_index = static_cast<int>(left / frames);
            loc.frame_in_item = left % frames;
            return loc;
        }
        left -= span;
    }
    const auto& last = entries.back();
    const std::int64_t frames = last.frames > 0 ? last.frames : 1;
    loc.entry = static_cast<int>(entries.size()) - 1;
    loc.loop_index = std::max(0, last.loops - 1);
    loc.frame_in_item = frames - 1;
    loc.ended = true;
    return loc;
}

}  // namespace mtp
