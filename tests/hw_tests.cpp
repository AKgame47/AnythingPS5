// Core slice C5 tests: audio out, pad input and save data host models. SYNTHETIC data only.
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "loader/nid.hpp"
#include "prx/hw_abi.hpp"
#include "prx/prx.hpp"
#include "services/save_data.hpp"

void pnr_hw_tests(int& pass, int& fail) {
  using namespace pnr::prx;
  auto check = [&](bool ok, const std::string& l) { if (ok) ++pass; else ++fail; std::printf("%s %s\n", ok ? "PASS" : "FAIL", l.c_str()); };
  PrxRegistry reg;
  auto call = [&](const char* lib, const char* name, std::vector<std::int64_t> a) -> std::int64_t {
    std::function<std::int64_t(const std::vector<std::int64_t>&)> fn; std::string err;
    if (reg.bind(pnr::loader::nid::compute_nid(name, lib), &fn, &err) != SCE_OK) return -9999;
    return fn(a);
  };
  auto P = [](const void* p) { return reinterpret_cast<std::int64_t>(p); };
  audio_reset(); pad_reset();

  // ---- audio ----
  check(call("libSceAudioOut", "sceAudioOutOpen", {0, 0, 0, 256, 48000, 1}) < 0, "audio: open before init is refused");
  check(call("libSceAudioOut", "sceAudioOutInit", {}) == 0, "audio: init");
  check(call("libSceAudioOut", "sceAudioOutOpen", {0, 0, 0, 0, 48000, 1}) < 0 && call("libSceAudioOut", "sceAudioOutOpen", {0, 0, 0, 256, 12345, 1}) < 0 && call("libSceAudioOut", "sceAudioOutOpen", {0, 0, 0, 256, 48000, 9}) < 0, "audio: zero length, odd rate and unknown format are refused");
  const std::int64_t h = call("libSceAudioOut", "sceAudioOutOpen", {0, 0, 0, 4, 48000, 1});
  check(h > 0 && audio_ports().size() == 1 && audio_ports()[0].channels == 2 && audio_ports()[0].frames_per_buffer == 4, "audio: stereo S16 port opens with a positive handle");
  std::int16_t buf[8] = {1, -1, 2, -2, 3, -3, 4, -4};
  int sunk = 0; std::size_t sunk_frames = 0;
  audio_set_sink([&](std::int32_t, const std::int16_t*, std::size_t frames, unsigned) { ++sunk; sunk_frames += frames; });
  check(call("libSceAudioOut", "sceAudioOutOutput", {h, P(buf)}) == 4, "audio: output consumes one buffer and returns its frame count");
  auto cap = audio_captured(static_cast<std::int32_t>(h));
  check(cap.size() == 8 && std::memcmp(cap.data(), buf, 16) == 0 && sunk == 1 && sunk_frames == 4, "audio: samples captured exactly and delivered to the host sink");
  check(call("libSceAudioOut", "sceAudioOutOutput", {h, 0}) == 0 && audio_captured(static_cast<std::int32_t>(h)).size() == 8, "audio: a null buffer is a flush and adds nothing");
  const std::int64_t hf = call("libSceAudioOut", "sceAudioOutOpen", {0, 0, 0, 2, 44100, 2});
  float fb[2] = {0.5f, 2.0f};
  check(hf > 0 && call("libSceAudioOut", "sceAudioOutOutput", {hf, P(fb)}) == 2, "audio: float mono port accepts a buffer");
  auto fc = audio_captured(static_cast<std::int32_t>(hf));
  check(fc.size() == 2 && std::abs(fc[0] - 16383) <= 1 && fc[1] == 32767, "audio: float converts to S16 and clips at full scale");
  check(call("libSceAudioOut", "sceAudioOutOutput", {9999, P(buf)}) < 0, "audio: output on an unknown handle is an error");
  check(call("libSceAudioOut", "sceAudioOutClose", {h}) == 0 && call("libSceAudioOut", "sceAudioOutClose", {h}) < 0, "audio: close once, closing again is an error");
  int opened = 0; for (int i = 0; i < 10; ++i) opened += call("libSceAudioOut", "sceAudioOutOpen", {0, 0, 0, 8, 48000, 0}) > 0;
  check(opened == 7 && audio_ports().size() == 8, "audio: at most 8 ports are open at once");
  audio_reset();

  // ---- pad ----
  check(call("libScePad", "scePadOpen", {0, 0, 0, 0}) < 0, "pad: open before init is refused");
  check(call("libScePad", "scePadInit", {}) == 0, "pad: init");
  check(call("libScePad", "scePadOpen", {0, 0, 1, 0}) < 0, "pad: only index 0 exists");
  const std::int64_t ph = call("libScePad", "scePadOpen", {0, 0, 0, 0});
  check(ph > 0 && call("libScePad", "scePadOpen", {0, 0, 0, 0}) < 0, "pad: one pad opens, a second open is refused");
  std::uint8_t out[16]; std::memset(out, 0xEE, sizeof out);
  check(call("libScePad", "scePadReadState", {ph, P(out)}) == 0 && out[4] == 128 && out[5] == 128 && out[8] == 0 && std::memcmp(out, "\0\0\0\0", 4) == 0, "pad: idle state is no buttons, sticks centred, triggers 0");
  check(out[10] == 0xEE && out[15] == 0xEE, "pad: bytes beyond offset 9 are left untouched");
  check(pad_apply_host_event("BTN_SOUTH", 1) && pad_apply_host_event("ABS_X", -32768) && pad_apply_host_event("ABS_Y", 32767) && pad_apply_host_event("ABS_Z", 200) && pad_apply_host_event("ABS_HAT0X", 1), "pad: mapped host events are accepted");
  call("libScePad", "scePadReadState", {ph, P(out)});
  std::uint32_t b; std::memcpy(&b, out, 4);
  check((b & pad_bits::CROSS) && (b & pad_bits::RIGHT) && !(b & pad_bits::LEFT) && out[4] == 0 && out[5] == 255 && out[8] == 200, "pad: cross, d-pad right, stick extremes and trigger read back");
  pad_apply_host_event("BTN_SOUTH", 0); pad_apply_host_event("ABS_HAT0X", 0);
  call("libScePad", "scePadReadState", {ph, P(out)}); std::memcpy(&b, out, 4);
  check(b == 0, "pad: release clears the bits");
  check(!pad_apply_host_event("BTN_MISSING", 1), "pad: an unmapped control is reported, not guessed");
  check(call("libScePad", "scePadReadState", {ph, 0}) < 0 && call("libScePad", "scePadReadState", {777, P(out)}) < 0, "pad: null buffer and unknown handle are errors");
  check(call("libScePad", "scePadClose", {ph}) == 0 && call("libScePad", "scePadReadState", {ph, P(out)}) < 0, "pad: closed handle no longer reads");
  pad_reset();

  // ---- save data ----
  namespace fs = std::filesystem;
  const std::string root = "/tmp/pnr_save_" + std::to_string(::getpid());
  fs::remove_all(root);
  {
    using pnr::services::SaveDataService;
    pnr::services::VirtualFileSystem vfs(root);
    SaveDataService sd(vfs, "user1", "game1", 100);
    check(SaveDataService::valid_name("slot0") && !SaveDataService::valid_name("../x") && !SaveDataService::valid_name("a/b") && !SaveDataService::valid_name(".hid") && !SaveDataService::valid_name("") && !SaveDataService::valid_name(std::string(33, 'a')), "save: names are validated");
    check(!sd.write("slot0", "a.dat", "x").ok, "save: writing to a directory that does not exist fails");
    check(sd.create_dir("slot0").ok && sd.write("slot0", "a.dat", "hello").ok, "save: create directory, write a file");
    std::string got;
    check(sd.read("slot0", "a.dat", &got).ok && got == "hello", "save: read returns what was written");
    check(sd.write("slot0", "a.dat", std::string(90, 'z')).ok && sd.usage("slot0") == 90, "save: overwrite replaces, usage is exact");
    check(!sd.write("slot0", "b.dat", std::string(20, 'q')).ok && !sd.list_files("slot0").empty() && sd.list_files("slot0").size() == 1, "save: quota exceeded refuses the write and leaves nothing behind");
    check(!sd.write("slot0", "../escape", "x").ok && !sd.create_dir("../up").ok && !sd.read("slot0", "..", &got).ok, "save: traversal names are refused");
    SaveDataService other(vfs, "user2", "game1", 100);
    check(!other.read("slot0", "a.dat", &got).ok && other.list_dirs().empty(), "save: another user cannot see the first user's saves");
    SaveDataService othergame(vfs, "user1", "game2", 100);
    check(othergame.list_dirs().empty(), "save: another game cannot see them either");
    check(sd.list_dirs().size() == 1 && sd.remove("slot0", "a.dat").ok && !sd.remove("slot0", "a.dat").ok && sd.remove_dir("slot0").ok && sd.list_dirs().empty(), "save: remove file, remove again fails, remove directory");
  }
  fs::remove_all(root);
}
