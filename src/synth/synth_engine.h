#pragma once
//
// SynthEngine: renderBuffer() entry point (plan §7). Commands arrive through
// a lock-free SPSC queue timestamped in absolute engine samples; they are
// applied at control-block boundaries (64 samples ≈ 1.3 ms -- the plan's
// event-timing resolution). Everything reachable from render() obeys the
// real-time rules: no allocation, no locks, no I/O.
//
#include "../sequencer/event_queue.h"
#include "channel_state.h"
#include "effects/chorus.h"
#include "effects/reverb.h"
#include "limiter.h"
#include "master_chain.h"
#include "channel_eq.h"
#include "sidechain.h"
#include "effects/delay_fx.h"
#include "patch_bank.h"
#include "voice_pool.h"

namespace msynth {

struct EngineCommand {
    enum class Type : u8 {
        NoteOn,        // slot, d1=note, d2=vel
        NoteOff,       // slot, d1=note
        CC,            // slot, d1=cc, d2=value
        Program,       // slot, d1=program
        PitchBend,     // slot, v=-8192..8191
        Aftertouch,    // slot, d1=pressure
        ChannelFade,   // slot, v=target Q16, v2=fade blocks
        MasterGain,    // v=Q16 gain
        DitherEnable,  // d1=0/1
        ReverbConfig,  // d1=param(0 room,1 damp,2 width,3 wet), v=Q16
        ChorusConfig,  // d1=param(0 rate,1 depthMs,2 wet), v=Q16
        ResetBank,     // d1=bank(0/1): full reset of its 16 channel slots
        AllSoundOffBank,  // d1=bank: fast-release every voice in the bank
        SetOutgoingBank,  // d1=bank or 0xFF: lower steal protection (crossfade)
        Duck,             // v=target Q16, v2=ramp blocks (dialogue/SFX priority)
        MasterParam,      // d1=MasterParamId, v=value (see master_chain.h)
        PolyAftertouch,   // slot, d1=note, d2=pressure
        TempoBpm,         // v=Q16 BPM (sequencer feeds tempo-synced LFOs)
        TuningCent,       // d1=pitch class 0..11, v=Q16 cents (microtuning)
        DelayConfig,      // d1=param(0 timeMs,1 feedback,2 wet,3 pingPong,4 syncBeats), v=Q16
        SidechainConfig,  // d1=param(0 source,1 note,2 targets,3 depth,4 atk,5 hold,6 rel), v=Q16
        ChannelEqParam,   // slot, d1=param(0 lowDb,1 midDb,2 midHz,3 highDb), v=Q16
    };

    u64 when = 0;  // absolute engine sample position; <= now applies immediately
    Type type = Type::NoteOn;
    u8 slot = 0;
    u8 d1 = 0, d2 = 0;
    i32 v = 0, v2 = 0;
};

using EngineQueue = SpscQueue<EngineCommand, 12>;  // 4096 commands

class SynthEngine {
  public:
    SynthEngine();

    // Control thread, before rendering starts (or between songs).
    void set_content(const PatchBank* bank, const DrumKit* kit) noexcept {
        bank_ = bank;
        kit_ = kit;
    }

    EngineQueue& queue() noexcept { return queue_; }

    // Audio thread. frames must be a multiple of CONTROL_BLOCK.
    void render(i16* out_interleaved, int frames) noexcept;

    u64 now() const noexcept { return sample_time_; }
    const PoolDiagnostics& pool_diag() const noexcept { return pool_diag_public_; }
    const ChannelState& channel(int slot) const noexcept { return channels_[slot]; }
    i32 active_note_count(int slot) const noexcept;
    // Distinct MIDI notes currently sounding on a slot (tool diagnostics).
    int active_notes(int slot, u8* out, int max) const noexcept;

    OutputStage& output() noexcept { return out_stage_; }
    // Mastering chain (bypassed by default). Meters accumulate on both paths;
    // take_master_meters() is a diagnostics read like pool_diag().
    const MasterChain& master() const noexcept { return master_; }
    MasterMeters take_master_meters() noexcept { return master_.take_meters(); }

    // Authoring hot-swap prep: fast-release voices holding pointers into a
    // patch about to be replaced. Single-threaded contexts only (offline /
    // worklet message handler), never concurrent with render().
    void fast_release_patch(const Patch* p) noexcept { pool_.fast_release_patch(p); }
    void fast_release_all_voices() noexcept { pool_.fast_release_all(); }

  private:
    void apply_command(const EngineCommand& c) noexcept;
    void note_on(u8 slot, u8 note, u8 vel) noexcept;
    void handle_cc(u8 slot, u8 cc, u8 val) noexcept;
    void handle_data_entry(ChannelState& ch, u8 msb) noexcept;
    void render_one_block(i16* out) noexcept;

    const PatchBank* bank_ = nullptr;
    const DrumKit* kit_ = nullptr;
    ChannelState channels_[NUM_CHANNEL_SLOTS];
    VoicePool pool_;
    PoolDiagnostics pool_diag_public_;
    EngineQueue queue_;

    Reverb reverb_;
    Chorus chorus_;
    DelayFx delay_fx_;
    ReverbParams reverb_params_;
    ChorusParams chorus_params_;
    DelayParams delay_params_;
    gain_t tempo_bpm_q16_ = 120 << GAIN_SHIFT;
    i32 tuning_cents_q16_[12] = {};
    OutputStage out_stage_;
    MasterChain master_;

    u64 sample_time_ = 0;
    u8 outgoing_bank_ = 0xFF;

    // Per-block stereo accumulators (i64 Q8.24; summed unclamped).
    i64 dry_l_[CONTROL_BLOCK], dry_r_[CONTROL_BLOCK];
    i64 rev_l_[CONTROL_BLOCK], rev_r_[CONTROL_BLOCK];
    i64 cho_l_[CONTROL_BLOCK], cho_r_[CONTROL_BLOCK];
    i64 del_l_[CONTROL_BLOCK], del_r_[CONTROL_BLOCK];
    // Per-channel dry lanes (mixer strips): voices write into their slot's
    // lane; active lanes run the channel EQ, then sum into dry. Integer
    // addition is exactly associative, so with every EQ flat this is
    // byte-identical to the old single-bus path.
    i64 chan_l_[NUM_CHANNEL_SLOTS][CONTROL_BLOCK] = {};
    i64 chan_r_[NUM_CHANNEL_SLOTS][CONTROL_BLOCK] = {};
    bool chan_dirty_[NUM_CHANNEL_SLOTS] = {};
    ChannelEq chan_eq_[NUM_CHANNEL_SLOTS];
    Sidechain sidechain_;
    SidechainParams sidechain_params_;
    i32 mst_l_[CONTROL_BLOCK], mst_r_[CONTROL_BLOCK];  // mastering scratch (Q8.24)
};

}  // namespace msynth
