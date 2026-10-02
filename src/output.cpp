#include "output.hpp"

#include "anc.hpp"
#include "audio.hpp"
#include "decode.hpp"
#include "overlay.hpp"
#include "pattern.hpp"
#include "playlist.hpp"
#include "v210.hpp"

#include <mxl/time.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace mtp {
namespace {

namespace fs = std::filesystem;

std::string hostname() {
    char buf[256] = {};
    if (gethostname(buf, sizeof(buf) - 1) != 0) return "player";
    return buf;
}

std::string clock_string(bool local) {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    if (local) localtime_r(&now, &tm);
    else gmtime_r(&now, &tm);
    char b[32];
    std::strftime(b, sizeof(b), "%H:%M:%S", &tm);
    return b;
}

int parse_tc_frames(const std::string& tc, int fps) {
    // HH:MM:SS:FF or HH:MM:SS;FF
    if (tc.size() < 11) return 0;
    auto grab = [&](int i) { return (tc[i] - '0') * 10 + (tc[i + 1] - '0'); };
    Timecode t;
    t.hh = grab(0);
    t.mm = grab(3);
    t.ss = grab(6);
    t.ff = grab(9);
    t.fps = fps;
    t.drop = tc.size() > 8 && tc[8] == ';';
    return static_cast<int>(timecode_to_frames(t));
}

}  // namespace

SourceDesc source_from_json(const nlohmann::json& j) {
    SourceDesc s;
    s.type = j.value("type", "pattern");
    s.pattern = j.value("pattern", "smpte_rp219");
    s.pluge = j.value("pluge", false);
    s.audio = j.value("audio", "sine");
    s.frequency = j.value("frequency", 1000.0);
    s.level_dbfs = j.value("level_dbfs", -18.0);
    s.sync_beep = j.value("sync_beep", s.pattern == "av_sync");
    s.item_id = j.value("item_id", j.value("item", ""));
    s.playlist_id = j.value("playlist_id", j.value("playlist", ""));
    if (j.contains("entries")) {
        for (const auto& e : j.at("entries")) {
            PlaylistEntry pe;
            pe.item_id = e.value("item_id", "");
            pe.loops = e.value("loops", 1);
            pe.frames = e.value("frames", 1);
            s.entries.push_back(pe);
        }
    }
    return s;
}

nlohmann::json source_to_json(const SourceDesc& s) {
    return nlohmann::json{{"type", s.type},
                          {"pattern", s.pattern},
                          {"pluge", s.pluge},
                          {"audio", s.audio},
                          {"frequency", s.frequency},
                          {"level_dbfs", s.level_dbfs},
                          {"sync_beep", s.sync_beep},
                          {"item_id", s.item_id},
                          {"playlist_id", s.playlist_id}};
}

Output::Output(int index, OutputConfig cfg, VideoFormat platform, MxlSession& mxl, Library& library, Uuid node, std::string font_dir,
               double ram_max_s, std::uint64_t ram_budget_bytes)
    : index_(index),
      mxl_(mxl),
      library_(library),
      node_(std::move(node)),
      font_dir_(std::move(font_dir)),
      platform_(std::move(platform)),
      ram_max_s_(ram_max_s),
      ram_budget_(ram_budget_bytes),
      cfg_(std::move(cfg)) {
    fmt_ = cfg_.format.empty() ? platform_ : *parse_format(cfg_.format);
    ids_ = derive_output_ids(node_, index_, fmt_, cfg_.audio_channels, cfg_.key_mode == KeyMode::V210a);
    loop_ = cfg_.loop;
    source_.pattern = "smpte_rp219";
    source_.audio = "sine";
}

Output::~Output() { join(); }

void Output::start() {
    stop_ = false;
    loader_ = std::thread([this] { loader_main(); });
    thread_ = std::thread([this] { writer_main(); });
}

void Output::join() {
    stop_ = true;
    if (loader_.joinable()) loader_.join();
    if (thread_.joinable()) thread_.join();
}

