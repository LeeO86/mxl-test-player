#include "app.hpp"

#include "anc.hpp"
#include "http_server.hpp"
#include "process.hpp"
#include "nmos.hpp"
#include "output.hpp"
#include "util.hpp"
#include "uuid_util.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace mtp {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string mime_of(const std::string& path) {
    if (path.size() >= 5 && path.ends_with(".html")) return "text/html; charset=utf-8";
    if (path.ends_with(".js")) return "text/javascript; charset=utf-8";
    if (path.ends_with(".css")) return "text/css; charset=utf-8";
    if (path.ends_with(".svg")) return "image/svg+xml";
    if (path.ends_with(".json")) return "application/json";
    if (path.ends_with(".png")) return "image/png";
    if (path.ends_with(".jpg") || path.ends_with(".jpeg")) return "image/jpeg";
    if (path.ends_with(".woff2")) return "font/woff2";
    return "application/octet-stream";
}

std::string find_web_root(const Config& cfg) {
    if (!cfg.web_root.empty() && fs::exists(cfg.web_root)) return cfg.web_root;
    const char* candidates[] = {"web/dist", "../share/mxl-test-player/web", "/usr/local/share/mxl-test-player/web"};
    for (const char* c : candidates)
        if (fs::exists(fs::path(c) / "index.html")) return c;
    return "web/dist";
}

json builtin_presets() {
    return json::array({{{"id", "bars_tone"},
                         {"name", "Bars + tone"},
                         {"builtin", true},
                         {"video", {{"pattern", "smpte_rp219"}, {"pluge", true}}},
                         {"audio", {{"signal", "sine"}, {"frequency", 1000}, {"level_dbfs", -18}}}},
                        {{"id", "av_sync"},
                         {"name", "A/V sync"},
                         {"builtin", true},
                         {"video", {{"pattern", "av_sync"}}},
                         {"audio", {{"signal", "sync_beep"}, {"level_dbfs", -18}}}},
                        {{"id", "field_order"},
                         {"name", "Field order"},
                         {"builtin", true},
                         {"video", {{"pattern", "field_order"}}},
                         {"audio", {{"signal", "sine"}, {"frequency", 440}, {"level_dbfs", -18}}}},
                        {{"id", "ident_16"},
                         {"name", "Ident 16 ch"},
                         {"builtin", true},
                         {"video", {{"pattern", "smpte_rp219"}}},
                         {"audio", {{"signal", "ident_beeps"}, {"level_dbfs", -18}}}},
                        {{"id", "motion"},
                         {"name", "Motion"},
                         {"builtin", true},
                         {"video", {{"pattern", "motion"}}},
                         {"audio", {{"signal", "pink"}, {"level_dbfs", -20}}}}});
}

SourceDesc source_from_preset(const json& p) {
    SourceDesc s;
    s.type = "pattern";
    s.pattern = p.value("video", json::object()).value("pattern", "smpte_rp219");
    s.pluge = p.value("video", json::object()).value("pluge", false);
    s.audio = p.value("audio", json::object()).value("signal", "sine");
    s.frequency = p.value("audio", json::object()).value("frequency", 1000.0);
    s.level_dbfs = p.value("audio", json::object()).value("level_dbfs", -18.0);
    s.sync_beep = s.pattern == "av_sync" || s.audio == "sync_beep";
    return s;
}

struct Upload {
    std::string id;
    std::string name;
    std::uint64_t size = 0;
    json options = json::object();
    std::map<int, fs::path> chunks;
};

}  // namespace

struct App::Impl {
    Config cfg;
    MxlSession mxl;
    Library library;
    NmosNode nmos;
    HttpServer web;
    HttpServer nmos_http;
    std::vector<std::unique_ptr<Output>> outputs;
    std::string web_root;
    Uuid node;
    std::string domain_id;
    std::vector<json> presets;
    std::vector<json> playlists;
    std::map<std::string, Upload> uploads;
    std::mutex mu;
    std::atomic<bool> stop{false};
    std::atomic<bool> shut{false};
    std::atomic<int> code{0};
    std::thread meter_thread;
    void shutdown_now();

    explicit Impl(Config c)
        : cfg(std::move(c)), library(cfg) {}

    void save_state();
    void load_state();
    void refresh_nmos();
    json metrics() const;
    void handle(const HttpRequest& req, HttpResponse& res);
    Output* out(int index);
};

