#include "config.hpp"

#include "util.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <map>
#include <sstream>
#include <utility>

namespace mtp {
namespace {

using json = nlohmann::json;

std::string env_get(const std::map<std::string, std::string>& env, const char* key) {
    auto it = env.find(key);
    if (it == env.end()) return {};
    return it->second;
}

int parse_int(const std::string& s, const char* key) {
    try {
        std::size_t idx = 0;
        const int v = std::stoi(s, &idx);
        if (idx != s.size()) throw ConfigError(std::string("invalid integer for ") + key);
        return v;
    } catch (const Error&) {
        throw;
    } catch (...) {
        throw ConfigError(std::string("invalid integer for ") + key);
    }
}

double parse_double(const std::string& s, const char* key) {
    try {
        std::size_t idx = 0;
        const double v = std::stod(s, &idx);
        if (idx != s.size()) throw ConfigError(std::string("invalid number for ") + key);
        return v;
    } catch (const Error&) {
        throw;
    } catch (...) {
        throw ConfigError(std::string("invalid number for ") + key);
    }
}

bool parse_bool(const std::string& s, const char* key) {
    if (s == "1" || s == "true" || s == "TRUE" || s == "yes" || s == "on") return true;
    if (s == "0" || s == "false" || s == "FALSE" || s == "no" || s == "off") return false;
    throw ConfigError(std::string("invalid boolean for ") + key);
}

nlohmann::json text_to_json_local(const TextLayerConfig& t) {
    return json{{"text", t.text},
                {"anchor", t.anchor},
                {"offset_x", t.offset_x},
                {"offset_y", t.offset_y},
                {"size", t.size},
                {"font", t.font},
                {"color", t.color},
                {"opacity", t.opacity},
                {"box", t.box},
                {"box_color", t.box_color},
                {"box_opacity", t.box_opacity},
                {"box_padding", t.box_padding},
                {"outline", t.outline},
                {"outline_color", t.outline_color},
                {"shadow", t.shadow}};
}

TextLayerConfig text_from_json(const json& j) {
    TextLayerConfig t;
    if (j.contains("text")) t.text = j.at("text").get<std::string>();
    if (j.contains("anchor")) t.anchor = j.at("anchor").get<std::string>();
    if (j.contains("offset_x")) t.offset_x = j.at("offset_x").get<double>();
    if (j.contains("offset_y")) t.offset_y = j.at("offset_y").get<double>();
    if (j.contains("size")) t.size = j.at("size").get<double>();
    if (j.contains("font")) t.font = j.at("font").get<std::string>();
    if (j.contains("color")) t.color = j.at("color").get<std::string>();
    if (j.contains("opacity")) t.opacity = j.at("opacity").get<double>();
    if (j.contains("box")) t.box = j.at("box").get<bool>();
    if (j.contains("box_color")) t.box_color = j.at("box_color").get<std::string>();
    if (j.contains("box_opacity")) t.box_opacity = j.at("box_opacity").get<double>();
    if (j.contains("box_padding")) t.box_padding = j.at("box_padding").get<double>();
    if (j.contains("outline")) t.outline = j.at("outline").get<bool>();
    if (j.contains("outline_color")) t.outline_color = j.at("outline_color").get<std::string>();
    if (j.contains("shadow")) t.shadow = j.at("shadow").get<bool>();
    return t;
}

MovingBoxConfig box_from_json(const json& j) {
    MovingBoxConfig b;
    if (j.contains("enabled")) b.enabled = j.at("enabled").get<bool>();
    if (j.contains("content")) b.content = j.at("content").get<std::string>();
    if (j.contains("color")) b.color = j.at("color").get<std::string>();
    if (j.contains("include_in_key")) b.include_in_key = j.at("include_in_key").get<bool>();
    if (j.contains("path")) b.motion.path = parse_motion_path(j.at("path").get<std::string>());
    if (j.contains("speed")) b.motion.speed = j.at("speed").get<double>();
    if (j.contains("size")) b.motion.size = j.at("size").get<double>();
    if (j.contains("opacity")) b.motion.opacity = j.at("opacity").get<double>();
    if (j.contains("start_x")) b.motion.start_x = j.at("start_x").get<double>();
    if (j.contains("start_y")) b.motion.start_y = j.at("start_y").get<double>();
    return b;
}

BurnInConfig parse_burnin(const json& j) {
    BurnInConfig b;
    if (j.contains("label")) b.label = j.at("label").get<bool>();
    if (j.contains("label_anchor")) b.label_anchor = j.at("label_anchor").get<std::string>();
    if (j.contains("timecode")) b.timecode = j.at("timecode").get<bool>();
    if (j.contains("utc_clock")) b.utc_clock = j.at("utc_clock").get<bool>();
    if (j.contains("local_clock")) b.local_clock = j.at("local_clock").get<bool>();
    if (j.contains("frame_counter")) b.frame_counter = j.at("frame_counter").get<bool>();
    if (j.contains("frame_counter_absolute")) b.frame_counter_absolute = j.at("frame_counter_absolute").get<bool>();
    if (j.contains("item")) b.item = j.at("item").get<bool>();
    if (j.contains("flow_id")) b.flow_id = j.at("flow_id").get<bool>();
    if (j.contains("on_keyed_stills")) b.on_keyed_stills = j.at("on_keyed_stills").get<bool>();
    if (j.contains("texts")) {
        for (const auto& t : j.at("texts")) b.texts.push_back(text_from_json(t));
    }
    if (j.contains("boxes")) {
        for (const auto& t : j.at("boxes")) b.boxes.push_back(box_from_json(t));
    }
    if (b.texts.size() > 8) throw ConfigError("at most 8 text layers");
    if (b.boxes.size() > 2) throw ConfigError("at most 2 moving boxes");
    return b;
}

OutputConfig output_from_json(const json& j, int index) {
    OutputConfig o;
    o.label = j.value("label", "Out " + std::to_string(index + 1));
    if (j.contains("format") && !j.at("format").is_null()) o.format = j.at("format").get<std::string>();
    if (j.contains("audio_channels")) o.audio_channels = j.at("audio_channels").get<int>();
    if (j.contains("anc")) o.anc = j.at("anc").get<bool>();
    if (j.contains("tc_source")) o.tc_source = parse_tc_source(j.at("tc_source").get<std::string>());
    if (j.contains("drop_frame")) o.drop_frame = j.at("drop_frame").get<bool>();
    if (j.contains("atc")) o.atc = parse_atc_kind(j.at("atc").get<std::string>());
    if (j.contains("free_start_frame")) o.free_start_frame = j.at("free_start_frame").get<std::int64_t>();
    if (j.contains("key_mode")) o.key_mode = parse_key_mode(j.at("key_mode").get<std::string>());
    if (j.contains("key_min")) o.key_min = j.at("key_min").get<int>();
    if (j.contains("key_max")) o.key_max = j.at("key_max").get<int>();
    if (j.contains("idle_key")) {
        const auto s = j.at("idle_key").get<std::string>();
        o.idle_key = (s == "transparent") ? IdleKey::Transparent : IdleKey::Opaque;
    }
    if (j.contains("pause_audio")) {
        const auto s = j.at("pause_audio").get<std::string>();
        o.pause_audio = (s == "hold" || s == "hold_tone") ? PauseAudio::HoldTone : PauseAudio::Silence;
    }
    if (j.contains("loop")) o.loop = j.at("loop").get<bool>();
    if (j.contains("burnin")) o.burnin = parse_burnin(j.at("burnin"));
    return o;
}

void validate(Config& c) {
    if (c.outputs < 1 || c.outputs > 16) throw ConfigError("PLAYER_OUTPUTS must be 1..16");
    if (c.audio_channels < 2 || c.audio_channels > 64) throw ConfigError("PLAYER_AUDIO_CHANNELS must be 2..64");
    if (c.convert_concurrency < 1 || c.convert_concurrency > 8) throw ConfigError("PLAYER_CONVERT_CONCURRENCY must be 1..8");
    if (c.ram_clip_max_s < 0 || c.ram_clip_max_s > 600) throw ConfigError("PLAYER_RAM_CLIP_MAX_S out of range");
    if (c.ram_budget_mb < 64) throw ConfigError("PLAYER_RAM_BUDGET_MB too small");
    if (c.preroll_frames < 1 || c.preroll_frames > 250) throw ConfigError("PLAYER_PREROLL_FRAMES out of range");
    if (c.web_port < 1 || c.web_port > 65535) throw ConfigError("WEB_PORT out of range");
    if (c.nmos_port < 1 || c.nmos_port > 65535) throw ConfigError("NMOS_PORT out of range");
    if (c.nmos_registry_port < 1 || c.nmos_registry_port > 65535) throw ConfigError("NMOS_REGISTRY_PORT out of range");
    if (c.nmos_query_port < 1 || c.nmos_query_port > 65535) throw ConfigError("NMOS_QUERY_PORT out of range");
    if (c.shutdown_timeout_s < 1 || c.shutdown_timeout_s > 600) throw ConfigError("SHUTDOWN_TIMEOUT_S out of range");
    if (c.history_duration_ns < 1000000ull) throw ConfigError("MXL_HISTORY_DURATION_NS too small");
    if (c.output_configs.size() > static_cast<std::size_t>(c.outputs)) c.output_configs.resize(c.outputs);
    while (c.output_configs.size() < static_cast<std::size_t>(c.outputs)) {
        OutputConfig o;
        o.label = "Out " + std::to_string(c.output_configs.size() + 1);
        c.output_configs.push_back(o);
    }
    for (auto& o : c.output_configs) {
        if (o.audio_channels == 0) o.audio_channels = c.audio_channels;
        if (o.audio_channels < 2 || o.audio_channels > 64) throw ConfigError("output audio_channels must be 2..64");
        if (!o.format.empty() && !parse_format(o.format)) throw ConfigError("invalid output format " + o.format);
        if (o.label.empty()) throw ConfigError("output label must not be empty");
        if (o.burnin.texts.size() > 8) throw ConfigError("at most 8 text layers");
        if (o.burnin.boxes.size() > 2) throw ConfigError("at most 2 moving boxes");
        if (o.key_min < 0 || o.key_max > 1023 || o.key_min >= o.key_max) throw ConfigError("invalid key range");
    }
    if (c.mxl_domain_dir.empty()) c.mxl_domain_dir = default_domain_dir(c.nmos_seed, c.mxl_scan_path);
}

}  // namespace

BurnInConfig burnin_from_json(const nlohmann::json& j) { return parse_burnin(j); }

nlohmann::json burnin_to_json(const BurnInConfig& b) {
    nlohmann::json texts = nlohmann::json::array();
    for (const auto& t : b.texts) texts.push_back(text_to_json_local(t));
    nlohmann::json boxes = nlohmann::json::array();
    for (const auto& box : b.boxes) {
        boxes.push_back(nlohmann::json{{"enabled", box.enabled},
                                       {"content", box.content},
                                       {"color", box.color},
                                       {"include_in_key", box.include_in_key},
                                       {"path", motion_path_name(box.motion.path)},
                                       {"speed", box.motion.speed},
                                       {"size", box.motion.size},
                                       {"opacity", box.motion.opacity},
                                       {"start_x", box.motion.start_x},
                                       {"start_y", box.motion.start_y}});
    }
    return nlohmann::json{{"label", b.label},
                          {"label_anchor", b.label_anchor},
                          {"timecode", b.timecode},
                          {"utc_clock", b.utc_clock},
                          {"local_clock", b.local_clock},
                          {"frame_counter", b.frame_counter},
                          {"frame_counter_absolute", b.frame_counter_absolute},
                          {"item", b.item},
                          {"flow_id", b.flow_id},
                          {"on_keyed_stills", b.on_keyed_stills},
                          {"texts", texts},
                          {"boxes", boxes}};
}

KeyMode parse_key_mode(std::string_view s) {
    if (s == "fill_key") return KeyMode::FillKey;
    if (s == "v210a") return KeyMode::V210a;
    return KeyMode::Off;
}

const char* key_mode_name(KeyMode m) {
    switch (m) {
        case KeyMode::FillKey: return "fill_key";
        case KeyMode::V210a: return "v210a";
        default: return "off";
    }
}

std::string default_domain_dir(const std::string& seed, const std::string& scan_path) {
    std::string short_id = seed.empty() ? "player" : seed;
    if (short_id.size() > 12) short_id = short_id.substr(0, 12);
    for (char& c : short_id) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) c = '-';
    }
    std::string root = scan_path.empty() ? "/Volumes/mxl" : scan_path;
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    return root + "/player-" + short_id;
}

