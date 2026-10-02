#include "pattern.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace mtp {
namespace {

struct Bar {
    const char* name;
    double r, g, b;
};

// 75% and 100% colour-bar sets in full-range RGB, converted to legal BT.709.
const Bar k75[] = {
    {"w", 0.75, 0.75, 0.75}, {"y", 0.75, 0.75, 0.00}, {"c", 0.00, 0.75, 0.75}, {"g", 0.00, 0.75, 0.00},
    {"m", 0.75, 0.00, 0.75}, {"r", 0.75, 0.00, 0.00}, {"b", 0.00, 0.00, 0.75},
};
const Bar k100[] = {
    {"w", 1, 1, 1}, {"y", 1, 1, 0}, {"c", 0, 1, 1}, {"g", 0, 1, 0}, {"m", 1, 0, 1}, {"r", 1, 0, 0}, {"b", 0, 0, 1},
};
const Bar kEbu10075[] = {
    {"w", 1, 1, 1}, {"y", 0.75, 0.75, 0}, {"c", 0, 0.75, 0.75}, {"g", 0, 0.75, 0},
    {"m", 0.75, 0, 0.75}, {"r", 0.75, 0, 0}, {"b", 0, 0, 0.75},
};

struct Raster {
    int w, h;
    std::vector<std::uint16_t> y, cb, cr;
    explicit Raster(int w_, int h_)
        : w(w_), h(h_), y(static_cast<std::size_t>(w) * h, kYBlack),
          cb(static_cast<std::size_t>(w / 2) * h, kCMid), cr(static_cast<std::size_t>(w / 2) * h, kCMid) {}
    void set_rgb(int x, int y0, double r, double g, double b) {
        if (x < 0 || y0 < 0 || x >= w || y0 >= h) return;
        const Yuv10 p = rgb_to_yuv709(r, g, b);
        y[static_cast<std::size_t>(y0) * w + x] = static_cast<std::uint16_t>(p.y);
        const int c = x / 2;
        cb[static_cast<std::size_t>(y0) * (w / 2) + c] = static_cast<std::uint16_t>(p.cb);
        cr[static_cast<std::size_t>(y0) * (w / 2) + c] = static_cast<std::uint16_t>(p.cr);
    }
    void fill_rect_rgb(int x0, int y0, int rw, int rh, double r, double g, double b) {
        for (int y = y0; y < y0 + rh; ++y)
            for (int x = x0; x < x0 + rw; ++x) set_rgb(x, y, r, g, b);
    }
    void set_yuv(int x, int y0, int yy, int cbv, int crv) {
        if (x < 0 || y0 < 0 || x >= w || y0 >= h) return;
        y[static_cast<std::size_t>(y0) * w + x] = static_cast<std::uint16_t>(yy);
        const int c = x / 2;
        cb[static_cast<std::size_t>(y0) * (w / 2) + c] = static_cast<std::uint16_t>(cbv);
        cr[static_cast<std::size_t>(y0) * (w / 2) + c] = static_cast<std::uint16_t>(crv);
    }
    void pack(std::uint8_t* dst) const {
        const std::uint32_t stride = v210_line_stride(w);
        for (int row = 0; row < h; ++row) {
            pack_v210_line(y.data() + static_cast<std::size_t>(row) * w, cb.data() + static_cast<std::size_t>(row) * (w / 2),
                           cr.data() + static_cast<std::size_t>(row) * (w / 2), w, dst + static_cast<std::size_t>(row) * stride);
        }
    }
};

void bars(Raster& r, const Bar* set, int n, bool pluge) {
    const int bar_h = pluge ? (r.h * 2) / 3 : r.h;
    const int bw = r.w / n;
    for (int i = 0; i < n; ++i) {
        const int x0 = i * bw;
        const int rw = (i == n - 1) ? (r.w - x0) : bw;
        r.fill_rect_rgb(x0, 0, rw, bar_h, set[i].r, set[i].g, set[i].b);
    }
    if (!pluge) return;
    // Bottom third: reverse-order chips and a PLUGE block.
    const int y1 = bar_h;
    const int mid_h = (r.h - bar_h) / 2;
    for (int i = 0; i < n; ++i) {
        const int src = n - 1 - i;
        const int x0 = i * bw;
        const int rw = (i == n - 1) ? (r.w - x0) : bw;
        r.fill_rect_rgb(x0, y1, rw, mid_h, set[src].r, set[src].g, set[src].b);
    }
    const int y2 = y1 + mid_h;
    const int rh = r.h - y2;
    // Black, -2% superblack, black, +2% grey, black, white.
    const int slots = 6;
    const int sw = r.w / slots;
    const double vals[] = {0.0, 0.0, 0.0, 0.02, 0.0, 1.0};
    const int yvals[] = {kYBlack, std::max(4, kYBlack - 18), kYBlack, kYBlack + 18, kYBlack, kYWhite};
    for (int i = 0; i < slots; ++i) {
        const int x0 = i * sw;
        const int rw = (i == slots - 1) ? r.w - x0 : sw;
        for (int y = y2; y < y2 + rh; ++y)
            for (int x = x0; x < x0 + rw; ++x) r.set_yuv(x, y, yvals[i], kCMid, kCMid);
        (void)vals;
    }
}

void ramp_h(Raster& r) {
    for (int y = 0; y < r.h; ++y)
        for (int x = 0; x < r.w; ++x) {
            const double t = r.w <= 1 ? 0 : static_cast<double>(x) / (r.w - 1);
            r.set_yuv(x, y, y_from_unit(t), kCMid, kCMid);
        }
}

void ramp_v(Raster& r) {
    for (int y = 0; y < r.h; ++y) {
        const double t = r.h <= 1 ? 0 : static_cast<double>(y) / (r.h - 1);
        const int yy = y_from_unit(t);
        for (int x = 0; x < r.w; ++x) r.set_yuv(x, y, yy, kCMid, kCMid);
    }
}

void luma_steps(Raster& r) {
    const int steps = 10;
    const int bw = std::max(1, r.w / steps);
    for (int i = 0; i < steps; ++i) {
        const double t = steps == 1 ? 0 : static_cast<double>(i) / (steps - 1);
        const int x0 = i * bw;
        const int rw = (i == steps - 1) ? r.w - x0 : bw;
        r.fill_rect_rgb(x0, 0, rw, r.h, t, t, t);
    }
}

void multiburst(Raster& r, bool sweep) {
    const int bands = sweep ? 1 : 6;
    const double freqs[] = {0.5, 1, 2, 3, 4, 5};  // cycles per 100 pixels roughly scaled below
    for (int y = 0; y < r.h; ++y) {
        for (int x = 0; x < r.w; ++x) {
            double cycles = 0;
            if (sweep) {
                const double t = static_cast<double>(x) / std::max(1, r.w);
                cycles = 1.0 + t * t * (r.w / 8.0);
            } else {
                const int b = std::min(bands - 1, x * bands / std::max(1, r.w));
                const double cpi = freqs[b] * (r.w / 1920.0);
                cycles = cpi;
            }
            const double p = std::sin(2.0 * 3.141592653589793 * x * (sweep ? (1.0 + (static_cast<double>(x) / r.w) * (r.w / 16.0)) / r.w
                                                                            : cycles / 100.0));
            // Simpler and stable: spatial frequency in cycles across the band.
            (void)p;
            double s;
            if (sweep) {
                const double t = static_cast<double>(x) / std::max(1, r.w - 1);
                const double f = 0.002 + t * t * 0.45;  // cycles per pixel
                s = std::sin(2.0 * 3.141592653589793 * f * x);
            } else {
                const int b = std::min(bands - 1, x * bands / r.w);
                const double cpp = (0.01 + freqs[b] * 0.02) * (1920.0 / r.w);
                s = std::sin(2.0 * 3.141592653589793 * cpp * x);
            }
            const int yy = y_from_unit(0.5 + 0.5 * s);
            r.set_yuv(x, y, yy, kCMid, kCMid);
            (void)cycles;
        }
    }
}

void zone(Raster& r, std::uint64_t frame, bool moving) {
    const double cx = (r.w - 1) / 2.0 + (moving ? std::sin(frame * 0.07) * r.w * 0.15 : 0);
    const double cy = (r.h - 1) / 2.0;
    const double k = (moving ? 0.0008 + 0.00005 * std::sin(frame * 0.03) : 0.0012) * (1920.0 / r.w);
    for (int y = 0; y < r.h; ++y) {
        for (int x = 0; x < r.w; ++x) {
            const double dx = x - cx;
            const double dy = y - cy;
            const double s = std::sin(k * (dx * dx + dy * dy) + (moving ? frame * 0.15 : 0));
            r.set_yuv(x, y, y_from_unit(0.5 + 0.5 * s), kCMid, kCMid);
        }
    }
}

void checker(Raster& r) {
    const int cell = std::max(8, r.w / 16);
    for (int y = 0; y < r.h; ++y)
        for (int x = 0; x < r.w; ++x) {
            const bool on = ((x / cell) ^ (y / cell)) & 1;
            r.set_yuv(x, y, on ? kYWhite : kYBlack, kCMid, kCMid);
        }
}

void grid(Raster& r) {
    for (int y = 0; y < r.h; ++y)
        for (int x = 0; x < r.w; ++x) r.set_yuv(x, y, y_from_unit(0.15), kCMid, kCMid);
    auto line_v = [&](int x) {
        for (int y = 0; y < r.h; ++y) r.set_yuv(x, y, kYWhite, kCMid, kCMid);
    };
    auto line_h = [&](int y) {
        for (int x = 0; x < r.w; ++x) r.set_yuv(x, y, kYWhite, kCMid, kCMid);
    };
    for (int i = 0; i <= 10; ++i) {
        line_v(std::min(r.w - 1, i * r.w / 10));
        line_h(std::min(r.h - 1, i * r.h / 10));
    }
    line_v(r.w / 2);
    line_h(r.h / 2);
}

void circle(Raster& r) {
    for (int y = 0; y < r.h; ++y)
        for (int x = 0; x < r.w; ++x) r.set_yuv(x, y, kYBlack, kCMid, kCMid);
    const double cx = (r.w - 1) / 2.0;
    const double cy = (r.h - 1) / 2.0;
    const double rad = r.h * 0.45;
    for (int y = 0; y < r.h; ++y) {
        for (int x = 0; x < r.w; ++x) {
            const double dx = (x - cx) / rad;
            const double dy = (y - cy) / rad;
            const double d = std::sqrt(dx * dx + dy * dy);
            if (std::fabs(d - 1.0) < 1.5 / rad || d < 0.01) r.set_yuv(x, y, kYWhite, kCMid, kCMid);
        }
    }
    for (int x = 0; x < r.w; ++x) r.set_yuv(x, r.h / 2, kYWhite, kCMid, kCMid);
    for (int y = 0; y < r.h; ++y) r.set_yuv(r.w / 2, y, kYWhite, kCMid, kCMid);
}

void safe_area(Raster& r) {
    grid(r);
    auto box = [&](double frac, int yv) {
        const int x0 = static_cast<int>(r.w * (1.0 - frac) / 2.0);
        const int y0 = static_cast<int>(r.h * (1.0 - frac) / 2.0);
        const int x1 = r.w - x0 - 1;
        const int y1 = r.h - y0 - 1;
        for (int x = x0; x <= x1; ++x) {
            r.set_yuv(x, y0, yv, kCMid, kCMid);
            r.set_yuv(x, y1, yv, kCMid, kCMid);
        }
        for (int y = y0; y <= y1; ++y) {
            r.set_yuv(x0, y, yv, kCMid, kCMid);
            r.set_yuv(x1, y, yv, kCMid, kCMid);
        }
    };
    box(0.90, kYWhite);
    box(0.80, y_from_unit(0.75));
}

void solid(Raster& r, double rr, double g, double b) { r.fill_rect_rgb(0, 0, r.w, r.h, rr, g, b); }

void motion(Raster& r, std::uint64_t frame) {
    solid(r, 0.05, 0.05, 0.08);
    const int bw = std::max(8, r.w / 32);
    const int x = static_cast<int>(frame % static_cast<std::uint64_t>(std::max(1, r.w - bw)));
    r.fill_rect_rgb(x, 0, bw, r.h, 1, 0.85, 0.1);
    // Rotating segment: a radial spoke so a frozen frame is obvious.
    const double ang = frame * 0.17;
    const double cx = r.w / 2.0;
    const double cy = r.h / 2.0;
    for (int t = 0; t < r.h / 2; ++t) {
        const int x0 = static_cast<int>(cx + std::cos(ang) * t);
        const int y0 = static_cast<int>(cy + std::sin(ang) * t * (r.h / static_cast<double>(r.w)));
        r.set_rgb(x0, y0, 0.1, 0.8, 1.0);
    }
}

void field_order(Raster& r, std::uint64_t frame, int field) {
    solid(r, 0, 0, 0);
    const int step = static_cast<int>(frame) * (field < 0 ? 1 : 2) + (field < 0 ? 0 : field);
    const int bw = std::max(6, r.w / 40);
    const int x = (step * bw) % std::max(1, r.w - bw);
    r.fill_rect_rgb(x, r.h / 4, bw, r.h / 2, 1, 1, 1);
    // Label stripe on field 1 so a swapped field is a different shade.
    if (field == 1) r.fill_rect_rgb(0, 0, r.w, std::max(2, r.h / 40), 1, 0, 0);
    else r.fill_rect_rgb(0, 0, r.w, std::max(2, r.h / 40), 0, 1, 0);
}

void smpte(Raster& r, bool pluge) {
    // RP 219 inspired layout: 75% bars, then a row of sub-bars, then PLUGE.
    bars(r, k75, 7, false);
    const int y1 = (r.h * 7) / 12;
    const int y2 = (r.h * 5) / 6;
    const Bar sub[] = {
        {"c", 0, 0.75, 0.75}, {"w", 1, 1, 1}, {"y", 0.75, 0.75, 0}, {"b", 0, 0, 0.75},
        {"k", 0, 0, 0},       {"r", 0.75, 0, 0}, {"m", 0.75, 0, 0.75},
    };
    const int n = 7;
    const int bw = r.w / n;
    for (int i = 0; i < n; ++i) {
        const int x0 = i * bw;
        const int rw = (i == n - 1) ? r.w - x0 : bw;
        r.fill_rect_rgb(x0, y1, rw, y2 - y1, sub[i].r, sub[i].g, sub[i].b);
    }
    if (pluge) {
        const int slots = 8;
        const int sw = r.w / slots;
        const int ys[] = {kYBlack, kYBlack - 14, kYBlack, kYBlack + 14, kYBlack, y_from_unit(0.5), kYWhite, kYBlack};
        for (int i = 0; i < slots; ++i) {
            const int x0 = i * sw;
            const int rw = (i == slots - 1) ? r.w - x0 : sw;
            const int yy = std::clamp(ys[i], 4, 1019);
            for (int y = y2; y < r.h; ++y)
                for (int x = x0; x < x0 + rw; ++x) r.set_yuv(x, y, yy, kCMid, kCMid);
        }
    } else {
        r.fill_rect_rgb(0, y2, r.w, r.h - y2, 0, 0, 0);
    }
}

}  // namespace

