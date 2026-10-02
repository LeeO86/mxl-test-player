#pragma once

#include "format.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mtp {

struct RgbaBuffer {
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> px;
};

struct SpriteSequence {
    int w = 0;
    int h = 0;
    std::vector<RgbaBuffer> frames;
    std::vector<double> durations;
};

enum class ScaleMode { Fit, Fill, Center, SpriteMax };

// Decode the first image (or the whole animation) to RGBA.
// `box_w/box_h` is the target canvas. SpriteMax scales so the longest side is box_w.
bool decode_visual(const std::string& path, ScaleMode mode, int box_w, int box_h, SpriteSequence& out, std::string& error);

struct RamClip {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::int64_t frames = 0;
    std::uint64_t audio_samples = 0;
    std::vector<std::uint8_t> v210;    // frames * v210_size
    std::vector<std::uint8_t> key;     // optional, same layout, empty if none
    std::vector<float> audio;          // interleaved, audio_samples * channels
};

bool load_mezzanine(const std::string& path, RamClip& out, std::string& error);

class MezzanineReader {
public:
    MezzanineReader();
    ~MezzanineReader();
    MezzanineReader(const MezzanineReader&) = delete;
    MezzanineReader& operator=(const MezzanineReader&) = delete;
    bool open(const std::string& path, std::string& error);
    void close();
    std::int64_t frame_count() const { return frames_; }
    int channels() const { return channels_; }
    int width() const { return width_; }
    int height() const { return height_; }
    // Copies one full frame of v210 and `audio_count` interleaved samples starting at `audio_start`.
    bool read(std::int64_t frame, std::uint8_t* v210, float* audio, std::uint64_t audio_start, int audio_count, std::string& error);

private:
    struct Impl;
    Impl* impl_ = nullptr;
    std::int64_t frames_ = 0;
    int channels_ = 0;
    int width_ = 0;
    int height_ = 0;
};

void rgba_to_fill_key(const std::uint8_t* rgba, int w, int h, bool premultiply, std::uint8_t* fill_v210, std::uint8_t* key_v210,
                      std::uint8_t* alpha10, int key_min, int key_max);

void extract_v210_field(const std::uint8_t* frame, int width, int height, int field, std::uint8_t* dst);

bool write_jpeg(const std::string& path, const std::uint8_t* rgb, int w, int h, int quality);
bool v210_to_jpeg(const std::string& path, const std::uint8_t* v210, int w, int h, int out_w);

}  // namespace mtp
