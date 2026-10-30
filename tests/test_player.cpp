#include "anc.hpp"
#include "audio.hpp"
#include "config.hpp"
#include "conform.hpp"
#include "decode.hpp"
#include "format.hpp"
#include "ids.hpp"
#include "media.hpp"
#include "motion.hpp"
#include "nmos.hpp"
#include "overlay.hpp"
#include "output.hpp"
#include "mxl_io.hpp"
#include "pattern.hpp"
#include "placeholders.hpp"
#include "playlist.hpp"
#include "process.hpp"
#include "timecode.hpp"
#include "uuid_util.hpp"
#include "v210.hpp"
#include "widgets.hpp"

#include <doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <tuple>
#include <vector>

#include <unistd.h>

using namespace mtp;

TEST_CASE("format parse and interlaced grain rate") {
    auto p = parse_format("1080p50");
    REQUIRE(p);
    CHECK(p->width == 1920);
    CHECK(p->height == 1080);
    CHECK(p->frame_rate == Rational{50, 1});
    CHECK_FALSE(p->interlaced());
    CHECK(p->grain_rate() == Rational{50, 1});

    auto i = parse_format("1080i25");
    REQUIRE(i);
    CHECK(i->interlaced());
    CHECK(i->frame_rate == Rational{25, 1});
    CHECK(i->grain_rate() == Rational{50, 1});
    CHECK(std::string(i->interlace_mode()) == "interlaced_tff");

    auto df = parse_format("1080i29.97");
    REQUIRE(df);
    CHECK(df->frame_rate == Rational{30000, 1001});
    CHECK(df->grain_rate() == Rational{60000, 1001});

    CHECK_FALSE(parse_format("720i50"));
    CHECK_FALSE(parse_format("1080i50"));
    CHECK(parse_format("2160p50")->width == 3840);
    CHECK(parse_format("720p59.94")->frame_rate == Rational{60000, 1001});
}

TEST_CASE("audio cadence is exact over 1000 grains") {
    const Rational r59{60000, 1001};
    std::uint64_t sum = 0;
    bool saw800 = false, saw801 = false;
    for (std::uint64_t i = 0; i < 1000; ++i) {
        const auto n = samples_in_grain(i, r59);
        CHECK((n == 800 || n == 801));
        if (n == 800) saw800 = true;
        if (n == 801) saw801 = true;
        sum += n;
    }
    CHECK(saw800);
    CHECK(saw801);
    CHECK(sum == samples_until_grain(1000, r59));
    CHECK(samples_in_grain(0, Rational{50, 1}) == 960);
    CHECK(samples_until_grain(3, Rational{50, 1}) == 2880);

    const Rational r29{30000, 1001};
    bool saw1601 = false, saw1602 = false;
    std::uint64_t sum29 = 0;
    for (std::uint64_t i = 0; i < 1000; ++i) {
        const auto n = samples_in_grain(i, r29);
        CHECK((n == 1601 || n == 1602));
        if (n == 1601) saw1601 = true;
        if (n == 1602) saw1602 = true;
        sum29 += n;
    }
    CHECK(saw1601);
    CHECK(saw1602);
    CHECK(sum29 == samples_until_grain(1000, r29));
}

TEST_CASE("loop conforming matches video frames times cadence") {
    const Rational rate{50, 1};
    const std::int64_t frames = 10;
    const auto samples = samples_until_grain(static_cast<std::uint64_t>(frames), rate);
    CHECK(samples == 960ull * 10);
    std::vector<float> src(4 * 100, 0.25f);
    std::vector<float> dst(samples * 2);
    conform_interleaved(src.data(), 4, 100, dst.data(), 2, samples, 0);
    CHECK(dst[0] == doctest::Approx(0.25f));
    CHECK(dst[1] == doctest::Approx(0.25f));
    // Padded channels dropped, missing samples are silence.
    CHECK(dst[(100) * 2] == doctest::Approx(0.f));
}

namespace {
// The whole-line composite before 1.0.1, as the reference.
void composite_whole_line(std::uint8_t* v210, int width, int height, int x0, int y0, int bw, int bh, const std::uint8_t* rgba,
                          double opacity) {
    const std::uint32_t stride = v210_line_stride(width);
    std::vector<std::uint16_t> y(static_cast<std::size_t>(width)), cb(static_cast<std::size_t>(width / 2)), cr(static_cast<std::size_t>(width / 2));
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
}  // namespace

TEST_CASE("burn-in composite touches only the groups under the box, same pixels as the whole-line version") {
    std::uint32_t seed = 9;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    for (int width : {1920, 3840}) {
        const int height = 40;
        std::vector<std::uint8_t> frame(v210_size(width, height));
        fill_v210_rgb(frame.data(), width, height, 0.2, 0.5, 0.7);
        struct Box { int x, y, w, h; double opacity; };
        for (Box box : {Box{100, 3, 301, 20, 1.0}, Box{-17, -5, 90, 30, 0.6}, Box{width - 50, 30, 120, 25, 0.9}, Box{7, 0, 5, 40, 1.0}}) {
            std::vector<std::uint8_t> rgba(static_cast<std::size_t>(box.w) * box.h * 4);
            for (auto& v : rgba) v = static_cast<std::uint8_t>(rnd());
            auto expected = frame;
            auto actual = frame;
            composite_whole_line(expected.data(), width, height, box.x, box.y, box.w, box.h, rgba.data(), box.opacity);
            composite_rgba_onto_v210(actual.data(), width, height, box.x, box.y, box.w, box.h, rgba.data(), box.opacity);
            CHECK(expected == actual);
        }
    }
}

TEST_CASE("the integer blend of opaque layers equals the double formula") {
    // composite_rgba_onto_v210 blends layers with opacity 1 in integers.
    int mismatches = 0;
    for (int alpha = 0; alpha < 256; ++alpha) {
        const double a = (alpha / 255.0) * 1.0;
        for (int under = 4; under <= 1019; under += 7) {
            for (int over = 4; over <= 1019; over += 5) {
                const auto exact = static_cast<std::uint16_t>(under * (1.0 - a) + over * a + 0.5);
                const auto fast = static_cast<std::uint16_t>((2 * (under * (255 - alpha) + over * alpha) + 255) / 510);
                mismatches += exact != fast;
            }
        }
    }
    CHECK(mismatches == 0);
}

TEST_CASE("restoring the composited boxes gives back the frame under the overlay") {
    // The writer keeps a still source in a grain slot and restores only the overlay's boxes.
    std::uint32_t seed = 11;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    const VideoFormat fmt = *parse_format("720p25");
    PatternRequest req;
    req.format = fmt;
    req.pattern = VideoPattern::SmpteRp219;
    const StillFrame still = still_pattern(req);
    REQUIRE(still.data != nullptr);
    CHECK(still_pattern(req).token == still.token);  // the same content keeps its token
    const std::vector<std::uint8_t> under(still.data, still.data + v210_size(fmt.width, fmt.height));
    auto frame = under;
    struct Box { int x, y, w, h; double opacity; };
    const std::vector<Box> boxes{{100, 3, 301, 20, 1.0}, {-17, -5, 90, 30, 0.6}, {fmt.width - 50, fmt.height - 10, 120, 25, 0.9}, {7, 0, 5, 720, 1.0}};
    for (const Box& box : boxes) {
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(box.w) * box.h * 4);
        for (auto& v : rgba) v = static_cast<std::uint8_t>(rnd());
        composite_rgba_onto_v210(frame.data(), fmt.width, fmt.height, box.x, box.y, box.w, box.h, rgba.data(), box.opacity);
    }
    CHECK(frame != under);
    for (const Box& box : boxes) restore_v210_rect(frame.data(), still.data, fmt.width, fmt.height, box.x, box.y, box.w, box.h);
    CHECK(frame == under);
    // Moving patterns have no still frame.
    req.pattern = VideoPattern::Motion;
    CHECK(still_pattern(req).data == nullptr);
}

namespace {
int a10_at(const std::vector<std::uint8_t>& plane, int width, int x, int y) {
    std::vector<std::uint16_t> a(static_cast<std::size_t>(width));
    unpack_alpha10_line(plane.data() + static_cast<std::size_t>(y) * alpha10_line_stride(width), width, a.data());
    return a[static_cast<std::size_t>(x)];
}
int luma_at(const std::vector<std::uint8_t>& frame, int width, int x, int y) {
    std::vector<std::uint16_t> l(static_cast<std::size_t>(width)), cb(static_cast<std::size_t>(width / 2)), cr(static_cast<std::size_t>(width / 2));
    unpack_v210_line(frame.data() + static_cast<std::size_t>(y) * v210_line_stride(width), width, l.data(), cb.data(), cr.data());
    return l[static_cast<std::size_t>(x)];
}
std::vector<std::uint8_t> solid_rgba(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < px.size(); i += 4) {
        px[i] = r;
        px[i + 1] = g;
        px[i + 2] = b;
        px[i + 3] = a;
    }
    return px;
}
}  // namespace