VideoPattern parse_video_pattern(std::string_view s) {
    if (s == "ebu_100_75" || s == "ebu100/75") return VideoPattern::Ebu100_75;
    if (s == "ebu_100_100" || s == "ebu100/100") return VideoPattern::Ebu100_100;
    if (s == "bars_75" || s == "ebu_75") return VideoPattern::Bars75;
    if (s == "grey_ramp_h" || s == "ramp_h") return VideoPattern::GreyRampH;
    if (s == "grey_ramp_v" || s == "ramp_v") return VideoPattern::GreyRampV;
    if (s == "luma_steps") return VideoPattern::LumaSteps;
    if (s == "multiburst") return VideoPattern::Multiburst;
    if (s == "sweep") return VideoPattern::Sweep;
    if (s == "zone_plate") return VideoPattern::ZonePlate;
    if (s == "zone_plate_moving") return VideoPattern::ZonePlateMoving;
    if (s == "checkerboard") return VideoPattern::Checkerboard;
    if (s == "grid" || s == "crosshatch") return VideoPattern::Grid;
    if (s == "circle") return VideoPattern::Circle;
    if (s == "safe_area") return VideoPattern::SafeArea;
    if (s == "black") return VideoPattern::Black;
    if (s == "white") return VideoPattern::White;
    if (s == "red") return VideoPattern::Red;
    if (s == "green") return VideoPattern::Green;
    if (s == "blue") return VideoPattern::Blue;
    if (s == "grey50" || s == "grey_50") return VideoPattern::Grey50;
    if (s == "motion") return VideoPattern::Motion;
    if (s == "field_order") return VideoPattern::FieldOrder;
    if (s == "av_sync") return VideoPattern::AvSync;
    return VideoPattern::SmpteRp219;
}

