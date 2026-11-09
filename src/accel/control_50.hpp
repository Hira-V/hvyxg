// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// The S-YXG50 chip voice's per-block control across a batch of voices (docs/ACCELERATION.md, "Control
// across voices"), and a target's kernel over it.
//
// The reference is syxg50::ChipVoice::control, voice by voice: the LFO, the pitch LFO's increment, the
// cutoff glide and the filter coefficient, the AEG step, the level glides, the gains and the mix ramps.
// Unlike the 2006LE's, it has no stage a kernel must give back: every lane is the kernel's. The one
// part it may leave: the filter coefficient at a rate other than 44.1 kHz (an asin and a sin), for a
// cutoff that moved -- the lane's need_f, which from_lanes works out with the reference's svf_f.
//
// The voices' fields are read and written where they are, in the ChipVoice objects (base + voice[i]),
// as runs of consecutive 32-bit words (Fields50, from ChipVoice::fields_50).

#include <cstddef>
#include <cstdint>

#include "accel/kernels.hpp"

namespace hvyxg::accel {

/// Where the kernel's fields are within a ChipVoice: byte offsets of runs of consecutive 32-bit words,
/// in this order (ChipVoice::fields_50 checks the layout).
struct Fields50 {
    std::uint32_t lfo;     // lfo_phase, lfo_step, lfo_tri, amd, fmd, pmd, inc, inc15
    std::uint32_t cut;     // cut_cur, cut_tgt, cut_glide_rate, f_v, f_of_v, f_prev, f_new
    std::uint32_t aeg_a;   // state, x, hold, init_level, target, step_a, step_d, reached
    std::uint32_t aeg_b;   // env_prev, env, slope, offset
    std::uint32_t lev_a;   // level_cur, level_tgt, pan_l_cur, pan_l_tgt, pan_r_cur, pan_r_tgt, dry, rev
    std::uint32_t lev_b;   // cho, var
    std::uint32_t gains;   // g_prev[0..6], g_new[0..6], mix_from[0..6], mix_to[0..6]
};

struct Lanes50 {
    static constexpr std::size_t kN = kBatch;
    std::size_t n = 0;
    std::uint8_t* base = nullptr;    // the voices: lane i's at base + voice[i]
    std::uint32_t voice[kN];
    // out
    std::int32_t done[kN];           // the AEG fell below -48 dB: the voice ends
    std::int32_t need_f[kN];         // the filter coefficient is from_lanes' (svf_f of v)
    std::int32_t v[kN];              // the cutoff svf_f is of
    // between the kernel's phases (scratch)
    float env[kN], env_prev[kN];
    std::int32_t am_level[kN];       // the amplitude LFO's level term
};

/// The tables and constants the kernel reads.
struct ControlTables50 {
    const std::int32_t* decay = nullptr;   // RateTables::decay (128)
    const std::uint32_t* lfo_saw = nullptr, *lfo_tri = nullptr, *lfo_saw_uni = nullptr, *lfo_tri_uni = nullptr;   // 4 each
    std::int32_t level_step = 0;           // decay[the level glide rate]
    float lin_m48 = 0.0f;                  // lin(0x80000): the -48 dB end
    bool rate_44k = true;                  // svf_f is the formula alone (no asin / sin)
};

// Opt-in (HVYXG_CONTROL_50): on the X280 it costs about what the scalar control it replaces did (the
// S-YXG50's control is light; its time is in the resampler and the filter -- docs/ACCELERATION.md), so
// the default build runs the reference. Exact either way.
#if defined(HVYXG_ACCEL_RVV) && defined(HVYXG_CONTROL_50)
namespace rvv {
void control_50(Lanes50& L, const Fields50& f, const ControlTables50& t);
} // namespace rvv
/// A target has a control kernel for the S-YXG50.
inline constexpr bool kControl50 = true;
inline void control_50(Lanes50& L, const Fields50& f, const ControlTables50& t) { rvv::control_50(L, f, t); }
#else
inline constexpr bool kControl50 = false;
inline void control_50(Lanes50&, const Fields50&, const ControlTables50&) {}
#endif

} // namespace hvyxg::accel
