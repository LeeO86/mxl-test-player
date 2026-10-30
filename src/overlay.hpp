#pragma once

#include "config.hpp"
#include "motion.hpp"
#include "placeholders.hpp"
#include "text.hpp"
#include "v210.hpp"

#include <map>
#include <string>
#include <vector>

namespace mtp {

struct SpriteFrame {
    const std::uint8_t* rgba = nullptr;
    int w = 0;
    int h = 0;
};

struct OverlayFrame {
    VideoFormat format;
    BurnInConfig burnin;
    PlaceholderVars vars;
    bool keyed_still = false;
    bool premultiplied = false;  // the fill under the overlay is premultiplied (a still converted so)
    std::uint64_t grain_index = 0;
    SpriteFrame sprites[2]{};  // resolved content for each box; null rgba = builtin
};

// An area the overlay drew on, in pixels (not clipped to the frame).
struct DrawnRect {
    int x = 0, y = 0, w = 0, h = 0;
};

class OverlayRenderer {
public:
    explicit OverlayRenderer(std::string font_dir);
    // Draws onto the fill and, every layer with its own opacity, onto the fill's alpha (the v210a
    // alpha plane or the fill_key key) when there is one. The areas drawn on are appended to
    // `rects` when given; the alpha is drawn on in the same areas.
    void apply(std::uint8_t* v210, const AlphaTarget& alpha, const OverlayFrame& frame, std::vector<DrawnRect>* rects = nullptr);

private:
    TextRenderer text_;
    std::map<std::string, RgbaImage> cache_;
    const RgbaImage& cached(const std::string& key, const std::string& utf8, const TextStyle& style);
};

}  // namespace mtp
