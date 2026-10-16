#pragma once

#include "timecode.hpp"

#include <cstdint>
#include <vector>

namespace mtp {

// RFC 8331 payload (no RTP header) carrying one SMPTE ST 12-2 ATC packet.
// The MXL video/smpte291 grain is 4096 bytes; unused bytes stay zero.
constexpr std::size_t kAncGrainSize = 4096;

struct AncPacket {
    Timecode tc;
    AtcKind kind = AtcKind::Ltc;
    int line = 10;
    bool interlaced = false;
    std::uint16_t sequence = 0;
};

std::vector<std::uint8_t> encode_anc_grain(const AncPacket& pkt);
// The packet for grain `index` of an output's data flow. At an interlaced format the data flow
// runs at the field rate, like the video: grain 2k carries the first field's packet and 2k+1 the
// second's, each with its own field flag (F, from tc.field) and a line of that field.
AncPacket anc_packet_for_grain(std::uint64_t index, const VideoFormat& fmt, const Timecode& tc, AtcKind kind);
// Returns false when the grain does not contain a DID/SDID 0x60/0x60 packet.
bool decode_anc_timecode(const std::uint8_t* grain, std::size_t size, Timecode& out, AtcKind& kind);

}  // namespace mtp