const char* video_pattern_name(VideoPattern p) {
    switch (p) {
        case VideoPattern::Ebu100_75: return "ebu_100_75";
        case VideoPattern::Ebu100_100: return "ebu_100_100";
        case VideoPattern::Bars75: return "bars_75";
        case VideoPattern::GreyRampH: return "grey_ramp_h";
        case VideoPattern::GreyRampV: return "grey_ramp_v";
        case VideoPattern::LumaSteps: return "luma_steps";
        case VideoPattern::Multiburst: return "multiburst";
        case VideoPattern::Sweep: return "sweep";
        case VideoPattern::ZonePlate: return "zone_plate";
        case VideoPattern::ZonePlateMoving: return "zone_plate_moving";
        case VideoPattern::Checkerboard: return "checkerboard";
        case VideoPattern::Grid: return "grid";
        case VideoPattern::Circle: return "circle";
        case VideoPattern::SafeArea: return "safe_area";
        case VideoPattern::Black: return "black";
        case VideoPattern::White: return "white";
        case VideoPattern::Red: return "red";
        case VideoPattern::Green: return "green";
        case VideoPattern::Blue: return "blue";
        case VideoPattern::Grey50: return "grey50";
        case VideoPattern::Motion: return "motion";
        case VideoPattern::FieldOrder: return "field_order";
        case VideoPattern::AvSync: return "av_sync";
        default: return "smpte_rp219";
    }
}

