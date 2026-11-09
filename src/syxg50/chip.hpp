// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// The S-YXG50 "chip": an AWM2-style voice renderer driven by register values, as the DLL's
// chip layer does it (per voice and 128-sample block: → LFO, pitch
// cutoff, AEG, levels, fetch
// filter + mix).
//
// Units: attenuation 0x10000 per octave (6.02 dB); envelope / level registers 0x1000 per
// unit (1/16 octave); cutoff 256 per octave (12 bit, chip C = reg + 0x800).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "accel/control_50.hpp"
#include "steal_fade.hpp"
#include "hvyxg/types.hpp"
#include "buses.hpp"
#include "eq.hpp"
#include "syxg50/bank_data.hpp"
#include "syxg50/dll_tables.hpp"

namespace hvyxg::syxg50 {

inline constexpr std::size_t kBlock = 128;
/// Attenuation at or above which a gain is 0.
inline constexpr std::int32_t kAttenOff = 0xfffff;

/// `lin(x)`: gain of attenuation `x`, linear within each octave.
inline float lin(std::int32_t x) {
    if (x < 0) return 1.0f;
    if (x >= kAttenOff) return 0.0f;
    const auto mant = static_cast<float>((~x & 0xffff) | 0x10000);
    return mant / (static_cast<float>(1 << (x >> 16)) * 131071.0f);
}

/// The inverse of `lin`.
std::int32_t inv_lin(float gain);

/// Rate tables for one output rate:
/// `2^26/TA[r]`, `2^27/TB[r]`, `2^27/TC[r]`, each × 44100/fs, truncated.
struct RateTables {
    std::array<std::int32_t, 128> attack{};
    std::array<std::int32_t, 128> decay{};
    std::array<std::int32_t, 128> release{};
    /// Cutoff C units → SVF coefficient scale for the output rate (see `svf_f`).
    float rate_ratio = 1.0f;
    float sample_rate = 44100.0f;

    RateTables(const dll::Tables& tb, float sample_rate);
};

/// One chip voice. The control layer writes the `set_*` registers; `render` runs one block.
class ChipVoice {
public:
    /// One block's filter, for a caller that runs the filter across voices (accel::svf50_lanes): the
    /// voice's state, and f's start and step and q.
    struct SvfStep {
        float* band = nullptr;
        float* low = nullptr;
        float f = 0.0f, df = 0.0f, q = 1.0f;
    };
    /// A block in steps, as render() does them (docs/ACCELERATION.md, "Control across voices"):
    /// begin (false: not active, nothing to do); control -- LFO, pitch, cutoff, AEG, levels, the gains
    /// and the mix ramps (no shared state: a caller may run a batch's together, accel::control_50);
    /// signal -- the samples into x and the filter's step; the filter over x, by the caller (svf); then
    /// EQ, fade and the mix.
    bool begin() const { return active_; }
    /// control() across a batch (accel/control_50.hpp): lane i is this voice; what the kernel leaves
    /// to the voice, after it.
    void to_lanes(accel::Lanes50& L, std::size_t i) const {
        L.voice[i] = static_cast<std::uint32_t>(reinterpret_cast<const std::uint8_t*>(this) - L.base);
    }
    void from_lanes(const accel::Lanes50& L, std::size_t i);
    static const accel::Fields50& fields_50();
    static accel::ControlTables50 control_tables(const RateTables& rt);
#if defined(HVYXG_CHECK_CONTROL)
    /// Debugging a control kernel: the fields control() writes that differ from `ref`'s, printed.
    bool differs_from(const ChipVoice& ref, const char* where) const;
#endif
    void control(const RateTables& rt);
    void signal(const RateTables& rt, Interpolation interp, float* x, SvfStep& svf);
    void render_end(Buses& buses, float* x);
    // ── registers ────────────────────────────────────────────────────────────────────
    /// Key-on: wave + all registers below must be set first.
    void start(const RateTables& rt);
    void set_wave(std::span<const std::uint8_t> wave, const WaveRef& w, std::uint32_t skip);
    /// Reg 0x680 from the control layer's pitch sum in cents:
    /// octave + 12-bit mantissa `round(2048·2^(-i/1200))`, scaled by 44100/fs. `cap3` = zone
    /// format bits 6-7 both set (octave capped at 3 instead of 7).
    void set_pitch(int cents, bool cap3 = false);
    /// Reg 0x400: cutoff (0 or 0x001..0x7ff, chip adds 0x800) and resonance 0..31.
    void set_filter(int cutoff11, int resonance5);
    /// Reg 0x440: cutoff glide rate (index into the decay table, 0x7f = fastest).
    void set_cutoff_glide(int rate) { cut_glide_rate_ = rate & 0x7f; }
    /// Reg 0x441 / 0x480 / 0x481: AMD (0..31), FMD (0..7), LFO speed (0..63), triangle wave,
    /// PMD (0..255).
    void set_lfo(int amd, int fmd, int speed, bool triangle, int pmd);
    /// Regs 0x4c0/0x4c1: attack (bit 7 = attack from `init_level`, else hold) and initial
    /// level (attenuation units of 0x1000).
    void set_attack(int reg4c0, int reg4c1) { reg4c0_ = reg4c0; reg4c1_ = reg4c1; }
    /// Regs 0x500/0x501: decay (bit 7 = release table) rate and target level. Takes effect at
    /// once.
    void set_decay(int reg500, int reg501);
    /// Regs 0x5c0 / 0x5c1 / 0x541 / 0x540 / 0x580 / 0x581: level, pan (L/R nibbles), dry,
    /// reverb, chorus, variation attenuation (0xff = off). Levels and pan glide.
    void set_levels(int level, int pan_l, int pan_r, int dry, int reverb, int chorus,
                    int variation);

