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
    const int xa = std::max(0, x0);
    const int xb = std::min(width, x0 + bw);
    if (xa >= xb) return;
    // Only the 6-pixel groups under the box are unpacked and packed again. The
    // whole line for every row of every text box was most of a 2160p grain.
    const int g0 = xa / 6;
    const int groups = (xb + 5) / 6 - g0;
    const int px0 = g0 * 6;
    const std::uint32_t stride = v210_line_stride(width);
    std::vector<std::uint16_t> y(static_cast<std::size_t>(groups) * 6);
    std::vector<std::uint16_t> cb(static_cast<std::size_t>(groups) * 3);
    std::vector<std::uint16_t> cr(static_cast<std::size_t>(groups) * 3);
    // Text and boxes repeat one colour with varying alpha: convert a colour once, not per pixel.
    std::uint32_t last_rgb = 0xFFFFFFFFu;
    Yuv10 over{};
    // The same doubles as (alpha / 255.0) * opacity, without a division per pixel.
    double alpha_of[256];
    for (int v = 0; v < 256; ++v) alpha_of[v] = (v / 255.0) * opacity;
    const bool opaque_layer = opacity == 1.0;  // text layers: integer blend, same results
    for (int row = 0; row < bh; ++row) {
        const int dy = y0 + row;
        if (dy < 0 || dy >= height) continue;
        auto* words = reinterpret_cast<std::uint32_t*>(v210 + static_cast<std::size_t>(dy) * stride) + static_cast<std::size_t>(g0) * 4;
        for (int g = 0; g < groups; ++g) {
            const std::uint32_t* w = words + g * 4;
            const int x = g * 6;
            const int c = g * 3;
            cb[c] = static_cast<std::uint16_t>(w[0] & 0x3FF);
            y[x] = static_cast<std::uint16_t>((w[0] >> 10) & 0x3FF);
            cr[c] = static_cast<std::uint16_t>((w[0] >> 20) & 0x3FF);
            y[x + 1] = static_cast<std::uint16_t>(w[1] & 0x3FF);
            cb[c + 1] = static_cast<std::uint16_t>((w[1] >> 10) & 0x3FF);
            y[x + 2] = static_cast<std::uint16_t>((w[1] >> 20) & 0x3FF);
            cr[c + 1] = static_cast<std::uint16_t>(w[2] & 0x3FF);
            y[x + 3] = static_cast<std::uint16_t>((w[2] >> 10) & 0x3FF);
            cb[c + 2] = static_cast<std::uint16_t>((w[2] >> 20) & 0x3FF);
            y[x + 4] = static_cast<std::uint16_t>(w[3] & 0x3FF);
            cr[c + 2] = static_cast<std::uint16_t>((w[3] >> 10) & 0x3FF);
            y[x + 5] = static_cast<std::uint16_t>((w[3] >> 20) & 0x3FF);
        }
        bool dirty = false;
        for (int col = 0; col < bw; ++col) {
            const int dx = x0 + col;
            if (dx < 0 || dx >= width) continue;
            const std::uint8_t* p = rgba + (static_cast<std::size_t>(row) * static_cast<std::size_t>(bw) + col) * 4u;
            const double a = alpha_of[p[3]];
            if (a <= 0.001) continue;
            const std::uint32_t rgb = p[0] | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16);
            if (rgb != last_rgb) {
                over = rgb_to_yuv709(p[0] / 255.0, p[1] / 255.0, p[2] / 255.0);
                last_rgb = rgb;
            }
            const int i = dx - px0;
            const int c = i / 2;
            if (opaque_layer) {
                // a = A/255: v·(1-a) + o·a + 0.5 is N/510 with N odd, never within 1/510 of an
                // integer, so the double formula truncates to exactly this.
                const int A = p[3];
                const int ia = 255 - A;
                y[i] = static_cast<std::uint16_t>((2 * (y[i] * ia + over.y * A) + 255) / 510);
                cb[c] = static_cast<std::uint16_t>((2 * (cb[c] * ia + over.cb * A) + 255) / 510);
                cr[c] = static_cast<std::uint16_t>((2 * (cr[c] * ia + over.cr * A) + 255) / 510);
            } else {
                y[i] = static_cast<std::uint16_t>(y[i] * (1.0 - a) + over.y * a + 0.5);
                cb[c] = static_cast<std::uint16_t>(cb[c] * (1.0 - a) + over.cb * a + 0.5);
                cr[c] = static_cast<std::uint16_t>(cr[c] * (1.0 - a) + over.cr * a + 0.5);
            }
            dirty = true;
        }
        if (!dirty) continue;
        for (int g = 0; g < groups; ++g) {
            std::uint32_t* w = words + g * 4;
            const int x = g * 6;
            const int c = g * 3;
            auto v = [](std::uint16_t s) { return static_cast<std::uint32_t>(s & 0x3FFu); };
            w[0] = v(cb[c]) | (v(y[x]) << 10) | (v(cr[c]) << 20);
            w[1] = v(y[x + 1]) | (v(cb[c + 1]) << 10) | (v(y[x + 2]) << 20);
            w[2] = v(cr[c + 1]) | (v(y[x + 3]) << 10) | (v(cb[c + 2]) << 20);
            w[3] = v(y[x + 4]) | (v(cr[c + 2]) << 10) | (v(y[x + 5]) << 20);
        }
    }
}

