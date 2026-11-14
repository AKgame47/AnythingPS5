// PNR PRX - audio output and pad input host models (Core slice C5), GPL-2.0.
// EXPERIMENTAL. Host-side models behind a few libSceAudioOut and libScePad symbols, bound by NID.
// Honest limits:
//  - Signatures use integer and pointer arguments only (a thunk limit). The argument ORDER and the
//    constants below come from public homebrew documentation and are NOT verified on a PlayStation.
//  - Audio: PNR keeps the submitted frames in a capture buffer and forwards them to an optional host
//    sink. With no sink nothing is played. No PS5 mixing, latency or timing behaviour is modelled:
//    sceAudioOutOutput returns at once.
//  - Pad: PNR writes ONLY the fields listed in PadData below into the caller's buffer (offsets 0..9).
//    The rest of the guest's structure is left untouched. No touchpad, motion, haptics or lightbar.
//  - Save data is not bound: its API is structure-heavy and its layouts are unverified. The host
//    model is services/save_data (Planned to bind).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "prx/prx.hpp"

namespace pnr::prx {

// ---- audio ----
enum class AudioFormat : std::uint32_t { S16Mono = 0, S16Stereo = 1, FloatMono = 2, FloatStereo = 3 };
struct AudioPortInfo { std::int32_t handle = 0; std::uint32_t frames_per_buffer = 0, sample_rate = 0, channels = 0; AudioFormat format = AudioFormat::S16Stereo; };
// Called for each submitted buffer, already converted to interleaved S16.
using AudioSink = std::function<void(std::int32_t handle, const std::int16_t* samples, std::size_t frames, unsigned channels)>;

// ---- pad ----
// Fields PNR writes (little endian), per the layout above:
//   0..3 buttons bitmask, 4 left X, 5 left Y, 6 right X, 7 right Y, 8 L2, 9 R2. Sticks 0..255, centre 128.
struct PadData { std::uint32_t buttons = 0; std::uint8_t lx = 128, ly = 128, rx = 128, ry = 128, l2 = 0, r2 = 0; };
// Button bits: from public homebrew documentation, unverified on a PlayStation.
namespace pad_bits {
constexpr std::uint32_t L3 = 0x2, R3 = 0x4, OPTIONS = 0x8, UP = 0x10, RIGHT = 0x20, DOWN = 0x40, LEFT = 0x80,
                        L1 = 0x400, R1 = 0x800, TRIANGLE = 0x1000, CIRCLE = 0x2000, CROSS = 0x4000, SQUARE = 0x8000;
}

// Feed one host input event (evdev style names, see the table in hw_abi.cpp). Returns false for an
// unmapped control. Thread safe.
bool pad_apply_host_event(const std::string& control, std::int32_t value);
PadData pad_current();
void pad_reset();

// Audio observation for tests and tools.
void audio_set_sink(AudioSink sink);
std::vector<std::int16_t> audio_captured(std::int32_t handle);
std::vector<AudioPortInfo> audio_ports();
void audio_reset();

void add_hw_abi_symbols(std::vector<PrxSymbol>& out);

}  // namespace pnr::prx
