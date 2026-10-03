#include "media.hpp"

#include "conform.hpp"
#include "decode.hpp"
#include "process.hpp"
#include "util.hpp"
#include "uuid_util.hpp"
#include "v210.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace mtp {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string ext_of(const std::string& path) {
    auto e = fs::path(path).extension().string();
    if (e.empty()) return ".bin";
    return e;
}

json probe_json(const std::string& path) {
    auto r = run_process({"ffprobe", "-v", "error", "-print_format", "json", "-show_format", "-show_streams", path});
    if (r.code != 0) return json::object();
    try {
        return json::parse(r.output);
    } catch (...) {
        return json::object();
    }
}

bool is_animated_codec(const std::string& codec) {
    return codec == "gif" || codec == "apng" || codec == "webp";
}

std::uint64_t dir_bytes(const fs::path& p) {
    std::uint64_t n = 0;
    std::error_code ec;
    if (!fs::exists(p, ec)) return 0;
    for (auto it = fs::recursive_directory_iterator(p, ec); it != fs::recursive_directory_iterator(); ++it) {
        if (it->is_regular_file()) n += static_cast<std::uint64_t>(it->file_size());
    }
    return n;
}

}  // namespace

const char* item_type_name(ItemType t) {
    switch (t) {
        case ItemType::Still: return "still";
        case ItemType::Sprite: return "sprite";
        default: return "video";
    }
}

ItemType parse_item_type(std::string_view s) {
    if (s == "still") return ItemType::Still;
    if (s == "sprite") return ItemType::Sprite;
    return ItemType::Video;
}

Library::Library(Config cfg) : cfg_(std::move(cfg)) {
    std::error_code ec;
    fs::create_directories(cfg_.library_dir, ec);
    load_index();
}

Library::~Library() { stop(); }

void Library::start() {
    stop_ = false;
    const int n = std::max(1, cfg_.convert_concurrency);
    for (int i = 0; i < n; ++i) workers_.emplace_back([this] { worker(); });
    if (!cfg_.import_dir.empty()) watcher_ = std::thread([this] { watch_import(); });
}

void Library::stop() {
    stop_ = true;
    {
        std::lock_guard lock(mu_);
        // wake workers by pushing nothing; they poll stop_
    }
    for (auto& t : workers_)
        if (t.joinable()) t.join();
    workers_.clear();
    if (watcher_.joinable()) watcher_.join();
}

void Library::load_index() {
    const auto path = fs::path(cfg_.library_dir) / "index.json";
    std::ifstream in(path);
    if (!in) return;
    json j;
    try {
        in >> j;
    } catch (...) {
        return;
    }
    if (!j.is_array()) return;
    for (const auto& el : j) {
        LibraryItem it;
        it.id = el.value("id", "");
        it.name = el.value("name", it.id);
        it.type = parse_item_type(el.value("type", "video"));
        if (el.contains("tags")) {
            for (const auto& t : el.at("tags")) it.tags.push_back(t.get<std::string>());
        }
        it.original = el.value("original", json::object());
        it.fit = el.value("fit", "fit");
        it.fps_mode = el.value("fps_mode", "drop");
        it.alpha_mode = el.value("alpha_mode", "straight");
        it.loudness = el.value("loudness", false);
        it.crossfade_ms = el.value("crossfade_ms", 0);
        it.map_channels = el.value("map_channels", 0);
        it.conversions = el.value("conversions", json::object());
        if (!it.id.empty()) items_.push_back(std::move(it));
    }
    bytes_ = dir_bytes(cfg_.library_dir);
}

void Library::save_index() const {
    json arr = json::array();
    for (const auto& it : items_) arr.push_back(to_json(it));
    const auto path = fs::path(cfg_.library_dir) / "index.json";
    std::ofstream out(path);
    out << arr.dump(2);
}

void Library::save_item(const LibraryItem& it) const {
    std::ofstream out(fs::path(item_dir(it.id)) / "item.json");
    out << to_json(it).dump(2);
}

nlohmann::json Library::to_json(const LibraryItem& it) const {
    return json{{"id", it.id},
                {"name", it.name},
                {"type", item_type_name(it.type)},
                {"tags", it.tags},
                {"original", it.original},
                {"fit", it.fit},
                {"fps_mode", it.fps_mode},
                {"alpha_mode", it.alpha_mode},
                {"loudness", it.loudness},
                {"crossfade_ms", it.crossfade_ms},
                {"map_channels", it.map_channels},
                {"conversions", it.conversions},
                {"in_use", it.in_use}};
}

