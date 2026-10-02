#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mtp {

struct PlaylistEntry {
    std::string item_id;
    std::int64_t frames = 0;  // duration of one pass
    int loops = 1;            // 0 = infinite (only meaningful on the last entry)
};

struct PlaylistLocate {
    int entry = 0;
    std::int64_t frame_in_item = 0;
    int loop_index = 0;
    bool ended = false;
};

// Map a monotonic playhead (frames since play start) onto a playlist entry.
PlaylistLocate locate_playlist(const std::vector<PlaylistEntry>& entries, std::int64_t playhead);

}  // namespace mtp