Output* App::Impl::out(int index) {
    if (index < 0 || index >= static_cast<int>(outputs.size())) return nullptr;
    return outputs[static_cast<std::size_t>(index)].get();
}

void App::Impl::save_state() {
    json arr = json::array();
    for (const auto& o : outputs) {
        auto st = o->status();
        arr.push_back(json{{"source", st["source"]},
                           {"transport", st["transport"]},
                           {"loop", st["loop"]},
                           {"burnin", st["burnin"]},
                           {"label", st["label"]},
                           {"format", st["format"]},
                           {"key_mode", st["key_mode"]},
                           {"anc", st["anc"]},
                           {"tc_source", st["tc_source"]},
                           {"master_video", st.value("master_video", true)},
                           {"master_audio", st.value("master_audio", true)},
                           {"master_data", st.value("master_data", true)},
                           {"master_key", st.value("master_key", true)}});
    }
    std::error_code ec;
    fs::create_directories(fs::path(cfg.state_path).parent_path(), ec);
    std::ofstream f(cfg.state_path);
    f << json{{"outputs", arr}, {"presets", presets}, {"playlists", playlists}}.dump(2);
}

void App::Impl::load_state() {
    std::ifstream in(cfg.state_path);
    if (!in) return;
    json j;
    try {
        in >> j;
    } catch (...) {
        return;
    }
    if (j.contains("presets") && j["presets"].is_array()) presets = j["presets"].get<std::vector<json>>();
    if (j.contains("playlists") && j["playlists"].is_array()) playlists = j["playlists"].get<std::vector<json>>();
    if (!j.contains("outputs")) return;
    int i = 0;
    for (const auto& o : j["outputs"]) {
        if (i >= static_cast<int>(outputs.size())) break;
        if (o.contains("burnin")) outputs[i]->set_burnin(burnin_from_json(o["burnin"]));
        if (o.contains("source")) outputs[i]->set_source(source_from_json(o["source"]));
        if (o.contains("transport")) outputs[i]->command(o["transport"].get<std::string>() == "pause" ? "pause" : o["transport"].get<std::string>() == "stop" ? "stop" : "play");
        const auto ids = outputs[i]->ids();
        if (o.contains("master_video")) outputs[i]->set_master(ids.video_sender.str(), o.at("master_video").get<bool>());
        if (o.contains("master_audio")) outputs[i]->set_master(ids.audio_sender.str(), o.at("master_audio").get<bool>());
        if (o.contains("master_data")) outputs[i]->set_master(ids.data_sender.str(), o.at("master_data").get<bool>());
        if (o.contains("master_key")) outputs[i]->set_master(ids.key_sender.str(), o.at("master_key").get<bool>());
        ++i;
    }
}

void App::Impl::refresh_nmos() {
    NmosModel m;
    m.node_id = node.str();
    m.device_id = uuid_v5(node, "device").str();
    m.label = cfg.nmos_label;
    m.host = cfg.nmos_host_address;
    m.api_port = cfg.nmos_port;
    m.tags = cfg.nmos_tags;
    m.domain_id = domain_id;
    for (const auto& o : outputs) {
        const auto st = o->status();
        json flows = json::object();
        for (const auto& def : o->flow_definitions()) {
            auto f = json::parse(def);
            const auto id = f.at("id").get<std::string>();
            flows[id] = std::move(f);
        }
        const auto ids = o->ids();
        auto push = [&](const Uuid& sender, const Uuid& flow, const Uuid& source, const char* role, const char* format) {
            NmosSenderState s;
            s.id = sender.str();
            s.flow_id = flow.str();
            s.source_id = source.str();
            s.label = st.value("label", "") + std::string(" ") + role;
            s.description = s.label;
            s.format = format;
            s.flow = flows.value(s.flow_id, json::object());
            s.index = o->index();
            m.senders.push_back(std::move(s));
        };
        push(ids.video_sender, ids.video_flow, ids.video_source, "Video", "urn:x-nmos:format:video");
        push(ids.audio_sender, ids.audio_flow, ids.audio_source, "Audio", "urn:x-nmos:format:audio");
        if (st.value("anc", true)) push(ids.data_sender, ids.data_flow, ids.data_source, "Data", "urn:x-nmos:format:data");
        if (st.value("key_mode", "off") == "fill_key") push(ids.key_sender, ids.key_flow, ids.key_source, "Key", "urn:x-nmos:format:video");
    }
    nmos.update(m);
}

