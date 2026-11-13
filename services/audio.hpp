// PNR Game Services - MODULE 08: Audio (v1)
// Portable audio backend interface. Only a null backend ships today: the
// dev sandbox has no audio device, and no PS5 audio algorithm is claimed
// (per the master prompt). Enumeration reports what the host actually
// exposes. Status: interface Implemented, host backends Planned.
#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include "core/runtime.hpp"

namespace pnr::services {

struct AudioDeviceInfo {
  std::string id;
  std::string name;
  unsigned channels = 2;
  unsigned sample_rate = 48000;
};

class IAudioStream {
public:
  virtual ~IAudioStream() = default;
  virtual Status write(const int16_t* samples, size_t frames) = 0;
  virtual Status set_volume(float v) = 0;  // 0..1
  virtual void close() = 0;
};

class IAudioBackend {
public:
  virtual ~IAudioBackend() = default;
  virtual std::string name() const = 0;
  virtual std::vector<AudioDeviceInfo> enumerate() = 0;
  virtual Status open_stream(const std::string& device_id,
                             IAudioStream** out) = 0;
};

// Null backend: honest "no audio available" on hosts without a device.
class NullAudioBackend final : public IAudioBackend {
public:
  std::string name() const override { return "null"; }
  std::vector<AudioDeviceInfo> enumerate() override { return {}; }
  Status open_stream(const std::string&, IAudioStream**) override {
    return Status::Fail(1, "audio", "no audio backend available on this host (audio devices Planned)");
  }
};

std::shared_ptr<IAudioBackend> make_audio_backend();

}  // namespace pnr::services