void render_pattern(const PatternRequest& req, std::uint8_t* v210) {
    const int w = req.format.width;
    const int h = (req.field >= 0 && req.format.interlaced()) ? req.format.field_height() : req.format.height;
    Raster r(w, h);
    const std::uint64_t frame = req.frame_index;
    switch (req.pattern) {
        case VideoPattern::Ebu100_75: bars(r, kEbu10075, 7, req.pluge); break;
        case VideoPattern::Ebu100_100: bars(r, k100, 7, req.pluge); break;
        case VideoPattern::Bars75: bars(r, k75, 7, req.pluge); break;
        case VideoPattern::GreyRampH: ramp_h(r); break;
        case VideoPattern::GreyRampV: ramp_v(r); break;
        case VideoPattern::LumaSteps: luma_steps(r); break;
        case VideoPattern::Multiburst: multiburst(r, false); break;
        case VideoPattern::Sweep: multiburst(r, true); break;
        case VideoPattern::ZonePlate: zone(r, frame, false); break;
        case VideoPattern::ZonePlateMoving: zone(r, frame, true); break;
        case VideoPattern::Checkerboard: checker(r); break;
        case VideoPattern::Grid: grid(r); break;
        case VideoPattern::Circle: circle(r); break;
        case VideoPattern::SafeArea: safe_area(r); break;
        case VideoPattern::Black: solid(r, 0, 0, 0); break;
        case VideoPattern::White: solid(r, 1, 1, 1); break;
        case VideoPattern::Red: solid(r, 1, 0, 0); break;
        case VideoPattern::Green: solid(r, 0, 1, 0); break;
        case VideoPattern::Blue: solid(r, 0, 0, 1); break;
        case VideoPattern::Grey50: solid(r, 0.5, 0.5, 0.5); break;
        case VideoPattern::Motion: motion(r, frame); break;
        case VideoPattern::FieldOrder: field_order(r, frame, req.field); break;
        case VideoPattern::AvSync: bars(r, k75, 7, false); break;
        case VideoPattern::SmpteRp219:
        default: smpte(r, req.pluge); break;
    }
    if (req.flash || (req.pattern == VideoPattern::AvSync && is_sync_frame(frame, req.format.frame_rate))) {
        solid(r, 1, 1, 1);
    }
    r.pack(v210);
}

}  // namespace mtp