json App::Impl::metrics() const {
    std::ostringstream os;
    os << "# HELP mxl_test_player_output_state Transport state 0=stop 1=play 2=pause\n";
    os << "# TYPE mxl_test_player_output_state gauge\n";
    os << "# TYPE mxl_test_player_grains_written_total counter\n";
    os << "# TYPE mxl_test_player_underruns_total counter\n";
    os << "# TYPE mxl_test_player_loops_total counter\n";
    os << "# TYPE mxl_test_player_decode_ahead_frames gauge\n";
    os << "# TYPE mxl_test_player_ram_clip_bytes gauge\n";
    os << "# TYPE mxl_test_player_library_items gauge\n";
    os << "# TYPE mxl_test_player_library_bytes gauge\n";
    os << "# TYPE mxl_test_player_upload_bytes_total counter\n";
    os << "# TYPE mxl_test_player_conversion_jobs gauge\n";
    std::uint64_t uploads = 0;
    for (auto& o : outputs) {
        const auto st = o->status();
        const std::string tr = st.value("transport", "stop");
        const int state = tr == "play" ? 1 : tr == "pause" ? 2 : 0;
        const int idx = o->index();
        os << "mxl_test_player_output_state{output=\"" << idx << "\"} " << state << "\n";
        os << "mxl_test_player_grains_written_total{output=\"" << idx << "\"} " << o->grains() << "\n";
        os << "mxl_test_player_underruns_total{output=\"" << idx << "\"} " << o->underruns() << "\n";
        os << "mxl_test_player_loops_total{output=\"" << idx << "\"} " << o->loops_done() << "\n";
        os << "mxl_test_player_decode_ahead_frames{output=\"" << idx << "\"} " << o->ahead() << "\n";
        os << "mxl_test_player_ram_clip_bytes{output=\"" << idx << "\"} " << o->ram_bytes() << "\n";
        os << "mxl_test_player_source_info{output=\"" << idx << "\",source=\"" << st["source"].value("type", "") << "\",item=\""
           << st.value("item", "") << "\"} 1\n";
    }
    int queued = 0, running = 0, failed = 0, done = 0;
    for (const auto& j : library.jobs()) {
        if (j.state == "queued") ++queued;
        else if (j.state == "running") ++running;
        else if (j.state == "failed") ++failed;
        else ++done;
    }
    os << "mxl_test_player_conversion_jobs{state=\"queued\"} " << queued << "\n";
    os << "mxl_test_player_conversion_jobs{state=\"running\"} " << running << "\n";
    os << "mxl_test_player_conversion_jobs{state=\"failed\"} " << failed << "\n";
    os << "mxl_test_player_conversion_jobs{state=\"done\"} " << done << "\n";
    os << "mxl_test_player_library_items " << library.item_count() << "\n";
    os << "mxl_test_player_library_bytes " << library.total_bytes() << "\n";
    os << "mxl_test_player_upload_bytes_total " << uploads << "\n";
    return json();  // placeholder, real text returned by handle
}

