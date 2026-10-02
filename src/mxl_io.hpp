#pragma once

#include "format.hpp"
#include "uuid_util.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mtp {

class MxlSession {
public:
    MxlSession() = default;
    ~MxlSession();
    MxlSession(const MxlSession&) = delete;
    MxlSession& operator=(const MxlSession&) = delete;

    void open(const std::string& domain_dir, const std::string& domain_id);
    void close();
    const std::string& domain_dir() const { return domain_; }
    const std::string& domain_id() const { return domain_id_; }
    void* instance() const { return instance_; }

private:
    std::string domain_;
    std::string domain_id_;
    void* instance_ = nullptr;
};

enum class FlowKind { Video, Audio, Data, Key };

class MxlFlow {
public:
    MxlFlow() = default;
    ~MxlFlow();
    MxlFlow(const MxlFlow&) = delete;
    MxlFlow& operator=(const MxlFlow&) = delete;

    bool active() const { return writer_ != nullptr; }
    const std::string& id() const { return id_; }

    // Creates or reopens the flow. Replaces any writer already held.
    void open(MxlSession& session, const std::string& flow_json);
    void close(MxlSession& session);

    bool write_video(std::uint64_t index, const std::uint8_t* data, std::size_t size);
    bool write_audio(std::uint64_t sample_index, const float* planar, int channels, int count);
    bool write_data(std::uint64_t index, const std::uint8_t* data, std::size_t size);

    std::uint64_t current_index(Rational rate) const;

private:
    std::string id_;
    void* writer_ = nullptr;
    int channels_ = 0;
};

std::string video_flow_json(const Uuid& id, const std::string& label, const std::string& group, const char* role, const VideoFormat& fmt,
                            bool v210a, const Uuid& source_id, const Uuid& device_id);
std::string audio_flow_json(const Uuid& id, const std::string& label, const std::string& group, int channels, const Uuid& source_id,
                            const Uuid& device_id);
std::string data_flow_json(const Uuid& id, const std::string& label, const std::string& group, const VideoFormat& fmt, const Uuid& source_id,
                           const Uuid& device_id);

struct GrainProbe {
    bool ok = false;
    std::uint64_t index = 0;
    int y = 0;
    int cb = 0;
    int cr = 0;
    std::uint64_t fnv = 0;
    std::vector<std::uint8_t> head;  // first 64 bytes
};

GrainProbe read_grain(MxlSession& session, const std::string& flow_id, std::uint64_t index, int width, int height, int timeout_ms);

}  // namespace mtp
