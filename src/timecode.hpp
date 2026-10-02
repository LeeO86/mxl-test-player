#pragma once

#include "format.hpp"

#include <cstdint>
#include <string>

namespace mtp {

enum class TcSource { Tai, Utc, Local, Media, Free };
enum class AtcKind { Ltc, Vitc1, Vitc2 };

TcSource parse_tc_source(std::string_view s);
const char* tc_source_name(TcSource s);
AtcKind parse_atc_kind(std::string_view s);
const char* atc_kind_name(AtcKind k);

struct Timecode {
    int hh = 0;
    int mm = 0;
    int ss = 0;
    int ff = 0;
    bool drop = false;
    int field = 0;  // 0 progressive or first field, 1 second field
    int fps = 25;

    std::string format() const;  // HH:MM:SS:FF or HH:MM:SS;FF
    bool operator==(const Timecode& o) const {
        return hh == o.hh && mm == o.mm && ss == o.ss && ff == o.ff && drop == o.drop && field == o.field;
    }
};

// Nominal counting rate: 24, 25, 30, 50, 60 (29.97 counts as 30, 59.94 as 60, 23.98 as 24).
int timecode_fps(Rational frame_rate);

// Convert a frame number (0-based, non-drop counting of real frames) into a display timecode.
Timecode frames_to_timecode(std::int64_t frame_number, int fps, bool drop, int field = 0);

// Inverse of frames_to_timecode for tests. Returns the real frame number.
std::int64_t timecode_to_frames(const Timecode& tc);

struct TimecodeQuery {
    TcSource source = TcSource::Tai;
    bool drop_frame = true;
    std::int64_t free_start_frame = 0;
    std::int64_t media_start_frame = 0;  // embedded start, frames at the output frame rate
    std::int64_t media_frame = 0;
    std::int64_t since_start_frame = 0;
    // Wall clock override for tests. 0 means read the clock.
    std::int64_t utc_epoch_seconds = -1;
    int tz_offset_seconds = 0;
};

// `grain_index` is the MXL grain index (field index when interlaced).
Timecode timecode_for_grain(std::uint64_t grain_index, const VideoFormat& fmt, const TimecodeQuery& q);

}  // namespace mtp
