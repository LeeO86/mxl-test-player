#include "timecode.hpp"

#include <ctime>

namespace mtp {
namespace {

int adjust_ntsc(std::int64_t framenum, int fps) {
    int drop_frames = 0;
    int frames_per_10mins = 0;
    if (fps == 30) {
        drop_frames = 2;
        frames_per_10mins = 17982;
    } else if (fps == 60) {
        drop_frames = 4;
        frames_per_10mins = 35964;
    } else {
        return static_cast<int>(framenum);
    }
    const std::int64_t d = framenum / frames_per_10mins;
    const std::int64_t m = framenum % frames_per_10mins;
    std::int64_t extra = 9 * drop_frames * d;
    if (m >= drop_frames) {
        extra += drop_frames * ((m - drop_frames) / (frames_per_10mins / 10));
    }
    return static_cast<int>(framenum + extra);
}

}  // namespace

TcSource parse_tc_source(std::string_view s) {
    if (s == "utc") return TcSource::Utc;
    if (s == "local") return TcSource::Local;
    if (s == "media") return TcSource::Media;
    if (s == "free") return TcSource::Free;
    return TcSource::Tai;
}

const char* tc_source_name(TcSource s) {
    switch (s) {
        case TcSource::Utc: return "utc";
        case TcSource::Local: return "local";
        case TcSource::Media: return "media";
        case TcSource::Free: return "free";
        default: return "tai";
    }
}

AtcKind parse_atc_kind(std::string_view s) {
    if (s == "vitc1" || s == "ATC_VITC1") return AtcKind::Vitc1;
    if (s == "vitc2" || s == "ATC_VITC2") return AtcKind::Vitc2;
    return AtcKind::Ltc;
}

const char* atc_kind_name(AtcKind k) {
    switch (k) {
        case AtcKind::Vitc1: return "vitc1";
        case AtcKind::Vitc2: return "vitc2";
        default: return "ltc";
    }
}

std::string Timecode::format() const {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d%c%02d", hh, mm, ss, drop ? ';' : ':', ff);
    return buf;
}

int timecode_fps(Rational frame_rate) {
    if (frame_rate == Rational{24000, 1001} || frame_rate == Rational{24, 1}) return 24;
    if (frame_rate == Rational{25, 1}) return 25;
    if (frame_rate == Rational{30000, 1001} || frame_rate == Rational{30, 1}) return 30;
    if (frame_rate == Rational{50, 1}) return 50;
    if (frame_rate == Rational{60000, 1001} || frame_rate == Rational{60, 1}) return 60;
    return static_cast<int>((frame_rate.num + frame_rate.den / 2) / frame_rate.den);
}

Timecode frames_to_timecode(std::int64_t frame_number, int fps, bool drop, int field) {
    if (fps <= 0) fps = 25;
    if (frame_number < 0) frame_number = 0;
    std::int64_t n = frame_number;
    const bool df = drop && (fps == 30 || fps == 60);
    if (df) n = adjust_ntsc(frame_number, fps);
    Timecode tc;
    tc.fps = fps;
    tc.drop = df;
    tc.field = field;
    tc.ff = static_cast<int>(n % fps);
    tc.ss = static_cast<int>((n / fps) % 60);
    tc.mm = static_cast<int>((n / (fps * 60)) % 60);
    tc.hh = static_cast<int>((n / (fps * 3600)) % 24);
    return tc;
}

std::int64_t timecode_to_frames(const Timecode& tc) {
    const int fps = tc.fps > 0 ? tc.fps : 25;
    std::int64_t frames = ((static_cast<std::int64_t>(tc.hh) * 60 + tc.mm) * 60 + tc.ss) * fps + tc.ff;
    if (!tc.drop) return frames;
    const int drop = fps == 60 ? 4 : 2;
    const int total_minutes = tc.hh * 60 + tc.mm;
    return frames - drop * (total_minutes - total_minutes / 10);
}

Timecode timecode_for_grain(std::uint64_t grain_index, const VideoFormat& fmt, const TimecodeQuery& q) {
    const int fps = timecode_fps(fmt.frame_rate);
    const bool inter = fmt.interlaced();
    const std::uint64_t frame_index = inter ? grain_index / 2 : grain_index;
    const int field = inter ? static_cast<int>(grain_index & 1u) : 0;
    const bool drop_default = (fmt.frame_rate == Rational{30000, 1001} || fmt.frame_rate == Rational{60000, 1001});
    const bool drop = q.drop_frame && drop_default;

    std::int64_t frame_number = 0;
    switch (q.source) {
        case TcSource::Media:
            frame_number = q.media_start_frame + static_cast<std::int64_t>(frame_index);
            break;
        case TcSource::Free:
            frame_number = q.free_start_frame + q.since_start_frame;
            break;
        case TcSource::Utc:
        case TcSource::Local: {
            std::int64_t sec = q.utc_epoch_seconds;
            if (sec < 0) {
                sec = static_cast<std::int64_t>(std::time(nullptr));
            }
            if (q.source == TcSource::Local) sec += q.tz_offset_seconds;
            sec %= 86400;
            if (sec < 0) sec += 86400;
            const std::uint64_t ts = index_to_timestamp_ns(grain_index, fmt.grain_rate());
            const std::uint64_t dur = grain_duration_ns(fmt.grain_rate());
            const int sub = dur ? static_cast<int>((ts % 1000000000ull) / (dur ? dur : 1)) : 0;
            // Time of day from the supplied UTC/local second, with the frame
            // field taken from the grain's position inside that second when the
            // caller passes utc_epoch_seconds < 0 we still use wall seconds and
            // the grain phase so picture and ANC stay locked to each other.
            frame_number = sec * fps + (sub % fps);
            if (q.utc_epoch_seconds >= 0) {
                // Tests pass an absolute second; the frame within the second is
                // the grain's phase at the frame rate.
                const Rational fr = fmt.frame_rate;
                const std::uint64_t frame_in_sec =
                    (index_to_timestamp_ns(frame_index, fr) % 1000000000ull) * static_cast<std::uint64_t>(fr.num) /
                    (1000000000ull * static_cast<std::uint64_t>(fr.den));
                frame_number = sec * fps + static_cast<std::int64_t>(frame_in_sec % static_cast<std::uint64_t>(fps));
            }
            break;
        }
        case TcSource::Tai:
        default: {
            const std::uint64_t ts = index_to_timestamp_ns(frame_index, fmt.frame_rate);
            const std::uint64_t sec = ts / 1000000000ull;
            const std::uint64_t sod = sec % 86400ull;
            const std::uint64_t frame_in_sec =
                ((ts % 1000000000ull) * static_cast<std::uint64_t>(fmt.frame_rate.num)) /
                (1000000000ull * static_cast<std::uint64_t>(fmt.frame_rate.den));
            frame_number = static_cast<std::int64_t>(sod * static_cast<std::uint64_t>(fps) + frame_in_sec);
            break;
        }
    }
    return frames_to_timecode(frame_number, fps, drop, field);
}

}  // namespace mtp
