// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// The S-YXG2006LE voice's per-block control across a batch of voices (docs/ACCELERATION.md, "Control
// across voices"), and a target's kernel over it.
//
// The reference is syxg2006le::DspVoice::control, voice by voice. A kernel takes the steady states with
// the reference's operations in its order, and clears ok[i] for any lane that leaves them this block --
// a stage end of the amp EG, an instant decay -- or that it does not take (ok[i] = 0 on the way in: an
// LFO delay or fade, the sine wave, a recompute); that voice runs the reference instead.
//
// The voices' state is read and written where it is, in the DspVoice objects: lane i's voice at
// base + voice[i], each field at its offset (FieldsLE, from DspVoice), by indexed loads and stores --
// no copies. A kernel writes a voice only where its lane stays ok, so a voice it gives back is untouched.
// What the lanes carry: the block's inputs worked out from the controls (scalar, by to_lanes), the
// voice's flags (bytes, which RVV 0.7.1 cannot gather without fractional LMUL), and what goes back to
// the flags (from_lanes).

#include <cstddef>
#include <cstdint>

#include "accel/kernels.hpp"

namespace hvyxg::accel {

/// The S-YXG2006LE's block (syxg2006le::kBlock; dsp_voice.cpp checks).
inline constexpr std::int32_t kBlockLE = 64;

/// Byte offsets of the fields the kernel reads and writes, within a DspVoice (all 32-bit; pos_hi: the
/// high word of the 64-bit sample position, for its sign).
struct FieldsLE {
    std::uint32_t lfo_wave, lfo_invert, lfo_phase, lfo_inc, lfo_pitch_depth, lfo_amp_depth, lfo_filter_depth, lfo_amod,
        lfo_sh, out_pitch, out_amp, out_filter, out_bias;
    struct Eg {
        std::uint32_t state, pos, inc0, inc1, inc2, inc_sustain, inc_release, level1, level2, level3, release_level,
            delay, delay_target;
    } eg[2];                         // [0] pitch, [1] filter
    std::uint32_t porta_pos, porta_step, pos_hi, loop_tune, inc, pitch_base;
    std::uint32_t cutoff_cur, cutoff_target, f_prev, f_new;
    std::uint32_t amp_state, amp_x, amp_hold, amp_xstep, amp_inc1, amp_inc2, amp_inc3, amp_inc_release, amp_level1,
        amp_level2, amp_level3, amp_decay1_rate, amp_decay2_rate, amp_gain_prev, amp_gain, amp_slope, amp_intercept;
    std::uint32_t stage_ticks, peak_timer;
    std::uint32_t level_x, pan_x[2], send_x[4], gain_prev[7], mix_from[7], mix_to[7];
};

struct LanesLE {
    static constexpr std::size_t kN = kBatch;
    std::size_t n = 0;
    std::uint8_t* base = nullptr;    // the voices: lane i's at base + voice[i]
    std::uint32_t voice[kN];

    // in: worked out by to_lanes
    std::int32_t ok[kN];             // 1 if the voice may take the kernel (out: 0 where it left the steady states)
    std::int32_t gated[kN];          // the 30 ms start gate is still closed
    std::int32_t pitch_base[kN];     // cents_to_log(pitch cents)
    std::int32_t cutoff_target[kN];  // cents_to_log(cutoff cents) + the rate's cutoff base
    std::int32_t ceiling[kN];        // the filter type's resonance-dependent ceiling (<< 8), or 0xFFF
    float amod_target[kN];           // controller AMod depth << 12
    float pmod[kN], fmod[kN];        // lfo_cents of the controller PMod / FMod depths
    std::int32_t level_t[kN], pan_t[2][kN], send_t[4][kN];   // output targets << 12
    std::int32_t amp_inc_attack[kN]; // max(1, inc[0]·128/255): the amp EG's state-0 step
    // the flags: in, and out
    std::int32_t sh_flag[kN], mixed[kN];
    // out
    std::int32_t finished[kN], peak_reset[kN];
};

/// The rate tables the kernel reads.
struct ControlTablesLE {
    const float* pitch = nullptr;    // RateTables::pitch (1024 mantissas)
    std::int32_t inc_cutoff_glide = 0, inc_sustain_glide = 0, inc_pan = 0, inc_send = 0;
};

#if defined(HVYXG_ACCEL_RVV) && !defined(HVYXG_NO_CONTROL_LE)   // (HVYXG_NO_CONTROL_LE: the reference, to compare)
namespace rvv {
void control_le(LanesLE& L, const FieldsLE& f, const ControlTablesLE& t);
} // namespace rvv
/// A target has a control kernel for the S-YXG2006LE.
inline constexpr bool kControlLE = true;
inline void control_le(LanesLE& L, const FieldsLE& f, const ControlTablesLE& t) { rvv::control_le(L, f, t); }
#else
inline constexpr bool kControlLE = false;
inline void control_le(LanesLE&, const FieldsLE&, const ControlTablesLE&) {}
#endif

} // namespace hvyxg::accel
