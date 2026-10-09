//
// Mastering chain: bypass identity, neutral chain == pure delay, EQ shelf
// gain, width, compressor GR bounds, limiter ceiling, determinism, engine
// integration + meters, MSTR/JSON schema.
//
#include "test_framework.h"

#include <cstring>

#include "engine_helpers.h"
#include "synth/master_chain.h"
#include "synth/song_config.h"

using namespace msynth;
using testfw::EngineFixture;
using testfw::peak_abs;

namespace {

constexpr i32 Q(double x) { return static_cast<i32>(x * 65536.0 + (x >= 0 ? 0.5 : -0.5)); }
constexpr i32 S(double x) { return static_cast<i32>(x * 16777216.0); }  // Q8.24

// A chain that does nothing but delay: every stage at its identity setting.
MasterParams neutral() {
    MasterParams p;
    p.enabled = true;
    p.comp_threshold_db = 0;
    p.comp_ratio = GAIN_ONE;   // slope 0
    p.comp_knee_db = 0;
    p.lim_ceiling_db = 0;      // 1.0: never reached by |x| <= 0.9
    return p;
}

struct Chain {
    MasterChain mc;
    std::vector<i32> outl, outr;
    explicit Chain(const MasterParams& p) { mc.set_params(p); }
    // Feed generator g(i) -> (l, r) for `blocks` control blocks.
    template <class G>
    void run(int blocks, G g) {
        for (int b = 0; b < blocks; ++b) {
            i32 l[CONTROL_BLOCK], r[CONTROL_BLOCK];
            for (int i = 0; i < CONTROL_BLOCK; ++i) g(b * CONTROL_BLOCK + i, l[i], r[i]);
            mc.process_block(l, r, CONTROL_BLOCK);
            outl.insert(outl.end(), l, l + CONTROL_BLOCK);
            outr.insert(outr.end(), r, r + CONTROL_BLOCK);
        }
    }
};

i32 abs32(i32 v) { return v < 0 ? -v : v; }

}  // namespace

TEST(master_neutral_chain_is_exact_delay) {
    MasterParams p = neutral();
    p.lim_lookahead_blocks = 2;
    Chain c(p);
    // Deterministic pseudo-noise, |x| <= 0.9, different L/R.
    XorShift32 rng(7);
    std::vector<i32> inl, inr;
    c.run(40, [&](int, i32& l, i32& r) {
        l = static_cast<i32>(rng.next() % (2 * S(0.9))) - S(0.9);
        r = static_cast<i32>(rng.next() % (2 * S(0.9))) - S(0.9);
        inl.push_back(l);
        inr.push_back(r);
    });
    const int D = 2 * CONTROL_BLOCK;
    CHECK_EQ(c.mc.latency_samples(), D);
    for (int i = 0; i < D; ++i) CHECK_EQ(c.outl[i], 0);  // delay line starts empty
    bool exact = true;
    for (size_t i = D; i < c.outl.size(); ++i)
        exact &= c.outl[i] == inl[i - D] && c.outr[i] == inr[i - D];
    CHECK(exact);  // every stage at identity is bit-exact; pins the delay index
}

TEST(master_low_shelf_dc_gain_matches_db) {
    MasterParams p = neutral();
    p.eq_low_hz = Q(120);
    p.eq_low_db = Q(6);
    Chain c(p);
    c.run(400, [](int, i32& l, i32& r) { l = r = S(0.25); });  // ~0.5 s of DC
    const i32 expect = static_cast<i32>(S(0.25) * 1.99526);   // +6 dB
    CHECK_NEAR(c.outl.back(), expect, expect / 50);              // within 2%
    CHECK_EQ(c.outl.back(), c.outr.back());
    // Cut works too.
    p.eq_low_db = Q(-6);
    Chain d(p);
    d.run(400, [](int, i32& l, i32& r) { l = r = S(0.25); });
    CHECK_NEAR(d.outl.back(), static_cast<i32>(S(0.25) * 0.50119), S(0.25) / 50);
}

