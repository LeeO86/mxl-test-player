#include "text.hpp"

#include "util.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <tuple>

namespace mtp {
namespace {

int utf8_next(std::string_view s, std::size_t& i) {
    if (i >= s.size()) return -1;
    const auto c = static_cast<unsigned char>(s[i++]);
    if (c < 0x80) return c;
    int need = 0;
    int cp = 0;
    if ((c & 0xE0) == 0xC0) {
        need = 1;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        need = 2;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        need = 3;
        cp = c & 0x07;
    } else {
        return 0xFFFD;
    }
    for (int n = 0; n < need; ++n) {
        if (i >= s.size()) return 0xFFFD;
        const auto cc = static_cast<unsigned char>(s[i++]);
        if ((cc & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (cc & 0x3F);
    }
    return cp;
}

void blend_px(std::uint8_t* d, std::uint8_t sr, std::uint8_t sg, std::uint8_t sb, std::uint8_t sa) {
    if (sa == 0) return;
    const float a = sa / 255.f;
    const float ia = 1.f - a * (d[3] / 255.f);
    // Source-over, both straight.
    const float da = d[3] / 255.f;
    const float out_a = a + da * (1.f - a);
    if (out_a <= 0.f) return;
    d[0] = static_cast<std::uint8_t>(std::clamp((sr * a + d[0] * da * (1.f - a)) / out_a, 0.f, 255.f));
    d[1] = static_cast<std::uint8_t>(std::clamp((sg * a + d[1] * da * (1.f - a)) / out_a, 0.f, 255.f));
    d[2] = static_cast<std::uint8_t>(std::clamp((sb * a + d[2] * da * (1.f - a)) / out_a, 0.f, 255.f));
    d[3] = static_cast<std::uint8_t>(std::clamp(out_a * 255.f, 0.f, 255.f));
    (void)ia;
}

}  // namespace

bool parse_hex_color(std::string_view s, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) {
    if (!s.empty() && s[0] == '#') s.remove_prefix(1);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (s.size() == 6) {
        const int r0 = nib(s[0]), r1 = nib(s[1]), g0 = nib(s[2]), g1 = nib(s[3]), b0 = nib(s[4]), b1 = nib(s[5]);
        if (r0 < 0 || r1 < 0 || g0 < 0 || g1 < 0 || b0 < 0 || b1 < 0) return false;
        r = static_cast<std::uint8_t>((r0 << 4) | r1);
        g = static_cast<std::uint8_t>((g0 << 4) | g1);
        b = static_cast<std::uint8_t>((b0 << 4) | b1);
        return true;
    }
    return false;
}

struct TextRenderer::Impl {
    FT_Library lib = nullptr;
    std::string font_dir;
    std::mutex mu;
    struct FaceKey {
        std::string font;
        int size;
        bool operator<(const FaceKey& o) const { return font < o.font || (font == o.font && size < o.size); }
    };
    std::map<FaceKey, FT_Face> faces;

    std::string path_for(const std::string& font) const {
        if (font.find("Mono") != std::string::npos) return font_dir + "/DejaVuSansMono.ttf";
        if (font.find("Bold") != std::string::npos) return font_dir + "/DejaVuSans-Bold.ttf";
        return font_dir + "/DejaVuSans.ttf";
    }

    FT_Face face_for(const std::string& font, int size) {
        FaceKey k{font, size};
        auto it = faces.find(k);
        if (it != faces.end()) return it->second;
        FT_Face face = nullptr;
        const auto path = path_for(font);
        if (FT_New_Face(lib, path.c_str(), 0, &face) != 0) {
            throw Error("failed to load font " + path);
        }
        FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(std::max(8, size)));
        faces.emplace(k, face);
        return face;
    }

    struct Glyph {
        int w = 0, h = 0, left = 0, top = 0, advance = 0;
        std::vector<std::uint8_t> cov;
    };
    std::map<std::tuple<std::string, int, int>, Glyph> glyphs;

    const Glyph& glyph(const std::string& font, int size, int cp) {
        auto key = std::make_tuple(font, size, cp);
        auto it = glyphs.find(key);
        if (it != glyphs.end()) return it->second;
        FT_Face face = face_for(font, size);
        if (FT_Load_Char(face, static_cast<FT_ULong>(cp), FT_LOAD_RENDER) != 0) {
            FT_Load_Char(face, '?', FT_LOAD_RENDER);
        }
        FT_GlyphSlot g = face->glyph;
        Glyph gl;
        gl.w = static_cast<int>(g->bitmap.width);
        gl.h = static_cast<int>(g->bitmap.rows);
        gl.left = g->bitmap_left;
        gl.top = g->bitmap_top;
        gl.advance = static_cast<int>(g->advance.x >> 6);
        gl.cov.assign(g->bitmap.buffer, g->bitmap.buffer + static_cast<std::size_t>(gl.w) * gl.h);
        auto [ins, _] = glyphs.emplace(key, std::move(gl));
        return ins->second;
    }
};

TextRenderer::TextRenderer(std::string font_dir) : impl_(new Impl) {
    impl_->font_dir = std::move(font_dir);
    if (FT_Init_FreeType(&impl_->lib) != 0) throw Error("FreeType init failed");
}

TextRenderer::~TextRenderer() {
    if (!impl_) return;
    for (auto& f : impl_->faces) FT_Done_Face(f.second);
    if (impl_->lib) FT_Done_FreeType(impl_->lib);
    delete impl_;
}

RgbaImage TextRenderer::render(const std::string& utf8, const TextStyle& style) {
    std::lock_guard lock(impl_->mu);
    struct Run {
        int cp;
        const Impl::Glyph* g;
    };
    std::vector<std::vector<Run>> lines;
    lines.emplace_back();
    std::size_t i = 0;
    int max_w = 0;
    const int line_h = std::max(style.pixel_size + style.pixel_size / 4, 1);
    while (i < utf8.size()) {
        if (utf8[i] == '\n') {
            ++i;
            lines.emplace_back();
            continue;
        }
        const int cp = utf8_next(utf8, i);
        if (cp < 0) break;
        const auto& g = impl_->glyph(style.font, style.pixel_size, cp);
        lines.back().push_back(Run{cp, &g});
    }
    if (lines.size() == 1 && lines[0].empty()) {
        lines[0].push_back(Run{' ', &impl_->glyph(style.font, style.pixel_size, ' ')});
    }
    std::vector<int> line_w;
    for (const auto& line : lines) {
        int w = 0;
        for (const auto& r : line) w += r.g->advance;
        line_w.push_back(w);
        max_w = std::max(max_w, w);
    }
    const int pad = style.box ? style.pad : (style.outline || style.shadow ? 2 : 0);
    const int tw = std::max(1, max_w) + pad * 2 + 4;
    const int th = std::max(1, static_cast<int>(lines.size()) * line_h) + pad * 2;
    RgbaImage img;
    img.w = tw;
    img.h = th;
    img.px.assign(static_cast<std::size_t>(tw) * th * 4, 0);
    if (style.box) {
        for (int y = 0; y < th; ++y) {
            for (int x = 0; x < tw; ++x) {
                auto* d = img.px.data() + (static_cast<std::size_t>(y) * tw + x) * 4;
                d[0] = style.br;
                d[1] = style.bg;
                d[2] = style.bb;
                d[3] = style.ba;
            }
        }
    }
    auto splat = [&](int ox, int oy, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
        int y0 = pad;
        for (std::size_t li = 0; li < lines.size(); ++li) {
            int xpen = pad + ox;
            const int baseline = y0 + style.pixel_size + oy;
            for (const auto& run : lines[li]) {
                const auto* glyph = run.g;
                for (int gy = 0; gy < glyph->h; ++gy) {
                    const int dy = baseline - glyph->top + gy;
                    if (dy < 0 || dy >= th) continue;
                    for (int gx = 0; gx < glyph->w; ++gx) {
                        const int dx = xpen + glyph->left + gx;
                        if (dx < 0 || dx >= tw) continue;
                        const std::uint8_t cov = glyph->cov[static_cast<std::size_t>(gy) * glyph->w + gx];
                        if (!cov) continue;
                        const std::uint8_t sa = static_cast<std::uint8_t>((static_cast<int>(cov) * a) / 255);
                        auto* d = img.px.data() + (static_cast<std::size_t>(dy) * tw + dx) * 4;
                        blend_px(d, r, g, b, sa);
                    }
                }
                xpen += glyph->advance;
            }
            y0 += line_h;
        }
    };
    if (style.shadow) splat(2, 2, 0, 0, 0, style.a);
    if (style.outline) {
        for (int oy = -1; oy <= 1; ++oy)
            for (int ox = -1; ox <= 1; ++ox)
                if (ox || oy) splat(ox, oy, style.or_, style.og, style.ob, style.a);
    }
    splat(0, 0, style.r, style.g, style.b, style.a);
    return img;
}

}  // namespace mtp