void restore_v210_rect(std::uint8_t* dst, const std::uint8_t* src, int width, int height, int x, int y, int bw, int bh) {
    const int xa = std::max(0, x);
    const int xb = std::min(width, x + bw);
    const int ya = std::max(0, y);
    const int yb = std::min(height, y + bh);
    if (bw <= 0 || bh <= 0 || xa >= xb || ya >= yb) return;
    const std::size_t stride = v210_line_stride(width);
    const std::size_t b0 = static_cast<std::size_t>(xa / 6) * 16;
    const std::size_t b1 = static_cast<std::size_t>((xb + 5) / 6) * 16;
    for (int row = ya; row < yb; ++row) {
        const std::size_t at = static_cast<std::size_t>(row) * stride;
        std::memcpy(dst + at + b0, src + at + b0, b1 - b0);
    }
}

namespace {

// Scratch rows of composite_rgba_over_alpha, kept per thread so the writer does not allocate.
struct OverScratch {
    std::vector<std::uint16_t> y, cb, cr, ky, kcb, kcr;
    std::vector<float> sa, sy, scb, scr, da, oa;
};

void unpack_groups(const std::uint32_t* w, int groups, std::uint16_t* y, std::uint16_t* cb, std::uint16_t* cr) {
    for (int g = 0; g < groups; ++g, w += 4) {
        const int x = g * 6;
        const int c = g * 3;
        cb[c] = static_cast<std::uint16_t>(w[0] & 0x3FF);
        y[x] = static_cast<std::uint16_t>((w[0] >> 10) & 0x3FF);
        cr[c] = static_cast<std::uint16_t>((w[0] >> 20) & 0x3FF);
        y[x + 1] = static_cast<std::uint16_t>(w[1] & 0x3FF);
        cb[c + 1] = static_cast<std::uint16_t>((w[1] >> 10) & 0x3FF);
        y[x + 2] = static_cast<std::uint16_t>((w[1] >> 20) & 0x3FF);
        cr[c + 1] = static_cast<std::uint16_t>(w[2] & 0x3FF);
        y[x + 3] = static_cast<std::uint16_t>((w[2] >> 10) & 0x3FF);
        cb[c + 2] = static_cast<std::uint16_t>((w[2] >> 20) & 0x3FF);
        y[x + 4] = static_cast<std::uint16_t>(w[3] & 0x3FF);
        cr[c + 2] = static_cast<std::uint16_t>((w[3] >> 10) & 0x3FF);
        y[x + 5] = static_cast<std::uint16_t>((w[3] >> 20) & 0x3FF);
    }
}

void pack_groups(std::uint32_t* w, int groups, const std::uint16_t* y, const std::uint16_t* cb, const std::uint16_t* cr) {
    auto v = [](std::uint16_t s) { return static_cast<std::uint32_t>(s & 0x3FFu); };
    for (int g = 0; g < groups; ++g, w += 4) {
        const int x = g * 6;
        const int c = g * 3;
        w[0] = v(cb[c]) | (v(y[x]) << 10) | (v(cr[c]) << 20);
        w[1] = v(y[x + 1]) | (v(cb[c + 1]) << 10) | (v(y[x + 2]) << 20);
        w[2] = v(cr[c + 1]) | (v(y[x + 3]) << 10) | (v(cb[c + 2]) << 20);
        w[3] = v(y[x + 4]) | (v(cr[c + 2]) << 10) | (v(y[x + 5]) << 20);
    }
}

std::uint16_t code10(double v) { return static_cast<std::uint16_t>(std::clamp(static_cast<int>(v + 0.5), 4, 1019)); }

}  // namespace