    // ── state ────────────────────────────────────────────────────────────────────────
    bool active() const { return active_; }
    void stop() { active_ = false; }
    /// Superset: fade the sound out linearly over `samples` (a stolen voice), then end.
    void start_steal_fade(std::uint32_t samples) { steal_fade_.start(samples); }
    /// The AEG reached its decay target (chip status bit 0x40 clear) in a decay state.
    bool decay_reached() const { return (state_ == 3 || state_ == 4) && reached_; }
    /// Current envelope amplitude (end of last block).
    float envelope() const { return env_; }
    int aeg_state() const { return state_; }
    /// Current envelope attenuation (0x10000 per octave).
    std::int32_t attenuation() const { return x_; }
    /// The AEG status byte the control layer's AEG monitor reads: 0xff for a voice
    /// that is not sounding, else the attenuation in 1.5-dB steps (bits 0-5), bit 6 = the
    /// decay / release target was reached in the last block, bit 7 = decay or release state.
    int status() const;

    /// Render one block into the buses (adds).
    void render(Buses& buses, const RateTables& rt, Interpolation interp);
    /// Render the dry pair into insertion chain `chain` of the buses (−1: the dry bus).
    void set_chain(int chain) { chain_ = chain; }

    /// The DLL's physical chip voice (0..255) this voice was keyed on, -1 none.
    int phys = -1;

    /// Superset: the part / drum-note EQ and HPF after the filter, before the outputs
    /// (eq.hpp; set by the tone module, nothing while flat).
    eq::VoiceEq eq;

private:
    void attack_setup(const RateTables& rt);
    void decay_setup(const RateTables& rt);
    void aeg_step();
    void fetch(const RateTables& rt, Interpolation interp, float* out);
    void update_gains(std::int32_t am);
    float svf_f(std::int32_t v) const;
    /// svf_f, remembering the last.
    float svf_f_memo(std::int32_t v) {
        if (v != f_v_) {
            f_v_ = v;
            f_of_v_ = svf_f(v);
        }
        return f_of_v_;
    }

    bool active_ = false;
    StealFade steal_fade_;
    /// Linear release reached: the chip takes no more register writes (flag bit 1 cleared).
    bool locked_ = false;
    /// The envelope fell below -48 dB.
    bool done_ = false;
    const RateTables* rt_ = nullptr;

    // wave
    const std::uint8_t* wave_ = nullptr;
    std::size_t wave_size_ = 0;
    std::size_t loop_start_ = 0;
    bool pcm8_ = false;
    std::int32_t pre_ = 0;
    std::int32_t loop_len_ = 0;
    std::uint32_t loop_frac_ = 0; // 32-bit fraction
    std::int32_t pos_ = 0;
    std::uint32_t frac_ = 0; // 32-bit fraction
    bool ended_ = false;
    bool ended_before_ = false;    // ended_ as the block began (control), for its samples (signal)
    int pending_cents_ = 0;
    bool pending_cap3_ = false;
    int lfo_speed_ = 0;
    int reg_cut_ = 0, reg_res_ = 0;
    float q_ = 1.0f;
    float band_ = 0.0f, low_ = 0.0f;
    int reg4c0_ = 0, reg4c1_ = 0, reg500_ = 0, reg501_ = 0;

    // The per-block control's fields, in runs of consecutive 32-bit words in the order the control
    // kernel reads them (accel/control_50.hpp; fields_50 checks the layout).
    // LFO and pitch: the phase, its step, the wave (1 triangle), the depths, the 17.15 increment before
    // the pitch LFO, and the block's (with it: control's, for signal).
    std::uint32_t lfo_phase_ = 0;
    std::int32_t lfo_step_ = 0;
    int lfo_tri_ = 1;
    int amd_ = 0, fmd_ = 0, pmd_ = 0;
    std::int32_t inc_ = 0x8000;
    std::int32_t inc15_ = 0x8000;
    // filter: the glided cutoff, its target and glide rate; svf_f's last argument and result (a cutoff
    // that holds still costs no asin / sin a block at rates other than 44.1 kHz); the coefficient at the
    // block's start and end.
    std::int32_t cut_cur_ = 0, cut_tgt_ = 0;
    int cut_glide_rate_ = 0x7f;
    std::int32_t f_v_ = INT32_MIN;
    float f_of_v_ = 0.0f;
    float f_prev_ = 0.0f, f_new_ = 0.0f;
    // AEG
    int state_ = 3;
    std::int32_t x_ = kAttenOff;   // c8
    std::int32_t hold_ = 0;        // dc
    std::int32_t init_level_ = 0;  // cc
    std::int32_t target_ = 0;      // d0
    std::int32_t step_a_ = 0;      // d4
    std::int32_t step_d_ = 0;      // d8
    int reached_ = 0;
    float env_prev_ = 0.0f, env_ = 0.0f; // e0, e4
    float slope_ = 0.0f, offset_ = 0.0f; // e8, ec

    // levels (attenuation)
    std::int32_t level_cur_ = 0, level_tgt_ = 0;
    std::int32_t pan_l_cur_ = 0, pan_l_tgt_ = 0;
    std::int32_t pan_r_cur_ = 0, pan_r_tgt_ = 0;
    std::int32_t dry_ = kAttenOff, rev_ = kAttenOff, cho_ = kAttenOff, var_ = kAttenOff;
    int chain_ = -1;
    /// Gains (without the envelope) at the start and end of the block:
    /// dry L, dry R, reverb, chorus L, chorus R, variation L, variation R; and the mix ramps' ends
    /// (gain × envelope), worked out by control.
    std::array<float, 7> g_prev_{};
    std::array<float, 7> g_new_{};
    std::array<float, 7> mix_from_{}, mix_to_{};
};

} // namespace hvyxg::syxg50