OutputIds Output::ids() const {
    std::lock_guard lock(mu_);
    return ids_;
}

VideoFormat Output::format() const {
    std::lock_guard lock(mu_);
    return fmt_;
}

AudioProgram Output::program_for(const SourceDesc& src, int channels) const {
    const auto sig = parse_audio_signal(src.audio);
    AudioProgram p;
    if (sig == AudioSignal::IdentFreq) p = make_ident_freq_program(channels, src.level_dbfs);
    else if (sig == AudioSignal::IdentBeeps) p = make_ident_beep_program(channels, src.level_dbfs);
    else if (sig == AudioSignal::IdentEbu) p = make_ebu_ident_program(channels, src.level_dbfs);
    else p = make_uniform_program(channels, sig, src.frequency, src.level_dbfs);
    p.sync_beep = src.sync_beep || src.pattern == "av_sync" || sig == AudioSignal::SyncBeep;
    return p;
}

Output::Media Output::load_media(const SourceDesc& src, const VideoFormat& fmt) const {
    Media media;
    if (src.type != "video" && src.type != "still") return media;
    if (src.item_id.empty() || !library_.exists(src.item_id)) return media;
    LibraryItem item;
    try {
        item = library_.get(src.item_id);
    } catch (...) {
        return media;
    }
    media.name = item.name;
    media.item_id = item.id;
    media.format = fmt.name;
    const auto dir = fs::path(library_.item_dir(item.id));
    if (item.type == ItemType::Still) {
        const auto conv = item.conversions.value(fmt.name, nlohmann::json::object());
        if (conv.value("status", "") != "ready") {
            library_.ensure_format(item.id, fmt);
            return media;
        }
        std::ifstream in(dir / ("frame-" + fmt.name + ".v210"), std::ios::binary);
        media.still_fill.assign(std::istreambuf_iterator<char>(in), {});
        std::ifstream kin(dir / ("key-" + fmt.name + ".v210"), std::ios::binary);
        media.still_key.assign(std::istreambuf_iterator<char>(kin), {});
        std::ifstream ain(dir / ("alpha-" + fmt.name + ".a10"), std::ios::binary);
        media.still_a10.assign(std::istreambuf_iterator<char>(ain), {});
        media.still = true;
        media.ready = !media.still_fill.empty();
        media.frames = 1;
        return media;
    }
    if (item.type != ItemType::Video) return media;
    const auto conv = item.conversions.value(fmt.name, nlohmann::json::object());
    if (conv.value("status", "") != "ready") {
        library_.ensure_format(item.id, fmt);
        return media;
    }
    media.frames = conv.value("frames", 1);
    media.channels = conv.value("audio_channels", 0);
    if (media.channels <= 0) {
        std::lock_guard lock(mu_);
        media.channels = cfg_.audio_channels;
    }
    if (item.original.contains("timecode")) media.tc_start = item.original.value("timecode", "00:00:00:00");
    const double seconds = fmt.frame_rate.to_double() > 0 ? static_cast<double>(media.frames) / fmt.frame_rate.to_double() : 0;
    const auto mezz = dir / conv.value("mezz", "mezz-" + fmt.name + ".mov");
    const std::uint64_t bytes = static_cast<std::uint64_t>(media.frames) * v210_size(fmt.width, fmt.height);
    if (seconds <= ram_max_s_ && bytes <= ram_budget_) {
        std::string err;
        if (load_mezzanine(mezz.string(), media.clip, err)) {
            media.ram = true;
            media.ready = true;
            media.frames = media.clip.frames;
            media.channels = media.clip.channels;
            return media;
        }
        log_warn("ram load failed: " + err);
    }
    media.ready = fs::exists(mezz);
    media.ram = false;
    return media;
}

