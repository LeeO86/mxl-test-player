#include "motion.hpp"

#include "util.hpp"

#include <algorithm>
#include <cmath>

namespace mtp {
namespace {

double triangle(double distance, double span) {
    if (span <= 1e-9) return 0;
    double m = std::fmod(distance, 2.0 * span);
    if (m < 0) m += 2.0 * span;
    return m <= span ? m : (2.0 * span - m);
}

struct Px {
    double x, y, w, h;
};

Px base_position(const MotionParams& m, std::uint64_t grain, const VideoFormat& fmt) {
    const double t = static_cast<double>(grain) * static_cast<double>(fmt.grain_rate().den) /
                     static_cast<double>(fmt.grain_rate().num);
    const double box_h = clampd(m.size, 0.02, 0.9) * fmt.height;
    const double aspect = m.content_h > 0 ? static_cast<double>(m.content_w) / static_cast<double>(m.content_h) : 16.0 / 9.0;
    double box_w = box_h * aspect;
    if (box_w > fmt.width * 0.9) {
        box_w = fmt.width * 0.9;
    }
    const double span_x = std::max(0.0, static_cast<double>(fmt.width) - box_w);
    const double span_y = std::max(0.0, static_cast<double>(fmt.height) - box_h);
    const double speed_px = m.speed * fmt.width;  // pixels per second along the primary axis
    Px p{0, 0, box_w, box_h};
    switch (m.path) {
        case MotionPath::Horizontal:
            p.x = triangle(m.start_x * span_x + speed_px * t, span_x);
            p.y = m.start_y * span_y;
            break;
        case MotionPath::Vertical:
            p.x = m.start_x * span_x;
            p.y = triangle(m.start_y * span_y + speed_px * t, span_y);
            break;
        case MotionPath::Circle: {
            const double ang0 = m.start_x * 2.0 * 3.141592653589793;
            const double ang = ang0 + t * m.speed * 2.0 * 3.141592653589793;
            const double cx = fmt.width / 2.0;
            const double cy = fmt.height / 2.0;
            const double rx = std::max(0.0, (fmt.width - box_w) / 2.0);
            const double ry = std::max(0.0, (fmt.height - box_h) / 2.0);
            p.x = cx + std::cos(ang) * rx - box_w / 2.0;
            p.y = cy + std::sin(ang) * ry - box_h / 2.0;
            break;
        }
        case MotionPath::Diagonal:
            p.x = triangle(m.start_x * span_x + speed_px * t, span_x);
            p.y = triangle(m.start_y * span_y + speed_px * 0.65 * t, span_y);
            break;
        case MotionPath::Bounce:
        default:
            p.x = triangle(m.start_x * span_x + speed_px * t, span_x);
            p.y = triangle(m.start_y * span_y + speed_px * 0.72 * t, span_y);
            break;
    }
    return p;
}

}  // namespace

MotionPath parse_motion_path(std::string_view s) {
    if (s == "horizontal") return MotionPath::Horizontal;
    if (s == "vertical") return MotionPath::Vertical;
    if (s == "circle") return MotionPath::Circle;
    if (s == "diagonal") return MotionPath::Diagonal;
    return MotionPath::Bounce;
}

const char* motion_path_name(MotionPath p) {
    switch (p) {
        case MotionPath::Horizontal: return "horizontal";
        case MotionPath::Vertical: return "vertical";
        case MotionPath::Circle: return "circle";
        case MotionPath::Diagonal: return "diagonal";
        default: return "bounce";
    }
}

MotionBox moving_box_at(const MotionParams& m, std::uint64_t grain_index, const VideoFormat& fmt) {
    const Px p = base_position(m, grain_index, fmt);
    MotionBox b;
    b.w = std::max(2, static_cast<int>(std::lround(p.w)));
    b.h = std::max(2, static_cast<int>(std::lround(p.h)));
    b.x = static_cast<int>(std::lround(p.x + m.continuity_x));
    b.y = static_cast<int>(std::lround(p.y + m.continuity_y));
    b.x = clampi(b.x, 0, std::max(0, fmt.width - b.w));
    b.y = clampi(b.y, 0, std::max(0, fmt.height - b.h));
    b.opacity = clampd(m.opacity, 0.0, 1.0);
    return b;
}

void retarget_motion(MotionParams& next, const MotionParams& prev, std::uint64_t grain_index, const VideoFormat& fmt) {
    const MotionBox cur = moving_box_at(prev, grain_index, fmt);
    next.continuity_x = 0;
    next.continuity_y = 0;
    const MotionBox neu = moving_box_at(next, grain_index, fmt);
    next.continuity_x = static_cast<double>(cur.x - neu.x);
    next.continuity_y = static_cast<double>(cur.y - neu.y);
}

int sprite_frame_at_time(double time_s, const double* durations, int count, double& total_out) {
    total_out = 0;
    if (count <= 0) return 0;
    for (int i = 0; i < count; ++i) total_out += std::max(0.0, durations[i]);
    if (total_out <= 0) return 0;
    double t = std::fmod(time_s, total_out);
    if (t < 0) t += total_out;
    double acc = 0;
    for (int i = 0; i < count; ++i) {
        acc += std::max(0.0, durations[i]);
        if (t < acc) return i;
    }
    return count - 1;
}

}  // namespace mtp