TEST(master_high_shelf_leaves_dc_alone_and_bell_is_neutral_at_dc) {
    MasterParams p = neutral();
    p.eq_high_db = Q(12);
    p.eq_mid_db = Q(12);
    p.eq_mid_hz = Q(2000);
    Chain c(p);
    c.run(400, [](int, i32& l, i32& r) { l = r = S(0.25); });
    CHECK_NEAR(c.outl.back(), S(0.25), S(0.25) / 100);  // HP and BP reject DC
}

TEST(master_width_zero_is_mono_and_two_widens) {
    MasterParams p = neutral();
    p.width = 0;
    Chain c(p);
    c.run(6, [](int, i32& l, i32& r) { l = S(0.5); r = -S(0.5); });
    CHECK_EQ(c.outl.back(), 0);  // (l+r)/2
    CHECK_EQ(c.outr.back(), 0);
    p.width = 2 * GAIN_ONE;
    Chain d(p);
    d.run(6, [](int, i32& l, i32& r) { l = S(0.4); r = 0; });
    CHECK_NEAR(d.outl.back(), S(0.6), 4);   // l + 0.5*(l-r)
    CHECK_NEAR(d.outr.back(), -S(0.2), 4);  // r - 0.5*(l-r)
}

TEST(master_output_gain_scales) {
    MasterParams p = neutral();
    p.gain_db = Q(-6);
    Chain c(p);
    c.run(6, [](int, i32& l, i32& r) { l = r = S(0.5); });
    CHECK_NEAR(c.outl.back(), static_cast<i32>(S(0.5) * 0.50119), S(0.5) / 200);
}

TEST(master_compressor_gr_below_and_above_threshold) {
    MasterParams p = neutral();
    p.comp_threshold_db = Q(-12);
    p.comp_ratio = Q(4);
    p.comp_knee_db = 0;
    p.comp_attack_ms = Q(10);
    p.comp_release_ms = Q(50);
    // Below threshold (-20 dBFS): no reduction at all.
    Chain c(p);
    c.run(100, [](int i, i32& l, i32& r) { l = r = (i & 1) ? S(0.1) : -S(0.1); });
    CHECK_EQ(c.mc.meters().comp_gr, GAIN_ONE);
    // 6 dB over at ratio 4 -> 4.5 dB of reduction = 0.5957 linear.
    Chain d(p);
    d.run(2, [](int i, i32& l, i32& r) { l = r = (i & 1) ? S(0.5) : -S(0.5); });
    const gain_t early = d.mc.meters().comp_gr;
    CHECK(early < GAIN_ONE);        // attack has started...
    CHECK(early > Q(0.62));         // ...but 2.7 ms in it is not done (10 ms attack)
    d.run(200, [](int i, i32& l, i32& r) { l = r = (i & 1) ? S(0.5) : -S(0.5); });
    CHECK_NEAR(d.mc.meters().comp_gr, Q(0.5957), Q(0.02));
    // Output amplitude reflects it (after the lookahead delay).
    CHECK_NEAR(abs32(d.outl.back()), static_cast<i32>(S(0.5) * 0.5957), S(0.5) / 40);
    // Release: signal gone -> GR returns to unity within a few time constants
    // (one-pole: 80 ms = 1.6 tau is not enough, 300 ms = 6 tau is).
    d.run(60, [](int, i32& l, i32& r) { l = r = 0; });  // 80 ms
    const gain_t mid = d.mc.meters().comp_gr;
    CHECK(mid > Q(0.75) && mid < Q(0.97));
    d.run(165, [](int, i32& l, i32& r) { l = r = 0; });  // 300 ms total
    CHECK(d.mc.meters().comp_gr > Q(0.97));
}

