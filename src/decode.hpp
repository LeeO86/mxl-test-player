#pragma once

#include "format.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mtp {

struct RgbaBuffer {
    int w = 0;
    int h = 0;
    std::vector<std::uint8_t> px;
};

enum class ScaleMode { Fit, Fill, Center, SpriteMax };

// Bounds of an image or animation decode. One frame is held at a time; an animation with more
// frames, or an image with more pixels, is refused with a clear error (a small GIF can hold
// thousands of full-size frames).
constexpr int kMaxAnimationFrames = 1000;
// 8K UHD (7680x4320) fits; the RGBA buffers of one frame are then at most 150 MB each.
constexpr std::int64_t kMaxImagePixels = 8192LL * 4608LL;

struct VisualInfo {
    int w = 0;
    int h = 0;
    int frames = 0;
    double total_s = 0;  // sum of the frame durations
};

// The codec name of the file's video stream ("gif", "png", "h264", …), read with libavformat; empty
// when it has none. Used when ffprobe gives nothing.
std::string probe_video_codec(const std::string& path);

// Size, frame count and duration of an image or animation from its packets, without decoding.
bool probe_visual(const std::string& path, VisualInfo& info, std::string& error);

// Decodes every frame once, in order, to RGBA at the source size (FFmpeg composites GIF and
// APNG frames onto the canvas, with transparency), and calls `on_frame` with the frame and its
// duration in seconds (0.1 when the file has none or a GIF says 10 ms or less). `on_frame` returns false to stop. Fails on a
// decode error, an image larger than kMaxImagePixels or more than kMaxAnimationFrames frames.
bool decode_visual_frames(const std::string& path, const std::function<bool(const RgbaBuffer&, double)>& on_frame, std::string& error);

// Scales RGBA (bilinear, premultiplied; the result is straight, RGB 0 where the alpha is 0) onto
// a canvas: Fit, Fill or Center in box_w x box_h, transparent around it; SpriteMax so the longest
// side is at most box_w (the canvas is the scaled size).
class RgbaFitter {
public:
    RgbaFitter() = default;
    ~RgbaFitter();
    RgbaFitter(const RgbaFitter&) = delete;
    RgbaFitter& operator=(const RgbaFitter&) = delete;
    void fit(const RgbaBuffer& src, ScaleMode mode, int box_w, int box_h, RgbaBuffer& out);

private:
    void* sws_ = nullptr;
    std::vector<std::uint8_t> premul_;
    std::vector<std::uint8_t> scaled_;
};

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

// Copies one field's lines out of a frame: lines 0 = the top field (even lines), 1 = the bottom
// field (odd lines). VideoFormat::field_lines gives the lines of a grain index.
void extract_v210_field(const std::uint8_t* frame, int width, int height, int lines, std::uint8_t* dst);

bool write_jpeg(const std::string& path, const std::uint8_t* rgb, int w, int h, int quality);
bool v210_to_jpeg(const std::string& path, const std::uint8_t* v210, int w, int h, int out_w);

}  // namespace mtp
