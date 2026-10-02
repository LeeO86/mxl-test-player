#pragma once

#include "format.hpp"
#include "uuid_util.hpp"

#include <string>

namespace mtp {

struct OutputIds {
    Uuid node;
    Uuid device;
    Uuid video_source;
    Uuid audio_source;
    Uuid data_source;
    Uuid key_source;
    Uuid video_flow;
    Uuid audio_flow;
    Uuid data_flow;
    Uuid key_flow;
    Uuid video_sender;
    Uuid audio_sender;
    Uuid data_sender;
    Uuid key_sender;
};

Uuid node_id_from_seed(std::string_view seed);
Uuid domain_id_from_seed(std::string_view seed);

// Flow ids include the format (and the video media type) so a format change
// mints new ids. They do not include the playing source.
OutputIds derive_output_ids(const Uuid& node, int index, const VideoFormat& fmt, int audio_channels, bool v210a);

}  // namespace mtp
