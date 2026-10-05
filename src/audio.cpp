#include "audio.hpp"

#include "util.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace mtp {
namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kSr = 48000.0;

float white_at(std::uint64_t sample, int ch) {
    std::uint32_t h = hash_u64(sample * 0x9E3779B185EBCA87ull + static_cast<std::uint64_t>(ch) * 0xC2B2AE3D27D4EB4Full);
    return (static_cast<float>(h) / 4294967295.0f) * 2.f - 1.f;
}

float pink_at(std::uint64_t sample, int ch) {
    // Deterministic octave sum so the sequence is a function of the sample index.
    float acc = 0.f;
    std::uint64_t step = 1;
    for (int o = 0; o < 8; ++o) {
        acc += white_at(sample / step, ch * 16 + o) * (1.f / (o + 1));
        step <<= 1;
    }
    return acc * 0.35f;
}

// The phase of freq·sample/48000 in [0, 1) cycles for an absolute (TAI) sample index. The plain
// argument of sin(2π·f·t) is about 1e13 radians: double resolves it only to about 0.002 rad,
// and libm needs its slow range reduction. The integer part of the frequency adds whole
// cycles every second, so only its fraction counts the seconds.
double tone_cycles(double freq, std::uint64_t sample) {
    const std::uint64_t seconds = sample / 48000u;
    const std::uint64_t rest = sample % 48000u;
    const double fraction = freq - std::floor(freq);
    double cycles = fraction * static_cast<double>(seconds);
    cycles -= std::floor(cycles);
    cycles += freq * static_cast<double>(rest) / kSr;
    return cycles - std::floor(cycles);
}

// A sine from the exact phase at a block start, advanced by rotation per sample (no libm call
// per sample; the error after a 48000-sample block is far below float resolution).
struct Phasor {
    double s = 0, c = 1, ds = 0, dc = 1;
    Phasor(double freq, std::uint64_t start) {
        const double p = 2.0 * kPi * tone_cycles(freq, start);
        s = std::sin(p);
        c = std::cos(p);
        const double d = 2.0 * kPi * freq / kSr;
        ds = std::sin(d);
        dc = std::cos(d);
    }
    double next() {
        const double v = s;
        const double ns = s * dc + c * ds;
        c = c * dc - s * ds;
        s = ns;
        return v;
    }
};

}  // namespace

AudioSignal parse_audio_signal(std::string_view s) {
    if (s == "ident_freq") return AudioSignal::IdentFreq;
    if (s == "ident_ebu") return AudioSignal::IdentEbu;
    if (s == "ident_beeps") return AudioSignal::IdentBeeps;
    if (s == "pink") return AudioSignal::Pink;
    if (s == "white") return AudioSignal::White;
    if (s == "sweep") return AudioSignal::Sweep;
    if (s == "silence") return AudioSignal::Silence;
    if (s == "polarity") return AudioSignal::Polarity;
    if (s == "sync_beep" || s == "beep") return AudioSignal::SyncBeep;
    return AudioSignal::Sine;
}

const char* audio_signal_name(AudioSignal s) {
    switch (s) {
        case AudioSignal::IdentFreq: return "ident_freq";
        case AudioSignal::IdentEbu: return "ident_ebu";
        case AudioSignal::IdentBeeps: return "ident_beeps";
        case AudioSignal::Pink: return "pink";
        case AudioSignal::White: return "white";
        case AudioSignal::Sweep: return "sweep";
        case AudioSignal::Silence: return "silence";
        case AudioSignal::Polarity: return "polarity";
        case AudioSignal::SyncBeep: return "sync_beep";
        default: return "sine";
    }
}

double dbfs_to_lin(double dbfs) { return std::pow(10.0, dbfs / 20.0); }

AudioProgram make_uniform_program(int channels, AudioSignal signal, double freq, double level_dbfs) {
    AudioProgram p;
    p.channels.resize(static_cast<std::size_t>(channels));
    for (int i = 0; i < channels; ++i) {
        p.channels[i].signal = signal;
        p.channels[i].frequency = freq;
        p.channels[i].level_dbfs = level_dbfs;
    }
    return p;
}

AudioProgram make_ident_freq_program(int channels, double level_dbfs) {
    AudioProgram p;
    p.channels.resize(static_cast<std::size_t>(channels));
    for (int i = 0; i < channels; ++i) {
        p.channels[i].signal = AudioSignal::IdentFreq;
        p.channels[i].frequency = 400.0 + 100.0 * i;
        p.channels[i].level_dbfs = level_dbfs;
    }
    return p;
}

AudioProgram make_ident_beep_program(int channels, double level_dbfs) {
    auto p = make_uniform_program(channels, AudioSignal::IdentBeeps, 1000.0, level_dbfs);
    return p;
}

