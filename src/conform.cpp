#include "conform.hpp"

#include <algorithm>
#include <cstring>

namespace mtp {

void conform_interleaved(const float* src, int src_channels, std::uint64_t src_samples, float* dst, int dst_channels,
                         std::uint64_t dst_samples, int crossfade_samples) {
    if (dst_channels < 1 || dst_samples == 0) return;
    std::fill(dst, dst + dst_samples * static_cast<std::uint64_t>(dst_channels), 0.f);
    if (src && src_channels > 0 && src_samples > 0) {
        const std::uint64_t n = std::min(src_samples, dst_samples);
        const int ch = std::min(src_channels, dst_channels);
        for (std::uint64_t i = 0; i < n; ++i) {
            for (int c = 0; c < ch; ++c) {
                dst[i * static_cast<std::uint64_t>(dst_channels) + c] = src[i * static_cast<std::uint64_t>(src_channels) + c];
            }
        }
    }
    if (crossfade_samples > 1 && dst_samples > static_cast<std::uint64_t>(crossfade_samples * 2)) {
        const int n = crossfade_samples;
        for (int i = 0; i < n; ++i) {
            const float a = static_cast<float>(i) / static_cast<float>(n);
            const std::uint64_t head = static_cast<std::uint64_t>(i);
            const std::uint64_t tail = dst_samples - static_cast<std::uint64_t>(n) + static_cast<std::uint64_t>(i);
            for (int c = 0; c < dst_channels; ++c) {
                const float h = dst[head * dst_channels + c];
                const float t = dst[tail * dst_channels + c];
                const float m = h * (1.f - a) + t * a;
                dst[head * dst_channels + c] = m;
                dst[tail * dst_channels + c] = m;
            }
        }
    }
}

}  // namespace mtp