void Output::loader_main() {
    int seen = -1;
    while (!stop_) {
        SourceDesc src;
        VideoFormat fmt;
        int ticket = 0;
        {
            std::lock_guard lock(mu_);
            src = load_override_ ? load_src_ : source_;
            fmt = fmt_;
            ticket = media_gen_;
        }
        if (ticket == seen) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        Media loaded = load_media(src, fmt);
        {
            std::lock_guard lock(mu_);
            if (media_gen_ == ticket) {
                published_ = std::make_shared<const Media>(std::move(loaded));
                const auto pub = published_.load();
                ram_bytes_ = 0;
                if (pub && pub->ram) ram_bytes_ = pub->clip.v210.size() + pub->clip.audio.size() * sizeof(float);
                seen = ticket;
            }
        }
    }
}

void Output::reopen(const OutputConfig& cfg, const VideoFormat& fmt) {
    if (flows_open_) {
        video_.close(mxl_);
        audio_.close(mxl_);
        data_.close(mxl_);
        key_.close(mxl_);
        flows_open_ = false;
    }
    ids_ = derive_output_ids(node_, index_, fmt, cfg.audio_channels, cfg.key_mode == KeyMode::V210a);
    const std::string group = cfg.label;
    try {
        video_.open(mxl_, video_flow_json(ids_.video_flow, cfg.label, group, "Video", fmt, cfg.key_mode == KeyMode::V210a, ids_.video_source,
                                          ids_.device));
        audio_.open(mxl_, audio_flow_json(ids_.audio_flow, cfg.label + " audio", group, cfg.audio_channels, ids_.audio_source, ids_.device));
        if (cfg.anc) {
            data_.open(mxl_, data_flow_json(ids_.data_flow, cfg.label + " data", group, fmt, ids_.data_source, ids_.device));
        }
        if (cfg.key_mode == KeyMode::FillKey) {
            key_.open(mxl_, video_flow_json(ids_.key_flow, cfg.label + " key", group, "Key", fmt, false, ids_.key_source, ids_.device));
        }
        flows_open_ = true;
    } catch (const std::exception& ex) {
        log_error(std::string("flow open failed: ") + ex.what());
    }
}

