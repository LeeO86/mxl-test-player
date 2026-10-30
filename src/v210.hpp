#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mtp {

inline std::uint32_t v210_line_stride(int width) {
    return static_cast<std::uint32_t>(((width + 47) / 48) * 128);
}

inline std::uint32_t alpha10_line_stride(int width) {
    return static_cast<std::uint32_t>(((width + 2) / 3) * 4);
}

inline std::size_t v210_size(int width, int height) {
    return static_cast<std::size_t>(v210_line_stride(width)) * static_cast<std::size_t>(height);
}

inline std::size_t alpha10_size(int width, int height) {
    return static_cast<std::size_t>(alpha10_line_stride(width)) * static_cast<std::size_t>(height);
}

// Legal-range helpers (BT.709 10-bit).
constexpr int kYBlack = 64;
constexpr int kYWhite = 940;
constexpr int kCBlack = 64;
constexpr int kCWhite = 960;
constexpr int kCMid = 512;

inline int y_from_unit(double y) {
    int v = static_cast<int>(y * 876.0 + 64.0 + 0.5);
    if (v < 4) v = 4;
    if (v > 1019) v = 1019;
    return v;
}
inline int c_from_offset(double c) {
    // c is -0.5..0.5
    int v = static_cast<int>(c * 896.0 + 512.0 + 0.5);
    if (v < 4) v = 4;
    if (v > 1019) v = 1019;
    return v;
}

struct Yuv10 {
    int y, cb, cr;
};

Yuv10 rgb_to_yuv709(double r, double g, double b);

void pack_v210_line(const std::uint16_t* y, const std::uint16_t* cb, const std::uint16_t* cr, int width, std::uint8_t* dst);
void unpack_v210_line(const std::uint8_t* src, int width, std::uint16_t* y, std::uint16_t* cb, std::uint16_t* cr);

void fill_v210(std::uint8_t* dst, int width, int height, int y, int cb, int cr);
void fill_v210_rgb(std::uint8_t* dst, int width, int height, double r, double g, double b);

void pack_alpha10_line(const std::uint16_t* a, int width, std::uint8_t* dst);
void unpack_alpha10_line(const std::uint8_t* src, int width, std::uint16_t* a);

// Map 0..1 alpha onto the configured luma key range.
int alpha_to_key(double a, int key_min = kYBlack, int key_max = kYWhite);

void composite_rgba_onto_v210(std::uint8_t* v210, int width, int height, int x, int y, int bw, int bh,
                              const std::uint8_t* rgba, double opacity);

// Copies back from `src` every 6-pixel group that composite_rgba_onto_v210 with the same box
// can change (rows and columns clipped to the frame).
void restore_v210_rect(std::uint8_t* dst, const std::uint8_t* src, int width, int height, int x, int y, int bw, int bh);

// The alpha that goes with a fill: the v210a alpha plane (10 bits, 0 transparent .. 1023 opaque)
// or the fill_key key frame (v210, alpha as luma from key_min to key_max, chroma at 512).
struct AlphaTarget {
    enum class Kind { None, A10, Key };
    Kind kind = Kind::None;
    std::uint8_t* data = nullptr;
    int key_min = kYBlack;
    int key_max = kYWhite;
};

// Composites `rgba` (straight alpha, times `opacity`) over the fill and its alpha, "over" as a
// keyer expects it: a_out = a_src + a_dst * (1 - a_src). A straight fill becomes
// (c_src * a_src + c_dst * a_dst * (1 - a_src)) / a_out, so over a transparent background it is
// the layer's own colour; a premultiplied fill becomes c_src * a_src + c_dst * (1 - a_src).
// Chroma is blended once per 4:2:2 pair, with the pair's mean alphas. Over an opaque background
// this is the same blend as composite_rgba_onto_v210, but for chroma (once per pair, not twice).
void composite_rgba_over_alpha(std::uint8_t* v210, const AlphaTarget& alpha, int width, int height, int x, int y, int bw, int bh,
                               const std::uint8_t* rgba, double opacity, bool premultiplied);

// Copies back from `src` every alpha word (3 pixels) that composite_rgba_over_alpha with the same
// box can change in a v210a alpha plane. For a fill_key key frame, restore_v210_rect applies.
void restore_a10_rect(std::uint8_t* dst, const std::uint8_t* src, int width, int height, int x, int y, int bw, int bh);

// A whole alpha plane at one value (0..1023).
void fill_alpha10(std::uint8_t* dst, int width, int height, int a);

}  // namespace mtp
