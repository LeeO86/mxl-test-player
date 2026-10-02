#pragma once

#include "format.hpp"

#include <string>

namespace mtp {

enum class MotionPath { Bounce, Horizontal, Vertical, Circle, Diagonal };

MotionPath parse_motion_path(std::string_view s);
const char* motion_path_name(MotionPath p);

struct MotionParams {
    MotionPath path = MotionPath::Bounce;
    double speed = 0.15;       // fraction of frame width per second
    double size = 0.12;        // fraction of frame height
    double opacity = 1.0;
    double start_x = 0.0;      // 0..1 of the travel span
    double start_y = 0.0;
    double continuity_x = 0;   // pixel delta applied so parameter edits do not jump
    double continuity_y = 0;
    int content_w = 160;       // sprite pixel size before fit (aspect)
    int content_h = 90;
};

struct MotionBox {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    double opacity = 1;
};

// Deterministic position from the TAI grain index. `continuity_*` is zero
// unless a live edit shifted the phase; persisting it makes a restart match.
MotionBox moving_box_at(const MotionParams& m, std::uint64_t grain_index, const VideoFormat& fmt);

// Adjust continuity so the box stays put at `grain_index` after `next` replaces `prev`.
void retarget_motion(MotionParams& next, const MotionParams& prev, std::uint64_t grain_index, const VideoFormat& fmt);

// Sprite frame chosen by elapsed TAI time, independent of the output rate.
int sprite_frame_at_time(double time_s, const double* durations, int count, double& total_out);

}  // namespace mtp
