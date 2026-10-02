#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mtp {

struct RgbaImage {
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> px;  // RGBA
    bool empty() const { return w <= 0 || h <= 0; }
};

struct TextStyle {
    std::string font = "DejaVu Sans";
    int pixel_size = 32;
    std::uint8_t r = 255, g = 255, b = 255, a = 255;
    bool box = false;
    std::uint8_t br = 0, bg = 0, bb = 0, ba = 128;
    int pad = 8;
    bool outline = false;
    std::uint8_t or_ = 0, og = 0, ob = 0;
    bool shadow = false;
};

class TextRenderer {
public:
    explicit TextRenderer(std::string font_dir);
    ~TextRenderer();
    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    RgbaImage render(const std::string& utf8, const TextStyle& style);

private:
    struct Impl;
    Impl* impl_;
};

bool parse_hex_color(std::string_view s, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b);

}  // namespace mtp