bool ipv4_literal(std::string_view s) {
    if (s.empty() || s.size() > 15) return false;
    int dots = 0;
    int group = -1;
    for (char c : s) {
        if (c == '.') {
            if (group < 0 || group > 255) return false;
            ++dots;
            group = -1;
            continue;
        }
        if (c < '0' || c > '9') return false;
        if (group < 0) group = 0;
        else if (group == 0) return false;
        group = group * 10 + (c - '0');
        if (group > 255) return false;
    }
    return dots == 3 && group >= 0 && group <= 255;
}

std::string detect_announce_address() {
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) != 0) throw ConfigError("NMOS_HOST_ADDRESS is unset and no interface address could be read");
    std::string found;
    for (auto* it = ifs; it; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
        const auto* in = reinterpret_cast<sockaddr_in*>(it->ifa_addr);
        char buf[INET_ADDRSTRLEN] = {};
        if (!inet_ntop(AF_INET, &in->sin_addr, buf, sizeof(buf))) continue;
        const std::string ip = buf;
        if (ip.rfind("127.", 0) == 0) continue;
        found = ip;
        break;
    }
    freeifaddrs(ifs);
    if (found.empty()) throw ConfigError("NMOS_HOST_ADDRESS is unset and no non-loopback IPv4 address was found");
    return found;
}

