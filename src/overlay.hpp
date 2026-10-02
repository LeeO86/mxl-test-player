#pragma once

#include "config.hpp"
#include "motion.hpp"
#include "placeholders.hpp"
#include "text.hpp"

#include <map>
#include <string>

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
    std::uint64_t grain_index = 0;
    SpriteFrame sprites[2]{};  // resolved content for each box; null rgba = builtin
};

class OverlayRenderer {
public:
    explicit OverlayRenderer(std::string font_dir);
    void apply(std::uint8_t* v210, std::uint8_t* key_v210, const OverlayFrame& frame);

private:
    TextRenderer text_;
    std::map<std::string, RgbaImage> cache_;
    const RgbaImage& cached(const std::string& key, const std::string& utf8, const TextStyle& style);
};

}  // namespace mtp
