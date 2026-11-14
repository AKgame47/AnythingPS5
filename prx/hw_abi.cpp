// PNR PRX - audio output and pad input host models (Core slice C5), GPL-2.0. See hw_abi.hpp.
#include "prx/hw_abi.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

#include "loader/nid.hpp"

namespace pnr::prx {
namespace {
using Args = const std::vector<std::int64_t>&;
constexpr std::int32_t kErrInvalid = -2;  // PNR's generic invalid-argument code, not a Sony error value

struct Port { AudioPortInfo info; std::vector<std::int16_t> captured; };
struct HwState {
  std::mutex mu;
  bool audio_init = false, pad_init = false;
  std::int32_t next_audio = 1, next_pad = 1;
  std::map<std::int32_t, Port> ports;
  std::map<std::int32_t, bool> pads;
  AudioSink sink;
  PadData pad;
};
HwState& st() { static HwState s; return s; }
std::int16_t clip16(float f) { f = std::max(-1.0f, std::min(1.0f, f)); return static_cast<std::int16_t>(f * 32767.0f); }
std::uint8_t axis8(std::int32_t v) {  // evdev-style signed 16 bit to 0..255
  const std::int32_t c = std::max(-32768, std::min(32767, v));
  return static_cast<std::uint8_t>((c + 32768) >> 8);
}
std::uint8_t trig8(std::int32_t v) { return static_cast<std::uint8_t>(std::max(0, std::min(255, v))); }
void set_bit(std::uint32_t& b, std::uint32_t bit, bool on) { if (on) b |= bit; else b &= ~bit; }
}  // namespace

bool pad_apply_host_event(const std::string& c, std::int32_t v) {
  std::lock_guard<std::mutex> l(st().mu);
  PadData& p = st().pad;
  const bool on = v != 0;
  using namespace pad_bits;
  if (c == "BTN_SOUTH") set_bit(p.buttons, CROSS, on);
  else if (c == "BTN_EAST") set_bit(p.buttons, CIRCLE, on);
  else if (c == "BTN_NORTH") set_bit(p.buttons, TRIANGLE, on);
  else if (c == "BTN_WEST") set_bit(p.buttons, SQUARE, on);
  else if (c == "BTN_TL") set_bit(p.buttons, L1, on);
  else if (c == "BTN_TR") set_bit(p.buttons, R1, on);
  else if (c == "BTN_START") set_bit(p.buttons, OPTIONS, on);
  else if (c == "BTN_THUMBL") set_bit(p.buttons, L3, on);
  else if (c == "BTN_THUMBR") set_bit(p.buttons, R3, on);
  else if (c == "ABS_HAT0X") { set_bit(p.buttons, LEFT, v < 0); set_bit(p.buttons, RIGHT, v > 0); }
  else if (c == "ABS_HAT0Y") { set_bit(p.buttons, UP, v < 0); set_bit(p.buttons, DOWN, v > 0); }
  else if (c == "ABS_X") p.lx = axis8(v);
  else if (c == "ABS_Y") p.ly = axis8(v);
  else if (c == "ABS_RX") p.rx = axis8(v);
  else if (c == "ABS_RY") p.ry = axis8(v);
  else if (c == "ABS_Z") p.l2 = trig8(v);
  else if (c == "ABS_RZ") p.r2 = trig8(v);
  else return false;
  return true;
}
PadData pad_current() { std::lock_guard<std::mutex> l(st().mu); return st().pad; }
void pad_reset() { std::lock_guard<std::mutex> l(st().mu); st().pad = PadData{}; st().pads.clear(); st().pad_init = false; st().next_pad = 1; }

void audio_set_sink(AudioSink s) { std::lock_guard<std::mutex> l(st().mu); st().sink = std::move(s); }
std::vector<std::int16_t> audio_captured(std::int32_t h) { std::lock_guard<std::mutex> l(st().mu); auto it = st().ports.find(h); return it == st().ports.end() ? std::vector<std::int16_t>{} : it->second.captured; }
std::vector<AudioPortInfo> audio_ports() { std::lock_guard<std::mutex> l(st().mu); std::vector<AudioPortInfo> o; for (auto& kv : st().ports) o.push_back(kv.second.info); return o; }
void audio_reset() { std::lock_guard<std::mutex> l(st().mu); st().ports.clear(); st().sink = nullptr; st().audio_init = false; st().next_audio = 1; }

void add_hw_abi_symbols(std::vector<PrxSymbol>& out) {
  auto add = [&](const char* lib, const char* name, std::function<std::int64_t(Args)> fn) {
    out.push_back(PrxSymbol{lib, name, loader::nid::compute_nid(name, lib), true, std::move(fn)});
  };
  const char* A = "libSceAudioOut";
  const char* P = "libScePad";
  add(A, "sceAudioOutInit", [](Args) -> std::int64_t { std::lock_guard<std::mutex> l(st().mu); st().audio_init = true; return 0; });
  // sceAudioOutOpen(userId, type, index, len, freq, param) -> handle > 0, or a negative error.
  add(A, "sceAudioOutOpen", [](Args a) -> std::int64_t {
    if (a.size() < 6) return kErrInvalid;
    const std::uint32_t len = static_cast<std::uint32_t>(a[3]), freq = static_cast<std::uint32_t>(a[4]), fmt = static_cast<std::uint32_t>(a[5]);
    if (len == 0 || len > 4096 || (freq != 48000 && freq != 44100) || fmt > 3) return kErrInvalid;
    std::lock_guard<std::mutex> l(st().mu);
    if (!st().audio_init) return kErrInvalid;
    if (st().ports.size() >= 8) return kErrInvalid;
    Port p;
    p.info.handle = st().next_audio++;
    p.info.frames_per_buffer = len; p.info.sample_rate = freq; p.info.format = static_cast<AudioFormat>(fmt);
    p.info.channels = (fmt == 0 || fmt == 2) ? 1 : 2;
    const std::int32_t h = p.info.handle;
    st().ports.emplace(h, std::move(p));
    return h;
  });
  // sceAudioOutOutput(handle, ptr) -> frames consumed (len), or negative. ptr may be null (a flush): then 0.
  add(A, "sceAudioOutOutput", [](Args a) -> std::int64_t {
    if (a.size() < 2) return kErrInvalid;
    std::lock_guard<std::mutex> l(st().mu);
    auto it = st().ports.find(static_cast<std::int32_t>(a[0]));
    if (it == st().ports.end()) return kErrInvalid;
    if (!a[1]) return 0;
    Port& p = it->second;
    const std::size_t n = std::size_t(p.info.frames_per_buffer) * p.info.channels;
    std::vector<std::int16_t> s(n);
    if (p.info.format == AudioFormat::S16Mono || p.info.format == AudioFormat::S16Stereo) {
      std::memcpy(s.data(), reinterpret_cast<const void*>(a[1]), n * 2);
    } else {
      const float* f = reinterpret_cast<const float*>(a[1]);
      for (std::size_t i = 0; i < n; ++i) { float v; std::memcpy(&v, f + i, 4); s[i] = clip16(v); }
    }
    p.captured.insert(p.captured.end(), s.begin(), s.end());
    if (st().sink) st().sink(p.info.handle, s.data(), p.info.frames_per_buffer, p.info.channels);
    return p.info.frames_per_buffer;
  });
  add(A, "sceAudioOutClose", [](Args a) -> std::int64_t {
    if (a.empty()) return kErrInvalid;
    std::lock_guard<std::mutex> l(st().mu);
    return st().ports.erase(static_cast<std::int32_t>(a[0])) ? 0 : kErrInvalid;
  });

  add(P, "scePadInit", [](Args) -> std::int64_t { std::lock_guard<std::mutex> l(st().mu); st().pad_init = true; return 0; });
  // scePadOpen(userId, type, index, param) -> handle > 0. Only index 0 (one pad) exists.
  add(P, "scePadOpen", [](Args a) -> std::int64_t {
    if (a.size() < 3) return kErrInvalid;
    std::lock_guard<std::mutex> l(st().mu);
    if (!st().pad_init || a[2] != 0 || !st().pads.empty()) return kErrInvalid;
    const std::int32_t h = st().next_pad++;
    st().pads[h] = true;
    return h;
  });
  // scePadReadState(handle, ptr): writes PadData fields at offsets 0..9 only.
  add(P, "scePadReadState", [](Args a) -> std::int64_t {
    if (a.size() < 2 || !a[1]) return kErrInvalid;
    std::lock_guard<std::mutex> l(st().mu);
    if (!st().pads.count(static_cast<std::int32_t>(a[0]))) return kErrInvalid;
    std::uint8_t* d = reinterpret_cast<std::uint8_t*>(a[1]);
    const PadData& p = st().pad;
    std::memcpy(d, &p.buttons, 4);
    d[4] = p.lx; d[5] = p.ly; d[6] = p.rx; d[7] = p.ry; d[8] = p.l2; d[9] = p.r2;
    return 0;
  });
  add(P, "scePadClose", [](Args a) -> std::int64_t {
    if (a.empty()) return kErrInvalid;
    std::lock_guard<std::mutex> l(st().mu);
    return st().pads.erase(static_cast<std::int32_t>(a[0])) ? 0 : kErrInvalid;
  });
}

}  // namespace pnr::prx
