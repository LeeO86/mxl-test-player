#include "mxl_io.hpp"

#include "util.hpp"
#include "v210.hpp"

#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/time.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace mtp {
namespace {

std::string json_escape(std::string s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o.push_back('\\');
        if (c == '\n') {
            o += "\\n";
            continue;
        }
        o.push_back(c);
    }
    return o;
}

std::string safe_group(std::string label) {
    for (char& c : label)
        if (c == ':') c = ' ';
    if (label.empty()) label = "Output";
    return label;
}

}  // namespace

void ensure_output_domain(const std::string& domain_dir, const std::string& domain_id, std::uint64_t history_duration_ns) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(domain_dir, ec);
    if (ec) throw Error("cannot create MXL domain " + domain_dir + ": " + ec.message());
    const auto def_path = fs::path(domain_dir) / "domain_def.json";
    if (fs::exists(def_path)) {
        std::ifstream in(def_path);
        nlohmann::json existing;
        try {
            in >> existing;
        } catch (const std::exception& ex) {
            throw ConfigError(std::string("existing domain_def.json is not JSON: ") + ex.what());
        }
        const auto id = existing.value("id", "");
        if (id != domain_id) {
            throw ConfigError("domain_def.json in " + domain_dir + " has id " + id + " which does not match MXL_OUTPUT_DOMAIN_ID " +
                              domain_id + "; refusing to overwrite another domain");
        }
    } else {
        std::ofstream out(def_path);
        out << nlohmann::json{{"id", domain_id}, {"label", "MXL Test Player"}}.dump(2) << "\n";
        if (!out) throw Error("cannot write " + def_path.string());
    }
    const auto opt_path = fs::path(domain_dir) / "options.json";
    if (!fs::exists(opt_path)) {
        std::ofstream out(opt_path);
        out << nlohmann::json{{"urn:x-mxl:option:history_duration/v1.0", history_duration_ns}}.dump(2) << "\n";
        if (!out) throw Error("cannot write " + opt_path.string());
    }
}

MxlSession::~MxlSession() { close(); }

void MxlSession::open(const std::string& domain_dir, const std::string& domain_id, std::uint64_t history_duration_ns) {
    close();
    domain_ = domain_dir;
    domain_id_ = domain_id;
    ensure_output_domain(domain_, domain_id_, history_duration_ns);
    instance_ = mxlCreateInstance(domain_.c_str(), nullptr);
    if (!instance_) throw Error("mxlCreateInstance failed for " + domain_);
    mxlGarbageCollectFlows(static_cast<mxlInstance>(instance_));
    bool tmp = false;
    if (mxlIsTmpFs(domain_.c_str(), &tmp) == MXL_STATUS_OK && !tmp) {
        log_warn("MXL domain is not on a tmpfs: " + domain_);
    }
    log_info("MXL domain " + domain_ + " id " + domain_id_);
}

void MxlSession::close() {
    if (instance_) {
        mxlDestroyInstance(static_cast<mxlInstance>(instance_));
        instance_ = nullptr;
    }
}

void MxlSession::remove_own_domain() {
    if (domain_.empty()) return;
    const auto name = std::filesystem::path(domain_).filename().string();
    if (name.empty() || name == "mxl" || name == "." || name == "..") {
        log_error("refusing to remove MXL path " + domain_);
        return;
    }
    std::error_code ec;
    std::filesystem::remove_all(domain_, ec);
    if (ec) log_error("failed to remove own domain " + domain_ + ": " + ec.message());
    else log_info("removed own MXL domain " + domain_);
    domain_.clear();
}

MxlFlow::~MxlFlow() {
    if (writer_) log_warn("MxlFlow destroyed while open");
}

void MxlFlow::open(MxlSession& session, const std::string& flow_json) {
    if (writer_) close(session);
    // Pull the id out of the JSON without a full parser: "id": "uuid"
    const auto key = flow_json.find("\"id\"");
    const auto q1 = flow_json.find('"', key + 4);
    const auto q2 = flow_json.find('"', q1 + 1);
    if (q1 == std::string::npos || q2 == std::string::npos) throw Error("flow json missing id");
    id_ = flow_json.substr(q1 + 1, q2 - q1 - 1);
    mxlFlowWriter w = nullptr;
    bool created = false;
    const auto st = mxlCreateFlowWriter(static_cast<mxlInstance>(session.instance()), flow_json.c_str(), nullptr, &w, nullptr, &created);
    if (st != MXL_STATUS_OK || !w) throw Error("mxlCreateFlowWriter failed status " + std::to_string(static_cast<int>(st)) + " id " + id_);
    writer_ = w;
    log_info(std::string(created ? "created" : "opened") + " flow " + id_);
}

