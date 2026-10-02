#pragma once

#include "util.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace mtp {

struct Rational {
    std::int64_t num = 0;
    std::int64_t den = 1;

    constexpr bool operator==(const Rational& o) const { return num == o.num && den == o.den; }
    constexpr double to_double() const { return den == 0 ? 0.0 : static_cast<double>(num) / static_cast<double>(den); }
};

enum class Scan { Progressive, InterlacedTff, InterlacedBff };

struct VideoFormat {
    std::string name;
    int width = 1920;
    int height = 1080;
    Scan scan = Scan::Progressive;
    Rational frame_rate{50, 1};

    bool interlaced() const { return scan != Scan::Progressive; }
    // Rate of an MXL grain. Pinned MXL 218ddaa stores interlaced video as fields
    // and doubles the frame rate declared in the flow definition.
    Rational grain_rate() const {
        if (!interlaced()) return frame_rate;
        return Rational{frame_rate.num * 2, frame_rate.den};
    }
    int field_height() const { return interlaced() ? height / 2 : height; }
    const char* interlace_mode() const {
        switch (scan) {
            case Scan::InterlacedTff: return "interlaced_tff";
            case Scan::InterlacedBff: return "interlaced_bff";
            default: return "progressive";
        }
    }
    // Rounded frames per second used for "once per second" sync marks.
    int nominal_fps() const {
        return static_cast<int>((frame_rate.num + frame_rate.den / 2) / frame_rate.den);
    }
};

std::optional<VideoFormat> parse_format(std::string_view text);
std::string format_list_help();

// Audio samples from the SMPTE epoch up to the start of grain `index`
// at `grain_rate` (the rate of the grains being written).
std::uint64_t samples_until_grain(std::uint64_t index, Rational grain_rate, std::int64_t sample_rate = 48000);
std::uint32_t samples_in_grain(std::uint64_t index, Rational grain_rate, std::int64_t sample_rate = 48000);

bool is_sync_frame(std::uint64_t frame_index, Rational frame_rate);

std::uint64_t grain_duration_ns(Rational grain_rate);
std::uint64_t index_to_timestamp_ns(std::uint64_t index, Rational grain_rate);
std::uint64_t timestamp_ns_to_index(std::uint64_t ts, Rational grain_rate);

}  // namespace mtp