AudioProgram make_ebu_ident_program(int channels, double level_dbfs) {
    return make_uniform_program(channels, AudioSignal::IdentEbu, 1000.0, level_dbfs);
}

void render_audio(const AudioProgram& prog, std::uint64_t start, int count, bool sync_frame, float* planar) {
    const int chs = static_cast<int>(prog.channels.size());
    for (int c = 0; c < chs; ++c) {
        float* dst = planar + static_cast<std::size_t>(c) * static_cast<std::size_t>(count);
        const AudioChannel& ch = prog.channels[static_cast<std::size_t>(c)];
        const double amp = dbfs_to_lin(ch.level_dbfs + ch.gain_db);
        Phasor sine(ch.signal == AudioSignal::IdentBeeps ? 1000.0 : ch.frequency, start);
        for (int i = 0; i < count; ++i) {
            const std::uint64_t s = start + static_cast<std::uint64_t>(i);
            const double t = static_cast<double>(s) / kSr;
            const double tone = sine.next();
            // Position within the second in samples; integers, not fmod() per sample.
            const std::uint64_t in_second = s % 48000u;
            double v = 0.0;
            switch (ch.signal) {
                case AudioSignal::Silence:
                    v = 0.0;
                    break;
                case AudioSignal::Sine:
                case AudioSignal::IdentFreq:
                    v = tone;
                    break;
                case AudioSignal::IdentEbu: {
                    v = tone;
                    if (c == 0) {
                        // Left channel interrupted: off for the first half of odd seconds, and
                        // (GLITS-style) for the first 250 ms of every second.
                        if (((s / 48000u) % 2) == 1 && in_second < 24000u) v = 0.0;
                        if (in_second < 12000u) v = 0.0;
                    }
                    break;
                }
                case AudioSignal::IdentBeeps: {
                    // n beeps for 1-based channel n. 80 ms beep, 80 ms gap, then a 400 ms rest.
                    const std::uint64_t n = static_cast<std::uint64_t>(c) + 1;
                    constexpr std::uint64_t kBeep = 3840;  // 80 ms
                    constexpr std::uint64_t kSlot = 7680;  // beep + gap
                    const std::uint64_t p = s % (n * kSlot + 19200u);
                    if (p / kSlot < n && p % kSlot < kBeep) v = tone;
                    break;
                }
                case AudioSignal::Pink:
                    v = pink_at(s, c);
                    break;
                case AudioSignal::White:
                    v = white_at(s, c);
                    break;
                case AudioSignal::Sweep: {
                    // 20 Hz -> 20 kHz log sweep over 8 seconds, repeating.
                    constexpr double dur = 8.0;
                    const double p = std::fmod(t, dur) / dur;
                    const double f = 20.0 * std::pow(1000.0, p);
                    // Integrate frequency approximately with the closed form of a log sweep.
                    const double k = std::log(1000.0) / dur;
                    const double tt = std::fmod(t, dur);
                    const double phase = 2.0 * kPi * (20.0 * (std::exp(k * tt) - 1.0) / k);
                    v = std::sin(phase);
                    (void)f;
                    break;
                }
                case AudioSignal::Polarity: {
                    const std::uint64_t p = s % 960u;  // 20 ms
                    if (p < 48u) v = 1.0;               // 1 ms
                    else if (p < 144u) v = -0.5;        // to 3 ms
                    else v = 0.0;
                    break;
                }
                case AudioSignal::SyncBeep:
                    v = 0.0;
                    break;
            }
            v *= amp;
            if (ch.mute) v = 0.0;
            if (ch.invert) v = -v;
            dst[i] = static_cast<float>(v);
        }
        if (sync_frame || ch.signal == AudioSignal::SyncBeep) {
            const double beep_amp = dbfs_to_lin(prog.beep_dbfs);
            const int beep_n = std::min(count, static_cast<int>(prog.beep_ms * 0.001 * kSr));
            const bool force = ch.signal == AudioSignal::SyncBeep;
            if (sync_frame || force) {
                for (int i = 0; i < beep_n; ++i) {
                    const double t = static_cast<double>(i) / kSr;
                    float b = static_cast<float>(std::sin(2.0 * kPi * prog.beep_hz * t) * beep_amp);
                    if (ch.invert) b = -b;
                    if (!ch.mute) {
                        if (force) dst[i] = b;
                        else dst[i] += b;
                    }
                }
            }
        }
    }
}

double goertzel_amplitude(const float* x, int n, double freq, double sample_rate) {
    const double w = 2.0 * kPi * freq / sample_rate;
    const double coeff = 2.0 * std::cos(w);
    double s0 = 0, s1 = 0, s2 = 0;
    for (int i = 0; i < n; ++i) {
        s0 = static_cast<double>(x[i]) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return 2.0 * std::sqrt(std::max(0.0, power)) / static_cast<double>(n);
}

}  // namespace mtp
