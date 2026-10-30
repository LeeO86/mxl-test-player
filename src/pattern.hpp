#pragma once

#include "format.hpp"
#include "v210.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace mtp {

enum class VideoPattern {
    SmpteRp219,
    Ebu100_75,
    Ebu100_100,
    Bars75,
    GreyRampH,
    GreyRampV,
    LumaSteps,
    Multiburst,
    Sweep,
    ZonePlate,
    ZonePlateMoving,
    Checkerboard,
    Grid,
    Circle,
    SafeArea,
    Black,
    White,
    Red,
    Green,
    Blue,
    Grey50,
    Motion,
    FieldOrder,
    AvSync,
    Transparent  // black at alpha 0 (v210a) or key_min (fill_key): a keyed output of burn-ins only
};

VideoPattern parse_video_pattern(std::string_view s);
const char* video_pattern_name(VideoPattern p);

struct PatternRequest {
    VideoFormat format;
    VideoPattern pattern = VideoPattern::SmpteRp219;
    bool pluge = false;
    std::uint64_t frame_index = 0;  // frame at the output frame rate
    // Interlaced: the field in time (0 first, 1 second) the frame is rendered for, else -1.
    // Moving patterns are sampled at that field's time (half a frame later for field 1); the
    // writer cuts the field's lines out of the whole frame.
    int field = -1;
    bool flash = false;
};

// Renders a whole frame (format.width x format.height).
void render_pattern(const PatternRequest& req, std::uint8_t* v210);

// A frame that stays the same from grain to grain, and a token that identifies its content:
// the same token (compared by address, kept alive by its holder) means the same bytes.
struct StillFrame {
    const std::uint8_t* data = nullptr;
    std::shared_ptr<const void> token;
};

// True for the patterns that change from frame to frame (rendered per grain).
bool pattern_moves(VideoPattern p);

// The cached frame of a pattern that does not move (rendered on first use, per thread), or an
// empty StillFrame for a moving one.
StillFrame still_pattern(const PatternRequest& req);

}  // namespace mtp
