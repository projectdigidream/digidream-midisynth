#pragma once
//
// Melodic patch bank + drum kit (plan §8, §17). Unknown program numbers
// resolve to the fallback patch -- never silence.
//
#include <map>
#include <memory>

#include "patch_def.h"

namespace msynth {

class PatchBank {
  public:
    const Patch* lookup(u8 program) const noexcept {
        auto it = programs_.find(program);
        return it != programs_.end() ? it->second.get() : &fallback_;
    }
    // Authored patch or null -- pointer identity for targeted voice release
    // before a hot swap (authoring tool).
    const Patch* exact(u8 program) const noexcept {
        auto it = programs_.find(program);
        return it != programs_.end() ? it->second.get() : nullptr;
    }
    void set_program(u8 program, std::unique_ptr<Patch> p) { programs_[program] = std::move(p); }
    Patch& fallback() noexcept { return fallback_; }
    size_t patch_count() const noexcept { return programs_.size(); }

  private:
    std::map<u8, std::unique_ptr<Patch>> programs_;
    Patch fallback_;  // must be populated at init (patch_loader)
};

class DrumKit {
  public:
    // Null = unmapped drum note (silent, by design -- unlike melodic programs).
    const Patch* lookup(u8 note) const noexcept { return notes_[note & 127].get(); }
    void set_note(u8 note, std::unique_ptr<Patch> p) { notes_[note & 127] = std::move(p); }
    size_t note_count() const noexcept {
        size_t n = 0;
        for (const auto& p : notes_)
            if (p) ++n;
        return n;
    }

  private:
    std::unique_ptr<Patch> notes_[128];
};

}  // namespace msynth
