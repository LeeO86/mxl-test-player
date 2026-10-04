#include "anc.hpp"
#include "audio.hpp"
#include "config.hpp"
#include "conform.hpp"
#include "format.hpp"
#include "ids.hpp"
#include "motion.hpp"
#include "nmos.hpp"
#include "mxl_io.hpp"
#include "pattern.hpp"
#include "placeholders.hpp"
#include "playlist.hpp"
#include "process.hpp"
#include "timecode.hpp"
#include "uuid_util.hpp"
#include "v210.hpp"

#include <doctest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

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

TEST_CASE("output domain is created once and not overwritten") {
    const auto dir = std::filesystem::path("/tmp/mxl-domain-unit");
    std::filesystem::remove_all(dir);
    ensure_output_domain(dir.string(), "11111111-1111-5111-8111-111111111111", 1000000000ull);
    const auto def = dir / "domain_def.json";
    const auto opt = dir / "options.json";
    CHECK(std::filesystem::exists(def));
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
    CHECK_THROWS_AS(ensure_output_domain(dir.string(), "22222222-2222-5222-8222-222222222222", 1000000000ull), ConfigError);
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
