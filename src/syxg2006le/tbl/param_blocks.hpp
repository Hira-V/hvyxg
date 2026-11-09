// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// S-YXG2006LE element parameter blocks A (16 B), B (28 B), C (36 B) and D (84 B).
//
// Field meanings come from the per-note component classes the DLL runs at note-on
//
// Common encodings:
//   * "curve" fields are u16 seg5 row indices (0xFFFF = none). The row is indexed by the scale
//     key (played key + group transpose) or by velocity - 1; the byte goes through
//     `curve_value` and is scaled by (nibble + 1) / 16 (rates, pan) or / 8 (levels, filter).
//   * rates are 0..127 indices into the engine's time tables (larger = faster);
//   * amp levels are attenuations in 1/16-octave units (≈ 0.376 dB), 0 = full, 255 ≈ -96 dB;
//   * pitch / filter EG levels and filter cutoffs are cents.

#include <array>
#include <cstdint>
#include <span>

#include "syxg2006le/tbl/dll_tables.hpp"

namespace hvyxg::syxg2006le::tbl {

/// a seg5 curve byte as a signed value (0x7F maps to +128, not +127).
constexpr int curve_value(std::uint8_t b) { return b == 0x7F ? 128 : static_cast<std::int8_t>(b); }

inline constexpr std::uint16_t kNoCurve = 0xFFFF;

/// Block A (16 bytes): the element LFO.
struct LfoBlock {
    std::uint8_t wave = 0;          ///< [0] & 0x0F: 0 saw, 1 tri, 2 square, 3 S&H, 4 sine; 5-8 = 0-3 with
                                    ///< unipolar amp/filter outputs starting at 0 (bank: 5, 6, 8)
    bool key_sync = false;          ///< [0] bit 6: phase reset at note-on
    std::uint8_t speed = 0;         ///< [1]: speed index → lfo_speed_value() → lfo_hz()
    std::uint8_t phase = 0;         ///< [2]: initial phase index (degrees)
    std::uint8_t amp_depth = 0;     ///< [3]: amplitude modulation depth, 0..128 attenuation units
    std::int16_t pitch_depth = 0;   ///< [4..5]: pitch depth, cents peak-to-peak (± depth/2), 0..400
    std::uint16_t filter_depth = 0; ///< [6..7]: filter modulation depth, cents, 0..4800
    std::uint16_t delay = 0;        ///< [8..9] & 0x7FF: delay, 10 ms units
    std::uint8_t fade_flags = 0;    ///< [9] >> 5: bit 2 pitch, bit 1 amp, bit 0 filter depth faded in
    std::uint16_t fade_in = 0;      ///< [10..11] & 0x7FF: fade-in, 10 ms units
    std::uint16_t unknown12 = 0;    ///< [12..13]: no reader found
    std::uint8_t source_gates = 0;  ///< [14] & 7: gate an extra filter/amp/pitch depth whose source is 0 (no effect)
    std::uint8_t invert = 0;        ///< [15] >> 5: bits 2/1/0 invert pitch / amp / filter output

    static LfoBlock from_bytes(std::span<const std::uint8_t, 16> b);
};

/// Block B (28 bytes): the pitch EG and the element tune
/// (pitch component).
struct PitchEgBlock {
    std::uint8_t rate_key_center = 60; ///< [0] & 0x7F: centre key of rate key scaling
    std::int8_t depth = 64;            ///< [1]: level depth, 64 = 100 %
    std::uint16_t delay = 0;           ///< [2..3] & 0x7FF: delay, 10 ms units (≤ 1000)
    std::array<std::uint8_t, 3> rate{}; ///< [4], [5], [6]: rates 1-3
    std::uint8_t release_rate = 0;     ///< [7] & 0x7F
    std::int16_t initial_level = 0;    ///< [8..9]: cents (clamped ±2400 after part offsets)
    std::array<std::int16_t, 3> level{}; ///< [10], [12], [14]: levels 1-3, cents
    std::int16_t release_level = 0;    ///< [16..17]: cents
    std::uint16_t depth_velocity_curve = kNoCurve; ///< [18..19]: velocity curve for the depth
    std::uint8_t depth_curve_scale = 0; ///< [22] & 0x0F
    std::int8_t rate_key_sens = 0;     ///< [23] & 0x3F as 6-bit signed
    std::int8_t rate_velocity_sens = 0; ///< [24] & 0x3F as 6-bit signed
    std::int8_t coarse = 0;            ///< [26] & 0x7F as 7-bit signed semitones (pitch sum)
    std::int8_t fine = 0;              ///< [27] & 0x7F as 7-bit signed, × 25/32 cents

