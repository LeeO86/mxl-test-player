#include "v210.hpp"

#include "util.hpp"

#include <algorithm>
#include <cstring>

namespace mtp {

Yuv10 rgb_to_yuv709(double r, double g, double b) {
    const double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    const double cb = -0.114572 * r - 0.385428 * g + 0.5 * b;
    const double cr = 0.5 * r - 0.454153 * g - 0.045847 * b;
    return Yuv10{y_from_unit(y), c_from_offset(cb), c_from_offset(cr)};
}

void pack_v210_line(const std::uint16_t* y, const std::uint16_t* cb, const std::uint16_t* cr, int width, std::uint8_t* dst) {
    const std::uint32_t stride = v210_line_stride(width);
    std::memset(dst, 0, stride);
    auto* w = reinterpret_cast<std::uint32_t*>(dst);
    auto Y = [&](int i) -> std::uint32_t { return i < width ? (y[i] & 0x3FFu) : 0u; };
    auto Cb = [&](int i) -> std::uint32_t {
        const int c = i / 2;
        return (i < width) ? (cb[c] & 0x3FFu) : static_cast<std::uint32_t>(kCMid);
    };
    auto Cr = [&](int i) -> std::uint32_t {
        const int c = i / 2;
        return (i < width) ? (cr[c] & 0x3FFu) : static_cast<std::uint32_t>(kCMid);
    };
    int o = 0;
    for (int x = 0; x < width; x += 6) {
        w[o++] = Cb(x) | (Y(x) << 10) | (Cr(x) << 20);
        w[o++] = Y(x + 1) | (Cb(x + 2) << 10) | (Y(x + 2) << 20);
        w[o++] = Cr(x + 2) | (Y(x + 3) << 10) | (Cb(x + 4) << 20);
        w[o++] = Y(x + 4) | (Cr(x + 4) << 10) | (Y(x + 5) << 20);
    }
}

void unpack_v210_line(const std::uint8_t* src, int width, std::uint16_t* y, std::uint16_t* cb, std::uint16_t* cr) {
    const auto* w = reinterpret_cast<const std::uint32_t*>(src);
    int o = 0;
    const int chroma_w = width / 2;
    for (int x = 0; x < width; x += 6) {
        const std::uint32_t w0 = w[o++];
        const std::uint32_t w1 = w[o++];
        const std::uint32_t w2 = w[o++];
        const std::uint32_t w3 = w[o++];
        auto putY = [&](int i, std::uint32_t v) {
            if (i < width) y[i] = static_cast<std::uint16_t>(v & 0x3FF);
        };
        auto putC = [&](int i, std::uint32_t cbv, std::uint32_t crv) {
            const int c = i / 2;
            if (c < chroma_w) {
                cb[c] = static_cast<std::uint16_t>(cbv & 0x3FF);
                cr[c] = static_cast<std::uint16_t>(crv & 0x3FF);
            }
        };
        putC(x, w0, w0 >> 20);
        putY(x, w0 >> 10);
        putY(x + 1, w1);
        putY(x + 2, w1 >> 20);
        putC(x + 2, w1 >> 10, w2);
        putY(x + 3, w2 >> 10);
        putY(x + 4, w3);
        putY(x + 5, w3 >> 20);
        putC(x + 4, w2 >> 20, w3 >> 10);
    }
}

void fill_v210(std::uint8_t* dst, int width, int height, int yv, int cb, int cr) {
    std::vector<std::uint16_t> y(static_cast<std::size_t>(width), static_cast<std::uint16_t>(yv));
    std::vector<std::uint16_t> cbb(static_cast<std::size_t>(width / 2), static_cast<std::uint16_t>(cb));
    std::vector<std::uint16_t> crr(static_cast<std::size_t>(width / 2), static_cast<std::uint16_t>(cr));
    const std::uint32_t stride = v210_line_stride(width);
    for (int row = 0; row < height; ++row) {
        pack_v210_line(y.data(), cbb.data(), crr.data(), width, dst + static_cast<std::size_t>(row) * stride);
    }
}

void fill_v210_rgb(std::uint8_t* dst, int width, int height, double r, double g, double b) {
    const Yuv10 yuv = rgb_to_yuv709(r, g, b);
    fill_v210(dst, width, height, yuv.y, yuv.cb, yuv.cr);
}

void pack_alpha10_line(const std::uint16_t* a, int width, std::uint8_t* dst) {
    const std::uint32_t stride = alpha10_line_stride(width);
    std::memset(dst, 0, stride);
    auto* w = reinterpret_cast<std::uint32_t*>(dst);
    int o = 0;
    for (int x = 0; x < width; x += 3) {
        const std::uint32_t a0 = a[x] & 0x3FFu;
        const std::uint32_t a1 = (x + 1 < width) ? (a[x + 1] & 0x3FFu) : 0u;
        const std::uint32_t a2 = (x + 2 < width) ? (a[x + 2] & 0x3FFu) : 0u;
        w[o++] = a0 | (a1 << 10) | (a2 << 20);
    }
}

void unpack_alpha10_line(const std::uint8_t* src, int width, std::uint16_t* a) {
    const auto* w = reinterpret_cast<const std::uint32_t*>(src);
    int o = 0;
    for (int x = 0; x < width; x += 3) {
        const std::uint32_t v = w[o++];
        a[x] = static_cast<std::uint16_t>(v & 0x3FF);
        if (x + 1 < width) a[x + 1] = static_cast<std::uint16_t>((v >> 10) & 0x3FF);
        if (x + 2 < width) a[x + 2] = static_cast<std::uint16_t>((v >> 20) & 0x3FF);
    }
}

int alpha_to_key(double a, int key_min, int key_max) {
    a = clampd(a, 0.0, 1.0);
    return key_min + static_cast<int>(a * (key_max - key_min) + 0.5);
}

void composite_rgba_onto_v210(std::uint8_t* v210, int width, int height, int x0, int y0, int bw, int bh,
                              const std::uint8_t* rgba, double opacity) {
    if (bw <= 0 || bh <= 0 || opacity <= 0.0) return;
    const std::uint32_t stride = v210_line_stride(width);
    std::vector<std::uint16_t> y(static_cast<std::size_t>(width));
    std::vector<std::uint16_t> cb(static_cast<std::size_t>(width / 2));
    std::vector<std::uint16_t> cr(static_cast<std::size_t>(width / 2));
    for (int row = 0; row < bh; ++row) {
        const int dy = y0 + row;
        if (dy < 0 || dy >= height) continue;
        std::uint8_t* line = v210 + static_cast<std::size_t>(dy) * stride;
        unpack_v210_line(line, width, y.data(), cb.data(), cr.data());
        bool dirty = false;
        for (int col = 0; col < bw; ++col) {
            const int dx = x0 + col;
            if (dx < 0 || dx >= width) continue;
            const std::uint8_t* p = rgba + (static_cast<std::size_t>(row) * static_cast<std::size_t>(bw) + col) * 4u;
            const double a = (p[3] / 255.0) * opacity;
            if (a <= 0.001) continue;
            const Yuv10 over = rgb_to_yuv709(p[0] / 255.0, p[1] / 255.0, p[2] / 255.0);
            const int c = dx / 2;
            y[dx] = static_cast<std::uint16_t>(y[dx] * (1.0 - a) + over.y * a + 0.5);
            cb[c] = static_cast<std::uint16_t>(cb[c] * (1.0 - a) + over.cb * a + 0.5);
            cr[c] = static_cast<std::uint16_t>(cr[c] * (1.0 - a) + over.cr * a + 0.5);
            dirty = true;
        }
        if (dirty) pack_v210_line(y.data(), cb.data(), cr.data(), width, line);
    }
}

}  // namespace mtp
