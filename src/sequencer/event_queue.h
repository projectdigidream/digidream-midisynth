#pragma once
//
// Lock-free SPSC ring buffer: control thread -> audio thread (plan §11/§12).
// Fixed capacity, power of two; a full queue drops the push and counts it
// (dropped_count must stay 0 in any healthy run -- it's a diagnostic, §11).
//
#include <atomic>

#include "../synth/fixmath.h"

namespace msynth {

template <typename T, int LOG2_CAP>
class SpscQueue {
  public:
    static constexpr u32 CAP = 1u << LOG2_CAP;
    static constexpr u32 MASK = CAP - 1;

    bool push(const T& v) noexcept {
        const u32 w = write_.load(std::memory_order_relaxed);
        const u32 r = read_.load(std::memory_order_acquire);
        if (w - r >= CAP) {
            ++dropped_;
            return false;
        }
        buf_[w & MASK] = v;
        write_.store(w + 1, std::memory_order_release);
        const u32 depth = w + 1 - r;
        if (depth > high_water_) high_water_ = depth;
        return true;
    }

    bool peek(T& out) const noexcept {
        const u32 r = read_.load(std::memory_order_relaxed);
        const u32 w = write_.load(std::memory_order_acquire);
        if (r == w) return false;
        out = buf_[r & MASK];
        return true;
    }

    void pop() noexcept { read_.store(read_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

    u32 size() const noexcept {
        return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire);
    }

    u32 high_water() const noexcept { return high_water_; }
    u64 dropped() const noexcept { return dropped_; }

  private:
    T buf_[CAP];
    std::atomic<u32> write_{0};
    std::atomic<u32> read_{0};
    u32 high_water_ = 0;
    u64 dropped_ = 0;
};

}  // namespace msynth