void Output::writer_main() {
    OverlayRenderer overlay(font_dir_);
    std::uint64_t last_index = 0;
    bool have_last = false;
    std::vector<std::uint8_t> held;
    MezzanineReader reader;
    std::string reader_path;
    std::int64_t last_loop_index = 0;
    while (!stop_) {
        OutputConfig cfg;
        SourceDesc src;
        VideoFormat fmt;
        int gen = 0;
        std::string transport;
        bool loop = true;
        {
            std::lock_guard lock(mu_);
            cfg = cfg_;
            src = source_;
            fmt = fmt_;
            gen = config_gen_;
            transport = transport_;
            loop = loop_;
        }
        if (gen != applied_gen_) {
            reopen(cfg, fmt);
            applied_gen_ = gen;
            have_last = false;
            {
                std::lock_guard lock(mu_);
                origin_valid_ = false;
                media_gen_++;
            }
            reader.close();
            reader_path.clear();
        }
        auto media = published_.load();
        if (!media) media = std::make_shared<const Media>();
        const Rational gr = fmt.grain_rate();
        mxlRational rr{gr.num, gr.den};
        const std::uint64_t now = mxlGetCurrentIndex(&rr);
        if (have_last && now <= last_index) {
            const auto ns = mxlGetNsUntilIndex(last_index + 1, &rr);
            if (ns != MXL_UNDEFINED_INDEX && ns > 0) mxlSleepForNs(std::min<std::uint64_t>(ns, 20000000ull));
            continue;
        }
        if (have_last && now > last_index + 1) underruns_ += now - last_index - 1;
        const std::uint64_t idx = now;
        const bool inter = fmt.interlaced();
        const std::uint64_t abs_frame = inter ? idx / 2 : idx;
        std::int64_t media_frame = 0;
        int field = inter ? static_cast<int>(idx & 1u) : -1;
        std::int64_t loop_index = 0;
        bool idle = transport == "stop";
        {
            std::lock_guard lock(mu_);
            if (!origin_valid_) {
                origin_frame_index_ = static_cast<std::int64_t>(abs_frame);
                play_origin_grain_ = idx;
                origin_valid_ = true;
            }
            const std::int64_t delta = static_cast<std::int64_t>(abs_frame) - origin_frame_index_;
            if (transport == "pause") media_frame = hold_frame_;
            else if (transport == "stop") media_frame = 0;
            else media_frame = origin_media_ + std::max<std::int64_t>(0, delta);
        }
        const std::int64_t dur = (src.type == "video" && media->frames > 0) ? media->frames : 0;
        if (dur > 0 && transport == "play") {
            if (loop) {
                loop_index = media_frame / dur;
                media_frame = media_frame % dur;
            } else if (media_frame >= dur) {
                media_frame = dur - 1;
                idle = false;
            }
        }
        if (src.type == "playlist" && !src.entries.empty()) {
            const auto loc = locate_playlist(src.entries, std::max<std::int64_t>(0, media_frame));
            loop_index = loc.loop_index;
            media_frame = loc.frame_in_item;
            const auto& entry = src.entries[static_cast<std::size_t>(loc.entry)];
            if (media->item_id != entry.item_id) {
                std::lock_guard lock(mu_);
                if (load_src_.item_id != entry.item_id) {
                    load_src_ = src;
                    load_src_.item_id = entry.item_id;
                    load_src_.type = "video";
                    try {
                        const auto item = library_.get(entry.item_id);
                        if (item.type == ItemType::Still) load_src_.type = "still";
                    } catch (...) {
                    }
                    load_override_ = true;
                    media_gen_++;
                }
            }
            src.type = media->still ? "still" : (media->ready ? "video" : src.type);
            src.item_id = entry.item_id;
        }
        if (loop_index != last_loop_index) {
            if (loop_index > last_loop_index) loops_ += static_cast<std::uint64_t>(loop_index - last_loop_index);
            last_loop_index = loop_index;
        }

        const std::size_t full_bytes = v210_size(fmt.width, fmt.height);
        std::vector<std::uint8_t> full(full_bytes);
        std::vector<std::uint8_t> key(full_bytes);
        std::vector<std::uint8_t> a10(alpha10_size(fmt.width, fmt.height));
        const int key_y = cfg.idle_key == IdleKey::Transparent ? cfg.key_min : cfg.key_max;
        fill_v210(key.data(), fmt.width, fmt.height, key_y, kCMid, kCMid);
        bool keyed_still = false;
        std::string item_name;
        if (idle || src.type == "pattern" || !media->ready) {
            if (!idle && src.type == "pattern") {
                PatternRequest req;
                req.format = fmt;
                req.pattern = parse_video_pattern(src.pattern);
                req.pluge = src.pluge;
                req.frame_index = static_cast<std::uint64_t>(std::max<std::int64_t>(0, media_frame));
                req.field = -1;
                req.flash = (src.sync_beep || src.pattern == "av_sync") && is_sync_frame(req.frame_index, fmt.frame_rate);
                render_pattern(req, full.data());
            } else {
                fill_v210(full.data(), fmt.width, fmt.height, kYBlack, kCMid, kCMid);
                // Not ready yet means the loader is still decoding. Keep black on time;
                // that wait is not an underrun.
            }
        } else if (media->still) {
            keyed_still = true;
            if (media->still_fill.size() == full.size()) std::memcpy(full.data(), media->still_fill.data(), full.size());
            if (media->still_key.size() == key.size()) std::memcpy(key.data(), media->still_key.data(), key.size());
            if (media->still_a10.size() == a10.size()) std::memcpy(a10.data(), media->still_a10.data(), a10.size());
            item_name = media->name;
        } else if (media->ram && media->clip.frames > 0) {
            const std::int64_t f = std::clamp<std::int64_t>(media_frame, 0, media->clip.frames - 1);
            const std::size_t off = static_cast<std::size_t>(f) * full.size();
            if (off + full.size() <= media->clip.v210.size()) std::memcpy(full.data(), media->clip.v210.data() + off, full.size());
            item_name = media->name;
        } else {
            fill_v210(full.data(), fmt.width, fmt.height, kYBlack, kCMid, kCMid);
        }

        PlaceholderVars vars;
        vars.label = cfg.label;
        vars.host = hostname();
        vars.item = item_name.empty() ? video_pattern_name(parse_video_pattern(src.pattern)) : item_name;
        vars.loop = std::to_string(loop_index);
        vars.frame = std::to_string(abs_frame);
        vars.utc = clock_string(false);
        vars.local = clock_string(true);
        vars.flow = ids_.video_flow.str().substr(0, 8);
        vars.tai = std::to_string(index_to_timestamp_ns(idx, gr));
        TimecodeQuery tq;
        tq.source = cfg.tc_source;
        tq.drop_frame = cfg.drop_frame;
        tq.free_start_frame = cfg.free_start_frame;
        tq.since_start_frame = std::max<std::int64_t>(0, media_frame);
        tq.media_frame = std::max<std::int64_t>(0, media_frame);
        tq.media_start_frame = parse_tc_frames(media->tc_start, timecode_fps(fmt.frame_rate));
        std::tm tm{};
        const std::time_t wall = std::time(nullptr);
        localtime_r(&wall, &tm);
        tq.tz_offset_seconds = static_cast<int>(tm.tm_gmtoff);
        const Timecode tc = timecode_for_grain(idx, fmt, tq);
        vars.timecode = tc.format();

        OverlayFrame of;
        of.format = fmt;
        of.burnin = cfg.burnin;
        of.vars = vars;
        of.keyed_still = keyed_still;
        of.grain_index = idx;
        for (int i = 0; i < std::min(2, static_cast<int>(cfg.burnin.boxes.size())); ++i) {
            const auto& box = cfg.burnin.boxes[static_cast<std::size_t>(i)];
            if (!box.enabled || box.content == "builtin" || box.content.empty()) continue;
            if (sprites_[i].id != box.content) {
                sprites_[i] = {};
                sprites_[i].id = box.content;
                try {
                    const auto dir = fs::path(library_.item_dir(box.content));
                    std::ifstream in(dir / "sprite.json");
                    nlohmann::json meta;
                    in >> meta;
                    sprites_[i].w = meta.value("w", 0);
                    sprites_[i].h = meta.value("h", 0);
                    for (const auto& fr : meta.value("frames", nlohmann::json::array())) {
                        std::ifstream f(dir / "sprite" / fr.value("file", ""), std::ios::binary);
                        std::vector<std::uint8_t> px(std::istreambuf_iterator<char>(f), {});
                        sprites_[i].frames.push_back(std::move(px));
                        sprites_[i].durations.push_back(fr.value("duration", 0.1));
                    }
                } catch (...) {
                    sprites_[i].frames.clear();
                }
            }
            if (!sprites_[i].frames.empty()) {
                double total = 0;
                const double t = static_cast<double>(index_to_timestamp_ns(idx, gr)) / 1e9;
                const int fi = sprite_frame_at_time(t, sprites_[i].durations.data(), static_cast<int>(sprites_[i].durations.size()), total);
                of.sprites[i].rgba = sprites_[i].frames[static_cast<std::size_t>(fi)].data();
                of.sprites[i].w = sprites_[i].w;
                of.sprites[i].h = sprites_[i].h;
            }
        }
        overlay.apply(full.data(), (cfg.key_mode != KeyMode::Off) ? key.data() : nullptr, of);

        std::vector<std::uint8_t> grain;
        std::vector<std::uint8_t> key_grain;
        if (inter) {
            grain.resize(v210_size(fmt.width, fmt.field_height()));
            key_grain.resize(grain.size());
            extract_v210_field(full.data(), fmt.width, fmt.height, field, grain.data());
            extract_v210_field(key.data(), fmt.width, fmt.height, field, key_grain.data());
        } else {
            grain = full;
            key_grain = key;
        }
        const bool write_v = master_v_.load();
        const bool write_a = master_a_.load();
        const bool write_d = master_d_.load() && cfg.anc;
        const bool write_k = master_k_.load() && cfg.key_mode == KeyMode::FillKey;
        bool late = false;
        if (write_v && flows_open_) {
            if (cfg.key_mode == KeyMode::V210a) {
                std::vector<std::uint8_t> both = grain;
                const int gh = inter ? fmt.field_height() : fmt.height;
                const std::size_t asz = alpha10_size(fmt.width, gh);
                std::vector<std::uint8_t> apart(asz);
                if (inter) {
                    const std::uint32_t stride = alpha10_line_stride(fmt.width);
                    int o = 0;
                    for (int y = field & 1; y < fmt.height; y += 2) {
                        if (static_cast<std::size_t>(y + 1) * stride <= a10.size())
                            std::memcpy(apart.data() + static_cast<std::size_t>(o) * stride, a10.data() + static_cast<std::size_t>(y) * stride, stride);
                        ++o;
                    }
                } else if (a10.size() >= asz) {
                    std::memcpy(apart.data(), a10.data(), asz);
                }
                both.insert(both.end(), apart.begin(), apart.end());
                late = !video_.write_video(idx, both.data(), both.size());
            } else {
                late = !video_.write_video(idx, grain.data(), grain.size());
            }
        }
        if (write_k && flows_open_) key_.write_video(idx, key_grain.data(), key_grain.size());

        const int channels = cfg.audio_channels;
        const std::uint32_t count = samples_in_grain(idx, gr);
        const std::uint64_t sample_index = samples_until_grain(idx, gr);
        std::vector<float> planar(static_cast<std::size_t>(channels) * count, 0.f);
        const bool silence = idle || (transport == "pause" && cfg.pause_audio == PauseAudio::Silence) ||
                             (src.type == "still" && src.audio == "silence");
        if (!silence && (src.type == "pattern" || transport == "pause")) {
            const bool flash = (src.sync_beep || src.pattern == "av_sync") && is_sync_frame(abs_frame, fmt.frame_rate) && field <= 0;
            render_audio(program_for(src, channels), sample_index, static_cast<int>(count), flash, planar.data());
        } else if (!silence && media->ram && media->clip.channels > 0 && media->clip.audio_samples > 0) {
            const Rational fr = fmt.frame_rate;
            const std::uint64_t base = samples_until_grain(static_cast<std::uint64_t>(std::max<std::int64_t>(0, media_frame)), fr);
            std::uint64_t offset = base;
            if (field == 1) offset += samples_in_grain(abs_frame * 2, gr);
            const int sch = media->clip.channels;
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::uint64_t s = (offset + i) % media->clip.audio_samples;
                for (int c = 0; c < channels; ++c) {
                    float v = 0;
                    if (c < sch) v = media->clip.audio[s * static_cast<std::uint64_t>(sch) + c];
                    planar[static_cast<std::size_t>(c) * count + i] = v;
                }
            }
        } else if (!silence && src.type == "still") {
            render_audio(program_for(src, channels), sample_index, static_cast<int>(count), false, planar.data());
        }
        if (write_a && flows_open_) {
            if (!audio_.write_audio(sample_index, planar.data(), channels, static_cast<int>(count))) late = true;
        }
        if (write_d && flows_open_) {
            AncPacket pkt;
            pkt.tc = tc;
            pkt.kind = cfg.atc;
            pkt.interlaced = inter;
            pkt.sequence = static_cast<std::uint16_t>(idx);
            auto grain_anc = encode_anc_grain(pkt);
            data_.write_data(idx, grain_anc.data(), grain_anc.size());
        }
        if (late) underruns_++;
        grains_++;
        last_index = idx;
        have_last = true;
        held = grain;
        if ((idx % 5) == 0) {
            std::lock_guard lock(thumb_mu_);
            // Keep a small JPEG of the full frame.
            std::string tmp = "/tmp";
            (void)tmp;
            std::vector<std::uint8_t> jpg_mem;
            // Encode via a temp path under the library parent is unnecessary; write into thumb_ as jpeg bytes.
            const std::string path = std::string("/tmp/mtp-thumb-") + std::to_string(index_) + ".jpg";
            if (v210_to_jpeg(path, full.data(), fmt.width, fmt.height, 320)) {
                std::ifstream in(path, std::ios::binary);
                thumb_.assign(std::istreambuf_iterator<char>(in), {});
            }
        }
        {
            std::lock_guard lock(meter_mu_);
            meters_.assign(static_cast<std::size_t>(channels), 0.f);
            for (int c = 0; c < channels; ++c) {
                float peak = 0;
                const float* p = planar.data() + static_cast<std::size_t>(c) * count;
                for (std::uint32_t i = 0; i < count; ++i) peak = std::max(peak, std::fabs(p[i]));
                meters_[static_cast<std::size_t>(c)] = peak;
            }
        }
        (void)held;
    }
    if (flows_open_) {
        video_.close(mxl_);
        audio_.close(mxl_);
        data_.close(mxl_);
        key_.close(mxl_);
        flows_open_ = false;
    }
}

