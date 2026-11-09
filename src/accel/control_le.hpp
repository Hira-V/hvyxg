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

/// Where the kernel's fields are within a DspVoice: byte offsets of runs of consecutive 32-bit words (a
/// segment load or store a run), in this order (DspVoice::fields_le checks the layout).
struct FieldsLE {
    std::uint32_t lfo_a;   // wave, invert, phase, inc, pitch_depth, amp_depth, filter_depth, amod
    std::uint32_t lfo_b;   // sh, out_pitch, out_amp, out_filter, out_bias
    struct Eg {
        std::uint32_t a;   // state, pos, delay, inc[0..2], inc_sustain, inc_release
        std::uint32_t b;   // level[0..3], release_level, delay_target
    } eg[2];               // [0] pitch, [1] filter
    std::uint32_t pitch;   // pitch_base, porta_pos, porta_step, inc, cutoff_cur, cutoff_target, f_prev, f_new
    std::uint32_t amp_a;   // state, x, hold, xstep, gain_prev, gain, slope, intercept
    std::uint32_t amp_b;   // inc[0..3], level[0..3]
    std::uint32_t amp_c;   // inc_release, decay1_rate, decay2_rate
    std::uint32_t ticks;   // stage_ticks, peak_timer
    std::uint32_t out;     // level_x, pan_x[0..1], send_x[0..3]
    std::uint32_t gains;   // gain_prev[0..6], mix_from[0..6], mix_to[0..6]
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
    std::int32_t loop_tune[kN];      // the loop's fine tune, 0 while the sample is in its pre-loop
    // the flags: in, and out
    std::int32_t sh_flag[kN], mixed[kN];
    // out
    std::int32_t finished[kN], peak_reset[kN];
    // between the kernel's phases (scratch)
    std::int32_t out_pitch[kN], out_amp[kN], out_filter[kN], out_bias[kN];
    float gain_prev[kN], gain[kN];
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