void App::Impl::handle(const HttpRequest& req, HttpResponse& res) {
    if (req.method == "OPTIONS") {
        res.status = 204;
        return;
    }
    auto text = [&](const json& j, int status = 200) {
        res.status = status;
        res.set_text(j.dump());
    };
    const auto& path = req.path;
    if (path == "/livez") {
        res.set_text("ok\n", "text/plain");
        return;
    }
    if (path == "/readyz") {
        if (nmos.registry_configured() && !nmos.registered()) {
            res.status = 503;
            res.set_text("not registered\n", "text/plain");
            return;
        }
        res.set_text("ok\n", "text/plain");
        return;
    }
    if (path == "/statusz") {
        json outs = json::array();
        for (auto& o : outputs) outs.push_back(o->status());
        text(json{{"ok", true}, {"outputs", outs}, {"domain", cfg.mxl_domain_dir}});
        return;
    }
    if (path == "/metrics") {
        std::ostringstream os;
        os << "# HELP mxl_test_player_grains_written_total Grains committed\n# TYPE mxl_test_player_grains_written_total counter\n";
        os << "# TYPE mxl_test_player_underruns_total counter\n# TYPE mxl_test_player_loops_total counter\n";
        os << "# TYPE mxl_test_player_output_state gauge\n# TYPE mxl_test_player_decode_ahead_frames gauge\n";
        os << "# TYPE mxl_test_player_ram_clip_bytes gauge\n# TYPE mxl_test_player_library_items gauge\n";
        os << "# TYPE mxl_test_player_library_bytes gauge\n# TYPE mxl_test_player_upload_bytes_total counter\n";
        os << "# TYPE mxl_test_player_conversion_jobs gauge\n# TYPE mxl_test_player_source_info gauge\n";
        os << "# TYPE mxl_test_player_conversion_duration_seconds histogram\n";
        for (auto& o : outputs) {
            const auto st = o->status();
            const std::string tr = st.value("transport", "stop");
            const int state = tr == "play" ? 1 : tr == "pause" ? 2 : 0;
            os << "mxl_test_player_output_state{output=\"" << o->index() << "\"} " << state << "\n";
            os << "mxl_test_player_grains_written_total{output=\"" << o->index() << "\"} " << o->grains() << "\n";
            os << "mxl_test_player_underruns_total{output=\"" << o->index() << "\"} " << o->underruns() << "\n";
            os << "mxl_test_player_loops_total{output=\"" << o->index() << "\"} " << o->loops_done() << "\n";
            os << "mxl_test_player_decode_ahead_frames{output=\"" << o->index() << "\"} " << o->ahead() << "\n";
            os << "mxl_test_player_ram_clip_bytes{output=\"" << o->index() << "\"} " << o->ram_bytes() << "\n";
            os << "mxl_test_player_source_info{output=\"" << o->index() << "\",source=\"" << st["source"].value("type", "pattern") << "\"} 1\n";
        }
        int counts[4] = {};
        const char* names[] = {"queued", "running", "done", "failed"};
        for (const auto& j : library.jobs()) {
            int k = 2;
            if (j.state == "queued") k = 0;
            else if (j.state == "running") k = 1;
            else if (j.state == "failed") k = 3;
            counts[k]++;
            if (j.duration_s > 0) os << "mxl_test_player_conversion_duration_seconds_sum " << j.duration_s << "\n";
        }
        for (int k = 0; k < 4; ++k) os << "mxl_test_player_conversion_jobs{state=\"" << names[k] << "\"} " << counts[k] << "\n";
        os << "mxl_test_player_library_items " << library.item_count() << "\n";
        os << "mxl_test_player_library_bytes " << library.total_bytes() << "\n";
        std::uint64_t up = 0;
        {
            std::lock_guard lock(mu);
            for (const auto& u : uploads) up += u.second.size;
        }
        os << "mxl_test_player_upload_bytes_total " << up << "\n";
        res.set_text(os.str(), "text/plain; version=0.0.4");
        return;
    }
    auto body_json = [&]() {
        if (req.body.empty()) return json::object();
        return json::parse(std::string(req.body.begin(), req.body.end()));
    };
    try {
        if (path == "/api/v1/config/export" && req.method == "GET") {
            text(json{{"version", 1},
                      {"note", "No secrets are stored. Deployment settings (ports, registry, seed) are informational and are not applied by import."},
                      {"deployment",
                       json{{"format", cfg.format.name},
                            {"audio_channels", cfg.audio_channels},
                            {"outputs_count", cfg.outputs},
                            {"nmos_label", cfg.nmos_label},
                            {"nmos_seed", cfg.nmos_seed},
                            {"nmos_host_address", cfg.nmos_host_address}}},
                      {"state", json{{"outputs", [&] {
                                          json arr = json::array();
                                          for (const auto& o : outputs) {
                                              auto st = o->status();
                                              arr.push_back(json{{"source", st["source"]},
                                                                  {"transport", st["transport"]},
                                                                  {"loop", st["loop"]},
                                                                  {"burnin", st["burnin"]},
                                                                  {"label", st["label"]},
                                                                  {"format", st["format"]},
                                                                  {"key_mode", st["key_mode"]},
                                                                  {"anc", st["anc"]},
                                                                  {"tc_source", st["tc_source"]},
                                                                  {"master_video", st["master_video"]},
                                                                  {"master_audio", st["master_audio"]},
                                                                  {"master_data", st["master_data"]},
                                                                  {"master_key", st["master_key"]}});
                                          }
                                          return arr;
                                      }()},
                                      {"presets", presets},
                                      {"playlists", playlists}}}});
            return;
        }
        if (path == "/api/v1/config/import" && req.method == "POST") {
            auto doc = body_json();
            json state = doc.contains("state") ? doc.at("state") : doc;
            {
                std::ofstream f(cfg.state_path);
                f << state.dump(2);
            }
            if (state.contains("presets")) presets = state.at("presets").get<std::vector<json>>();
            if (state.contains("playlists")) playlists = state.at("playlists").get<std::vector<json>>();
            load_state();
            save_state();
            text(json{{"ok", true}});
            return;
        }
        if (path == "/api/v1/config" && req.method == "GET") {
            text(json{{"format", cfg.format.name},
                      {"outputs", cfg.outputs},
                      {"audio_channels", cfg.audio_channels},
                      {"library_dir", cfg.library_dir},
                      {"web_port", cfg.web_port},
                      {"nmos_port", cfg.nmos_port},
                      {"domain", cfg.mxl_domain_dir},
                      {"domain_id", domain_id}});
            return;
        }
        if (path == "/api/v1/outputs" && req.method == "GET") {
            json arr = json::array();
            for (auto& o : outputs) arr.push_back(o->status());
            text(arr);
            return;
        }
        if (path.rfind("/api/v1/outputs/", 0) == 0) {
            auto rest = path.substr(std::string("/api/v1/outputs/").size());
            const auto slash = rest.find('/');
            const int index = std::stoi(slash == std::string::npos ? rest : rest.substr(0, slash));
            auto* o = out(index);
            if (!o) {
                text(json{{"error", "not found"}}, 404);
                return;
            }
            const std::string leaf = slash == std::string::npos ? "" : rest.substr(slash + 1);
            if (leaf.empty() && req.method == "GET") {
                text(o->status());
                return;
            }
            if (leaf.empty() && (req.method == "PATCH" || req.method == "PUT")) {
                o->patch(body_json());
                refresh_nmos();
                save_state();
                text(o->status());
                return;
            }
            if (leaf == "transport" && req.method == "POST") {
                o->command(body_json().value("action", "play"));
                save_state();
                text(o->status());
                return;
            }
            if (leaf == "source" && (req.method == "PUT" || req.method == "POST")) {
                auto b = body_json();
                if (b.contains("preset")) {
                    const auto id = b["preset"].get<std::string>();
                    json found;
                    for (const auto& p : builtin_presets())
                        if (p["id"] == id) found = p;
                    for (const auto& p : presets)
                        if (p.value("id", "") == id) found = p;
                    if (found.is_null()) {
                        text(json{{"error", "unknown preset"}}, 404);
                        return;
                    }
                    o->set_source(source_from_preset(found));
                } else {
                    o->set_source(source_from_json(b));
                }
                save_state();
                text(o->status());
                return;
            }
            if (leaf == "burnin" && (req.method == "PUT" || req.method == "PATCH")) {
                o->set_burnin(burnin_from_json(body_json()));
                save_state();
                text(o->status());
                return;
            }
            if (leaf == "seek" && req.method == "POST") {
                o->seek(body_json().value("frame", 0));
                text(o->status());
                return;
            }
            if (leaf == "thumbnail" || leaf == "thumbnail.jpg") {
                auto jpg = o->thumbnail();
                if (jpg.empty()) {
                    res.status = 204;
                    return;
                }
                res.status = 200;
                res.content_type = "image/jpeg";
                res.body = std::move(jpg);
                return;
            }
            if (leaf == "probe" && req.method == "GET") {
                const auto idx_s = query_get(req.query, "index");
                std::uint64_t index = idx_s.empty() ? 0 : std::strtoull(idx_s.c_str(), nullptr, 10);
                const auto st = o->status();
                const auto fmt = o->format();
                auto video = read_grain(mxl, st.value("video_flow_id", ""), index, fmt.width, fmt.interlaced() ? fmt.field_height() : fmt.height, 500);
                auto anc = read_grain(mxl, st.value("data_flow_id", ""), index, 0, 0, 200);
                Timecode tc;
                AtcKind kind = AtcKind::Ltc;
                std::string tc_s;
                if (anc.ok) {
                    if (decode_anc_timecode(anc.head.data(), anc.head.size(), tc, kind) ||
                        (anc.head.size() >= 16 && decode_anc_timecode(anc.head.data(), 4096, tc, kind))) {
                        tc_s = tc.format();
                    }
                }
                // The probe head is only 64 bytes; re-read is limited. Decode from head if the header fits.
                text(json{{"ok", video.ok},
                          {"index", index},
                          {"y", video.y},
                          {"cb", video.cb},
                          {"cr", video.cr},
                          {"fnv", std::to_string(video.fnv)},
                          {"anc_ok", anc.ok},
                          {"anc_timecode", tc_s},
                          {"video_flow_id", st["video_flow_id"]}});
                return;
            }
            text(json{{"error", "not found"}}, 404);
            return;
        }
        if (path == "/api/v1/library" && req.method == "GET") {
            json arr = json::array();
            const auto q = query_get(req.query, "q");
            for (const auto& it : library.list()) {
                if (!q.empty() && it.name.find(q) == std::string::npos) {
                    bool tag = false;
                    for (const auto& t : it.tags)
                        if (t.find(q) != std::string::npos) tag = true;
                    if (!tag) continue;
                }
                arr.push_back(library.to_json(it));
            }
            text(arr);
            return;
        }
        if (path.rfind("/api/v1/library/", 0) == 0) {
            auto rest = path.substr(std::string("/api/v1/library/").size());
            const auto slash = rest.find('/');
            const auto id = slash == std::string::npos ? rest : rest.substr(0, slash);
            const auto leaf = slash == std::string::npos ? "" : rest.substr(slash + 1);
            if (!library.exists(id)) {
                text(json{{"error", "not found"}}, 404);
                return;
            }
            if (leaf.empty() && req.method == "GET") {
                text(library.to_json(library.get(id)));
                return;
            }
            if (leaf.empty() && req.method == "DELETE") {
                std::string err;
                if (!library.remove(id, err)) {
                    text(json{{"error", err}}, 409);
                    return;
                }
                text(json{{"ok", true}});
                return;
            }
            if (leaf.empty() && (req.method == "PATCH" || req.method == "PUT")) {
                auto b = body_json();
                std::vector<std::string> tags;
                if (b.contains("tags"))
                    for (const auto& t : b["tags"]) tags.push_back(t.get<std::string>());
                std::string err;
                auto cur = library.get(id);
                if (!b.contains("tags")) tags = cur.tags;
                if (!library.update_meta(id, b.value("name", ""), tags, err)) {
                    text(json{{"error", err}}, 400);
                    return;
                }
                text(library.to_json(library.get(id)));
                return;
            }
            if (leaf == "reconvert" && req.method == "POST") {
                std::string err;
                if (!library.reconvert(id, body_json(), cfg.format, err)) {
                    text(json{{"error", err}}, 400);
                    return;
                }
                text(json{{"ok", true}});
                return;
            }
        }
        if (path == "/api/v1/jobs" && req.method == "GET") {
            json arr = json::array();
            for (const auto& j : library.jobs()) {
                arr.push_back(json{{"id", j.id},
                                   {"item_id", j.item_id},
                                   {"format", j.format},
                                   {"state", j.state},
                                   {"progress", j.progress},
                                   {"error", j.error},
                                   {"duration_s", j.duration_s}});
            }
            text(arr);
            return;
        }
        if (path == "/api/v1/uploads" && req.method == "POST") {
            auto b = body_json();
            Upload u;
            u.id = uuid_v5(uuid_namespace_dns(), b.value("name", "upload") + std::to_string(uploads.size())).str();
            u.name = b.value("name", "upload.bin");
            u.size = b.value("size", 0);
            u.options = b.value("options", json::object());
            if (u.size > cfg.upload_limit_bytes) {
                text(json{{"error", "file exceeds upload limit"}}, 413);
                return;
            }
            fs::create_directories(fs::path(cfg.library_dir) / "_uploads" / u.id);
            {
                std::lock_guard lock(mu);
                uploads[u.id] = u;
            }
            text(json{{"id", u.id}, {"chunk_size", 8 * 1024 * 1024}}, 201);
            return;
        }
        if (path.rfind("/api/v1/uploads/", 0) == 0) {
            auto rest = path.substr(std::string("/api/v1/uploads/").size());
            const auto slash = rest.find('/');
            const auto id = slash == std::string::npos ? rest : rest.substr(0, slash);
            std::unique_lock lock(mu);
            auto it = uploads.find(id);
            if (it == uploads.end()) {
                text(json{{"error", "unknown upload"}}, 404);
                return;
            }
            if (rest.find("/chunks/") != std::string::npos && req.method == "PUT") {
                const auto num = std::stoi(rest.substr(rest.rfind('/') + 1));
                auto dest = fs::path(cfg.library_dir) / "_uploads" / id / ("chunk-" + std::to_string(num));
                std::ofstream out(dest, std::ios::binary);
                out.write(reinterpret_cast<const char*>(req.body.data()), static_cast<std::streamsize>(req.body.size()));
                it->second.chunks[num] = dest;
                text(json{{"ok", true}, {"chunk", num}, {"bytes", req.body.size()}});
                return;
            }
            if (rest.size() >= id.size() + 9 && rest.find("/complete") != std::string::npos && req.method == "POST") {
                auto assembled = fs::path(cfg.library_dir) / "_uploads" / id / it->second.name;
                {
                    std::ofstream out(assembled, std::ios::binary);
                    for (const auto& ch : it->second.chunks) {
                        std::ifstream in(ch.second, std::ios::binary);
                        out << in.rdbuf();
                    }
                    out.flush();
                }
                auto opts = it->second.options;
                auto name = it->second.name;
                uploads.erase(it);
                lock.unlock();
                auto item = library.ingest_file(assembled.string(), name, opts, cfg.format);
                std::error_code ec;
                fs::remove_all(fs::path(cfg.library_dir) / "_uploads" / id, ec);
                text(library.to_json(item), 201);
                return;
            }
            json received = json::array();
            for (const auto& ch : it->second.chunks) received.push_back(ch.first);
            text(json{{"id", id}, {"received", received}, {"size", it->second.size}});
            return;
        }
        if (path == "/api/v1/presets" && req.method == "GET") {
            json arr = builtin_presets();
            for (const auto& p : presets) arr.push_back(p);
            text(arr);
            return;
        }
        if (path == "/api/v1/presets" && req.method == "POST") {
            auto b = body_json();
            if (!b.contains("id")) b["id"] = uuid_v5(node, b.value("name", "preset")).str();
            b["builtin"] = false;
            presets.push_back(b);
            save_state();
            text(b, 201);
            return;
        }
        if (path.rfind("/api/v1/presets/", 0) == 0 && req.method == "DELETE") {
            const auto id = path.substr(std::string("/api/v1/presets/").size());
            presets.erase(std::remove_if(presets.begin(), presets.end(), [&](const json& p) { return p.value("id", "") == id; }), presets.end());
            save_state();
            text(json{{"ok", true}});
            return;
        }
        if (path == "/api/v1/playlists" && req.method == "GET") {
            text(playlists);
            return;
        }
        if (path == "/api/v1/playlists" && req.method == "POST") {
            auto b = body_json();
            if (!b.contains("id")) b["id"] = uuid_v5(node, std::to_string(playlists.size()) + b.value("name", "pl")).str();
            playlists.push_back(b);
            save_state();
            text(b, 201);
            return;
        }
        if (path.rfind("/api/v1/playlists/", 0) == 0) {
            const auto id = path.substr(std::string("/api/v1/playlists/").size());
            for (auto it = playlists.begin(); it != playlists.end(); ++it) {
                if (it->value("id", "") != id) continue;
                if (req.method == "DELETE") {
                    playlists.erase(it);
                    save_state();
                    text(json{{"ok", true}});
                    return;
                }
                if (req.method == "PUT" || req.method == "PATCH") {
                    auto b = body_json();
                    b["id"] = id;
                    *it = b;
                    save_state();
                    text(b);
                    return;
                }
                text(*it);
                return;
            }
            text(json{{"error", "not found"}}, 404);
            return;
        }
        if (path == "/api/v1/nmos" && req.method == "GET") {
            text(json{{"node_id", node.str()},
                      {"registry", cfg.nmos_registry_address},
                      {"registry_port", cfg.nmos_registry_port},
                      {"port", cfg.nmos_port},
                      {"dns_sd", cfg.nmos_dns_sd},
                      {"domain_id", domain_id}});
            return;
        }
    } catch (const std::exception& ex) {
        text(json{{"error", ex.what()}}, 400);
        return;
    }
    // Static UI.
    if (req.method == "GET") {
        std::string rel = path == "/" ? "/index.html" : path;
        if (rel.find("..") != std::string::npos) {
            text(json{{"error", "bad path"}}, 400);
            return;
        }
        fs::path file = fs::path(web_root) / rel.substr(1);
        if (fs::is_directory(file)) file /= "index.html";
        if (!fs::exists(file)) file = fs::path(web_root) / "index.html";
        if (fs::exists(file) && fs::is_regular_file(file)) {
            std::ifstream in(file, std::ios::binary);
            res.body.assign(std::istreambuf_iterator<char>(in), {});
            res.content_type = mime_of(file.string());
            res.status = 200;
            return;
        }
    }
    text(json{{"error", "not found"}}, 404);
}

