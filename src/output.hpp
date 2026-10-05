#pragma once

#include "config.hpp"
#include "decode.hpp"
#include "ids.hpp"
#include "media.hpp"
#include "mxl_io.hpp"
#include "playlist.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mtp {

struct SourceDesc {
    std::string type = "pattern";
    std::string pattern = "smpte_rp219";
    bool pluge = false;
    std::string audio = "sine";
    double frequency = 1000;
    double level_dbfs = -18;
    bool sync_beep = false;
    std::string item_id;
    std::string playlist_id;
    std::vector<PlaylistEntry> entries;
};

class Output {
public:
    Output(int index, OutputConfig cfg, VideoFormat platform, MxlSession& mxl, Library& library, Uuid node, std::string font_dir,
           double ram_max_s, std::uint64_t ram_budget_bytes);
    ~Output();
    void start();
    void join();

    int index() const { return index_; }
    nlohmann::json status() const;
    nlohmann::json ids_json() const;
    void command(const std::string& action);
    void set_source(const SourceDesc& src);
    void set_burnin(const BurnInConfig& b);
    void patch(const nlohmann::json& body);
    void seek(std::int64_t frame);
    void set_master(const std::string& sender_id, bool enabled);
    std::vector<std::uint8_t> thumbnail() const;
    std::vector<float> meters() const;
    OutputIds ids() const;
    // flow_def.json of every flow this output writes (also its IS-04 flows).
    std::vector<std::string> flow_definitions() const;
    VideoFormat format() const;
    std::uint64_t grains() const { return grains_.load(); }
    std::uint64_t underruns() const { return underruns_.load(); }
    std::uint64_t loops_done() const { return loops_.load(); }
    int ahead() const { return ahead_.load(); }
    std::uint64_t ram_bytes() const { return ram_bytes_.load(); }

    std::function<void()> on_change;

private:
    struct Media {
        bool ready = false;
        bool ram = false;
        bool still = false;
        std::string item_id;
        std::string format;
        RamClip clip;
        std::vector<std::uint8_t> still_fill;
        std::vector<std::uint8_t> still_key;
        std::vector<std::uint8_t> still_a10;
        std::int64_t frames = 1;
        int channels = 0;
        std::string name;
        std::string tc_start = "00:00:00:00";
    };

    void writer_main();
    void loader_main();
    struct FlowDefs {
        std::string video, audio, data, key;  // empty when the flow is off
    };
    static FlowDefs flow_defs(const OutputIds& ids, const OutputConfig& cfg, const VideoFormat& fmt);
    void reopen(const OutputConfig& cfg, const VideoFormat& fmt);
    AudioProgram program_for(const SourceDesc& src, int channels) const;
    Media load_media(const SourceDesc& src, const VideoFormat& fmt) const;

    int index_;
    MxlSession& mxl_;
    Library& library_;
    Uuid node_;
    std::string font_dir_;
    VideoFormat platform_;
    double ram_max_s_ = 20;
    std::uint64_t ram_budget_ = 4096ull << 20;

    mutable std::mutex mu_;
    OutputConfig cfg_;
    SourceDesc source_;
    VideoFormat fmt_;
    OutputIds ids_;
    std::string transport_ = "play";
    bool loop_ = true;
    std::int64_t origin_frame_index_ = 0;
    std::int64_t origin_media_ = 0;
    std::int64_t hold_frame_ = 0;
    std::uint64_t play_origin_grain_ = 0;
    bool origin_valid_ = false;
    int config_gen_ = 0;
    int applied_gen_ = -1;
    int media_gen_ = 0;
    SourceDesc load_src_;
    bool load_override_ = false;
    std::atomic<std::shared_ptr<const Media>> published_;

    std::atomic<bool> stop_{false};
    std::atomic<bool> master_v_{true};
    std::atomic<bool> master_a_{true};
    std::atomic<bool> master_d_{true};
    std::atomic<bool> master_k_{true};
    std::atomic<std::uint64_t> grains_{0};
    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<std::uint64_t> loops_{0};
    std::atomic<int> ahead_{0};
    std::atomic<std::uint64_t> ram_bytes_{0};
    std::thread thread_;
    std::thread loader_;

    MxlFlow video_, audio_, data_, key_;
    bool flows_open_ = false;

    struct Sprite {
        std::string id;
        int w = 0, h = 0;
        std::vector<std::vector<std::uint8_t>> frames;
        std::vector<double> durations;
    };
    Sprite sprites_[2];

    // The writer publishes a rendered frame every few grains; thumbnail() encodes
    // the JPEG from it when asked, so the writer thread never encodes.
    struct Frame {
        std::vector<std::uint8_t> v210;
        int width = 0;
        int height = 0;
        std::uint64_t index = 0;
    };
    std::atomic<std::shared_ptr<const Frame>> thumb_frame_;
    mutable std::atomic<bool> thumb_wanted_{false};  // grains rendered in place are copied out only on request
    mutable std::mutex thumb_mu_;
    mutable std::vector<std::uint8_t> thumb_;
    mutable std::uint64_t thumb_index_ = 0;
    mutable std::mutex meter_mu_;
    std::vector<float> meters_;
};

SourceDesc source_from_json(const nlohmann::json& j);
nlohmann::json source_to_json(const SourceDesc& s);

}  // namespace mtp