void composite_rgba_over_alpha(std::uint8_t* v210, const AlphaTarget& alpha, int width, int height, int x0, int y0, int bw, int bh,
                               const std::uint8_t* rgba, double opacity, bool premultiplied) {
    if (alpha.kind == AlphaTarget::Kind::None || !alpha.data) {
        composite_rgba_onto_v210(v210, width, height, x0, y0, bw, bh, rgba, opacity);
        return;
    }
    if (bw <= 0 || bh <= 0 || opacity <= 0.0) return;
    const int xa = std::max(0, x0);
    const int xb = std::min(width, x0 + bw);
    if (xa >= xb) return;
    const int g0 = xa / 6;
    const int groups = (xb + 5) / 6 - g0;
    const int px0 = g0 * 6;
    const int span = groups * 6;
    const std::uint32_t stride = v210_line_stride(width);
    const std::uint32_t astride = alpha10_line_stride(width);
    const bool key = alpha.kind == AlphaTarget::Kind::Key;
    const double krange = std::max(1, alpha.key_max - alpha.key_min);
    thread_local OverScratch s;
    const auto n = static_cast<std::size_t>(span);
    if (s.y.size() < n) {
        for (auto* v : {&s.y, &s.ky}) v->resize(n);
        for (auto* v : {&s.cb, &s.cr, &s.kcb, &s.kcr}) v->resize(n / 2);
        for (auto* v : {&s.sa, &s.sy, &s.scb, &s.scr, &s.da, &s.oa}) v->resize(n);
    }
    float alpha_of[256];
    for (int v = 0; v < 256; ++v) alpha_of[v] = static_cast<float>((v / 255.0) * opacity);
    std::uint32_t last_rgb = 0xFFFFFFFFu;
    Yuv10 over{};
    for (int row = 0; row < bh; ++row) {
        const int dy = y0 + row;
        if (dy < 0 || dy >= height) continue;
        // The layer's alpha and colour for every pixel of the groups (0 outside the box).
        bool any = false;
        for (int i = 0; i < span; ++i) {
            const int dx = px0 + i;
            const int col = dx - x0;
            s.sa[i] = 0;
            if (dx < xa || dx >= xb || col < 0 || col >= bw) continue;
            const std::uint8_t* p = rgba + (static_cast<std::size_t>(row) * static_cast<std::size_t>(bw) + col) * 4u;
            const float a = alpha_of[p[3]];
            if (a <= 0.001f) continue;
            const std::uint32_t rgb = p[0] | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16);
            if (rgb != last_rgb) {
                over = rgb_to_yuv709(p[0] / 255.0, p[1] / 255.0, p[2] / 255.0);
                last_rgb = rgb;
            }
            s.sa[i] = a;
            s.sy[i] = static_cast<float>(over.y);
            s.scb[i] = static_cast<float>(over.cb);
            s.scr[i] = static_cast<float>(over.cr);
            any = true;
        }
        if (!any) continue;
        auto* words = reinterpret_cast<std::uint32_t*>(v210 + static_cast<std::size_t>(dy) * stride) + static_cast<std::size_t>(g0) * 4;
        unpack_groups(words, groups, s.y.data(), s.cb.data(), s.cr.data());
        std::uint32_t* kwords = nullptr;
        std::uint32_t* awords = nullptr;
        if (key) {
            kwords = reinterpret_cast<std::uint32_t*>(alpha.data + static_cast<std::size_t>(dy) * stride) + static_cast<std::size_t>(g0) * 4;
            unpack_groups(kwords, groups, s.ky.data(), s.kcb.data(), s.kcr.data());
            for (int i = 0; i < span; ++i) s.da[i] = static_cast<float>(std::clamp((s.ky[i] - alpha.key_min) / krange, 0.0, 1.0));
        } else {
            awords = reinterpret_cast<std::uint32_t*>(alpha.data + static_cast<std::size_t>(dy) * astride);
            for (int i = 0; i < span; ++i) {
                const int dx = px0 + i;
                s.da[i] = dx < width ? static_cast<float>(((awords[dx / 3] >> ((dx % 3) * 10)) & 0x3FFu) / 1023.0) : 0.f;
            }
        }
        // Luma and alpha per pixel.
        for (int i = 0; i < span; ++i) {
            const float sa = s.sa[i];
            const float da = s.da[i];
            s.oa[i] = da;
            if (sa <= 0) continue;
            const float oa = sa + da * (1.f - sa);
            s.oa[i] = oa;
            const double yd = s.y[i];
            s.y[i] = premultiplied ? code10(s.sy[i] * sa + yd * (1.0 - sa)) : code10((s.sy[i] * sa + yd * da * (1.0 - sa)) / oa);
        }
        // Chroma once per pair, with the pair's mean alphas (taken before this layer).
        for (int c = 0; c < span / 2; ++c) {
            const float s0 = s.sa[2 * c];
            const float s1 = s.sa[2 * c + 1];
            const float ssum = s0 + s1;
            if (ssum <= 0) continue;
            const double sa = ssum / 2.0;
            const double da = (s.da[2 * c] + s.da[2 * c + 1]) / 2.0;
            const double cbs = (s.scb[2 * c] * s0 + s.scb[2 * c + 1] * s1) / ssum;
            const double crs = (s.scr[2 * c] * s0 + s.scr[2 * c + 1] * s1) / ssum;
            if (premultiplied) {
                s.cb[c] = code10(cbs * sa + s.cb[c] * (1.0 - sa));
                s.cr[c] = code10(crs * sa + s.cr[c] * (1.0 - sa));
            } else {
                const double oa = sa + da * (1.0 - sa);
                s.cb[c] = code10((cbs * sa + s.cb[c] * da * (1.0 - sa)) / oa);
                s.cr[c] = code10((crs * sa + s.cr[c] * da * (1.0 - sa)) / oa);
            }
        }
        pack_groups(words, groups, s.y.data(), s.cb.data(), s.cr.data());
        if (key) {
            for (int i = 0; i < span; ++i)
                if (s.sa[i] > 0) s.ky[i] = static_cast<std::uint16_t>(alpha_to_key(s.oa[i], alpha.key_min, alpha.key_max));
            pack_groups(kwords, groups, s.ky.data(), s.kcb.data(), s.kcr.data());
        } else {
            for (int i = 0; i < span; ++i) {
                const int dx = px0 + i;
                if (s.sa[i] <= 0 || dx >= width) continue;
                const auto a10 = static_cast<std::uint32_t>(std::clamp(static_cast<int>(s.oa[i] * 1023.0 + 0.5), 0, 1023));
                const int shift = (dx % 3) * 10;
                awords[dx / 3] = (awords[dx / 3] & ~(0x3FFu << shift)) | (a10 << shift);
            }
        }
    }
}