nlohmann::json Output::status() const {
    std::lock_guard lock(mu_);
    auto media = published_.load();
    if (!media) media = std::make_shared<const Media>();
    const double fps = fmt_.frame_rate.to_double();
    double progress = 0;
    double remain = 0;
    if (media->frames > 1 && fps > 0) {
        const std::int64_t frame = transport_ == "pause" ? hold_frame_ : 0;
        progress = media->frames ? static_cast<double>(frame % media->frames) / media->frames : 0;
        remain = media->frames / fps;
        (void)frame;
    }
    return nlohmann::json{{"index", index_},
                          {"label", cfg_.label},
                          {"format", fmt_.name},
                          {"audio_channels", cfg_.audio_channels},
                          {"transport", transport_},
                          {"loop", loop_},
                          {"source", source_to_json(source_)},
                          {"burnin", burnin_to_json(cfg_.burnin)},
                          {"key_mode", key_mode_name(cfg_.key_mode)},
                          {"anc", cfg_.anc},
                          {"tc_source", tc_source_name(cfg_.tc_source)},
                          {"drop_frame", cfg_.drop_frame},
                          {"atc", atc_kind_name(cfg_.atc)},
                          {"video_flow_id", ids_.video_flow.str()},
                          {"audio_flow_id", ids_.audio_flow.str()},
                          {"data_flow_id", ids_.data_flow.str()},
                          {"key_flow_id", ids_.key_flow.str()},
                          {"video_sender_id", ids_.video_sender.str()},
                          {"audio_sender_id", ids_.audio_sender.str()},
                          {"data_sender_id", ids_.data_sender.str()},
                          {"key_sender_id", ids_.key_sender.str()},
                          {"grains", grains_.load()},
                          {"underruns", underruns_.load()},
                          {"loops", loops_.load()},
                          {"decode_ahead", ahead_.load()},
                          {"ram_bytes", ram_bytes_.load()},
                          {"progress", progress},
                          {"remaining_s", remain},
                          {"media_ready", media->ready},
                          {"item", media->name}};
}