std::string Library::item_dir(const std::string& id) const { return (fs::path(cfg_.library_dir) / id).string(); }

std::vector<LibraryItem> Library::list() const {
    std::lock_guard lock(mu_);
    return items_;
}

LibraryItem Library::get(const std::string& id) const {
    std::lock_guard lock(mu_);
    for (const auto& it : items_)
        if (it.id == id) return it;
    throw Error("item not found");
}

bool Library::exists(const std::string& id) const {
    std::lock_guard lock(mu_);
    for (const auto& it : items_)
        if (it.id == id) return true;
    return false;
}

void Library::enqueue(const std::string& id, const VideoFormat& format) {
    for (auto& j : jobs_) {
        if (j.item_id == id && j.format == format.name && (j.state == "queued" || j.state == "running")) return;
    }
    ConvertJob job;
    job.id = uuid_v5(uuid_namespace_dns(), id + format.name + std::to_string(jobs_.size())).str();
    job.item_id = id;
    job.format = format.name;
    job.state = "queued";
    jobs_.push_back(job);
    for (auto& it : items_) {
        if (it.id == id) {
            it.conversions[format.name] = json{{"status", "queued"}, {"log", ""}};
            save_item(it);
        }
    }
    save_index();
}

LibraryItem Library::ingest_file(const std::string& path, const std::string& name, const nlohmann::json& options, const VideoFormat& format) {
    const auto probed = probe_json(path);
    std::string vcodec, acodec, rate = "0/1", scan = "progressive";
    int w = 0, h = 0, ach = 0;
    double duration = 0;
    int vframes = 0;
    if (probed.contains("format")) duration = std::atof(probed["format"].value("duration", "0").c_str());
    for (const auto& st : probed.value("streams", json::array())) {
        if (st.value("codec_type", "") == "video" && w == 0) {
            vcodec = st.value("codec_name", "");
            w = st.value("width", 0);
            h = st.value("height", 0);
            rate = st.value("avg_frame_rate", st.value("r_frame_rate", "0/1"));
            const auto fo = st.value("field_order", "progressive");
            scan = (fo == "tt" || fo == "tb") ? "interlaced_tff" : (fo == "bb" || fo == "bt") ? "interlaced_bff" : "progressive";
            vframes = std::atoi(st.value("nb_frames", "0").c_str());
        } else if (st.value("codec_type", "") == "audio" && ach == 0) {
            acodec = st.value("codec_name", "");
            ach = st.value("channels", st.value("nb_channels", 0));
        }
    }
    const bool image_codec = vcodec == "png" || vcodec == "mjpeg" || vcodec == "tiff" || vcodec == "webp" || vcodec == "gif" ||
                             vcodec == "apng" || vcodec == "bmp";
    ItemType type = ItemType::Video;
    if (image_codec && (is_animated_codec(vcodec) || vframes > 1)) type = ItemType::Sprite;
    else if (image_codec) type = ItemType::Still;

    LibraryItem it;
    it.id = uuid_v5(uuid_namespace_dns(), path + "|" + name + "|" + std::to_string(fs::file_size(path))).str();
    it.name = name.empty() ? fs::path(path).stem().string() : name;
    it.type = type;
    it.fit = options.value("fit", "fit");
    it.fps_mode = options.value("fps_mode", "drop");
    it.alpha_mode = options.value("alpha_mode", "straight");
    it.loudness = options.value("loudness", false);
    it.crossfade_ms = options.value("crossfade_ms", 0);
    it.map_channels = options.value("map_channels", 0);
    if (options.contains("tags")) {
        for (const auto& t : options.at("tags")) it.tags.push_back(t.get<std::string>());
    }
    it.original = json{{"filename", "original" + ext_of(path)},
                       {"codec", vcodec},
                       {"audio_codec", acodec},
                       {"width", w},
                       {"height", h},
                       {"rate", rate},
                       {"scan", scan},
                       {"duration", duration},
                       {"audio_channels", ach},
                       {"frames", vframes}};
    fs::create_directories(item_dir(it.id));
    fs::copy_file(path, fs::path(item_dir(it.id)) / it.original["filename"].get<std::string>(), fs::copy_options::overwrite_existing);
    {
        std::lock_guard lock(mu_);
        items_.push_back(it);
        enqueue(it.id, format);
        bytes_ = dir_bytes(cfg_.library_dir);
    }
    if (on_change) on_change();
    return it;
}

