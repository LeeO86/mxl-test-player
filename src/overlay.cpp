#include "overlay.hpp"

#include "v210.hpp"

#include <algorithm>

namespace mtp {
namespace {

struct Placed {
    int x = 0;
    int y = 0;
    RgbaImage img;
    double opacity = 1;
    bool on_key = false;
};

int anchor_x(const std::string& a, int frame_w, int box_w) {
    if (a.size() < 2) return 0;
    const char h = a[1];
    if (h == 'c') return (frame_w - box_w) / 2;
    if (h == 'r') return frame_w - box_w;
    return 0;
}

int anchor_y(const std::string& a, int frame_h, int box_h) {
    if (a.empty()) return 0;
    const char v = a[0];
    if (v == 'm') return (frame_h - box_h) / 2;
    if (v == 'b') return frame_h - box_h;
    return 0;
}

TextStyle style_from(const TextLayerConfig& t, int frame_h) {
    TextStyle s;
    s.font = t.font;
    s.pixel_size = std::max(8, static_cast<int>(t.size / 100.0 * frame_h));
    parse_hex_color(t.color, s.r, s.g, s.b);
    s.a = static_cast<std::uint8_t>(clampd(t.opacity, 0, 1) * 255);
    s.box = t.box;
    parse_hex_color(t.box_color, s.br, s.bg, s.bb);
    s.ba = static_cast<std::uint8_t>(clampd(t.box_opacity, 0, 1) * 255);
    s.pad = std::max(0, static_cast<int>(t.box_padding / 100.0 * frame_h));
    s.outline = t.outline;
    parse_hex_color(t.outline_color, s.or_, s.og, s.ob);
    s.shadow = t.shadow;
    return s;
}

std::string style_key(const TextStyle& s, const std::string& text) {
    return s.font + "|" + std::to_string(s.pixel_size) + "|" + std::to_string(s.r) + "," + std::to_string(s.g) + "," +
           std::to_string(s.b) + "," + std::to_string(s.a) + "|" + std::to_string(s.box) + std::to_string(s.ba) + "|" +
           std::to_string(s.outline) + std::to_string(s.shadow) + "|" + text;
}

RgbaImage solid_box(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    RgbaImage img;
    img.w = w;
    img.h = h;
    img.px.resize(static_cast<std::size_t>(w) * h * 4);
    for (int i = 0; i < w * h; ++i) {
        img.px[i * 4] = r;
        img.px[i * 4 + 1] = g;
        img.px[i * 4 + 2] = b;
        img.px[i * 4 + 3] = a;
    }
    return img;
}

void blit_rgba(RgbaImage& dst, int x0, int y0, const RgbaImage& src) {
    for (int y = 0; y < src.h; ++y) {
        const int dy = y0 + y;
        if (dy < 0 || dy >= dst.h) continue;
        for (int x = 0; x < src.w; ++x) {
            const int dx = x0 + x;
            if (dx < 0 || dx >= dst.w) continue;
            const auto* s = src.px.data() + (static_cast<std::size_t>(y) * src.w + x) * 4;
            auto* d = dst.px.data() + (static_cast<std::size_t>(dy) * dst.w + dx) * 4;
            const float a = s[3] / 255.f;
            if (a <= 0) continue;
            const float da = d[3] / 255.f;
            const float oa = a + da * (1.f - a);
            if (oa <= 0) continue;
            d[0] = static_cast<std::uint8_t>((s[0] * a + d[0] * da * (1.f - a)) / oa);
            d[1] = static_cast<std::uint8_t>((s[1] * a + d[1] * da * (1.f - a)) / oa);
            d[2] = static_cast<std::uint8_t>((s[2] * a + d[2] * da * (1.f - a)) / oa);
            d[3] = static_cast<std::uint8_t>(oa * 255.f);
        }
    }
}

void scale_sprite(const SpriteFrame& s, int dw, int dh, RgbaImage& out) {
    out.w = dw;
    out.h = dh;
    out.px.assign(static_cast<std::size_t>(dw) * dh * 4, 0);
    if (!s.rgba || s.w <= 0 || s.h <= 0 || dw <= 0 || dh <= 0) return;
    for (int y = 0; y < dh; ++y) {
        const int sy = std::min(s.h - 1, y * s.h / dh);
        for (int x = 0; x < dw; ++x) {
            const int sx = std::min(s.w - 1, x * s.w / dw);
            const auto* p = s.rgba + (static_cast<std::size_t>(sy) * s.w + sx) * 4;
            auto* d = out.px.data() + (static_cast<std::size_t>(y) * dw + x) * 4;
            d[0] = p[0];
            d[1] = p[1];
            d[2] = p[2];
            d[3] = p[3];
        }
    }
}

}  // namespace

OverlayRenderer::OverlayRenderer(std::string font_dir) : text_(std::move(font_dir)) {}

const RgbaImage& OverlayRenderer::cached(const std::string& key, const std::string& utf8, const TextStyle& style) {
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    if (cache_.size() > 64) cache_.clear();
    auto img = text_.render(utf8, style);
    auto [ins, _] = cache_.emplace(key, std::move(img));
    return ins->second;
}

void OverlayRenderer::apply(std::uint8_t* v210, std::uint8_t* key_v210, const OverlayFrame& frame) {
    const bool keyed = frame.keyed_still;
    if (keyed && !frame.burnin.on_keyed_stills) {
        // Standard burn-ins and text stay off. Moving boxes are drawn on fill
        // unless include_in_key is set, and only when the box itself is enabled.
    }
    const int fw = frame.format.width;
    const int fh = frame.format.height;
    const bool draw_text = !keyed || frame.burnin.on_keyed_stills;

    std::vector<Placed> layers;
    auto add_text = [&](const std::string& text, const TextLayerConfig& cfg) {
        if (!draw_text || text.empty()) return;
        const auto style = style_from(cfg, fh);
        const auto expanded = expand_placeholders(text, frame.vars);
        const auto key = style_key(style, expanded);
        const auto& img = cached(key, expanded, style);
        Placed p;
        p.img = img;
        const int ox = static_cast<int>(cfg.offset_x / 100.0 * fw);
        const int oy = static_cast<int>(cfg.offset_y / 100.0 * fh);
        p.x = anchor_x(cfg.anchor, fw, img.w) + ox;
        p.y = anchor_y(cfg.anchor, fh, img.h) + oy;
        p.opacity = 1;
        layers.push_back(std::move(p));
    };

    if (draw_text && frame.burnin.label) {
        TextLayerConfig t;
        t.text = "{label}";
        t.anchor = frame.burnin.label_anchor;
        t.size = 6;
        t.box = true;
        t.offset_x = 1.5;
        t.offset_y = 1.5;
        add_text(t.text, t);
    }
    if (draw_text && frame.burnin.timecode) {
        TextLayerConfig t;
        t.text = "{timecode}";
        t.anchor = "tc";
        t.font = "DejaVu Sans Mono";
        t.size = 7;
        t.box = true;
        t.offset_y = 2;
        add_text(t.text, t);
    }
    if (draw_text && (frame.burnin.utc_clock || frame.burnin.local_clock)) {
        TextLayerConfig t;
        t.text = frame.burnin.utc_clock ? "UTC {utc}" : "LOC {local}";
        if (frame.burnin.utc_clock && frame.burnin.local_clock) t.text = "UTC {utc}  {local}";
        t.anchor = "tr";
        t.font = "DejaVu Sans Mono";
        t.size = 3.2;
        t.box = true;
        t.offset_x = -1.5;
        t.offset_y = 1.5;
        add_text(t.text, t);
    }
    if (draw_text && frame.burnin.frame_counter) {
        TextLayerConfig t;
        t.text = frame.burnin.frame_counter_absolute ? "F {frame}" : "F {frame}";
        t.anchor = "br";
        t.font = "DejaVu Sans Mono";
        t.size = 3.2;
        t.box = true;
        t.offset_x = -1.5;
        t.offset_y = -1.5;
        add_text(t.text, t);
    }
    if (draw_text && frame.burnin.item) {
        TextLayerConfig t;
        t.text = "{item}  L{loop}";
        t.anchor = "bl";
        t.size = 3.2;
        t.box = true;
        t.offset_x = 1.5;
        t.offset_y = -1.5;
        add_text(t.text, t);
    }
    if (draw_text && frame.burnin.flow_id) {
        TextLayerConfig t;
        t.text = "{flow}";
        t.anchor = "ml";
        t.font = "DejaVu Sans Mono";
        t.size = 2.6;
        t.box = true;
        t.offset_x = 1.5;
        add_text(t.text, t);
    }
    if (draw_text) {
        for (const auto& t : frame.burnin.texts) add_text(t.text, t);
    }

    const int nboxes = std::min(2, static_cast<int>(frame.burnin.boxes.size()));
    for (int i = 0; i < nboxes; ++i) {
        const auto& box = frame.burnin.boxes[static_cast<std::size_t>(i)];
        if (!box.enabled) continue;
        MotionParams mp = box.motion;
        if (frame.sprites[i].rgba) {
            mp.content_w = frame.sprites[i].w;
            mp.content_h = frame.sprites[i].h;
        } else {
            mp.content_w = 320;
            mp.content_h = 180;
        }
        const MotionBox at = moving_box_at(mp, frame.grain_index, frame.format);
        RgbaImage img;
        if (frame.sprites[i].rgba) {
            scale_sprite(frame.sprites[i], at.w, at.h, img);
        } else {
            std::uint8_t r, g, b;
            parse_hex_color(box.color, r, g, b);
            img = solid_box(at.w, at.h, r, g, b, 255);
            TextStyle ts;
            ts.font = "DejaVu Sans Mono";
            ts.pixel_size = std::max(10, at.h / 4);
            ts.r = ts.g = ts.b = 0;
            ts.a = 255;
            const auto counter = text_.render(frame.vars.frame, ts);
            blit_rgba(img, (at.w - counter.w) / 2, (at.h - counter.h) / 2, counter);
        }
        Placed p;
        p.x = at.x;
        p.y = at.y;
        p.opacity = at.opacity;
        p.on_key = box.include_in_key;
        p.img = std::move(img);
        layers.push_back(std::move(p));
    }

    for (const auto& layer : layers) {
        composite_rgba_onto_v210(v210, fw, fh, layer.x, layer.y, layer.img.w, layer.img.h, layer.img.px.data(), layer.opacity);
        if (key_v210 && layer.on_key) {
            // Raise the key where the overlay is opaque.
            RgbaImage white = layer.img;
            for (std::size_t i = 0; i + 3 < white.px.size(); i += 4) {
                white.px[i] = white.px[i + 1] = white.px[i + 2] = 255;
            }
            composite_rgba_onto_v210(key_v210, fw, fh, layer.x, layer.y, white.w, white.h, white.px.data(), layer.opacity);
        }
    }
}

}  // namespace mtp