nlohmann::json Output::ids_json() const { return status(); }

void Output::command(const std::string& action) {
    std::function<void()> cb;
    {
        std::lock_guard lock(mu_);
        if (action == "play") {
            transport_ = "play";
            origin_valid_ = false;
            origin_media_ = hold_frame_;
        } else if (action == "pause") {
            transport_ = "pause";
        } else if (action == "stop") {
            transport_ = "stop";
            hold_frame_ = 0;
            origin_media_ = 0;
            origin_valid_ = false;
        } else if (action == "restart") {
            transport_ = "play";
            origin_media_ = 0;
            hold_frame_ = 0;
            origin_valid_ = false;
        } else if (action == "step") {
            transport_ = "pause";
            hold_frame_++;
        }
        cb = on_change;
    }
    if (cb) cb();
}

void Output::set_source(const SourceDesc& src) {
    std::function<void()> cb;
    {
        std::lock_guard lock(mu_);
        source_ = src;
        if (src.pattern == "av_sync") source_.sync_beep = true;
        origin_valid_ = false;
        origin_media_ = 0;
        hold_frame_ = 0;
        load_override_ = false;
        // Reload the picture without recreating MXL flows.
        media_gen_++;
        cb = on_change;
    }
    if (cb) cb();
}

void Output::set_burnin(const BurnInConfig& b) {
    std::function<void()> cb;
    {
        std::lock_guard lock(mu_);
        cfg_.burnin = b;
        if (cfg_.burnin.texts.size() > 8) cfg_.burnin.texts.resize(8);
        if (cfg_.burnin.boxes.size() > 2) cfg_.burnin.boxes.resize(2);
        cb = on_change;
    }
    if (cb) cb();
}