bool Library::reconvert(const std::string& id, const nlohmann::json& options, const VideoFormat& format, std::string& error) {
    std::lock_guard lock(mu_);
    for (auto& it : items_) {
        if (it.id != id) continue;
        if (options.contains("fit")) it.fit = options.at("fit").get<std::string>();
        if (options.contains("fps_mode")) it.fps_mode = options.at("fps_mode").get<std::string>();
        if (options.contains("alpha_mode")) it.alpha_mode = options.at("alpha_mode").get<std::string>();
        if (options.contains("loudness")) it.loudness = options.at("loudness").get<bool>();
        if (options.contains("crossfade_ms")) it.crossfade_ms = options.at("crossfade_ms").get<int>();
        if (options.contains("map_channels")) it.map_channels = options.at("map_channels").get<int>();
        enqueue(id, format);
        return true;
    }
    error = "not found";
    return false;
}

bool Library::remove(const std::string& id, std::string& error) {
    std::lock_guard lock(mu_);
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        if (it->id != id) continue;
        if (it->in_use) {
            error = "item is in use";
            return false;
        }
        std::error_code ec;
        fs::remove_all(item_dir(id), ec);
        items_.erase(it);
        save_index();
        bytes_ = dir_bytes(cfg_.library_dir);
        return true;
    }
    error = "not found";
    return false;
}

bool Library::update_meta(const std::string& id, const std::string& name, const std::vector<std::string>& tags, std::string& error) {
    std::lock_guard lock(mu_);
    for (auto& it : items_) {
        if (it.id != id) continue;
        if (!name.empty()) it.name = name;
        it.tags = tags;
        save_item(it);
        save_index();
        return true;
    }
    error = "not found";
    return false;
}

void Library::purge_uploads() {
    std::error_code ec;
    fs::remove_all(fs::path(cfg_.library_dir) / "_uploads", ec);
}

void Library::mark_used(const std::string& id, bool used) {
    std::lock_guard lock(mu_);
    for (auto& it : items_)
        if (it.id == id) it.in_use = used;
}

void Library::ensure_format(const std::string& id, const VideoFormat& format) {
    std::lock_guard lock(mu_);
    for (const auto& it : items_) {
        if (it.id != id) continue;
        const auto st = it.conversions.value(format.name, json::object()).value("status", "");
        if (st == "ready" || st == "queued" || st == "running") return;
        enqueue(id, format);
        return;
    }
}

std::vector<ConvertJob> Library::jobs() const {
    std::lock_guard lock(mu_);
    return jobs_;
}

std::uint64_t Library::total_bytes() const {
    std::lock_guard lock(mu_);
    return bytes_;
}

int Library::item_count() const {
    std::lock_guard lock(mu_);
    return static_cast<int>(items_.size());
}