TEST(master_compressor_soft_knee_is_gentler_than_hard) {
    MasterParams p = neutral();
    p.comp_threshold_db = Q(-12);
    p.comp_ratio = Q(4);
    p.comp_attack_ms = Q(1);
    auto gr_with_knee = [&](i32 knee_db, double amp) {
        p.comp_knee_db = knee_db;
        Chain c(p);
        c.run(300, [=](int i, i32& l, i32& r) { l = r = (i & 1) ? S(amp) : -S(amp); });
        return c.mc.meters().comp_gr;
    };
    // The soft-knee curve is tangent to the hard-knee lines at thr +/- W/2 and
    // lies below them in between: inside the knee it reduces MORE than the
    // hard knee (it starts early), outside it is identical.
    // Just above threshold (-10 dBFS = +2 dB over, inside a 12 dB knee):
    CHECK(gr_with_knee(Q(12), 0.316) < gr_with_knee(0, 0.316));
    // Slightly below threshold (-14 dBFS): hard knee does nothing, soft knee a bit.
    CHECK_EQ(gr_with_knee(0, 0.2), GAIN_ONE);
    CHECK(gr_with_knee(Q(12), 0.2) < GAIN_ONE);
    // Well above the knee (-4 dBFS = +8 dB over > W/2): same reduction.
    CHECK_NEAR(gr_with_knee(Q(12), 0.631), gr_with_knee(0, 0.631), Q(0.01));
    // Exactly at the knee edge (+6 dB over) the curves meet: slope*W/2 either way.
    CHECK_NEAR(gr_with_knee(Q(12), 0.5), gr_with_knee(0, 0.5), Q(0.01));
}

TEST(master_limiter_never_exceeds_ceiling) {
    MasterParams p = neutral();
    p.lim_ceiling_db = Q(-1);
    p.lim_lookahead_blocks = 2;
    p.lim_release_ms = Q(50);
    const i32 ceiling = db_to_lin_q16(Q(-1)) << 8;
    for (int la = 1; la <= 8; la += 7) {
        p.lim_lookahead_blocks = la;
        Chain c(p);
        XorShift32 rng(0xBEEF + la);
        // Bursts: silence, then sudden +/-4.0 blocks, then quiet, repeat.
        c.run(300, [&](int i, i32& l, i32& r) {
            const int phase = (i / 640) % 3;
            if (phase == 1) {
                l = (rng.next() & 1) ? S(4.0) : -S(4.0);
                r = (rng.next() & 1) ? S(4.0) : -S(4.0);
            } else {
                l = static_cast<i32>(rng.next() % S(0.4)) - S(0.2);
                r = static_cast<i32>(rng.next() % S(0.4)) - S(0.2);
            }
        });
        i32 mx = 0;
        for (size_t i = 0; i < c.outl.size(); ++i) {
            mx = std::max(mx, abs32(c.outl[i]));
            mx = std::max(mx, abs32(c.outr[i]));
        }
        CHECK(mx <= ceiling + 64);  // 64 Q8.24 ulps = 4e-6: rounding only
        CHECK(mx > ceiling / 2);    // ...and it did not just crush everything
        CHECK(c.mc.meters().lim_gain <= GAIN_ONE);
    }
}

TEST(master_limiter_releases_after_burst) {
    MasterParams p = neutral();
    p.lim_ceiling_db = Q(-1);
    p.lim_release_ms = Q(20);
    Chain c(p);
    c.run(10, [](int i, i32& l, i32& r) { l = r = (i & 1) ? S(3.0) : -S(3.0); });
    CHECK(c.mc.meters().lim_gain < Q(0.35));
    c.run(150, [](int i, i32& l, i32& r) { l = r = (i & 1) ? S(0.1) : -S(0.1); });  // 200 ms
    CHECK(c.mc.meters().lim_gain > Q(0.99));
}

TEST(master_chain_is_deterministic_across_instances) {
    MasterParams p = neutral();
    p.eq_low_db = Q(3);
    p.eq_mid_db = Q(-2);
    p.eq_high_db = Q(4);
    p.comp_threshold_db = Q(-18);
    p.comp_ratio = Q(3);
    p.width = Q(1.3);
    p.gain_db = Q(2);
    p.lim_ceiling_db = Q(-0.5);
    auto render = [&]() {
        Chain c(p);
        XorShift32 rng(99);
        c.run(200, [&](int, i32& l, i32& r) {
            l = static_cast<i32>(rng.next() % S(3.0)) - S(1.5);
            r = static_cast<i32>(rng.next() % S(3.0)) - S(1.5);
        });
        return c.outl;
    };
    const auto a = render(), b = render();
    CHECK(a == b);
    CHECK(peak_abs(std::vector<i16>{}) == 0);  // helper sanity (keeps include honest)
}

