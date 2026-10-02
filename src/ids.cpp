#include "ids.hpp"

namespace mtp {

Uuid node_id_from_seed(std::string_view seed) { return uuid_from_seed(seed); }

Uuid domain_id_from_seed(std::string_view seed) { return uuid_v5(uuid_from_seed(seed), "domain"); }

OutputIds derive_output_ids(const Uuid& node, int index, const VideoFormat& fmt, int audio_channels, bool v210a) {
    const std::string base = "output/" + std::to_string(index);
    const std::string fmt_key = fmt.name + (v210a ? "/v210a" : "/v210");
    OutputIds id;
    id.node = node;
    id.device = uuid_v5(node, "device");
    id.video_source = uuid_v5(node, base + "/source/video");
    id.audio_source = uuid_v5(node, base + "/source/audio");
    id.data_source = uuid_v5(node, base + "/source/data");
    id.key_source = uuid_v5(node, base + "/source/key");
    id.video_flow = uuid_v5(id.video_source, fmt_key);
    id.audio_flow = uuid_v5(id.audio_source, fmt.name + "/ch" + std::to_string(audio_channels));
    id.data_flow = uuid_v5(id.data_source, fmt.name + "/smpte291");
    id.key_flow = uuid_v5(id.key_source, fmt.name + "/key");
    id.video_sender = uuid_v5(id.video_flow, "sender");
    id.audio_sender = uuid_v5(id.audio_flow, "sender");
    id.data_sender = uuid_v5(id.data_flow, "sender");
    id.key_sender = uuid_v5(id.key_flow, "sender");
    return id;
}

}  // namespace mtp
