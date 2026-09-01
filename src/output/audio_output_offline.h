#pragma once
//
// Offline (faster-than-realtime) renderer: the exact same renderBuffer path
// as live playback, called in a tight loop with no threading -- which is what
// makes golden-file testing meaningful (plan §12/§14).
//
#include <vector>

#include "../sequencer/music_director.h"

namespace msynth {

class OfflineRenderer {
  public:
    OfflineRenderer() : director_(engine_) {}

    SynthEngine& engine() noexcept { return engine_; }
    MusicDirector& director() noexcept { return director_; }

    // Render `frames` (rounded down to whole buffers) of interleaved stereo.
    void render(std::vector<i16>& out, u64 frames) {
        i16 buffer[BUFFER_FRAMES * 2];
        u64 done = 0;
        while (done + BUFFER_FRAMES <= frames) {
            director_.advance(BUFFER_FRAMES);
            engine_.render(buffer, BUFFER_FRAMES);
            out.insert(out.end(), buffer, buffer + BUFFER_FRAMES * 2);
            done += BUFFER_FRAMES;
        }
    }

  private:
    SynthEngine engine_;
    MusicDirector director_;
};

}  // namespace msynth