TEST_CASE("burn-ins go over the alpha: a_out = a_src + a_dst * (1 - a_src), the fill stays straight") {
    const int width = 96, height = 4;
    // Three columns of destination alpha: transparent, half, opaque (v210a codes 0, 512, 1023).
    std::vector<std::uint8_t> fill(v210_size(width, height));
    fill_v210(fill.data(), width, height, kYBlack, kCMid, kCMid);
    std::vector<std::uint8_t> a10(alpha10_size(width, height));
    std::vector<std::uint16_t> line(static_cast<std::size_t>(width));
    for (int x = 0; x < width; ++x) line[static_cast<std::size_t>(x)] = x < 32 ? 0 : x < 64 ? 512 : 1023;
    for (int y = 0; y < height; ++y) pack_alpha10_line(line.data(), width, a10.data() + static_cast<std::size_t>(y) * alpha10_line_stride(width));
    // White at alpha 128/255 and layer opacity 0.6: a_src = 0.3012.
    const auto white = solid_rgba(width, height, 255, 255, 255, 128);
    const double as = 128 / 255.0 * 0.6;
    AlphaTarget target{AlphaTarget::Kind::A10, a10.data(), kYBlack, kYWhite};
    composite_rgba_over_alpha(fill.data(), target, width, height, 0, 0, width, height, white.data(), 0.6, false);
    for (int x : {5, 40, 80}) {
        const double ad = (x < 32 ? 0 : x < 64 ? 512 : 1023) / 1023.0;
        const double ao = as + ad * (1 - as);
        CHECK(a10_at(a10, width, x, 2) == static_cast<int>(ao * 1023 + 0.5));
        // Straight fill: (c_src a_src + c_dst a_dst (1 - a_src)) / a_out.
        const double yexp = (940 * as + 64 * ad * (1 - as)) / ao;
        CHECK(std::abs(luma_at(fill, width, x, 2) - yexp) <= 1.0);
    }
    // Over a transparent background the fill is the layer's own colour, not darkened.
    CHECK(luma_at(fill, width, 5, 0) == 940);
    // Outside the layer nothing changes.
    std::vector<std::uint8_t> fill2(v210_size(width, height));
    fill_v210(fill2.data(), width, height, kYBlack, kCMid, kCMid);
    std::vector<std::uint8_t> a2(alpha10_size(width, height));
    AlphaTarget t2{AlphaTarget::Kind::A10, a2.data(), kYBlack, kYWhite};
    const auto box = solid_rgba(10, 2, 255, 255, 255, 255);
    composite_rgba_over_alpha(fill2.data(), t2, width, height, 20, 1, 10, 2, box.data(), 1.0, false);
    CHECK(a10_at(a2, width, 19, 1) == 0);
    CHECK(a10_at(a2, width, 20, 1) == 1023);
    CHECK(a10_at(a2, width, 29, 2) == 1023);
    CHECK(a10_at(a2, width, 30, 2) == 0);
    CHECK(a10_at(a2, width, 25, 0) == 0);
    CHECK(a10_at(a2, width, 25, 3) == 0);
}

TEST_CASE("burn-ins go over the fill_key key with the key range, and premultiplied fills blend linearly") {
    const int width = 48, height = 2;
    const int kmin = 64, kmax = 940;
    std::vector<std::uint8_t> fill(v210_size(width, height));
    fill_v210(fill.data(), width, height, kYBlack, kCMid, kCMid);
    std::vector<std::uint8_t> key(v210_size(width, height));
    fill_v210(key.data(), width, height, kmin, kCMid, kCMid);  // transparent
    const auto grey = solid_rgba(width, height, 128, 128, 128, 255);
    AlphaTarget target{AlphaTarget::Kind::Key, key.data(), kmin, kmax};
    composite_rgba_over_alpha(fill.data(), target, width, height, 0, 0, width, height, grey.data(), 0.5, false);
    CHECK(luma_at(key, width, 10, 1) == alpha_to_key(0.5, kmin, kmax));
    const int grey_y = rgb_to_yuv709(128 / 255.0, 128 / 255.0, 128 / 255.0).y;
    CHECK(luma_at(fill, width, 10, 1) == grey_y);  // straight: the colour itself
    // A second layer at 0.5 over 0.5: 0.75.
    composite_rgba_over_alpha(fill.data(), target, width, height, 0, 0, width, height, grey.data(), 0.5, false);
    CHECK(luma_at(key, width, 10, 1) == alpha_to_key(0.75, kmin, kmax));
    // Premultiplied: the fill is blended towards black like the old composite.
    std::vector<std::uint8_t> pfill(v210_size(width, height));
    fill_v210(pfill.data(), width, height, kYBlack, kCMid, kCMid);
    std::vector<std::uint8_t> pkey(v210_size(width, height));
    fill_v210(pkey.data(), width, height, kmin, kCMid, kCMid);
    AlphaTarget pt{AlphaTarget::Kind::Key, pkey.data(), kmin, kmax};
    composite_rgba_over_alpha(pfill.data(), pt, width, height, 0, 0, width, height, grey.data(), 0.5, true);
    CHECK(std::abs(luma_at(pfill, width, 10, 1) - (grey_y * 0.5 + 64 * 0.5)) <= 1.0);
    CHECK(luma_at(pkey, width, 10, 1) == alpha_to_key(0.5, kmin, kmax));
    // Without an alpha target it is the plain composite.
    std::vector<std::uint8_t> a(v210_size(width, height)), b(v210_size(width, height));
    fill_v210_rgb(a.data(), width, height, 0.2, 0.4, 0.6);
    b = a;
    composite_rgba_over_alpha(a.data(), AlphaTarget{}, width, height, 3, 0, 20, 2, grey.data(), 0.7, false);
    composite_rgba_onto_v210(b.data(), width, height, 3, 0, 20, 2, grey.data(), 0.7);
    CHECK(a == b);
}

TEST_CASE("restoring the alpha plane under the composited boxes gives back the plane") {
    const VideoFormat fmt = *parse_format("720p25");
    std::vector<std::uint8_t> under(alpha10_size(fmt.width, fmt.height));
    fill_alpha10(under.data(), fmt.width, fmt.height, 1023);
    auto plane = under;
    std::vector<std::uint8_t> fill(v210_size(fmt.width, fmt.height));
    fill_v210(fill.data(), fmt.width, fmt.height, kYBlack, kCMid, kCMid);
    std::uint32_t seed = 5;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    struct Box { int x, y, w, h; };
    // Transparent under-plane so every drawn pixel changes.
    fill_alpha10(under.data(), fmt.width, fmt.height, 0);
    plane = under;
    const std::vector<Box> boxes{{100, 3, 301, 20}, {-17, -5, 91, 30}, {fmt.width - 50, fmt.height - 10, 120, 25}, {7, 0, 5, 720}, {1, 1, 1, 1}};
    for (const Box& box : boxes) {
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(box.w) * box.h * 4);
        for (auto& v : rgba) v = static_cast<std::uint8_t>(rnd() | 0x80);
        AlphaTarget t{AlphaTarget::Kind::A10, plane.data(), kYBlack, kYWhite};
        composite_rgba_over_alpha(fill.data(), t, fmt.width, fmt.height, box.x, box.y, box.w, box.h, rgba.data(), 0.8, false);
    }
    CHECK(plane != under);
    for (const Box& box : boxes) restore_a10_rect(plane.data(), under.data(), fmt.width, fmt.height, box.x, box.y, box.w, box.h);
    CHECK(plane == under);
}

TEST_CASE("every burn-in writes the alpha: text and moving box on a v210a plane") {
    const VideoFormat fmt = *parse_format("720p25");
    std::vector<std::uint8_t> fill(v210_size(fmt.width, fmt.height));
    fill_v210(fill.data(), fmt.width, fmt.height, kYBlack, kCMid, kCMid);
    std::vector<std::uint8_t> a10(alpha10_size(fmt.width, fmt.height));  // transparent
    OverlayRenderer overlay(std::string(MTP_SOURCE_DIR) + "/assets/fonts");
    OverlayFrame of;
    of.format = fmt;
    of.burnin = BurnInConfig{};
    of.burnin.label = of.burnin.timecode = of.burnin.frame_counter = of.burnin.item = of.burnin.flow_id = false;
    TextLayerConfig t;
    t.text = "ALPHA";
    t.anchor = "tl";
    t.size = 10;
    t.box = true;
    t.box_opacity = 0.5;
    of.burnin.texts.push_back(t);
    MovingBoxConfig box;
    box.enabled = true;
    box.motion.path = MotionPath::Horizontal;
    box.motion.start_y = 1.0;  // bottom of the frame
    box.motion.speed = 0;
    box.motion.opacity = 0.6;
    of.burnin.boxes.push_back(box);
    std::vector<DrawnRect> rects;
    overlay.apply(fill.data(), AlphaTarget{AlphaTarget::Kind::A10, a10.data(), kYBlack, kYWhite}, of, &rects);
    REQUIRE(rects.size() == 2);
    // The text's background box at 50 %, the moving box at 60 %, nothing elsewhere.
    const DrawnRect& tr = rects[0];
    CHECK(a10_at(a10, fmt.width, tr.x + 1, tr.y + 1) == static_cast<int>(127 / 255.0 * 1023 + 0.5));  // box alpha 127
    const DrawnRect& br = rects[1];
    CHECK(a10_at(a10, fmt.width, br.x + br.w / 2, br.y + 2) == static_cast<int>(0.6 * 1023 + 0.5));
    CHECK(a10_at(a10, fmt.width, fmt.width / 2, fmt.height / 2) == 0);
}