    bool active() const { return initial_level || level[0] || level[1] || level[2]; }
    static PitchEgBlock from_bytes(std::span<const std::uint8_t, 28> b);
};

/// Block C (36 bytes): the amplitude EG and the level
/// scaling curves.
struct AmpEgBlock {
    std::uint8_t attack_rate = 0;   ///< [1]: ≥ 0x80 = instant (starts at level_1)
    std::uint8_t decay1_rate = 0;   ///< [2]
    std::uint8_t decay2_rate = 0;   ///< [3]
    std::uint8_t decay3_rate = 0;   ///< [4]: 0 = hold level_3 (sustain)
    std::uint8_t release_rate = 0;  ///< [5]: pedal up
    std::uint8_t damper_release_rate = 0; ///< [6]: damper fully down
    std::uint8_t level_0 = 0;       ///< [7]: start level (attenuation)
    std::uint8_t level_1 = 0;       ///< [8]: attack target
    std::uint8_t level_2 = 0;       ///< [9]: decay1 target
    std::uint8_t level_3 = 0;       ///< [10]: decay2 target / sustain
    std::uint16_t level_key_curve = kNoCurve;        ///< [12..13], scale [30] >> 4, / 8
    std::uint16_t rate_key_curve = kNoCurve;         ///< [14..15], scale [30] & 0xF, all rates but attack
    std::uint16_t level_velocity_curve = kNoCurve;   ///< [16..17], scale [31] >> 4, / 8
    std::uint16_t attack_velocity_curve = kNoCurve;  ///< [18..19], scale [31] & 0xF
    std::uint16_t decay1_velocity_curve = kNoCurve;  ///< [20..21], scale [32] >> 4
    std::uint16_t rate_velocity_curve = kNoCurve;    ///< [22..23], scale [32] & 0xF, all rates but attack
    std::uint16_t soft_key_curve = kNoCurve;         ///< [24..25], scale [33] >> 4 (soft pedal ≥ 64)
    std::uint16_t soft_velocity_curve = kNoCurve;    ///< [26..27], scale [31] & 0xF (soft pedal ≥ 64)
    std::uint16_t held_time_curve = kNoCurve;        ///< [28..29], scale [34] >> 4 (index)
    std::array<std::uint8_t, 5> scales{};            ///< [30..34]: the nibble pairs above

    static AmpEgBlock from_bytes(std::span<const std::uint8_t, 36> b);
};

/// One 20-byte filter entry of block D.
struct FilterEntry {
    std::uint8_t type = 0;          ///< [0] & 0x0F: 0/8 LPF; others select the other SVF taps
    std::uint8_t flags = 0;         ///< [0] >> 6: unexplained
    std::uint8_t resonance = 0;     ///< [1]: clamped 16..140 after offsets
    std::uint16_t cutoff = 0;       ///< [2..3]: cents scale, clamped 9600..19200
    std::uint8_t byte4 = 0;         ///< [4]: 0x30 - b → (unexplained)
    std::uint8_t gain_a = 0;        ///< [5] & 7, 3-bit signed s: output gain g((3 - s) << 16)
    std::uint16_t gain_b = 0;       ///< [6..7] & 0x1FF, 9-bit signed v (≤ 48): output gain g((48 - v) << 12)
    std::uint16_t cutoff_key_curve = kNoCurve;      ///< [10..11], scale [18] >> 4
    std::uint16_t cutoff_velocity_curve = kNoCurve; ///< [12..13], scale [18] & 0xF
    std::uint16_t cutoff_soft_curve = kNoCurve;     ///< [14..15], scale [19] >> 4 (soft pedal ≥ 64)
    std::uint16_t resonance_velocity_curve = kNoCurve; ///< [16..17], scale [19] & 0xF
    std::uint8_t scale_a = 0, scale_b = 0;           ///< [18], [19]