void restore_a10_rect(std::uint8_t* dst, const std::uint8_t* src, int width, int height, int x, int y, int bw, int bh) {
    const int xa = std::max(0, x);
    const int xb = std::min(width, x + bw);
    const int ya = std::max(0, y);
    const int yb = std::min(height, y + bh);
    if (bw <= 0 || bh <= 0 || xa >= xb || ya >= yb) return;
    const std::size_t stride = alpha10_line_stride(width);
    // The pixels composite_rgba_over_alpha can change: its 6-pixel groups, clipped to the line.
    const int pa = (xa / 6) * 6;
    const int pb = std::min(width, ((xb + 5) / 6) * 6);
    const std::size_t b0 = static_cast<std::size_t>(pa / 3) * 4;
    const std::size_t b1 = std::min(stride, static_cast<std::size_t>((pb + 2) / 3) * 4);
    for (int row = ya; row < yb; ++row) {
        const std::size_t at = static_cast<std::size_t>(row) * stride;
        std::memcpy(dst + at + b0, src + at + b0, b1 - b0);
    }
}

void fill_alpha10(std::uint8_t* dst, int width, int height, int a) {
    std::vector<std::uint16_t> line(static_cast<std::size_t>(width), static_cast<std::uint16_t>(std::clamp(a, 0, 1023)));
    const std::uint32_t stride = alpha10_line_stride(width);
    for (int row = 0; row < height; ++row) pack_alpha10_line(line.data(), width, dst + static_cast<std::size_t>(row) * stride);
}

}  // namespace mtp
