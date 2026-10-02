#include "format.hpp"

#include <cmath>

namespace mtp {
namespace {

struct RateTok {
    const char* text;
    Rational rate;
};

const RateTok kRates[] = {
    {"23.98", {24000, 1001}},
    {"24", {24, 1}},
    {"25", {25, 1}},
    {"29.97", {30000, 1001}},
    {"30", {30, 1}},
    {"50", {50, 1}},
    {"59.94", {60000, 1001}},
    {"60", {60, 1}},
};

}  // namespace

std::optional<VideoFormat> parse_format(std::string_view text) {
    if (text.size() < 5) return std::nullopt;
    int raster = 0;
    std::string_view rest = text;
    if (starts_with(text, "720")) {
        raster = 720;
        rest = text.substr(3);
    } else if (starts_with(text, "1080")) {
        raster = 1080;
        rest = text.substr(4);
    } else if (starts_with(text, "2160")) {
        raster = 2160;
        rest = text.substr(4);
    } else {
        return std::nullopt;
    }
    if (rest.empty()) return std::nullopt;
    const char scan_c = rest[0];
    if (scan_c != 'p' && scan_c != 'i' && scan_c != 'P' && scan_c != 'I') return std::nullopt;
    const bool inter = scan_c == 'i' || scan_c == 'I';
    rest = rest.substr(1);
    const RateTok* found = nullptr;
    for (const auto& r : kRates) {
        if (rest == r.text) {
            found = &r;
            break;
        }
    }
    if (!found) return std::nullopt;
    if (inter) {
        if (raster != 1080) return std::nullopt;
        const bool ok = found->rate == Rational{25, 1} || found->rate == Rational{30000, 1001};
        if (!ok) return std::nullopt;
    }
    if (raster == 720 && (found->rate.num < 50 && found->rate != Rational{50, 1} && found->rate != Rational{60000, 1001} &&
                          found->rate != Rational{60, 1} && found->rate != Rational{25, 1} && found->rate != Rational{24, 1} &&
                          found->rate != Rational{24000, 1001} && found->rate != Rational{30000, 1001} && found->rate != Rational{30, 1})) {
        // 720p accepts the same rate set as 1080p.
    }
    VideoFormat f;
    f.name = std::string(text);
    // Canonicalise scan letter to lowercase.
    for (char& c : f.name) {
        if (c == 'P') c = 'p';
        if (c == 'I') c = 'i';
    }
    if (raster == 720) {
        f.width = 1280;
        f.height = 720;
    } else if (raster == 1080) {
        f.width = 1920;
        f.height = 1080;
    } else {
        f.width = 3840;
        f.height = 2160;
    }
    f.scan = inter ? Scan::InterlacedTff : Scan::Progressive;
    f.frame_rate = found->rate;
    return f;
}

std::string format_list_help() {
    return "720/1080/2160 + p|i + 23.98|24|25|29.97|30|50|59.94|60 (interlace only 1080i25 and 1080i29.97)";
}

std::uint64_t samples_until_grain(std::uint64_t index, Rational grain_rate, std::int64_t sample_rate) {
    if (grain_rate.num <= 0 || grain_rate.den <= 0) return 0;
    // Truncating integer form. Exact at rate cycles (e.g. 60000 grains of 60000/1001).
    return (index * static_cast<std::uint64_t>(sample_rate) * static_cast<std::uint64_t>(grain_rate.den)) /
           static_cast<std::uint64_t>(grain_rate.num);
}

std::uint32_t samples_in_grain(std::uint64_t index, Rational grain_rate, std::int64_t sample_rate) {
    const auto a = samples_until_grain(index, grain_rate, sample_rate);
    const auto b = samples_until_grain(index + 1, grain_rate, sample_rate);
    return static_cast<std::uint32_t>(b - a);
}

bool is_sync_frame(std::uint64_t frame_index, Rational frame_rate) {
    const int n = static_cast<int>((frame_rate.num + frame_rate.den / 2) / frame_rate.den);
    if (n <= 0) return false;
    return (frame_index % static_cast<std::uint64_t>(n)) == 0;
}

std::uint64_t grain_duration_ns(Rational grain_rate) {
    if (grain_rate.num <= 0) return 0;
    return (static_cast<std::uint64_t>(grain_rate.den) * 1000000000ull) / static_cast<std::uint64_t>(grain_rate.num);
}

std::uint64_t index_to_timestamp_ns(std::uint64_t index, Rational grain_rate) {
    return index * grain_duration_ns(grain_rate);
}

std::uint64_t timestamp_ns_to_index(std::uint64_t ts, Rational grain_rate) {
    const auto d = grain_duration_ns(grain_rate);
    if (d == 0) return 0;
    return ts / d;
}

}  // namespace mtp