TEST_CASE("importing the same file again keeps one library item") {
    const auto dir = std::filesystem::temp_directory_path() / "mtp-library-unit";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "import");
    const auto file = (dir / "import" / "clip.bin").string();
    std::ofstream(file, std::ios::binary) << "not really a clip";
    Config cfg;
    cfg.library_dir = (dir / "library").string();
    const VideoFormat fmt = *parse_format("1080p50");
    {
        Library library(cfg);  // not started: no conversion workers
        const auto a = library.ingest_file(file, "clip.bin", nlohmann::json::object(), fmt);
        const auto b = library.ingest_file(file, "clip.bin", nlohmann::json::object(), fmt);  // the next start's scan
        CHECK(a.id == b.id);
        CHECK(library.list().size() == 1);
    }
    // An index written by 1.0.2 with the item repeated loads it once.
    const std::string entry = R"({"id":"33768ac9-a3ae-5d23-883d-52ce5b159dfb","name":"clip","type":"video","conversions":{}})";
    std::filesystem::create_directories(dir / "library2");
    std::ofstream(dir / "library2" / "index.json") << "[" << entry << "," << entry << "," << entry << "]";
    cfg.library_dir = (dir / "library2").string();
    Library reloaded(cfg);
    CHECK(reloaded.list().size() == 1);
    std::filesystem::remove_all(dir);
}

TEST_CASE("an animation plays at its own frame timing, mapped onto whole output frames") {
    // 10 frames of 0.1 s: 1 s per loop.
    const std::vector<double> even(10, 0.1);
    CHECK(animation_loop_frames(1.0, Rational{50, 1}) == 50);
    CHECK(animation_loop_frames(1.0, Rational{25, 1}) == 25);
    CHECK(animation_loop_frames(1.0, Rational{60000, 1001}) == 60);
    CHECK(animation_loop_frames(0.07, Rational{50, 1}) == 4);  // 3.5 frames, rounded
    CHECK(animation_loop_frames(0.001, Rational{25, 1}) == 1);
    CHECK(animation_loop_frames(0, Rational{25, 1}) == 1);
    for (std::int64_t k = 0; k < 50; ++k) CHECK(animation_frame_at(k, 50, even.data(), 10) == static_cast<int>(k / 5));
    CHECK(animation_frame_at(50, 50, even.data(), 10) == 0);  // the next loop
    CHECK(animation_frame_at(-1, 50, even.data(), 10) == 9);
    // Uneven delays (0.2, 0.04, 0.16 s) at 25p: 10 output frames, 5 + 1 + 4.
    const std::vector<double> uneven{0.2, 0.04, 0.16};
    const auto loop25 = animation_loop_frames(0.4, Rational{25, 1});
    REQUIRE(loop25 == 10);
    std::vector<int> seq;
    for (std::int64_t k = 0; k < loop25; ++k) seq.push_back(animation_frame_at(k, loop25, uneven.data(), 3));
    CHECK(seq == std::vector<int>{0, 0, 0, 0, 0, 1, 2, 2, 2, 2});
    // At 59.94 the loop is 24 output frames (23.976 rounded): each frame keeps its share.
    const auto loop5994 = animation_loop_frames(0.4, Rational{60000, 1001});
    REQUIRE(loop5994 == 24);
    int counts[3] = {0, 0, 0};
    for (std::int64_t k = 0; k < loop5994; ++k) counts[animation_frame_at(k, loop5994, uneven.data(), 3)]++;
    CHECK(counts[0] == 12);
    CHECK(counts[1] == 3);
    CHECK(counts[2] == 9);
    // One frame is always frame 0.
    CHECK(animation_frame_at(7, 25, even.data(), 1) == 0);
}

