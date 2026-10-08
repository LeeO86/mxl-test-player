#pragma once

#include "audio.hpp"
#include "format.hpp"
#include "motion.hpp"
#include "pattern.hpp"
#include "timecode.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace mtp {

enum class KeyMode { Off, FillKey, V210a };
enum class AlphaMode { Straight, Premultiplied };
enum class FitMode { Fit, Fill, Center };
enum class FpsMode { RepeatDrop, Motion };
enum class PauseAudio { Silence, HoldTone };
enum class IdleKey { Opaque, Transparent };

KeyMode parse_key_mode(std::string_view s);
const char* key_mode_name(KeyMode m);

struct TextLayerConfig {
    std::string text;
    std::string anchor = "bl";  // tl tc tr ml mc mr bl bc br
    double offset_x = 0;        // percent of frame
    double offset_y = 0;
    double size = 4;            // percent of frame height
    std::string font = "DejaVu Sans";
    std::string color = "#FFFFFF";
    double opacity = 1;
    bool box = false;
    std::string box_color = "#000000";
    double box_opacity = 0.5;
    double box_padding = 0.4;  // percent of frame height
    bool outline = false;
    std::string outline_color = "#000000";
    bool shadow = false;
};

struct MovingBoxConfig {
    bool enabled = false;
    std::string content = "builtin";  // "builtin" or a library item id
    MotionParams motion;
    std::string color = "#FFCC00";
    bool include_in_key = false;
};

struct BurnInConfig {
    bool label = true;
    std::string label_anchor = "tl";
    bool timecode = true;
    bool utc_clock = false;
    bool local_clock = false;
    bool frame_counter = true;
    bool frame_counter_absolute = true;
    bool item = true;
    bool flow_id = true;
    bool on_keyed_stills = false;
    std::vector<TextLayerConfig> texts;
    std::vector<MovingBoxConfig> boxes;  // up to 2
};

BurnInConfig burnin_from_json(const nlohmann::json& j);
nlohmann::json burnin_to_json(const BurnInConfig& b);

struct OutputConfig {
    std::string label;
    std::string format;  // empty = platform format
    int audio_channels = 0;  // 0 = platform default
    bool anc = true;
    TcSource tc_source = TcSource::Tai;
    bool drop_frame = true;
    AtcKind atc = AtcKind::Ltc;
    std::int64_t free_start_frame = 0;
    KeyMode key_mode = KeyMode::Off;
    int key_min = 64;
    int key_max = 940;
    IdleKey idle_key = IdleKey::Opaque;
    PauseAudio pause_audio = PauseAudio::Silence;
    bool loop = true;
    BurnInConfig burnin;
};

// One effective setting and where it came from: "environment", "file", "argument" (--config)
// or "default". Listed by GET /api/v1/config.
struct SettingOrigin {
    std::string key;
    std::string value;
    std::string source;
};

struct Config {
    VideoFormat format;
    int outputs = 2;
    int audio_channels = 16;
    std::string library_dir = "/data/library";
    std::string import_dir;
    int convert_concurrency = 1;
    double ram_clip_max_s = 20;
    int ram_budget_mb = 4096;
    int preroll_frames = 25;
    std::string config_dir = "/config";
    std::string mxl_scan_path = "/Volumes/mxl";
    std::string mxl_domain_dir;
    std::string mxl_domain_id;
    std::uint64_t history_duration_ns = 1000000000ull;
    bool mxl_cleanup_on_exit = false;
    std::string nmos_registry_address;
    int nmos_registry_port = 3210;
    std::string nmos_query_address;
    int nmos_query_port = 3211;
    bool nmos_dns_sd = false;
    int nmos_port = 3282;
    std::string nmos_seed;
    std::string nmos_label = "MXL Test Player";
    nlohmann::json nmos_tags = nlohmann::json::object();
    std::string nmos_host_address;
    int shutdown_timeout_s = 10;
    int web_port = 8130;
    std::string config_path = "/config/player.json";
    std::string state_path = "/config/state.json";
    std::string font_dir;
    std::string web_root;
    std::uint64_t upload_limit_bytes = 20ull << 30;
    int sprite_max_px = 512;
    std::vector<OutputConfig> output_configs;
    std::vector<SettingOrigin> settings;  // every setting in README order
};

// env > file > defaults. Throws Error on invalid values (caller exits 78).
Config load_config(const std::string& file_path, const char* const* envp);

std::string default_domain_dir(const std::string& seed, const std::string& scan_path = "/Volumes/mxl");
bool ipv4_literal(std::string_view s);
std::string detect_announce_address();
void require_announce_address(const std::string& ip);

}  // namespace mtp
