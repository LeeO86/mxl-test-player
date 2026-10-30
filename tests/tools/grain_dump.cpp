// Lab tool: writes the payload of one grain of an MXL flow to a file, as a reader gets it.
//   player_grain_dump <domain dir> <flow id> <grain index | -N for N grains back from the head> <out file>
// Used by tests/integration/keying.py to check the v210a alpha plane and the fill_key key.
#include <mxl/flow.h>
#include <mxl/mxl.h>
#include <mxl/time.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: %s <domain dir> <flow id> <index|-N> <out file>\n", argv[0]);
        return 2;
    }
    mxlInstance inst = mxlCreateInstance(argv[1], "");
    if (!inst) return 3;
    mxlFlowReader reader = nullptr;
    if (mxlCreateFlowReader(inst, argv[2], "", &reader) != MXL_STATUS_OK || !reader) return 4;
    long long want = std::atoll(argv[3]);
    std::uint64_t index = static_cast<std::uint64_t>(want);
    if (want < 0) {
        mxlFlowInfo info{};
        if (mxlFlowReaderGetInfo(reader, &info) != MXL_STATUS_OK) return 5;
        index = info.runtime.headIndex - static_cast<std::uint64_t>(-want);
    }
    mxlGrainInfo gi{};
    std::uint8_t* payload = nullptr;
    if (mxlFlowReaderGetGrain(reader, index, 200000000ull, &gi, &payload) != MXL_STATUS_OK || !payload) return 6;
    FILE* f = std::fopen(argv[4], "wb");
    if (!f) return 7;
    std::fwrite(payload, 1, gi.grainSize, f);
    std::fclose(f);
    std::printf("%llu %u\n", static_cast<unsigned long long>(index), gi.grainSize);
    mxlReleaseFlowReader(inst, reader);
    mxlDestroyInstance(inst);
    return 0;
}