void MxlFlow::close(MxlSession& session) {
    if (!writer_) return;
    mxlReleaseFlowWriter(static_cast<mxlInstance>(session.instance()), static_cast<mxlFlowWriter>(writer_));
    writer_ = nullptr;
}

bool MxlFlow::write_video(std::uint64_t index, const std::uint8_t* data, std::size_t size) { return write_data(index, data, size); }

bool MxlFlow::write_data(std::uint64_t index, const std::uint8_t* data, std::size_t size) {
    if (!writer_ || !data) return false;
    mxlGrainInfo info{};
    std::uint8_t* payload = nullptr;
    auto st = mxlFlowWriterOpenGrain(static_cast<mxlFlowWriter>(writer_), index, &info, &payload);
    if (st != MXL_STATUS_OK || !payload) return false;
    const std::size_t n = std::min(size, static_cast<std::size_t>(info.grainSize));
    std::memcpy(payload, data, n);
    if (n < info.grainSize) std::memset(payload + n, 0, info.grainSize - n);
    info.flags = 0;
    info.validSlices = info.totalSlices;
    st = mxlFlowWriterCommitGrain(static_cast<mxlFlowWriter>(writer_), &info);
    return st == MXL_STATUS_OK;
}

bool MxlFlow::write_audio(std::uint64_t sample_index, const float* planar, int channels, int count) {
    if (!writer_ || !planar || count <= 0 || channels <= 0) return false;
    mxlMutableWrappedMultiBufferSlice slices{};
    auto st = mxlFlowWriterOpenSamples(static_cast<mxlFlowWriter>(writer_), sample_index, static_cast<std::size_t>(count), &slices);
    if (st != MXL_STATUS_OK) return false;
    const int nch = static_cast<int>(slices.count);
    const auto stride = slices.stride;
    const int use = std::min(channels, nch);
    for (int c = 0; c < use; ++c) {
        const float* src = planar + static_cast<std::size_t>(c) * static_cast<std::size_t>(count);
        int left = count;
        for (int frag = 0; frag < 2 && left > 0; ++frag) {
            auto& fr = slices.base.fragments[frag];
            if (!fr.pointer || fr.size == 0) continue;
            auto* dst = reinterpret_cast<float*>(static_cast<std::uint8_t*>(fr.pointer) + static_cast<std::size_t>(c) * stride);
            const int n = std::min(left, static_cast<int>(fr.size / sizeof(float)));
            std::memcpy(dst, src, static_cast<std::size_t>(n) * sizeof(float));
            src += n;
            left -= n;
        }
    }
    st = mxlFlowWriterCommitSamples(static_cast<mxlFlowWriter>(writer_));
    return st == MXL_STATUS_OK;
}

std::uint64_t MxlFlow::current_index(Rational rate) const {
    mxlRational r{rate.num, rate.den};
    return mxlGetCurrentIndex(&r);
}

std::string video_flow_json(const Uuid& id, const std::string& label, const std::string& group, const char* role, const VideoFormat& fmt,
                            bool v210a, const Uuid& source_id, const Uuid& device_id) {
    const std::string media = v210a ? "video/v210a" : "video/v210";
    const int cw = fmt.width / 2;
    std::string s = "{\n";
    s += "  \"id\": \"" + id.str() + "\",\n";
    s += "  \"label\": \"" + json_escape(label) + "\",\n";
    s += "  \"description\": \"" + json_escape(label) + "\",\n";
    s += "  \"format\": \"urn:x-nmos:format:video\",\n";
    s += "  \"media_type\": \"" + media + "\",\n";
    s += "  \"tags\": {\"urn:x-nmos:tag:grouphint/v1.0\": [\"" + json_escape(safe_group(group)) + ":" + role + "\"]},\n";
    s += "  \"grain_rate\": {\"numerator\": " + std::to_string(fmt.frame_rate.num) + ", \"denominator\": " +
         std::to_string(fmt.frame_rate.den) + "},\n";
    s += "  \"frame_width\": " + std::to_string(fmt.width) + ",\n";
    s += "  \"frame_height\": " + std::to_string(fmt.height) + ",\n";
    s += "  \"interlace_mode\": \"" + std::string(fmt.interlace_mode()) + "\",\n";
    s += "  \"colorspace\": \"BT709\",\n";
    s += "  \"source_id\": \"" + source_id.str() + "\",\n";
    s += "  \"device_id\": \"" + device_id.str() + "\",\n";
    s += "  \"parents\": [],\n";
    s += "  \"components\": [\n";
    s += "    {\"name\": \"Y\", \"width\": " + std::to_string(fmt.width) + ", \"height\": " + std::to_string(fmt.height) +
         ", \"bit_depth\": 10},\n";
    s += "    {\"name\": \"Cb\", \"width\": " + std::to_string(cw) + ", \"height\": " + std::to_string(fmt.height) + ", \"bit_depth\": 10},\n";
    s += "    {\"name\": \"Cr\", \"width\": " + std::to_string(cw) + ", \"height\": " + std::to_string(fmt.height) + ", \"bit_depth\": 10}\n";
    s += "  ]\n}\n";
    return s;
}

