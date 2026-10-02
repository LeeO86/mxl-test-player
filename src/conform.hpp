#pragma once

#include <cstdint>

namespace mtp {

// Interleaved float conform: channel map (keep source channels, pad silence),
// trim or pad to dst_samples, optional linear crossfade of the loop point.
void conform_interleaved(const float* src, int src_channels, std::uint64_t src_samples, float* dst, int dst_channels,
                         std::uint64_t dst_samples, int crossfade_samples);

}  // namespace mtp