void require_announce_address(const std::string& ip) {
    if (!ipv4_literal(ip)) throw ConfigError("NMOS_HOST_ADDRESS must be an IPv4 address, not a hostname: " + ip);
    if (ip == "0.0.0.0" || ip.rfind("127.", 0) == 0) {
        throw ConfigError("NMOS_HOST_ADDRESS must not be " + ip);
    }
}

Config load_config(const std::string& file_path, const char* const* envp) {
    std::map<std::string, std::string> env;
    if (envp) {
        for (const char* const* p = envp; *p; ++p) {
            std::string s(*p);
            const auto eq = s.find('=');
            if (eq == std::string::npos) continue;
            env.emplace(s.substr(0, eq), s.substr(eq + 1));
        }
    }
    Config c;
    // Where each setting came from (environment, file, argument or default), for GET /api/v1/config.
    std::map<std::string, std::string> origin;
    c.config_dir = "/config";
    if (auto e = env_get(env, "CONFIG_DIR"); !e.empty()) c.config_dir = e;
    if (!file_path.empty()) c.config_path = file_path;
    else if (auto e = env_get(env, "PLAYER_CONFIG"); !e.empty()) c.config_path = e;
    else c.config_path = c.config_dir + "/player.json";
    origin["PLAYER_CONFIG"] = !file_path.empty() ? "argument" : !env_get(env, "PLAYER_CONFIG").empty() ? "environment" : "default";

    json file = json::object();
    {
        std::ifstream in(c.config_path);
        if (in) {
            try {
                in >> file;
            } catch (const std::exception& ex) {
                throw ConfigError(std::string("invalid config file: ") + ex.what());
            }
            if (!file.is_object()) throw ConfigError("config file must be a JSON object");
        }
    }
    auto note = [&](const char* envk, bool in_file) {
        origin[envk] = !env_get(env, envk).empty() ? "environment" : in_file ? "file" : "default";
    };
    auto pick_str = [&](const char* envk, const char* filek, const std::string& def) {
        note(envk, file.contains(filek) && file.at(filek).is_string());
        if (auto e = env_get(env, envk); !e.empty()) return e;
        if (file.contains(filek) && file.at(filek).is_string()) return file.at(filek).get<std::string>();
        return def;
    };
    auto pick_int = [&](const char* envk, const char* filek, int def) {
        note(envk, file.contains(filek) && file.at(filek).is_number_integer());
        if (auto e = env_get(env, envk); !e.empty()) return parse_int(e, envk);
        if (file.contains(filek) && file.at(filek).is_number_integer()) return file.at(filek).get<int>();
        return def;
    };
    auto pick_dbl = [&](const char* envk, const char* filek, double def) {
        note(envk, file.contains(filek) && file.at(filek).is_number());
        if (auto e = env_get(env, envk); !e.empty()) return parse_double(e, envk);
        if (file.contains(filek) && file.at(filek).is_number()) return file.at(filek).get<double>();
        return def;
    };
    auto pick_bool = [&](const char* envk, const char* filek, bool def) {
        note(envk, file.contains(filek) && file.at(filek).is_boolean());
        if (auto e = env_get(env, envk); !e.empty()) return parse_bool(e, envk);
        if (file.contains(filek) && file.at(filek).is_boolean()) return file.at(filek).get<bool>();
        return def;
    };

    note("CONFIG_DIR", file.contains("config_dir") && file.at("config_dir").is_string());
    if (env_get(env, "CONFIG_DIR").empty() && file.contains("config_dir") && file.at("config_dir").is_string()) {
        c.config_dir = file.at("config_dir").get<std::string>();
    }
    const auto fmt = pick_str("PLAYER_FORMAT", "format", "1080p50");
    auto parsed = parse_format(fmt);
    if (!parsed) throw ConfigError("invalid PLAYER_FORMAT " + fmt);
    c.format = *parsed;
    c.outputs = pick_int("PLAYER_OUTPUTS", "outputs_count", 2);
    c.audio_channels = pick_int("PLAYER_AUDIO_CHANNELS", "audio_channels", 16);
    c.library_dir = pick_str("PLAYER_LIBRARY_DIR", "library_dir", "/data/library");
    c.import_dir = pick_str("PLAYER_IMPORT_DIR", "import_dir", "");
    c.convert_concurrency = pick_int("PLAYER_CONVERT_CONCURRENCY", "convert_concurrency", 1);
    c.ram_clip_max_s = pick_dbl("PLAYER_RAM_CLIP_MAX_S", "ram_clip_max_s", 20);
    c.ram_budget_mb = pick_int("PLAYER_RAM_BUDGET_MB", "ram_budget_mb", 4096);
    c.preroll_frames = pick_int("PLAYER_PREROLL_FRAMES", "preroll_frames", 25);
    c.mxl_scan_path = pick_str("MXL_DOMAIN_SCAN_PATH", "mxl_scan_path", "/Volumes/mxl");
    c.mxl_domain_dir = pick_str("MXL_OUTPUT_DOMAIN_DIR", "mxl_domain_dir", "");
    c.mxl_domain_id = pick_str("MXL_OUTPUT_DOMAIN_ID", "mxl_domain_id", "");
    note("MXL_HISTORY_DURATION_NS", file.contains("history_duration_ns") && file.at("history_duration_ns").is_number());
    if (auto e = env_get(env, "MXL_HISTORY_DURATION_NS"); !e.empty()) {
        try {
            std::size_t idx = 0;
            c.history_duration_ns = std::stoull(e, &idx, 10);
            if (idx != e.size()) throw ConfigError("invalid integer");
        } catch (const ConfigError&) {
            throw;
        } catch (...) {
            throw ConfigError("invalid integer for MXL_HISTORY_DURATION_NS");
        }
    } else if (file.contains("history_duration_ns") && file.at("history_duration_ns").is_number()) {
        c.history_duration_ns = file.at("history_duration_ns").get<std::uint64_t>();
    }
    c.mxl_cleanup_on_exit = pick_bool("MXL_CLEANUP_ON_EXIT", "mxl_cleanup_on_exit", false);
    c.nmos_registry_address = pick_str("NMOS_REGISTRY_ADDRESS", "nmos_registry_address", "");
    c.nmos_registry_port = pick_int("NMOS_REGISTRY_PORT", "nmos_registry_port", 3210);
    c.nmos_query_address = pick_str("NMOS_QUERY_ADDRESS", "nmos_query_address", c.nmos_registry_address);
    c.nmos_query_port = pick_int("NMOS_QUERY_PORT", "nmos_query_port", c.nmos_registry_port + 1);
    c.nmos_dns_sd = pick_bool("NMOS_DNS_SD", "nmos_dns_sd", false);
    if (c.nmos_dns_sd) throw ConfigError("NMOS_DNS_SD=true is not available; this build has no DNS-SD or Avahi. Set NMOS_DNS_SD=false");
    c.nmos_port = pick_int("NMOS_PORT", "nmos_port", 3282);
    c.nmos_seed = pick_str("NMOS_SEED", "nmos_seed", "");
    note("HOST_ID", false);
    if (c.nmos_seed.empty()) {
        // HOST_ID is a legacy seed prefix, not an address that is announced.
        auto host = env_get(env, "HOST_ID");
        if (host.empty()) host = "host";
        c.nmos_seed = host + "-player";
    }
    c.nmos_label = pick_str("NMOS_LABEL", "nmos_label", "MXL Test Player");
    note("NMOS_TAGS", file.contains("nmos_tags"));
    if (auto e = env_get(env, "NMOS_TAGS"); !e.empty()) {
        try {
            c.nmos_tags = json::parse(e);
        } catch (const std::exception& ex) {
            throw ConfigError(std::string("NMOS_TAGS is not JSON: ") + ex.what());
        }
    } else if (file.contains("nmos_tags")) {
        c.nmos_tags = file.at("nmos_tags");
    }
    if (!c.nmos_tags.is_object()) throw ConfigError("NMOS_TAGS must be a JSON object of tag name to string array");
    for (auto it = c.nmos_tags.begin(); it != c.nmos_tags.end(); ++it) {
        if (!it.value().is_array()) throw ConfigError("NMOS_TAGS values must be arrays of strings");
        for (const auto& v : it.value()) {
            if (!v.is_string()) throw ConfigError("NMOS_TAGS values must be arrays of strings");
        }
    }
    c.nmos_host_address = pick_str("NMOS_HOST_ADDRESS", "nmos_host_address", "");
    if (c.nmos_host_address.empty()) c.nmos_host_address = detect_announce_address();
    require_announce_address(c.nmos_host_address);
    c.shutdown_timeout_s = pick_int("SHUTDOWN_TIMEOUT_S", "shutdown_timeout_s", 10);
    c.web_port = pick_int("WEB_PORT", "web_port", 8130);
    note("PLAYER_STATE", file.contains("state_path") && file.at("state_path").is_string());
    if (auto e = env_get(env, "PLAYER_STATE"); !e.empty()) c.state_path = e;
    else if (file.contains("state_path") && file.at("state_path").is_string()) c.state_path = file.at("state_path").get<std::string>();
    else c.state_path = c.config_dir + "/state.json";
    c.font_dir = pick_str("PLAYER_FONT_DIR", "font_dir", "");
    c.web_root = pick_str("PLAYER_WEB_ROOT", "web_root", "");
    c.sprite_max_px = pick_int("PLAYER_SPRITE_MAX_PX", "sprite_max_px", 512);
    note("PLAYER_UPLOAD_LIMIT_GB", file.contains("upload_limit_gb"));
    if (auto e = env_get(env, "PLAYER_UPLOAD_LIMIT_GB"); !e.empty()) {
        c.upload_limit_bytes = static_cast<std::uint64_t>(parse_double(e, "PLAYER_UPLOAD_LIMIT_GB") * (1ull << 30));
    } else if (file.contains("upload_limit_gb")) {
        c.upload_limit_bytes = static_cast<std::uint64_t>(file.at("upload_limit_gb").get<double>() * (1ull << 30));
    }
    if (file.contains("outputs") && file.at("outputs").is_array()) {
        int i = 0;
        for (const auto& o : file.at("outputs")) {
            c.output_configs.push_back(output_from_json(o, i++));
        }
    }
    validate(c);
    auto num = [](double v) {
        std::ostringstream os;
        os << v;
        return os.str();
    };
    const auto flag = [](bool v) { return std::string(v ? "true" : "false"); };
    const std::pair<const char*, std::string> values[] = {
        {"CONFIG_DIR", c.config_dir},
        {"PLAYER_CONFIG", c.config_path},
        {"PLAYER_STATE", c.state_path},
        {"PLAYER_FORMAT", c.format.name},
        {"PLAYER_OUTPUTS", std::to_string(c.outputs)},
        {"PLAYER_AUDIO_CHANNELS", std::to_string(c.audio_channels)},
        {"PLAYER_LIBRARY_DIR", c.library_dir},
        {"PLAYER_IMPORT_DIR", c.import_dir},
        {"PLAYER_CONVERT_CONCURRENCY", std::to_string(c.convert_concurrency)},
        {"PLAYER_RAM_CLIP_MAX_S", num(c.ram_clip_max_s)},
        {"PLAYER_RAM_BUDGET_MB", std::to_string(c.ram_budget_mb)},
        {"PLAYER_PREROLL_FRAMES", std::to_string(c.preroll_frames)},
        {"PLAYER_FONT_DIR", c.font_dir},
        {"PLAYER_WEB_ROOT", c.web_root},
        {"PLAYER_UPLOAD_LIMIT_GB", num(static_cast<double>(c.upload_limit_bytes) / static_cast<double>(1ull << 30))},
        {"PLAYER_SPRITE_MAX_PX", std::to_string(c.sprite_max_px)},
        {"MXL_DOMAIN_SCAN_PATH", c.mxl_scan_path},
        {"MXL_OUTPUT_DOMAIN_DIR", c.mxl_domain_dir},
        {"MXL_OUTPUT_DOMAIN_ID", c.mxl_domain_id},
        {"MXL_HISTORY_DURATION_NS", std::to_string(c.history_duration_ns)},
        {"MXL_CLEANUP_ON_EXIT", flag(c.mxl_cleanup_on_exit)},
        {"NMOS_REGISTRY_ADDRESS", c.nmos_registry_address},
        {"NMOS_REGISTRY_PORT", std::to_string(c.nmos_registry_port)},
        {"NMOS_QUERY_ADDRESS", c.nmos_query_address},
        {"NMOS_QUERY_PORT", std::to_string(c.nmos_query_port)},
        {"NMOS_DNS_SD", flag(c.nmos_dns_sd)},
        {"NMOS_PORT", std::to_string(c.nmos_port)},
        {"NMOS_SEED", c.nmos_seed},
        {"NMOS_LABEL", c.nmos_label},
        {"NMOS_TAGS", c.nmos_tags.dump()},
        {"NMOS_HOST_ADDRESS", c.nmos_host_address},
        {"HOST_ID", env_get(env, "HOST_ID")},
        {"SHUTDOWN_TIMEOUT_S", std::to_string(c.shutdown_timeout_s)},
        {"WEB_PORT", std::to_string(c.web_port)},
    };
    for (const auto& [key, value] : values) c.settings.push_back({key, value, origin.count(key) ? origin[key] : "default"});
    return c;
}

}  // namespace mtp