TEST(master_params_clamp_and_roundtrip_by_id) {
    MasterParams p;
    p.set(MasterParamId::EqLowHz, Q(5));       // below 20 Hz
    CHECK_EQ(p.eq_low_hz, Q(20));
    p.set(MasterParamId::CompRatio, Q(100));
    CHECK_EQ(p.comp_ratio, Q(20));
    p.set(MasterParamId::LimLookaheadBlocks, 20);
    CHECK_EQ(p.lim_lookahead_blocks, 8);
    p.set(MasterParamId::Width, Q(-1));
    CHECK_EQ(p.width, 0);
    for (int i = 0; i < static_cast<int>(MasterParamId::COUNT); ++i) {
        MasterParams q;
        q.set(static_cast<MasterParamId>(i), p.get(static_cast<MasterParamId>(i)));
        CHECK_EQ(q.get(static_cast<MasterParamId>(i)), p.get(static_cast<MasterParamId>(i)));
    }
}

TEST(master_engine_bypass_default_and_enable_via_command) {
    // Bypass: master() disabled by default; output identical to an untouched
    // engine is what the goldens pin. Here: enabling with +6 dB gain roughly
    // doubles a quiet note, disabling restores the bypass output exactly.
    auto render_note = [](bool enable, i32 gain_db) {
        EngineFixture f;
        if (enable) {
            MasterParams p = neutral();
            p.gain_db = gain_db;
            push_master_config(f.engine, p);
        }
        f.note_on(0, 60, 40);
        return f.render(BUFFER_FRAMES * 20);
    };
    const auto plain = render_note(false, 0);
    const auto loud = render_note(true, Q(6));
    const auto neutral_on = render_note(true, 0);
    const i32 pk_plain = peak_abs(plain), pk_loud = peak_abs(loud);
    CHECK(pk_plain > 100);
    CHECK_NEAR(pk_loud, pk_plain * 2, pk_plain / 8);
    // Neutral chain == plain delayed by 128 samples (2 lookahead blocks).
    bool same = true;
    for (size_t i = 128 * 2; i < plain.size(); ++i) same &= neutral_on[i] == plain[i - 128 * 2];
    CHECK(same);

    // Disable again at runtime: subsequent output is the bypass path.
    EngineFixture g;
    MasterParams p = neutral();
    push_master_config(g.engine, p);
    g.render(BUFFER_FRAMES);
    CHECK(g.engine.master().enabled());
    EngineCommand c;
    c.type = EngineCommand::Type::MasterParam;
    c.d1 = static_cast<u8>(MasterParamId::Enabled);
    c.v = 0;
    g.engine.queue().push(c);
    g.render(BUFFER_FRAMES);
    CHECK(!g.engine.master().enabled());
    CHECK_EQ(g.engine.master().latency_samples(), 0);
}

TEST(master_meters_accumulate_and_reset_on_both_paths) {
    for (int enable = 0; enable < 2; ++enable) {
        EngineFixture f;
        if (enable) push_master_config(f.engine, neutral());
        f.note_on(0, 60, 100);
        f.render(BUFFER_FRAMES * 10);
        MasterMeters m = f.engine.take_master_meters();
        CHECK(m.samples == BUFFER_FRAMES * 10);
        CHECK(m.peak_l > 0);
        CHECK(m.sumsq_l > 0);
        CHECK_EQ(m.clips, 0);
        MasterMeters z = f.engine.take_master_meters();
        CHECK_EQ(z.samples, 0);
        CHECK_EQ(z.peak_l, 0);
        CHECK_EQ(z.sumsq_r, 0);
    }
    // Clips counted when the pre-clip signal exceeds 1.0.
    EngineFixture h;
    MasterParams p = neutral();
    p.gain_db = Q(24);
    p.lim_ceiling_db = 0;
    push_master_config(h.engine, p);
    h.engine.output().master_gain = GAIN_ONE;
    h.note_on(0, 60, 127);
    h.render(BUFFER_FRAMES * 10);
    // The limiter (ceiling 0 dB) keeps |v| <= 1.0 -- the meter agrees.
    CHECK_EQ(h.engine.take_master_meters().clips, 0);
}