std::string audio_flow_json(const Uuid& id, const std::string& label, const std::string& group, int channels, const Uuid& source_id,
                            const Uuid& device_id) {
    std::string s = "{\n";
    s += "  \"id\": \"" + id.str() + "\",\n";
    s += "  \"label\": \"" + json_escape(label) + "\",\n";
    s += "  \"description\": \"" + json_escape(label) + "\",\n";
    s += "  \"format\": \"urn:x-nmos:format:audio\",\n";
    s += "  \"media_type\": \"audio/float32\",\n";
    s += "  \"tags\": {\"urn:x-nmos:tag:grouphint/v1.0\": [\"" + json_escape(safe_group(group)) + ":Audio\"]},\n";
    s += "  \"sample_rate\": {\"numerator\": 48000, \"denominator\": 1},\n";
    s += "  \"channel_count\": " + std::to_string(channels) + ",\n";
    s += "  \"bit_depth\": 32,\n";
    s += "  \"source_id\": \"" + source_id.str() + "\",\n";
    s += "  \"device_id\": \"" + device_id.str() + "\",\n";
    s += "  \"parents\": []\n}\n";
    return s;
}

std::string data_flow_json(const Uuid& id, const std::string& label, const std::string& group, const VideoFormat& fmt, const Uuid& source_id,
                           const Uuid& device_id) {
    std::string s = "{\n";
    s += "  \"id\": \"" + id.str() + "\",\n";
    s += "  \"label\": \"" + json_escape(label) + "\",\n";
    s += "  \"description\": \"" + json_escape(label) + "\",\n";
    s += "  \"format\": \"urn:x-nmos:format:data\",\n";
    s += "  \"media_type\": \"video/smpte291\",\n";
    s += "  \"tags\": {\"urn:x-nmos:tag:grouphint/v1.0\": [\"" + json_escape(safe_group(group)) + ":Data\"]},\n";
    s += "  \"grain_rate\": {\"numerator\": " + std::to_string(fmt.frame_rate.num) + ", \"denominator\": " +
         std::to_string(fmt.frame_rate.den) + "},\n";
    s += "  \"source_id\": \"" + source_id.str() + "\",\n";
    s += "  \"device_id\": \"" + device_id.str() + "\",\n";
    s += "  \"parents\": []\n}\n";
    return s;
}

GrainProbe read_grain(MxlSession& session, const std::string& flow_id, std::uint64_t index, int width, int height, int timeout_ms) {
    GrainProbe probe;
    probe.index = index;
    mxlFlowReader reader = nullptr;
    auto st = mxlCreateFlowReader(static_cast<mxlInstance>(session.instance()), flow_id.c_str(), nullptr, &reader);
    if (st != MXL_STATUS_OK || !reader) return probe;
    mxlGrainInfo info{};
    std::uint8_t* payload = nullptr;
    st = mxlFlowReaderGetGrain(reader, index, static_cast<std::uint64_t>(timeout_ms) * 1000000ull, &info, &payload);
    if (st == MXL_STATUS_OK && payload && info.grainSize > 0) {
        probe.ok = true;
        const std::size_t n = std::min<std::size_t>(info.grainSize, 4096);
        probe.head.assign(payload, payload + n);
        probe.fnv = fnv1a64(payload, info.grainSize);
        if (width > 0 && height > 0 && info.grainSize >= v210_line_stride(width)) {
            std::vector<std::uint16_t> y(static_cast<std::size_t>(width));
            std::vector<std::uint16_t> cb(static_cast<std::size_t>(width / 2));
            std::vector<std::uint16_t> cr(static_cast<std::size_t>(width / 2));
            const int row = std::min(height - 1, height / 2);
            unpack_v210_line(payload + static_cast<std::size_t>(row) * v210_line_stride(width), width, y.data(), cb.data(), cr.data());
            const int x = width / 2;
            probe.y = y[static_cast<std::size_t>(x)];
            probe.cb = cb[static_cast<std::size_t>(x / 2)];
            probe.cr = cr[static_cast<std::size_t>(x / 2)];
        }
    }
    mxlReleaseFlowReader(static_cast<mxlInstance>(session.instance()), reader);
    return probe;
}

}  // namespace mtp
