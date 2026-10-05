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

// sin(2π·freq·sample/48000) for an absolute (TAI) sample index. The plain argument is about
// 1e13 radians: double resolves it only to about 0.002 rad, and libm needs its slow range
// reduction for every sample. The phase is reduced to [0, 1) cycles first: the integer part
// of the frequency adds whole cycles every second.
double tone(double freq, std::uint64_t sample) {
    const std::uint64_t seconds = sample / 48000u;
    const std::uint64_t rest = sample % 48000u;
    const double fraction = freq - std::floor(freq);
    double cycles = fraction * static_cast<double>(seconds);
    cycles -= std::floor(cycles);
    cycles += freq * static_cast<double>(rest) / kSr;
    cycles -= std::floor(cycles);
    return std::sin(2.0 * kPi * cycles);
}

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
        for (int i = 0; i < count; ++i) {
            const std::uint64_t s = start + static_cast<std::uint64_t>(i);
            const double t = static_cast<double>(s) / kSr;
            double v = 0.0;
            switch (ch.signal) {
                case AudioSignal::Silence:
                    v = 0.0;
                    break;
                case AudioSignal::Sine:
                case AudioSignal::IdentFreq:
                    v = tone(ch.frequency, s);
                    break;
                case AudioSignal::IdentEbu: {
                    v = tone(ch.frequency, s);
                    if (c == 0) {
                        // Left channel interrupted: 0.5 s tone, 0.5 s silence.
                        if (static_cast<int>(t) % 2 == 1 && (t - std::floor(t)) < 0.5) {
                            // tone on even seconds fully, and first half of odd? 
                        }
                        const double phase = std::fmod(t, 1.0);
                        const int sec = static_cast<int>(std::floor(t));
                        if ((sec % 2) == 1 && phase < 0.5) v = 0.0;
                        // Simpler classic GLITS-style: 250 ms off every second on the left.
                        if (std::fmod(t, 1.0) < 0.25) v = 0.0;
                    }
                    break;
                }
                case AudioSignal::IdentBeeps: {
                    // n beeps for 1-based channel n. 80 ms beep, 80 ms gap, then a rest.
                    const int n = c + 1;
                    const double beep = 0.080;
                    const double gap = 0.080;
                    const double rest = 0.400;
                    const double cycle = n * (beep + gap) + rest;
                    const double p = std::fmod(t, cycle);
                    const double slot = beep + gap;
                    const int which = static_cast<int>(p / slot);
                    const double in = p - which * slot;
                    if (which < n && in < beep) v = tone(1000.0, s);
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
                    const double p = std::fmod(t, 0.020);
                    if (p < 0.001) v = 1.0;
                    else if (p < 0.003) v = -0.5;
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
