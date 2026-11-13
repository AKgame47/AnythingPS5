#include "services/audio.hpp"

namespace pnr::services {

std::shared_ptr<IAudioBackend> make_audio_backend() {
  // Only the null backend exists: no audio device API is probed in this
  // build (ALSA/Pulse headers not present in the dev sandbox; Planned).
  return std::make_shared<NullAudioBackend>();
}

}  // namespace pnr::services