TEST_CASE("an animated GIF is converted once into a sprite and a clip with alpha, bounded by the RAM budget") {
    const auto dir = std::filesystem::temp_directory_path() / "mtp-anim-unit";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    auto ffmpeg = [&](const std::string& args) { return std::system(("ffmpeg -hide_banner -loglevel error -y " + args).c_str()); };
    // 4 frames of 0.1 s: a red square on a transparent background.
    const auto small = (dir / "square.gif").string();
    REQUIRE(ffmpeg("-f lavfi -i \"color=c=black@0.0:s=64x36:r=10:d=0.4,format=rgba,"
                   "geq=r=255:g=0:b=0:a='if(between(X,8,23)*between(Y,8,23),255,0)'\" "
                   "-vf \"split[a][b];[a]palettegen=reserve_transparent=1[p];[b][p]paletteuse=alpha_threshold=128\" -loop 0 " + small) == 0);
    // A small file with many full-size frames: 20 identical 1280x720 frames.
    const auto big = (dir / "big.gif").string();
    REQUIRE(ffmpeg("-f lavfi -i color=c=blue:s=1280x720:r=10 -frames:v 20 " + big) == 0);
    // More frames than the limit.
    const auto many = (dir / "many.gif").string();
    REQUIRE(ffmpeg("-f lavfi -i testsrc=s=16x16:r=100 -frames:v 1001 " + many) == 0);
    VisualInfo info;
    std::string err;
    REQUIRE(probe_visual(big, info, err));
    CHECK(info.frames == 20);
    CHECK(info.w == 1280);
    CHECK(info.total_s == doctest::Approx(2.0));

    Config cfg;
    cfg.library_dir = (dir / "library").string();
    cfg.ram_budget_mb = 64;  // 20 frames of 720p fill and alpha are 74 MB
    const VideoFormat fmt = *parse_format("720p25");
    Library library(cfg);
    library.start();
    const auto a = library.ingest_file(small, "square", nlohmann::json::object(), fmt);
    const auto b = library.ingest_file(big, "big", nlohmann::json::object(), fmt);
    const auto c = library.ingest_file(many, "many", nlohmann::json::object(), fmt);
    for (const auto* it : {&a, &b, &c}) library.wait_ready(it->id, fmt.name, 60000);
    library.stop();

    const auto ca = library.get(a.id).conversions.at(fmt.name);
    CHECK(library.get(a.id).type == ItemType::Sprite);
    CHECK(ca.at("status") == "ready");
    CHECK(ca.at("frames") == 4);
    CHECK(ca.at("clip") == true);
    CHECK(ca.at("clip_frames") == 10);
    const auto adir = std::filesystem::path(library.item_dir(a.id));
    const std::size_t fill = v210_size(fmt.width, fmt.height), alpha = alpha10_size(fmt.width, fmt.height);
    CHECK(std::filesystem::file_size(adir / "anim-720p25.v210") == 4 * fill);
    CHECK(std::filesystem::file_size(adir / "anim-720p25.a10") == 4 * alpha);
    CHECK(std::filesystem::exists(adir / "sprite.json"));
    // Fit: 64x36 scaled 20x to 1280x720, the square at (160..480, 160..480). Alpha 1023 there, 0 around.
    std::ifstream ain(adir / "anim-720p25.a10", std::ios::binary);
    std::vector<std::uint8_t> plane(alpha);
    ain.read(reinterpret_cast<char*>(plane.data()), static_cast<std::streamsize>(alpha));
    CHECK(a10_at(plane, fmt.width, 320, 320) == 1023);
    CHECK(a10_at(plane, fmt.width, 800, 500) == 0);
    // The fill is black where the alpha is 0 (the GIF's transparent colour is white).
    std::ifstream fin(adir / "anim-720p25.v210", std::ios::binary);
    std::vector<std::uint8_t> first(fill);
    fin.read(reinterpret_cast<char*>(first.data()), static_cast<std::streamsize>(fill));
    CHECK(luma_at(first, fmt.width, 800, 500) == kYBlack);
    CHECK(luma_at(first, fmt.width, 320, 320) == rgb_to_yuv709(1, 0, 0).y);

    // Too big as a clip for the budget: refused with a reason, still a sprite.
    const auto cb = library.get(b.id).conversions.at(fmt.name);
    CHECK(cb.at("status") == "ready");
    CHECK(cb.at("clip") == false);
    CHECK(cb.at("clip_error").get<std::string>().find("PLAYER_RAM_BUDGET_MB") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(std::filesystem::path(library.item_dir(b.id)) / "anim-720p25.v210"));

    // Too many frames: refused.
    const auto cc = library.get(c.id).conversions.at(fmt.name);
    CHECK(cc.at("status") == "failed");
    CHECK(cc.at("error").get<std::string>().find("more than 1000") != std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST_CASE("v210 pack roundtrip and legal black") {
    const int w = 48, h = 2;
    std::vector<std::uint8_t> buf(v210_size(w, h));
    fill_v210(buf.data(), w, h, kYBlack, kCMid, kCMid);
    std::vector<std::uint16_t> y(w), cb(w / 2), cr(w / 2);
    unpack_v210_line(buf.data(), w, y.data(), cb.data(), cr.data());
    CHECK(y[0] == kYBlack);
    CHECK(cb[0] == kCMid);
    CHECK(cr[0] == kCMid);
    CHECK(v210_line_stride(1920) == 5120);
}

TEST_CASE("interlaced field order: tff and bff formats") {
    auto t = parse_format("1080i25");
    REQUIRE(t);
    CHECK(t->scan == Scan::InterlacedTff);
    CHECK(t->name == "1080i25");
    auto t2 = parse_format("1080i25tff");
    REQUIRE(t2);
    CHECK(t2->scan == Scan::InterlacedTff);
    CHECK(t2->name == "1080i25");  // the same flow ids as 1080i25
    auto b = parse_format("1080i25bff");
    REQUIRE(b);
    CHECK(b->scan == Scan::InterlacedBff);
    CHECK(b->name == "1080i25bff");
    CHECK(std::string(b->interlace_mode()) == "interlaced_bff");
    CHECK(b->grain_rate() == Rational{50, 1});
    CHECK(b->field_height() == 540);
    auto b2 = parse_format("1080I29.97bff");
    REQUIRE(b2);
    CHECK(b2->name == "1080i29.97bff");
    CHECK(b2->frame_rate == Rational{30000, 1001});
    CHECK_FALSE(parse_format("1080p25bff"));
    CHECK_FALSE(parse_format("1080i50bff"));
    CHECK_FALSE(parse_format("1080ibff"));

    // Grain 2k is the first field of frame k: top lines for tff, bottom lines for bff.
    CHECK(t->field_lines(0) == 0);
    CHECK(t->field_lines(1) == 1);
    CHECK(t->field_lines(1000) == 0);
    CHECK(b->field_lines(0) == 1);
    CHECK(b->field_lines(1) == 0);
    CHECK(b->field_lines(1001) == 0);

    // The flow definition the writer creates says bff, at the frame rate (MXL doubles it for the grains).
    const Uuid id = uuid_from_seed("bff-flow");
    const auto def = nlohmann::json::parse(video_flow_json(id, "Out 1", "Out 1", "Video", *b, false, id, id));
    CHECK(def["interlace_mode"] == "interlaced_bff");
    CHECK(def["frame_height"] == 1080);
    CHECK(def["grain_rate"]["numerator"] == 25);
    CHECK(def["grain_rate"]["denominator"] == 1);
}

namespace {

// The lines a reader puts a field grain in, written independently of the player as the
// multiviewer states it (src/media/interlace.cpp fieldLineParity): grain 2k is the first field,
// the top field for tff and the bottom field for bff.
int reader_line_parity(std::uint64_t index, bool bff) { return static_cast<int>(index & 1u) ^ (bff ? 1 : 0); }

// Weaves field grain `index` with the grain before it into a whole frame.
std::vector<std::uint8_t> reader_weave(const std::vector<std::uint8_t>& cur, const std::vector<std::uint8_t>& prev, std::uint64_t index,
                                       bool bff, int width, int height) {
    const std::size_t stride = v210_line_stride(width);
    std::vector<std::uint8_t> out(v210_size(width, height));
    const int pc = reader_line_parity(index, bff);
    for (int r = 0; r < height / 2; ++r) {
        std::memcpy(out.data() + static_cast<std::size_t>(2 * r + pc) * stride, cur.data() + static_cast<std::size_t>(r) * stride, stride);
        std::memcpy(out.data() + static_cast<std::size_t>(2 * r + (1 - pc)) * stride, prev.data() + static_cast<std::size_t>(r) * stride, stride);
    }
    return out;
}

// The grain the writer makes for MXL grain `idx` (output.cpp): the frame rendered for the
// field's time, then the field's lines cut out of it.
std::vector<std::uint8_t> writer_field_grain(const VideoFormat& fmt, VideoPattern pattern, std::uint64_t idx) {
    std::vector<std::uint8_t> full(v210_size(fmt.width, fmt.height));
    PatternRequest req;
    req.format = fmt;
    req.pattern = pattern;
    req.frame_index = idx / 2;
    req.field = static_cast<int>(idx & 1u);
    render_pattern(req, full.data());
    std::vector<std::uint8_t> grain(v210_size(fmt.width, fmt.field_height()));
    extract_v210_field(full.data(), fmt.width, fmt.height, fmt.field_lines(idx), grain.data());
    return grain;
}

std::vector<std::uint8_t> whole_frame(const VideoFormat& fmt, VideoPattern pattern, std::uint64_t frame, int field) {
    std::vector<std::uint8_t> full(v210_size(fmt.width, fmt.height));
    PatternRequest req;
    req.format = fmt;
    req.pattern = pattern;
    req.frame_index = frame;
    req.field = field;
    render_pattern(req, full.data());
    return full;
}

// Left edge of the field_order pattern's white bar on frame line y, -1 without one.
int bar_x(const std::uint8_t* frame, int width, int y) {
    std::vector<std::uint16_t> yy(static_cast<std::size_t>(width)), cb(static_cast<std::size_t>(width / 2)), cr(static_cast<std::size_t>(width / 2));
    unpack_v210_line(frame + static_cast<std::size_t>(y) * v210_line_stride(width), width, yy.data(), cb.data(), cr.data());
    for (int x = 0; x < width; ++x)
        if (yy[static_cast<std::size_t>(x)] > 900) return x;
    return -1;
}

}  // namespace

TEST_CASE("interlaced grains: each field in its own lines, de-woven by a reader (tff and bff)") {
    for (const char* name : {"1080i25", "1080i25bff", "1080i29.97bff"}) {
        CAPTURE(name);
        const VideoFormat fmt = *parse_format(name);
        const bool bff = fmt.bottom_field_first();
        const int w = fmt.width;
        const int h = fmt.height;
        const std::size_t stride = v210_line_stride(w);

        // A frame whose every line carries its own number in luma: the reader must put each
        // line back where it was.
        std::vector<std::uint8_t> numbered(v210_size(w, h));
        {
            std::vector<std::uint16_t> y(static_cast<std::size_t>(w)), c(static_cast<std::size_t>(w / 2), kCMid);
            for (int line = 0; line < h; ++line) {
                std::fill(y.begin(), y.end(), static_cast<std::uint16_t>(4 + line % 1000));  // 10-bit
                pack_v210_line(y.data(), c.data(), c.data(), w, numbered.data() + static_cast<std::size_t>(line) * stride);
            }
        }
        for (std::uint64_t k : {0ull, 7ull}) {
            std::vector<std::uint8_t> first(v210_size(w, h / 2)), second(v210_size(w, h / 2));
            extract_v210_field(numbered.data(), w, h, fmt.field_lines(2 * k), first.data());
            extract_v210_field(numbered.data(), w, h, fmt.field_lines(2 * k + 1), second.data());
            std::vector<std::uint16_t> y(static_cast<std::size_t>(w)), cb(static_cast<std::size_t>(w / 2)), cr(static_cast<std::size_t>(w / 2));
            // The first field in time is the top field (line 0) for tff, the bottom one (line 1) for bff.
            unpack_v210_line(first.data(), w, y.data(), cb.data(), cr.data());
            CHECK(y[0] == 4 + (bff ? 1 : 0));
            unpack_v210_line(second.data(), w, y.data(), cb.data(), cr.data());
            CHECK(y[0] == 4 + (bff ? 0 : 1));
            const auto woven = reader_weave(second, first, 2 * k + 1, bff, w, h);
            CHECK(woven == numbered);
        }

        // Moving patterns: each field is sampled at its own time, so the woven frame holds the
        // first field's picture in the first field's lines and the second's in the others.
        for (std::uint64_t k : {0ull, 5ull}) {
            const auto g0 = writer_field_grain(fmt, VideoPattern::FieldOrder, 2 * k);
            const auto g1 = writer_field_grain(fmt, VideoPattern::FieldOrder, 2 * k + 1);
            CHECK(g0 != g1);
            const auto woven = reader_weave(g1, g0, 2 * k + 1, bff, w, h);
            const auto at_first = whole_frame(fmt, VideoPattern::FieldOrder, k, 0);
            const auto at_second = whole_frame(fmt, VideoPattern::FieldOrder, k, 1);
            const int first_lines = bff ? 1 : 0;
            bool lines_match = true;
            for (int line = 0; line < h; ++line) {
                const auto& src = (line & 1) == first_lines ? at_first : at_second;
                if (std::memcmp(woven.data() + static_cast<std::size_t>(line) * stride, src.data() + static_cast<std::size_t>(line) * stride, stride) != 0)
                    lines_match = false;
            }
            CHECK(lines_match);
            // The bar moved right from the first field to the second (temporal order).
            const int mid = h / 2;
            const int x_first = bar_x(woven.data(), w, mid + ((mid & 1) == first_lines ? 0 : 1));
            const int x_second = bar_x(woven.data(), w, mid + ((mid & 1) == first_lines ? 1 : 0));
            CHECK(x_first >= 0);
            CHECK(x_second > x_first);
            // The next frame's first field is later still.
            const auto g2 = writer_field_grain(fmt, VideoPattern::FieldOrder, 2 * k + 2);
            const auto next = reader_weave(g2, g1, 2 * k + 2, bff, w, h);
            const int x_next = bar_x(next.data(), w, mid + ((mid & 1) == first_lines ? 0 : 1));
            CHECK(x_next > x_second);
        }

        // The motion pattern and the moving zone plate change between the two fields of a frame.
        for (auto p : {VideoPattern::Motion, VideoPattern::ZonePlateMoving}) {
            CHECK(whole_frame(fmt, p, 3, 0) != whole_frame(fmt, p, 3, 1));
        }
        // Patterns that do not move are the same in both fields.
        CHECK(whole_frame(fmt, VideoPattern::SmpteRp219, 3, 0) == whole_frame(fmt, VideoPattern::SmpteRp219, 3, 1));
    }
    // Progressive: the field is ignored and a pattern is sampled once per frame as before.
    const VideoFormat p = *parse_format("1080p25");
    CHECK(whole_frame(p, VideoPattern::Motion, 3, -1) == whole_frame(p, VideoPattern::Motion, 3, 1));
}

TEST_CASE("pattern checksums are stable and solids hit legal levels") {
    VideoFormat fmt = *parse_format("720p25");
    // Use a tiny manual raster via 720 only if we render full 720. That's fine.
    std::vector<std::uint8_t> buf(v210_size(fmt.width, fmt.height));
    PatternRequest req;
    req.format = fmt;
    req.pattern = VideoPattern::Black;
    render_pattern(req, buf.data());
    const auto black = fnv1a64(buf.data(), buf.size());
    render_pattern(req, buf.data());
    CHECK(fnv1a64(buf.data(), buf.size()) == black);
    std::vector<std::uint16_t> y(fmt.width), cb(fmt.width / 2), cr(fmt.width / 2);
    unpack_v210_line(buf.data(), fmt.width, y.data(), cb.data(), cr.data());
    CHECK(y[100] == kYBlack);

    req.pattern = VideoPattern::White;
    render_pattern(req, buf.data());
    unpack_v210_line(buf.data(), fmt.width, y.data(), cb.data(), cr.data());
    CHECK(y[100] == kYWhite);
    CHECK(fnv1a64(buf.data(), buf.size()) != black);

    req.pattern = VideoPattern::SmpteRp219;
    render_pattern(req, buf.data());
    const auto bars = fnv1a64(buf.data(), buf.size());
    render_pattern(req, buf.data());
    CHECK(fnv1a64(buf.data(), buf.size()) == bars);

    req.pattern = VideoPattern::Motion;
    req.frame_index = 0;
    render_pattern(req, buf.data());
    const auto m0 = fnv1a64(buf.data(), buf.size());
    req.frame_index = 3;
    render_pattern(req, buf.data());
    CHECK(fnv1a64(buf.data(), buf.size()) != m0);
}

TEST_CASE("cached still patterns match a fresh render") {
    const VideoFormat fmt = *parse_format("720p25");
    std::vector<std::uint8_t> buf(v210_size(fmt.width, fmt.height));
    PatternRequest req;
    req.format = fmt;
    req.pattern = VideoPattern::Bars75;
    render_pattern(req, buf.data());
    const auto bars = fnv1a64(buf.data(), buf.size());
    req.pluge = true;
    render_pattern(req, buf.data());
    const auto pluge = fnv1a64(buf.data(), buf.size());
    CHECK(pluge != bars);
    req.flash = true;
    render_pattern(req, buf.data());
    const auto flash = fnv1a64(buf.data(), buf.size());
    CHECK(flash != pluge);
    // Cache hits overwrite the whole grain and follow every key change.
    std::fill(buf.begin(), buf.end(), 0xAB);
    req.flash = false;
    render_pattern(req, buf.data());
    CHECK(fnv1a64(buf.data(), buf.size()) == pluge);
    req.pluge = false;
    render_pattern(req, buf.data());
    CHECK(fnv1a64(buf.data(), buf.size()) == bars);
    req.flash = true;
    render_pattern(req, buf.data());
    CHECK(fnv1a64(buf.data(), buf.size()) != bars);

    req.flash = false;
    req.pattern = VideoPattern::ZonePlateMoving;
    req.frame_index = 0;
    render_pattern(req, buf.data());
    const auto z0 = fnv1a64(buf.data(), buf.size());
    req.frame_index = 1;
    render_pattern(req, buf.data());
    CHECK(fnv1a64(buf.data(), buf.size()) != z0);
}

TEST_CASE("av sync flash lines up with beep sample index") {
    const Rational rate{50, 1};
    int flashes = 0;
    for (std::uint64_t frame = 0; frame < 100; ++frame) {
        if (!is_sync_frame(frame, rate)) continue;
        ++flashes;
        const auto sample = samples_until_grain(frame, rate);
        CHECK(sample / 960 == frame);
        AudioProgram prog = make_uniform_program(1, AudioSignal::Silence, 1000, -18);
        prog.sync_beep = true;
        std::vector<float> audio(960);
        render_audio(prog, sample, 960, true, audio.data());
        const double amp = goertzel_amplitude(audio.data(), 960, 1000.0, 48000.0);
        CHECK(amp > 0.05);
    }
    CHECK(flashes == 2);
}

TEST_CASE("sine level and ident cadence") {
    AudioProgram prog = make_uniform_program(1, AudioSignal::Sine, 1000, -18);
    std::vector<float> audio(4800);
    render_audio(prog, 0, 4800, false, audio.data());
    const double amp = goertzel_amplitude(audio.data(), 4800, 1000.0, 48000.0);
    CHECK(amp == doctest::Approx(dbfs_to_lin(-18)).epsilon(0.05));

    AudioProgram beeps = make_ident_beep_program(3, -18);
    std::vector<float> planar(3 * 48000);
    render_audio(beeps, 0, 48000, false, planar.data());
    auto bursts = [](const float* x, int n) {
        int count = 0;
        bool on = false;
        for (int i = 0; i < n; ++i) {
            const bool hot = std::fabs(x[i]) > 0.01f;
            if (hot && !on) ++count;
            on = hot;
        }
        return count;
    };
    CHECK(bursts(planar.data(), 48000) >= 1);
    CHECK(bursts(planar.data() + 48000, 48000) >= 2);
    CHECK(bursts(planar.data() + 2 * 48000, 48000) >= 3);
}

TEST_CASE("tones keep their exact phase at TAI sample indexes") {
    // About 2026 in TAI samples: sin(2π·f·t) with t in seconds would lose the phase here.
    const std::uint64_t start = 84'000'000'000'000ull + 12345;
    for (const auto& [freq, cycle_num, cycle_den] :
         {std::tuple<double, std::uint64_t, std::uint64_t>{1000.0, 1000, 48000}, {997.0, 997, 48000}, {997.5, 1995, 96000}}) {
        AudioProgram prog = make_uniform_program(1, AudioSignal::Sine, freq, 0);
        std::vector<float> audio(960);
        render_audio(prog, start, 960, false, audio.data());
        double worst = 0;
        for (int i = 0; i < 960; ++i) {
            // Exact phase in integers: frac(freq·s/48000) = (num·s mod den) / den.
            const std::uint64_t s = start + static_cast<std::uint64_t>(i);
            const auto p = static_cast<unsigned __int128>(cycle_num) * s % cycle_den;
            const double expected = std::sin(2.0 * 3.141592653589793 * static_cast<double>(p) / static_cast<double>(cycle_den));
            worst = std::max(worst, std::fabs(expected - static_cast<double>(audio[static_cast<std::size_t>(i)])));
        }
        CHECK(worst < 1e-6);
    }
}

TEST_CASE("drop frame timecode and anc roundtrip") {
    auto tc = frames_to_timecode(0, 30, true);
    CHECK(tc.format() == "00:00:00;00");
    // Real frame 1800 is displayed as 00:01:00;02 (frame numbers 00 and 01 are skipped).
    auto tc2 = frames_to_timecode(1800, 30, true);
    CHECK(tc2.ff == 2);
    CHECK(tc2.ss == 0);
    CHECK(tc2.mm == 1);
    CHECK(timecode_to_frames(tc2) == 1800);

    VideoFormat fmt = *parse_format("1080i29.97");
    TimecodeQuery q;
    q.source = TcSource::Free;
    q.drop_frame = true;
    q.free_start_frame = 1800;
    q.since_start_frame = 0;
    auto shown = timecode_for_grain(0, fmt, q);
    CHECK(shown.drop);
    CHECK(shown.field == 0);
    auto shown_f2 = timecode_for_grain(1, fmt, q);
    CHECK(shown_f2.field == 1);
    CHECK(shown_f2.format() == shown.format());

    AncPacket pkt;
    pkt.tc = shown_f2;
    pkt.kind = AtcKind::Vitc1;
    pkt.interlaced = true;
    pkt.line = 10;
    auto grain = encode_anc_grain(pkt);
    CHECK(grain.size() == 4096);
    Timecode back;
    AtcKind kind = AtcKind::Ltc;
    REQUIRE(decode_anc_timecode(grain.data(), grain.size(), back, kind));
    CHECK(kind == AtcKind::Vitc1);
    CHECK(back.hh == shown_f2.hh);
    CHECK(back.mm == shown_f2.mm);
    CHECK(back.ss == shown_f2.ss);
    CHECK(back.ff == shown_f2.ff);
    CHECK(back.drop);
    CHECK(back.field == 1);
}

TEST_CASE("interlaced ANC runs at the field rate: one grain per field at the video field's index") {
    const Uuid id = uuid_from_seed("anc-flow");
    auto rate = [&](const char* format) {
        const auto def = nlohmann::json::parse(data_flow_json(id, "Out 1 data", "Out 1", *parse_format(format), id, id));
        return Rational{def["grain_rate"]["numerator"].get<std::int64_t>(), def["grain_rate"]["denominator"].get<std::int64_t>()};
    };
    // Twice the frame rate the video flow declares (MXL doubles that one itself).
    CHECK(rate("1080i25") == Rational{50, 1});
    CHECK(rate("1080i25bff") == Rational{50, 1});
    CHECK(rate("1080i29.97") == Rational{60000, 1001});
    CHECK(rate("1080p25") == Rational{25, 1});  // progressive unchanged
    CHECK(rate("1080p59.94") == Rational{60000, 1001});

    auto field_of = [](const std::vector<std::uint8_t>& g) { return g[3] >> 6; };
    auto line_of = [](const std::vector<std::uint8_t>& g) {
        const std::uint32_t h = (std::uint32_t{g[6]} << 24) | (std::uint32_t{g[7]} << 16) | (std::uint32_t{g[8]} << 8) | g[9];
        return static_cast<int>((h >> 20) & 0x7FFu);
    };

    // Grains 2k and 2k+1 carry frame k's timecode; the second has the field flag, F = 0b11 and a field-2 line.
    const VideoFormat fmt = *parse_format("1080i25");
    TimecodeQuery q;
    q.source = TcSource::Media;
    q.media_start_frame = 100;
    const std::uint64_t k = 7;
    const auto g1 = encode_anc_grain(anc_packet_for_grain(2 * k, fmt, timecode_for_grain(2 * k, fmt, q), AtcKind::Ltc));
    const auto g2 = encode_anc_grain(anc_packet_for_grain(2 * k + 1, fmt, timecode_for_grain(2 * k + 1, fmt, q), AtcKind::Ltc));
    CHECK(field_of(g1) == 0x2);
    CHECK(field_of(g2) == 0x3);
    CHECK(line_of(g1) == 10);
    CHECK(line_of(g2) == 572);
    Timecode t1, t2;
    AtcKind kind = AtcKind::Ltc;
    REQUIRE(decode_anc_timecode(g1.data(), g1.size(), t1, kind));
    REQUIRE(decode_anc_timecode(g2.data(), g2.size(), t2, kind));
    CHECK(t1.format() == "00:00:04:07");
    CHECK(t2.format() == t1.format());
    CHECK(t1.field == 0);
    CHECK(t2.field == 1);

    // Progressive: F = 0b00, line 10.
    const VideoFormat p = *parse_format("1080p25");
    const auto gp = encode_anc_grain(anc_packet_for_grain(15, p, timecode_for_grain(15, p, q), AtcKind::Ltc));
    CHECK(field_of(gp) == 0);
    CHECK(line_of(gp) == 10);
}

TEST_CASE("ANC grain bytes are the RFC 8331 payload from the Length field on, as MXL stores it") {
    // Reference vectors: mxl-st2110-gateway codec::serialiseGrain (the gateway's ANC ingest and
    // egress codec, which follows MXL docs/Architecture.md and mxl-data-probe) for the same packet.
    // Length(16) ANC_Count(8) F(2) reserved(22), then the packet at byte 6.
    const std::vector<std::uint8_t> progressive = {
        0x00, 0x20, 0x01, 0x00, 0x00, 0x00, 0x00, 0xAF, 0xFE, 0x00, 0x98, 0x26, 0x04, 0x41, 0x20, 0x80, 0x11, 0x08, 0x02,
        0x00, 0x80, 0x23, 0x08, 0x02, 0x00, 0x80, 0x12, 0x08, 0x02, 0x00, 0x80, 0x11, 0x08, 0x02, 0x60, 0x00, 0x00, 0x00};
    const std::vector<std::uint8_t> field2 = {
        0x00, 0x20, 0x01, 0xC0, 0x00, 0x00, 0x23, 0xCF, 0xFE, 0x00, 0x98, 0x26, 0x04, 0x42, 0x28, 0x80, 0x11, 0x08, 0x02,
        0x00, 0x80, 0x23, 0x08, 0x02, 0x00, 0x80, 0x12, 0x08, 0x02, 0x00, 0x80, 0x29, 0x08, 0x02, 0xE8, 0x00, 0x00, 0x00};
    auto matches = [](const std::vector<std::uint8_t>& grain, const std::vector<std::uint8_t>& ref) {
        REQUIRE(grain.size() == kAncGrainSize);
        CHECK(std::vector<std::uint8_t>(grain.begin(), grain.begin() + ref.size()) == ref);
        CHECK(std::all_of(grain.begin() + ref.size(), grain.end(), [](std::uint8_t b) { return b == 0; }));
    };

    AncPacket a;
    a.tc.hh = 10;
    a.tc.mm = 20;
    a.tc.ss = 30;
    a.tc.ff = 12;
    a.line = 10;
    matches(encode_anc_grain(a), progressive);  // LTC, F = 0b00

    AncPacket b = a;
    b.interlaced = true;
    b.tc.field = 1;
    b.line = 572;
    b.kind = AtcKind::Vitc1;
    matches(encode_anc_grain(b), field2);  // VITC1, F = 0b11, line 572

    Timecode tc;
    AtcKind kind = AtcKind::Ltc;
    REQUIRE(decode_anc_timecode(field2.data(), field2.size(), tc, kind));
    CHECK(tc.format() == "10:20:30:12");
    CHECK(tc.field == 1);
    CHECK(kind == AtcKind::Vitc1);
}

TEST_CASE("moving box is a pure function of the grain index") {
    VideoFormat fmt = *parse_format("1080p50");
    MotionParams m;
    m.path = MotionPath::Bounce;
    m.speed = 0.2;
    m.size = 0.1;
    const auto a = moving_box_at(m, 1000, fmt);
    const auto b = moving_box_at(m, 1000, fmt);
    CHECK(a.x == b.x);
    CHECK(a.y == b.y);
    const auto c = moving_box_at(m, 1000 + 50, fmt);
    CHECK((c.x != a.x || c.y != a.y));
    MotionParams next = m;
    next.speed = 0.05;
    retarget_motion(next, m, 1000, fmt);
    const auto kept = moving_box_at(next, 1000, fmt);
    CHECK(std::abs(kept.x - a.x) <= 1);
    CHECK(std::abs(kept.y - a.y) <= 1);

    const double durs[] = {0.1, 0.1, 0.2};
    double total = 0;
    CHECK(sprite_frame_at_time(0.05, durs, 3, total) == 0);
    CHECK(sprite_frame_at_time(0.15, durs, 3, total) == 1);
    CHECK(sprite_frame_at_time(0.25, durs, 3, total) == 2);
    CHECK(total == doctest::Approx(0.4));
    // Same timestamp selects the same sprite frame on 50p and 59.94.
    const double t = 1.25;
    const auto f50 = sprite_frame_at_time(std::fmod(t, total), durs, 3, total);
    const auto f60 = sprite_frame_at_time(std::fmod(t, total), durs, 3, total);
    CHECK(f50 == f60);
}

TEST_CASE("playlist sequencing and placeholders") {
    std::vector<PlaylistEntry> e{{"a", 10, 2}, {"b", 5, 1}, {"c", 4, 0}};
    auto at0 = locate_playlist(e, 0);
    CHECK(at0.entry == 0);
    CHECK(at0.frame_in_item == 0);
    auto at15 = locate_playlist(e, 15);
    CHECK(at15.entry == 0);
    CHECK(at15.loop_index == 1);
    CHECK(at15.frame_in_item == 5);
    auto at20 = locate_playlist(e, 20);
    CHECK(at20.entry == 1);
    CHECK(at20.frame_in_item == 0);
    auto at25 = locate_playlist(e, 25);
    CHECK(at25.entry == 2);
    CHECK(at25.frame_in_item == 0);
    auto later = locate_playlist(e, 25 + 4 * 3 + 1);
    CHECK(later.entry == 2);
    CHECK(later.loop_index == 3);
    CHECK_FALSE(later.ended);

    PlaceholderVars v;
    v.label = "Out 1";
    v.timecode = "01:02:03:04";
    v.frame = "9";
    auto s = expand_placeholders("[{label}] {timecode} f{frame} {nope}", v);
    CHECK(s == "[Out 1] 01:02:03:04 f9 {nope}");
}

TEST_CASE("config env overrides file and ids are stable") {
    const char* env[] = {"PLAYER_FORMAT=720p50", "PLAYER_OUTPUTS=1", "PLAYER_AUDIO_CHANNELS=8", "PLAYER_LIBRARY_DIR=/tmp/lib-test",
                         "WEB_PORT=9", "NMOS_SEED=unit-player", "MXL_OUTPUT_DOMAIN_DIR=/tmp/mxl-unit", nullptr};
    // No file.
    auto cfg = load_config("/tmp/does-not-exist-player.json", env);
    CHECK(cfg.format.name == "720p50");
    CHECK(cfg.outputs == 1);
    CHECK(cfg.audio_channels == 8);
    CHECK(cfg.web_port == 9);
    CHECK(cfg.mxl_domain_dir == "/tmp/mxl-unit");
    CHECK(cfg.output_configs.size() == 1);

    const char* bad[] = {"PLAYER_OUTPUTS=99", "NMOS_SEED=x", nullptr};
    CHECK_THROWS_AS(load_config("/tmp/does-not-exist-player.json", bad), Error);

    auto node = node_id_from_seed("unit-player");
    auto a = derive_output_ids(node, 0, *parse_format("1080p50"), 16, false);
    auto b = derive_output_ids(node, 0, *parse_format("1080p50"), 16, false);
    auto c = derive_output_ids(node, 0, *parse_format("1080p25"), 16, false);
    CHECK(a.video_flow == b.video_flow);
    CHECK(a.video_flow != c.video_flow);
    CHECK(a.video_flow.str().size() == 36);
    // Source id does not include the format, so a source swap would not retarget it.
    CHECK(a.video_source == c.video_source);
    // Senders are stable ports: a new format, v210a or channel count keeps them, only the flow changes.
    auto d = derive_output_ids(node, 0, *parse_format("1080p50"), 16, true);
    auto e = derive_output_ids(node, 0, *parse_format("1080p50"), 8, false);
    CHECK(a.video_flow != d.video_flow);
    CHECK(a.audio_flow != e.audio_flow);
    for (const auto* x : {&c, &d, &e}) {
        CHECK(a.video_sender == x->video_sender);
        CHECK(a.audio_sender == x->audio_sender);
        CHECK(a.data_sender == x->data_sender);
        CHECK(a.key_sender == x->key_sender);
    }
    auto other = derive_output_ids(node, 1, *parse_format("1080p50"), 16, false);
    CHECK(a.video_sender != other.video_sender);
    CHECK(a.video_sender != a.audio_sender);
}

TEST_CASE("platform settings, aliases and announce address") {
    const char* env[] = {"NMOS_SEED=sport-player",
                         "HOST_ID=should-not-win",
                         "NMOS_LABEL=Sport",
                         "NMOS_TAGS={\"urn:x-srf:production\":[\"sport-sa\"],\"urn:x-srf:function\":[\"player1\"]}",
                         "NMOS_REGISTRY_ADDRESS=10.0.0.5",
                         "NMOS_REGISTRY_PORT=4000",
                         "NMOS_HOST_ADDRESS=10.1.2.3",
                         "CONFIG_DIR=/tmp/player-cfg",
                         "MXL_DOMAIN_SCAN_PATH=/tmp/mxl-root",
                         "MXL_HISTORY_DURATION_NS=5000000000",
                         "MXL_CLEANUP_ON_EXIT=true",
                         "SHUTDOWN_TIMEOUT_S=12",
                         "PLAYER_FORMAT=1080p50",
                         "PLAYER_OUTPUTS=1",
                         nullptr};
    auto cfg = load_config("", env);
    CHECK(cfg.nmos_seed == "sport-player");
    CHECK(cfg.nmos_label == "Sport");
    CHECK(cfg.nmos_tags["urn:x-srf:production"][0] == "sport-sa");
    CHECK(cfg.nmos_query_address == "10.0.0.5");
    CHECK(cfg.nmos_query_port == 4001);
    CHECK(cfg.nmos_host_address == "10.1.2.3");
    CHECK(cfg.config_dir == "/tmp/player-cfg");
    CHECK(cfg.config_path == "/tmp/player-cfg/player.json");
    CHECK(cfg.state_path == "/tmp/player-cfg/state.json");
    CHECK(cfg.mxl_domain_dir == "/tmp/mxl-root/player-sport-player");
    CHECK(cfg.history_duration_ns == 5000000000ull);
    CHECK(cfg.mxl_cleanup_on_exit);
    CHECK(cfg.shutdown_timeout_s == 12);
    CHECK_FALSE(cfg.nmos_dns_sd);

    const char* alias[] = {"NMOS_SEED=alias-player", "NMOS_HOST_ADDRESS=10.9.8.7", "PLAYER_STATE=/tmp/legacy-state.json", "PLAYER_FORMAT=1080p50",
                           "PLAYER_OUTPUTS=1", nullptr};
    auto aliased = load_config("", alias);
    CHECK(aliased.state_path == "/tmp/legacy-state.json");

    // Neither NMOS_SEED nor HOST_ID: a random seed per CONFIG_DIR, kept there, so two players differ.
    const auto seed_a = std::filesystem::temp_directory_path() / ("mtp-seed-a-" + std::to_string(::getpid()));
    const auto seed_b = std::filesystem::temp_directory_path() / ("mtp-seed-b-" + std::to_string(::getpid()));
    const std::string dir_a = "CONFIG_DIR=" + seed_a.string(), dir_b = "CONFIG_DIR=" + seed_b.string();
    const char* no_seed_a[] = {dir_a.c_str(), "NMOS_HOST_ADDRESS=10.9.8.7", "PLAYER_FORMAT=1080p50", "PLAYER_OUTPUTS=1", nullptr};
    const char* no_seed_b[] = {dir_b.c_str(), "NMOS_HOST_ADDRESS=10.9.8.7", "PLAYER_FORMAT=1080p50", "PLAYER_OUTPUTS=1", nullptr};
    const auto first = load_config("", no_seed_a).nmos_seed;
    CHECK(first.rfind("player-", 0) == 0);
    CHECK(first.size() == 23);
    CHECK(load_config("", no_seed_a).nmos_seed == first);
    CHECK(load_config("", no_seed_b).nmos_seed != first);
    std::filesystem::remove_all(seed_a);
    std::filesystem::remove_all(seed_b);

    const char* host_id[] = {"HOST_ID=box-a", "NMOS_HOST_ADDRESS=10.9.8.7", "PLAYER_FORMAT=1080p50", "PLAYER_OUTPUTS=1", nullptr};
    CHECK(load_config("", host_id).nmos_seed == "box-a-player");

    const char* bad_host[] = {"NMOS_HOST_ADDRESS=player-pod", "NMOS_SEED=x", "PLAYER_FORMAT=1080p50", "PLAYER_OUTPUTS=1", nullptr};
    CHECK_THROWS_AS(load_config("", bad_host), ConfigError);
    const char* loop[] = {"NMOS_HOST_ADDRESS=127.0.0.1", "NMOS_SEED=x", "PLAYER_FORMAT=1080p50", "PLAYER_OUTPUTS=1", nullptr};
    CHECK_THROWS_AS(load_config("", loop), ConfigError);
    const char* dns[] = {"NMOS_DNS_SD=true", "NMOS_HOST_ADDRESS=10.1.2.3", "NMOS_SEED=x", "PLAYER_FORMAT=1080p50", "PLAYER_OUTPUTS=1", nullptr};
    CHECK_THROWS_AS(load_config("", dns), ConfigError);

    auto detected = detect_announce_address();
    CHECK(ipv4_literal(detected));
    CHECK(detected.rfind("127.", 0) != 0);
}

TEST_CASE("a playlist source keeps its entries through the state file") {
    const auto j = nlohmann::json::parse(R"({"type": "playlist", "playlist_id": "pl", "item_id": "a",
        "entries": [{"item_id": "a", "loops": 2, "frames": 300}, {"item_id": "b", "loops": 0, "frames": 1}]})");
    const auto back = source_from_json(source_to_json(source_from_json(j)));
    REQUIRE(back.entries.size() == 2);
    CHECK(back.entries[0].item_id == "a");
    CHECK(back.entries[0].loops == 2);
    CHECK(back.entries[0].frames == 300);
    CHECK(back.entries[1].loops == 0);
    CHECK(back.playlist_id == "pl");
}

TEST_CASE("every setting lists its value and origin") {
    const auto file = std::filesystem::path("/tmp/player-origin-unit.json");
    {
        std::ofstream f(file);
        f << R"({"format": "720p25", "nmos_label": "From file", "outputs_count": 3})";
    }
    const char* env[] = {"PLAYER_OUTPUTS=2", "NMOS_HOST_ADDRESS=10.1.2.3", "NMOS_SEED=origin-player", nullptr};
    const auto cfg = load_config(file.string(), env);
    auto find = [&](const std::string& key) {
        for (const auto& s : cfg.settings)
            if (s.key == key) return s;
        return SettingOrigin{key, "", "missing"};
    };
    CHECK(find("PLAYER_OUTPUTS").value == "2");
    CHECK(find("PLAYER_OUTPUTS").source == "environment");  // the environment wins over the file's 3
    CHECK(find("PLAYER_FORMAT").value == "720p25");
    CHECK(find("PLAYER_FORMAT").source == "file");
    CHECK(find("NMOS_LABEL").value == "From file");
    CHECK(find("NMOS_LABEL").source == "file");
    CHECK(find("PLAYER_AUDIO_CHANNELS").value == "16");
    CHECK(find("PLAYER_AUDIO_CHANNELS").source == "default");
    CHECK(find("PLAYER_RAM_CLIP_MAX_S").value == "20");
    CHECK(find("PLAYER_UPLOAD_LIMIT_GB").value == "20");
    CHECK(find("PLAYER_CONFIG").value == file.string());
    CHECK(find("PLAYER_CONFIG").source == "argument");
    CHECK(find("NMOS_QUERY_PORT").value == "3211");
    CHECK(find("NMOS_TAGS").value == "{}");
    CHECK(find("WEB_PORT").source == "default");
    for (const auto& s : cfg.settings) {
        INFO(s.key);
        CHECK((s.source == "environment" || s.source == "file" || s.source == "argument" || s.source == "default"));
    }
    std::filesystem::remove(file);
}

TEST_CASE("WIDGET_FRAME_ANCESTORS defaults to 'self' and must be one source list") {
    const char* plain[] = {"NMOS_SEED=widget-player", "NMOS_HOST_ADDRESS=10.1.2.3", nullptr};
    auto cfg = load_config("", plain);
    CHECK(cfg.widget_frame_ancestors == "'self'");
    const char* set[] = {"NMOS_SEED=widget-player", "NMOS_HOST_ADDRESS=10.1.2.3", "WIDGET_FRAME_ANCESTORS='self' https://designer.example", nullptr};
    cfg = load_config("", set);
    CHECK(cfg.widget_frame_ancestors == "'self' https://designer.example");
    bool listed = false;
    for (const auto& s : cfg.settings)
        if (s.key == "WIDGET_FRAME_ANCESTORS") listed = s.value == "'self' https://designer.example" && s.source == "environment";
    CHECK(listed);
    const char* semicolon[] = {"NMOS_SEED=widget-player", "NMOS_HOST_ADDRESS=10.1.2.3", "WIDGET_FRAME_ANCESTORS='self'; script-src *", nullptr};
    CHECK_THROWS_AS(load_config("", semicolon), Error);
    const char* comma[] = {"NMOS_SEED=widget-player", "NMOS_HOST_ADDRESS=10.1.2.3", "WIDGET_FRAME_ANCESTORS=https://a.example, https://b.example", nullptr};
    CHECK_THROWS_AS(load_config("", comma), Error);
}

TEST_CASE("widgets: the list with CORS for listed origins, the page with CSP frame-ancestors") {
    Config cfg;
    cfg.outputs = 2;
    cfg.widget_frame_ancestors = "'self' https://designer.example";
    auto get = [&](const std::string& path, const std::string& query = "", const std::string& origin = "", const std::string& method = "GET") {
        HttpRequest req;
        req.method = method;
        req.path = path;
        req.query = query;
        if (!origin.empty()) req.headers["origin"] = origin;
        HttpResponse res;
        widget_response(cfg, req, "<!doctype html><title>ui</title>", res);
        return res;
    };
    auto header = [](const HttpResponse& res, const std::string& key) { return res.headers.count(key) ? res.headers.at(key) : std::string("(none)"); };

    auto list = get("/widgets", "", "https://designer.example");
    CHECK(list.status == 200);
    CHECK_FALSE(list.cors);
    CHECK(header(list, "Access-Control-Allow-Origin") == "https://designer.example");
    CHECK(header(list, "Vary") == "Origin");
    const auto body = nlohmann::json::parse(std::string(list.body.begin(), list.body.end()));
    REQUIRE(body.is_array());
    REQUIRE(body.size() == 1);
    CHECK(body[0]["id"] == "transport");
    CHECK(body[0]["min_size"] == nlohmann::json{{"w", 420}, {"h", 160}});
    CHECK(body[0]["params"]["properties"]["output"]["minimum"] == 1);
    CHECK(body[0]["params"]["properties"]["output"]["maximum"] == 2);
    CHECK(body[0]["params"]["required"] == nlohmann::json::array({"output"}));
    CHECK_FALSE(body[0]["version"].get<std::string>().empty());
    auto other = get("/widgets", "", "https://evil.example");
    CHECK(other.status == 200);
    CHECK(header(other, "Access-Control-Allow-Origin") == "(none)");
    CHECK(header(other, "Vary") == "Origin");
    CHECK(header(get("/widgets"), "Access-Control-Allow-Origin") == "(none)");
    auto post = get("/widgets", "", "", "POST");
    CHECK(post.status == 405);
    CHECK(header(post, "Allow") == "GET");
    CHECK(get("/widgets", "", "", "OPTIONS").status == 405);
    CHECK(get("/widget/transport", "output=1", "", "POST").status == 405);

    auto page = get("/widget/transport", "output=2&theme=transparent");
    CHECK(page.status == 200);
    CHECK_FALSE(page.cors);
    CHECK(page.content_type.rfind("text/html", 0) == 0);
    CHECK(std::string(page.body.begin(), page.body.end()) == "<!doctype html><title>ui</title>");
    CHECK(header(page, "Content-Security-Policy") == "frame-ancestors 'self' https://designer.example");
    CHECK(header(page, "X-Frame-Options") == "(none)");
    CHECK(get("/widget/transport", "theme=light&output=1").status == 200);
    CHECK(get("/widget/transport").status == 400);
    CHECK(get("/widget/transport", "output=0").status == 400);
    CHECK(get("/widget/transport", "output=3").status == 400);
    CHECK(get("/widget/transport", "output=x").status == 400);
    CHECK(get("/widget/transport", "output=1&theme=pink").status == 400);
    CHECK(get("/widget/clips", "output=1").status == 404);
    CHECK(widget_path("/widgets"));
    CHECK(widget_path("/widget/transport"));
    CHECK_FALSE(widget_path("/api/v1/outputs"));
}

TEST_CASE("output domain is created once and not overwritten") {
    const auto dir = std::filesystem::path("/tmp/mxl-domain-unit");
    std::filesystem::remove_all(dir);
    ensure_output_domain(dir.string(), "11111111-1111-5111-8111-111111111111", 1000000000ull);
    const auto def = dir / "domain_def.json";
    const auto opt = dir / "options.json";
    CHECK(std::filesystem::exists(def));
    {
        // BCP-007-03 schema: id, label, description and tags are required.
        std::ifstream def_in(def);
        nlohmann::json d;
        def_in >> d;
        CHECK(d["id"] == "11111111-1111-5111-8111-111111111111");
        CHECK(d["label"].is_string());
        CHECK(d["description"].is_string());
        CHECK(d["tags"].is_object());
    }
    const auto first = std::filesystem::last_write_time(def);
    const auto opt_first = std::filesystem::last_write_time(opt);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ensure_output_domain(dir.string(), "11111111-1111-5111-8111-111111111111", 42);
    CHECK(std::filesystem::last_write_time(def) == first);
    CHECK(std::filesystem::last_write_time(opt) == opt_first);
    std::ifstream in(opt);
    nlohmann::json j;
    in >> j;
    CHECK(j["urn:x-mxl:option:history_duration/v1.0"] == 1000000000ull);
    // Another id is logged and kept, not overwritten.
    CHECK(ensure_output_domain(dir.string(), "22222222-2222-5222-8222-222222222222", 1000000000ull) ==
          "11111111-1111-5111-8111-111111111111");
    CHECK(std::filesystem::last_write_time(def) == first);
    std::filesystem::remove_all(dir);
}

TEST_CASE("stop_children does not wait out a long job") {
    const auto start = std::chrono::steady_clock::now();
    std::thread worker([] { run_process({"/bin/sleep", "30"}); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop_children();
    worker.join();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    CHECK(ms < 5000);
}

TEST_CASE("NMOS resources carry the fields IS-04 v1.3 requires") {
    const auto node = node_id_from_seed("nmos-unit");
    const auto ids = derive_output_ids(node, 0, *parse_format("1080p50"), 16, false);
    const auto fmt = *parse_format("1080p50");
    NmosModel m;
    m.node_id = node.str();
    m.device_id = ids.device.str();
    m.host = "10.1.2.3";
    auto sender = [&](const Uuid& id, const Uuid& source, const char* format, const std::string& def) {
        NmosSenderState s;
        s.id = id.str();
        s.source_id = source.str();
        s.label = s.description = "Out 1";
        s.format = format;
        s.flow = nlohmann::json::parse(def);
        s.flow_id = s.flow.at("id").get<std::string>();
        return s;
    };
    const auto video = sender(ids.video_sender, ids.video_source, "urn:x-nmos:format:video",
                              video_flow_json(ids.video_flow, "Out 1", "Out 1", "Video", fmt, false, ids.video_source, ids.device));
    const auto audio = sender(ids.audio_sender, ids.audio_source, "urn:x-nmos:format:audio",
                              audio_flow_json(ids.audio_flow, "Out 1 audio", "Out 1", 16, ids.audio_source, ids.device));
    const std::string v = "1:0";
    // resource_core.json
    auto core = [](const nlohmann::json& j) {
        for (const char* k : {"id", "version", "label", "description", "tags"}) CHECK_MESSAGE(j.contains(k), k);
    };
    const auto n = nmos_node_json(m, v);
    core(n);
    for (const char* k : {"href", "api", "caps", "services", "clocks", "interfaces"}) CHECK_MESSAGE(n.contains(k), k);
    const auto d = nmos_device_json(m, v);
    core(d);
    for (const char* k : {"type", "node_id", "senders", "receivers", "controls"}) CHECK_MESSAGE(d.contains(k), k);
    for (const auto* s : {&video, &audio}) {
        const auto src = nmos_source_json(m, *s, v);
        core(src);
        for (const char* k : {"caps", "device_id", "parents", "clock_name", "format"}) CHECK_MESSAGE(src.contains(k), k);
        const auto snd = nmos_sender_json(m, *s, v);
        core(snd);
        for (const char* k : {"flow_id", "transport", "device_id", "manifest_href", "interface_bindings", "subscription"}) CHECK_MESSAGE(snd.contains(k), k);
        core(nmos_flow_json(m, *s, v));
    }
    CHECK(nmos_source_json(m, audio, v).at("channels").size() == 16);
    const auto vf = nmos_flow_json(m, video, v);
    for (const char* k : {"frame_width", "frame_height", "colorspace", "source_id", "device_id", "parents"}) CHECK_MESSAGE(vf.contains(k), k);
    CHECK(vf.at("version") == v);
    CHECK(nmos_flow_json(m, audio, v).contains("sample_rate"));
}

TEST_CASE("a format change deletes the old flow and keeps the sender") {
    const auto node = node_id_from_seed("stale-unit");
    auto model = [&](const OutputIds& ids, bool anc) {
        NmosModel m;
        m.node_id = node.str();
        auto push = [&](const Uuid& sender, const Uuid& flow, const Uuid& source) {
            NmosSenderState s;
            s.id = sender.str();
            s.flow_id = flow.str();
            s.source_id = source.str();
            m.senders.push_back(s);
        };
        push(ids.video_sender, ids.video_flow, ids.video_source);
        push(ids.audio_sender, ids.audio_flow, ids.audio_source);
        if (anc) push(ids.data_sender, ids.data_flow, ids.data_source);
        return m;
    };
    const auto v210 = derive_output_ids(node, 1, *parse_format("1080p50"), 16, false);
    const auto v210a = derive_output_ids(node, 1, *parse_format("1080p50"), 16, true);
    const auto before = model(v210, true);
    const auto registered = nmos_resources(before);
    CHECK(registered.size() == 9);
    CHECK(nmos_stale_resources(registered, before).empty());

    // v210 -> v210a: one new video flow; the old one goes, the sender and source stay.
    const auto after = model(v210a, true);
    const auto stale = nmos_stale_resources(registered, after);
    REQUIRE(stale.size() == 1);
    CHECK(stale[0] == std::make_pair(std::string("flows"), v210.video_flow.str()));
    const auto now = nmos_resources(after);
    CHECK(now.at(v210.video_sender.str()) == "senders");
    CHECK(now.at(v210a.video_flow.str()) == "flows");

    // ANC off: its sender, flow and source go, the sender first.
    const auto no_anc = nmos_stale_resources(registered, model(v210, false));
    REQUIRE(no_anc.size() == 3);
    CHECK(no_anc[0] == std::make_pair(std::string("senders"), v210.data_sender.str()));
    CHECK(no_anc[1] == std::make_pair(std::string("flows"), v210.data_flow.str()));
    CHECK(no_anc[2] == std::make_pair(std::string("sources"), v210.data_source.str()));
}