bool Library::wait_ready(const std::string& id, const std::string& format, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard lock(mu_);
            for (const auto& it : items_) {
                if (it.id != id) continue;
                const auto st = it.conversions.value(format, json::object()).value("status", "");
                if (st == "ready") return true;
                if (st == "failed") return false;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

void Library::worker() {
    while (!stop_) {
        ConvertJob job;
        VideoFormat fmt;
        LibraryItem snapshot;
        bool have = false;
        {
            std::lock_guard lock(mu_);
            for (auto& j : jobs_) {
                if (j.state == "queued") {
                    j.state = "running";
                    job = j;
                    have = true;
                    break;
                }
            }
            if (have) {
                auto parsed = parse_format(job.format);
                if (!parsed) {
                    for (auto& j : jobs_)
                        if (j.id == job.id) {
                            j.state = "failed";
                            j.error = "bad format";
                        }
                    have = false;
                } else {
                    fmt = *parsed;
                    for (auto& it : items_)
                        if (it.id == job.item_id) snapshot = it;
                }
            }
        }
        if (!have) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = convert_one(snapshot, fmt, job);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        {
            std::lock_guard lock(mu_);
            for (auto& it : items_) {
                if (it.id == snapshot.id) {
                    it.conversions[fmt.name] = snapshot.conversions[fmt.name];
                    it.conversions[fmt.name]["duration_s"] = secs;
                    save_item(it);
                }
            }
            for (auto& j : jobs_) {
                if (j.id == job.id) {
                    j.state = ok ? "done" : "failed";
                    j.progress = ok ? 1 : j.progress;
                    j.error = job.error;
                    j.duration_s = secs;
                }
            }
            save_index();
            bytes_ = dir_bytes(cfg_.library_dir);
        }
        if (on_change) on_change();
    }
}

bool Library::convert_one(LibraryItem& item, const VideoFormat& format, ConvertJob& job) {
    try {
        const auto dir = fs::path(item_dir(item.id));
        const auto original = dir / item.original.value("filename", "original.bin");
        std::ofstream logf(dir / "convert.log", std::ios::app);
        auto log = [&](const std::string& s) {
            logf << s << "\n";
            job.error.clear();
        };
        if (item.type == ItemType::Still || item.type == ItemType::Sprite) {
            SpriteSequence seq;
            std::string err;
            const bool sprite = item.type == ItemType::Sprite;
            const ScaleMode mode = sprite ? ScaleMode::SpriteMax
                                          : item.fit == "fill" ? ScaleMode::Fill
                                          : item.fit == "center" || item.fit == "1:1" ? ScaleMode::Center
                                                                                      : ScaleMode::Fit;
            const int bw = sprite ? cfg_.sprite_max_px : format.width;
            const int bh = sprite ? cfg_.sprite_max_px : format.height;
            if (!decode_visual(original.string(), mode, bw, bh, seq, err)) {
                job.error = err;
                item.conversions[format.name] = json{{"status", "failed"}, {"error", err}};
                return false;
            }
            if (!sprite && seq.frames.size() > 1) {
                // A multi-frame image uploaded as a still is a sprite.
                item.type = ItemType::Sprite;
            }
            if (item.type == ItemType::Sprite) {
                fs::create_directories(dir / "sprite");
                json frames = json::array();
                double total = 0;
                int i = 0;
                for (const auto& f : seq.frames) {
                    char name[32];
                    std::snprintf(name, sizeof(name), "frame-%04d.rgba", i);
                    std::ofstream out(dir / "sprite" / name, std::ios::binary);
                    out.write(reinterpret_cast<const char*>(f.px.data()), static_cast<std::streamsize>(f.px.size()));
                    const double d = seq.durations[static_cast<std::size_t>(i)];
                    total += d;
                    frames.push_back({{"file", name}, {"duration", d}, {"w", f.w}, {"h", f.h}});
                    ++i;
                }
                json meta{{"frames", frames}, {"total", total}, {"w", seq.w}, {"h", seq.h}};
                std::ofstream mf(dir / "sprite.json");
                mf << meta.dump(2);
                if (!seq.frames.empty()) {
                    // thumb from first frame
                    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(seq.w) * seq.h * 3);
                    for (int p = 0; p < seq.w * seq.h; ++p) {
                        rgb[p * 3] = seq.frames[0].px[p * 4];
                        rgb[p * 3 + 1] = seq.frames[0].px[p * 4 + 1];
                        rgb[p * 3 + 2] = seq.frames[0].px[p * 4 + 2];
                    }
                    write_jpeg((dir / "thumb.jpg").string(), rgb.data(), seq.w, seq.h, 80);
                }
                item.conversions[format.name] = json{{"status", "ready"}, {"frames", seq.frames.size()}, {"sprite", true}};
                log("sprite frames " + std::to_string(seq.frames.size()));
                return true;
            }
            const auto& fr = seq.frames[0];
            // Still was scaled to the target raster.
            std::vector<std::uint8_t> fill(v210_size(format.width, format.height));
            std::vector<std::uint8_t> key(fill.size());
            std::vector<std::uint8_t> a10(alpha10_size(format.width, format.height));
            const bool premul = item.alpha_mode == "premultiplied";
            rgba_to_fill_key(fr.px.data(), fr.w, fr.h, premul, fill.data(), key.data(), a10.data(), 64, 940);
            {
                std::ofstream out(dir / ("frame-" + format.name + ".v210"), std::ios::binary);
                out.write(reinterpret_cast<const char*>(fill.data()), static_cast<std::streamsize>(fill.size()));
            }
            {
                std::ofstream out(dir / ("key-" + format.name + ".v210"), std::ios::binary);
                out.write(reinterpret_cast<const char*>(key.data()), static_cast<std::streamsize>(key.size()));
            }
            {
                std::ofstream out(dir / ("alpha-" + format.name + ".a10"), std::ios::binary);
                out.write(reinterpret_cast<const char*>(a10.data()), static_cast<std::streamsize>(a10.size()));
            }
            v210_to_jpeg((dir / "thumb.jpg").string(), fill.data(), format.width, format.height, 320);
            item.conversions[format.name] = json{{"status", "ready"}, {"frames", 1}, {"has_alpha", true}, {"audio_samples", 0}};
            log("still converted");
            return true;
        }

        // Video mezzanine.
        const bool dst_i = format.interlaced();
        const int prog_fps_num = dst_i ? static_cast<int>(format.frame_rate.num * 2) : static_cast<int>(format.frame_rate.num);
        const int prog_fps_den = static_cast<int>(format.frame_rate.den);
        std::string vf;
        const std::string scan = item.original.value("scan", "progressive");
        const bool src_i = scan.find("interlace") != std::string::npos;
        if (src_i) vf += "bwdif=mode=send_frame,";
        const std::string cs = item.original.value("color_space", "");
        if (cs == "smpte170m" || cs == "bt470bg" || cs == "smpte240m") vf += "colorspace=all=bt709:iall=bt601,";
        if (item.fit == "fill") {
            vf += "scale=" + std::to_string(format.width) + ":" + std::to_string(format.height) +
                  ":force_original_aspect_ratio=increase,crop=" + std::to_string(format.width) + ":" + std::to_string(format.height) + ",";
        } else if (item.fit == "center" || item.fit == "1:1") {
            vf += "pad=" + std::to_string(format.width) + ":" + std::to_string(format.height) + ":(ow-iw)/2:(oh-ih)/2:black,crop=" +
                  std::to_string(format.width) + ":" + std::to_string(format.height) + ",";
        } else {
            vf += "scale=" + std::to_string(format.width) + ":" + std::to_string(format.height) +
                  ":force_original_aspect_ratio=decrease,pad=" + std::to_string(format.width) + ":" + std::to_string(format.height) +
                  ":(ow-iw)/2:(oh-ih)/2:black,";
        }
        const std::string fps = std::to_string(prog_fps_num) + "/" + std::to_string(prog_fps_den);
        if (item.fps_mode == "motion") vf += "minterpolate=fps=" + fps + ":mi_mode=mci,";
        else vf += "fps=" + fps + ",";
        if (dst_i) vf += "tinterlace=mode=interleave_top,";
        vf += "format=yuv422p10le";
        const auto video_only = dir / "video-tmp.mov";
        const auto mezz = dir / ("mezz-" + format.name + ".mov");
        auto pr = run_process({"ffmpeg", "-hide_banner", "-y", "-i", original.string(), "-an", "-vf", vf, "-c:v", "prores_ks", "-profile:v",
                               "3", "-pix_fmt", "yuv422p10le", video_only.string()},
                              [&](std::string_view line) {
                                  auto s = std::string(line);
                                  if (s.rfind("out_time_ms=", 0) == 0) {
                                      // progress is informational
                                  }
                              });
        // ffmpeg writes progress to stdout only with -progress. Parse stderr for errors.
        if (pr.code != 0) {
            job.error = "video transcode failed";
            item.conversions[format.name] = json{{"status", "failed"}, {"error", job.error}, {"log", pr.output}};
            log(pr.output);
            return false;
        }
        const int dst_ch = item.map_channels > 0 ? item.map_channels : std::max(2, item.original.value("audio_channels", 0));
        const int use_ch = std::clamp(dst_ch, 2, 64);
        std::string af = "aresample=48000";
        if (item.loudness) af = "loudnorm=I=-23:TP=-1.5:LRA=11," + af;
        const auto raw = dir / "audio.f32le";
        auto ar = run_process({"ffmpeg", "-hide_banner", "-y", "-i", original.string(), "-vn", "-af", af, "-f", "f32le", "-ac",
                               std::to_string(std::max(1, item.original.value("audio_channels", 0) == 0 ? 1 : item.original.value("audio_channels", 1))),
                               raw.string()});
        std::vector<float> src_audio;
        int src_ch = std::max(1, item.original.value("audio_channels", 0));
        if (item.original.value("audio_channels", 0) == 0) src_ch = 0;
        if (ar.code == 0 && src_ch > 0 && fs::exists(raw)) {
            const auto bytes = fs::file_size(raw);
            src_audio.resize(bytes / sizeof(float));
            std::ifstream in(raw, std::ios::binary);
            in.read(reinterpret_cast<char*>(src_audio.data()), static_cast<std::streamsize>(bytes));
        }
        const auto probed = probe_json(video_only.string());
        std::int64_t frames = 0;
        for (const auto& st : probed.value("streams", json::array())) {
            if (st.value("codec_type", "") == "video") frames = std::atoll(st.value("nb_frames", "0").c_str());
        }
        if (frames <= 0) {
            job.error = "mezzanine has no frames";
            item.conversions[format.name] = json{{"status", "failed"}, {"error", job.error}};
            return false;
        }
        const std::uint64_t need = samples_until_grain(static_cast<std::uint64_t>(frames), format.frame_rate);
        std::vector<float> dst(need * static_cast<std::uint64_t>(use_ch), 0.f);
        const std::uint64_t src_samples = src_ch > 0 ? src_audio.size() / static_cast<std::uint64_t>(src_ch) : 0;
        const int xfade = static_cast<int>(item.crossfade_ms * 48);
        conform_interleaved(src_audio.empty() ? nullptr : src_audio.data(), src_ch, src_samples, dst.data(), use_ch, need, xfade);
        const auto conf = dir / "audio-conformed.f32le";
        {
            std::ofstream out(conf, std::ios::binary);
            out.write(reinterpret_cast<const char*>(dst.data()), static_cast<std::streamsize>(dst.size() * sizeof(float)));
        }
        auto mx = run_process({"ffmpeg", "-hide_banner", "-y", "-i", video_only.string(), "-f", "f32le", "-ar", "48000", "-ac",
                               std::to_string(use_ch), "-i", conf.string(), "-map", "0:v", "-map", "1:a", "-c:v", "copy", "-c:a",
                               "pcm_f32le", "-shortest", mezz.string()});
        if (mx.code != 0) {
            job.error = "mux failed";
            item.conversions[format.name] = json{{"status", "failed"}, {"error", job.error}, {"log", mx.output}};
            log(mx.output);
            return false;
        }
        run_process({"ffmpeg", "-hide_banner", "-y", "-i", mezz.string(), "-vf", "scale=320:-1", "-frames:v", "1", (dir / "thumb.jpg").string()});
        std::error_code ec;
        fs::remove(video_only, ec);
        fs::remove(raw, ec);
        fs::remove(conf, ec);
        item.conversions[format.name] = json{{"status", "ready"},
                                             {"frames", frames},
                                             {"audio_samples", need},
                                             {"audio_channels", use_ch},
                                             {"mezz", mezz.filename().string()},
                                             {"has_alpha", false}};
        log("mezzanine frames " + std::to_string(frames) + " samples " + std::to_string(need));
        return true;
    } catch (const std::exception& ex) {
        job.error = ex.what();
        item.conversions[format.name] = json{{"status", "failed"}, {"error", job.error}};
        return false;
    }
}

void Library::watch_import() {
    std::map<std::string, std::uint64_t> seen;
    while (!stop_) {
        std::error_code ec;
        if (fs::exists(cfg_.import_dir, ec)) {
            for (const auto& ent : fs::directory_iterator(cfg_.import_dir, ec)) {
                if (!ent.is_regular_file()) continue;
                const auto p = ent.path().string();
                const auto sz = ent.file_size();
                auto it = seen.find(p);
                if (it != seen.end() && it->second == sz) continue;
                // Wait until the size is stable.
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                std::error_code ec2;
                const auto sz2 = fs::file_size(p, ec2);
                if (ec2 || sz2 != sz) continue;
                seen[p] = sz;
                try {
                    ingest_file(p, ent.path().filename().string(), json::object(), cfg_.format);
                    log_info("imported " + p);
                } catch (const std::exception& ex) {
                    log_warn(std::string("import failed: ") + ex.what());
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

}  // namespace mtp
