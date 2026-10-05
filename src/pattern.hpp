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
    AvSync
};

VideoPattern parse_video_pattern(std::string_view s);
const char* video_pattern_name(VideoPattern p);

struct PatternRequest {
    VideoFormat format;
    VideoPattern pattern = VideoPattern::SmpteRp219;
    bool pluge = false;
    std::uint64_t frame_index = 0;  // frame at the output frame rate
    int field = -1;                 // 0/1 when writing an interlaced field grain, else -1
    bool flash = false;
};

// Writes one MXL grain (a frame, or one field when format is interlaced and field >= 0).
void render_pattern(const PatternRequest& req, std::uint8_t* v210);

// A frame that stays the same from grain to grain, and a token that identifies its content:
// the same token (compared by address, kept alive by its holder) means the same bytes.
struct StillFrame {
    const std::uint8_t* data = nullptr;
    std::shared_ptr<const void> token;
};

// The cached frame of a pattern that does not move (rendered on first use, per thread), or an
// empty StillFrame for a moving one.
StillFrame still_pattern(const PatternRequest& req);

}  // namespace mtp