App::App(Config cfg) : impl_(new Impl(std::move(cfg))) {
    impl_->node = node_id_from_seed(impl_->cfg.nmos_seed);
    impl_->domain_id = impl_->cfg.mxl_domain_id.empty() ? domain_id_from_seed(impl_->cfg.nmos_seed).str() : impl_->cfg.mxl_domain_id;
    if (impl_->cfg.font_dir.empty()) {
        if (fs::exists("assets/fonts/DejaVuSans.ttf")) impl_->cfg.font_dir = "assets/fonts";
        else if (fs::exists("/usr/local/share/mxl-test-player/fonts/DejaVuSans.ttf"))
            impl_->cfg.font_dir = "/usr/local/share/mxl-test-player/fonts";
        else impl_->cfg.font_dir = "assets/fonts";
    }
    impl_->web_root = find_web_root(impl_->cfg);
    impl_->mxl.open(impl_->cfg.mxl_domain_dir, impl_->domain_id, impl_->cfg.history_duration_ns);
    impl_->domain_id = impl_->mxl.domain_id();
    for (int i = 0; i < impl_->cfg.outputs; ++i) {
        auto o = std::make_unique<Output>(i, impl_->cfg.output_configs[static_cast<std::size_t>(i)], impl_->cfg.format, impl_->mxl, impl_->library,
                                          impl_->node, impl_->cfg.font_dir, impl_->cfg.ram_clip_max_s,
                                          static_cast<std::uint64_t>(impl_->cfg.ram_budget_mb) << 20);
        o->on_change = [this] { impl_->save_state(); };
        impl_->outputs.push_back(std::move(o));
    }
    impl_->load_state();
    for (auto& o : impl_->outputs) o->start();
    impl_->library.on_change = [this] {
        for (auto& o : impl_->outputs) o->library_changed();
    };
    impl_->library.start();
    impl_->nmos.on_master = [this](const std::string& id, bool en) {
        for (auto& o : impl_->outputs) o->set_master(id, en);
    };
    impl_->refresh_nmos();
    impl_->nmos.start(impl_->cfg.nmos_registry_address, impl_->cfg.nmos_registry_port, impl_->cfg.nmos_query_address,
                      impl_->cfg.nmos_query_port);
    impl_->nmos_http.start(impl_->cfg.nmos_port, [this](const HttpRequest& rq, HttpResponse& rs) { impl_->nmos.handle(rq, rs); });
    impl_->web.start(impl_->cfg.web_port, [this](const HttpRequest& rq, HttpResponse& rs) { impl_->handle(rq, rs); });
    impl_->meter_thread = std::thread([this] {
        while (!impl_->stop) {
            json outs = json::array();
            for (auto& o : impl_->outputs) {
                json m = json::array();
                for (float p : o->meters()) m.push_back(p);
                outs.push_back(json{{"index", o->index()}, {"peaks", m}, {"status", o->status()}});
            }
            json jobs = json::array();
            for (const auto& j : impl_->library.jobs()) jobs.push_back(json{{"id", j.id}, {"state", j.state}, {"progress", j.progress}, {"item_id", j.item_id}});
            impl_->web.broadcast_text(json{{"type", "tick"}, {"outputs", outs}, {"jobs", jobs}}.dump());
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    log_info("web :" + std::to_string(impl_->cfg.web_port) + " nmos :" + std::to_string(impl_->cfg.nmos_port));
}

void App::Impl::shutdown_now() {
    if (shut.exchange(true)) return;
    const int timeout_s = std::max(1, cfg.shutdown_timeout_s);
    std::thread([timeout_s] {
        std::this_thread::sleep_for(std::chrono::seconds(timeout_s));
        _exit(143);
    }).detach();
    stop = true;
    stop_children();
    if (meter_thread.joinable()) meter_thread.join();
    web.stop();
    nmos_http.stop();
    library.stop();
    library.purge_uploads();
    for (auto& o : outputs) o->join();
    save_state();
    nmos.deregister();
    const bool cleanup = cfg.mxl_cleanup_on_exit;
    mxl.close();
    if (cleanup) mxl.remove_own_domain();
}

App::~App() {
    if (!impl_) return;
    impl_->shutdown_now();
    delete impl_;
    impl_ = nullptr;
}

int App::run() {
    while (!impl_->stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    impl_->shutdown_now();
    return impl_->code;
}

void App::request_stop(int code) {
    impl_->code = code;
    impl_->stop = true;
}

}  // namespace mtp
