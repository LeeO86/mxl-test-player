#pragma once

#include "format.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mtp {

enum class AudioSignal {
    Sine,
    IdentFreq,
    IdentEbu,
    IdentBeeps,
    Pink,
    White,
    Sweep,
    Silence,
    Polarity,
    SyncBeep
};

AudioSignal parse_audio_signal(std::string_view s);
const char* audio_signal_name(AudioSignal s);

struct AudioChannel {
    AudioSignal signal = AudioSignal::Sine;
    double frequency = 1000.0;
    double level_dbfs = -18.0;
    bool mute = false;
    double gain_db = 0.0;
    bool invert = false;
};

struct AudioProgram {
    std::vector<AudioChannel> channels;
    bool sync_beep = false;
    double beep_hz = 1000.0;
    double beep_ms = 20.0;
    double beep_dbfs = -18.0;
};

AudioProgram make_uniform_program(int channels, AudioSignal signal, double freq, double level_dbfs);
AudioProgram make_ident_freq_program(int channels, double level_dbfs);
AudioProgram make_ident_beep_program(int channels, double level_dbfs);
AudioProgram make_ebu_ident_program(int channels, double level_dbfs);

double dbfs_to_lin(double dbfs);

// Render `count` samples starting at absolute sample `start` (48 kHz).
// `planar` is channels * count, channel-major.
// `sync_frame` emits the sync beep at the start of this buffer.
void render_audio(const AudioProgram& prog, std::uint64_t start, int count, bool sync_frame, float* planar);

// Goertzel amplitude estimate of a real sine (peak, not RMS).
double goertzel_amplitude(const float* x, int n, double freq, double sample_rate);

}  // namespace mtp