void Output::patch(const nlohmann::json& body) {
    std::function<void()> cb;
    {
    std::lock_guard lock(mu_);
    bool reopen = false;
    if (body.contains("label")) {
        cfg_.label = body.at("label").get<std::string>();
        reopen = true;
    }
    if (body.contains("format")) {
        cfg_.format = body.at("format").get<std::string>();
        auto f = parse_format(cfg_.format);
        if (!f) throw Error("invalid format");
        fmt_ = *f;
        reopen = true;
    }
    if (body.contains("audio_channels")) {
        cfg_.audio_channels = body.at("audio_channels").get<int>();
        reopen = true;
    }
    if (body.contains("key_mode")) {
        cfg_.key_mode = parse_key_mode(body.at("key_mode").get<std::string>());
        reopen = true;
    }
    if (body.contains("anc")) {
        cfg_.anc = body.at("anc").get<bool>();
        reopen = true;
    }
    if (body.contains("tc_source")) cfg_.tc_source = parse_tc_source(body.at("tc_source").get<std::string>());
    if (body.contains("drop_frame")) cfg_.drop_frame = body.at("drop_frame").get<bool>();
    if (body.contains("atc")) cfg_.atc = parse_atc_kind(body.at("atc").get<std::string>());
    if (body.contains("loop")) loop_ = cfg_.loop = body.at("loop").get<bool>();
    if (body.contains("pause_audio")) {
        const auto s = body.at("pause_audio").get<std::string>();
        cfg_.pause_audio = (s == "hold" || s == "hold_tone") ? PauseAudio::HoldTone : PauseAudio::Silence;
    }
    if (body.contains("idle_key")) cfg_.idle_key = body.at("idle_key").get<std::string>() == "transparent" ? IdleKey::Transparent : IdleKey::Opaque;
    if (body.contains("burnin")) cfg_.burnin = burnin_from_json(body.at("burnin"));
    if (reopen) {
        ids_ = derive_output_ids(node_, index_, fmt_, cfg_.audio_channels, cfg_.key_mode == KeyMode::V210a);
        config_gen_++;
        origin_valid_ = false;
    }
    cb = on_change;
    }
    if (cb) cb();
}

void Output::seek(std::int64_t frame) {
    std::function<void()> cb;
    {
        std::lock_guard lock(mu_);
        if (frame < 0) frame = 0;
        origin_media_ = frame;
        hold_frame_ = frame;
        origin_valid_ = false;
        cb = on_change;
    }
    if (cb) cb();
}

void Output::set_master(const std::string& sender_id, bool enabled) {
    OutputIds id;
    {
        std::lock_guard lock(mu_);
        id = ids_;
    }
    if (sender_id == id.video_sender.str()) master_v_ = enabled;
    else if (sender_id == id.audio_sender.str()) master_a_ = enabled;
    else if (sender_id == id.data_sender.str()) master_d_ = enabled;
    else if (sender_id == id.key_sender.str()) master_k_ = enabled;
}

std::vector<std::uint8_t> Output::thumbnail() const {
    std::lock_guard lock(thumb_mu_);
    return thumb_;
}

std::vector<float> Output::meters() const {
    std::lock_guard lock(meter_mu_);
    return meters_;
}

}  // namespace mtp
