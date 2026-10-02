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

}  // namespace mtp
