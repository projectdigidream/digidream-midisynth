#pragma once
//
// Compact SHA-256 (golden-file hashing, plan §14). Not performance-critical.
//
#include <string>
#include <vector>

#include "../synth/fixmath.h"

namespace msynth {

class Sha256 {
  public:
    Sha256() { reset(); }

    void reset() {
        static constexpr u32 H0[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        for (int i = 0; i < 8; ++i) h_[i] = H0[i];
        len_ = 0;
        buf_used_ = 0;
    }

    void update(const void* data, size_t n) {
        const u8* p = static_cast<const u8*>(data);
        len_ += n;
        while (n > 0) {
            const size_t take = n < (64 - buf_used_) ? n : (64 - buf_used_);
            for (size_t i = 0; i < take; ++i) buf_[buf_used_ + i] = p[i];
            buf_used_ += take;
            p += take;
            n -= take;
            if (buf_used_ == 64) {
                block(buf_);
                buf_used_ = 0;
            }
        }
    }

    std::string hex_digest() {
        const u64 bits = len_ * 8;
        u8 pad[72];
        size_t padlen = 0;
        pad[padlen++] = 0x80;
        while ((buf_used_ + padlen) % 64 != 56) pad[padlen++] = 0;
        for (int i = 7; i >= 0; --i) pad[padlen++] = static_cast<u8>(bits >> (i * 8));
        update(pad, padlen);

        static const char* hex = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (int i = 0; i < 8; ++i)
            for (int b = 28; b >= 0; b -= 4) out += hex[(h_[i] >> b) & 0xF];
        return out;
    }

  private:
    static u32 rotr(u32 x, int n) { return (x >> n) | (x << (32 - n)); }

    void block(const u8* p) {
        static constexpr u32 K[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
            0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
            0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
            0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
            0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
            0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
            0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
            0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
        u32 w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<u32>(p[i * 4]) << 24) | (static_cast<u32>(p[i * 4 + 1]) << 16) |
                   (static_cast<u32>(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            const u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        u32 a = h_[0], b = h_[1], c = h_[2], d = h_[3];
        u32 e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            const u32 S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const u32 ch = (e & f) ^ (~e & g);
            const u32 t1 = h + S1 + ch + K[i] + w[i];
            const u32 S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const u32 maj = (a & b) ^ (a & c) ^ (b & c);
            const u32 t2 = S0 + maj;
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
        h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
    }

    u32 h_[8];
    u64 len_ = 0;
    u8 buf_[64];
    size_t buf_used_ = 0;
};

inline std::string sha256_hex(const void* data, size_t n) {
    Sha256 s;
    s.update(data, n);
    return s.hex_digest();
}

}  // namespace msynth