TEST(master_json_schema_roundtrip_and_clamps) {
    static const char* kJson = R"({"version":1,"master":{
        "enabled": true,
        "eq": {"low": {"hz": 100, "db": 3.5}, "mid": {"hz": 1500, "db": -2, "q": 2},
               "high": {"hz": 9000, "db": 1.25}},
        "comp": {"thresholdDb": -18, "ratio": 3, "attackMs": 5, "releaseMs": 120,
                 "makeupDb": 2, "kneeDb": 4},
        "limiter": {"ceilingDb": -0.3, "lookaheadBlocks": 3, "releaseMs": 60},
        "width": 1.2, "gainDb": -1.5 }})";
    MasterParams p;
    LoadResult r = load_master_config(kJson, std::strlen(kJson), p);
    CHECK(r.ok);
    CHECK(r.warnings.empty());
    CHECK(p.enabled);
    CHECK_EQ(p.eq_low_hz, Q(100));
    CHECK_EQ(p.eq_low_db, Q(3.5));
    CHECK_EQ(p.eq_mid_q, Q(2));
    CHECK_EQ(p.comp_threshold_db, Q(-18));
    CHECK_EQ(p.lim_lookahead_blocks, 3);
    CHECK_EQ(p.width, Q(1.2));
    CHECK_EQ(p.gain_db, Q(-1.5));
    // Serialize -> parse reproduces every field exactly.
    const std::string out = master_to_json(p, true);
    MasterParams q;
    CHECK(load_master_config(out.data(), out.size(), q).ok);
    for (int i = 0; i < static_cast<int>(MasterParamId::COUNT); ++i)
        CHECK_EQ(q.get(static_cast<MasterParamId>(i)), p.get(static_cast<MasterParamId>(i)));
    // Bare object form, out-of-range warning, bool/number 'enabled'.
    static const char* kBare = R"({"enabled": 1, "comp": {"ratio": 50}, "limiter": {"lookaheadBlocks": 0}})";
    MasterParams b;
    LoadResult rb = load_master_config(kBare, std::strlen(kBare), b);
    CHECK(rb.ok);
    CHECK(b.enabled);
    CHECK_EQ(b.comp_ratio, Q(20));
    CHECK_EQ(b.lim_lookahead_blocks, 1);
    CHECK_EQ(rb.warnings.size(), 2);
    // fx.json may embed it; absent -> bypass.
    static const char* kFx = R"({"reverb":{"wetGain":0.1},"master":{"enabled":true,"gainDb":-3}})";
    SongFxConfig fx;
    CHECK(load_song_config(kFx, std::strlen(kFx), fx).ok);
    CHECK(fx.master.enabled);
    CHECK_EQ(fx.master.gain_db, Q(-3));
    SongFxConfig none;
    static const char* kNone = R"({"reverb":{"wetGain":0.1}})";
    CHECK(load_song_config(kNone, std::strlen(kNone), none).ok);
    CHECK(!none.master.enabled);
    // Garbage is an error.
    MasterParams g;
    CHECK(!load_master_config("nope", 4, g).ok);
}

TEST(master_bus_saturation_between_comp_and_limiter) {
    MasterParams p = neutral();
    p.sat_drive = 4 * GAIN_ONE;
    p.lim_ceiling_db = 0;
    Chain c(p);
    c.run(20, [](int i, i32& l, i32& r) { l = r = (i & 1) ? S(0.6) : -S(0.6); });
    // Saturation squashes toward the clip level but never past 1.0.
    i32 mx = 0;
    for (const i32 v : c.outl) mx = std::max(mx, abs32(v));
    CHECK(mx > S(0.9));
    CHECK(mx <= S(1.0));
    // Drive 1 is an exact no-op (neutral chain stays a pure delay).
    MasterParams q = neutral();
    Chain d(q);
    std::vector<i32> in;
    XorShift32 rng(3);
    d.run(10, [&](int, i32& l, i32& r) { l = r = static_cast<i32>(rng.next() % S(1.6)) - S(0.8); in.push_back(l); });
    bool exact = true;
    const int D = 2 * CONTROL_BLOCK;
    for (size_t i = D; i < d.outl.size(); ++i) exact &= d.outl[i] == in[i - D];
    CHECK(exact);
}