    static FilterEntry from_bytes(std::span<const std::uint8_t, 20> b);
};

/// Block D (84 bytes): the filter EG and the filter setup.
struct FilterBlock {
    std::int8_t depth = 64;          ///< [1]: EG level depth, 64 = 100 %
    std::uint16_t delay = 0;         ///< [2..3] & 0x7FF: delay, 10 ms units
    std::array<std::uint8_t, 3> rate{}; ///< [4], [5], [6]
    std::uint8_t release_rate = 0;   ///< [7] & 0x7F: damper off
    std::uint8_t damper_release_rate = 0; ///< [8] & 0x7F: damper on
    std::array<std::int16_t, 4> level{}; ///< [10], [12], [14], [16]: levels 0-3, cents
    std::int16_t release_level = 0;  ///< [18..19]: damper off
    std::int16_t damper_release_level = 0; ///< [20..21]: damper on
    std::uint16_t rate_key_curve = kNoCurve;          ///< [22..23], scale ([32] & 0xF) + 1
    std::uint16_t depth_velocity_curve = kNoCurve;    ///< [24..25], scale ([33] >> 4) + 1
    std::uint16_t delay_velocity_curve = kNoCurve;    ///< [26..27], scale ([33] & 0xF) + 1
    std::uint16_t rate1_velocity_curve = kNoCurve;    ///< [28..29], scale [34] >> 4
    std::uint16_t rate_velocity_curve = kNoCurve;     ///< [30..31], scale ([34] & 0xF) + 1
    std::array<std::uint8_t, 3> scales{};             ///< [32..34]
    std::uint16_t cutoff_key_curve = kNoCurve;        ///< [38..39], scale [43] >> 4
    std::array<FilterEntry, 2> filter{}; ///< [44..63], [64..83]; the DSP only ever uses the first

    bool eg_active() const { return level[0] || level[1] || level[2] || level[3]; }
    static FilterBlock from_bytes(std::span<const std::uint8_t, 84> b);
};

// ---- Engine conversions (DSP side). Verified against real-DLL renders, see the doc. ----

/// Per-64-sample-tick step of the amp-EG decay, filter EG, pitch EG and cutoff glide for rate
/// 0..127: trunc((44100/fs)·2^26 / S[rate]), S = float. The amp
/// EG moves in level units × 4096, the filter / pitch EGs in 1/65536 octave (cents·4096/75).
int eg_increment(const dll::Tables& tb, int rate, double fs);

/// Attenuation (1/16-octave level units, 0..255) to linear gain: the engine's piecewise-linear
/// 2^(-x/65536) with x = units << 12. 24 units → 0.375, 16 → 0.5.
double attenuation_gain(int units);

/// Pan attenuation (pan 0..128). A voice output's attenuation is
/// P[pan] + 8 + P[part pan] (left) and P[128 - part pan] + 8 + P[128 - pan] (right).
std::uint8_t pan_attenuation(const dll::Tables& tb, int pan);

/// LFO speed index (block A [1], clamped 2..93) → speed value v (≈ Hz × 100).
int lfo_speed_value(const dll::Tables& tb, int index);

/// LFO phase step per 64-sample tick on a 2^20 cycle for speed value v (6..1016), including the
/// engine's 6-bit re-quantization.
int lfo_increment(int v, double fs);

/// LFO frequency in Hz for speed value v (index 75 → v 509 → 5.131 Hz).
double lfo_hz(int v, double fs);

/// Log cutoff (1/65536 octave) the filter DSP sees for a cutoff in cents (after all offsets):
/// trunc(cents·4096/75) + trunc(65536·log2(44100/fs)), clamped 0x80000..0x100000.
int filter_log_cutoff(int cents, double fs);

/// State-variable-filter frequency coefficient for a log cutoff: (256 + ((L >> 8) & 255)) /
/// 2^(24 - (L >> 16)), at most 1 (≈ 2^(L/65536 - 16); fc ≈ fs·asin(f/2)/π).
double filter_coefficient(int log_cutoff);

/// State-variable-filter damping for a resonance 16..140: (64 - (r & 31)) / 2^((r >> 5) + 5).
double filter_damping(int resonance);

} // namespace hvyxg::syxg2006le::tbl
