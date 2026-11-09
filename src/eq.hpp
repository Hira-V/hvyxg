// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// XG equalisers that neither Yamaha DLL renders (measured: docs/ENGINE.md, "Superset: EQ"),
// implemented as superset behaviour after the XG specification and the MU2000:
//
// - `VoiceEq`: the part EQ (08 nn 72/73/76/77) and part HPF (0A nn 20) on each voice before
//   its pan and sends; drum voices take the drum-setup note EQ (3n rr 20/21/24/25) instead of
//   the part EQ, and the note HPF (3n rr 50) adds to the part HPF (MU2000 firmware).
// - `MultiEqStage`: the five-band multi EQ (02 40) on the final stereo output.
//
// Both cost nothing while flat: no samples are touched, so the default output is bit-identical
// to an engine without them. Coefficients are designed only when a parameter changes; there is
// no allocation. A change while a filter runs (or starts / stops running) crossfades from the
// old filter to the new one over `kChangeFadeSeconds`, both running on the same input, so EQ
// moves during playback do not click or zipper; a voice's first settings apply at once.

#include <array>
#include <cstddef>
#include <cstdint>

#include "midi/xg_ram.hpp"

namespace hvyxg::midi {
struct Part;
}

namespace hvyxg::eq {

/// The XG EQ frequency table (XG specification, "EQ frequency"): value 0..60 → Hz.
inline constexpr std::array<std::uint16_t, 61> kFrequencyHz = {
    20,    22,    25,    28,    32,    36,    40,    45,    50,    56,    63,    70,    80,
    90,    100,   110,   125,   140,   160,   180,   200,   225,   250,   280,   315,   355,
    400,   450,   500,   560,   630,   700,   800,   900,   1000,  1100,  1200,  1400,  1600,
    1800,  2000,  2200,  2500,  2800,  3200,  3600,  4000,  4500,  5000,  5600,  6300,  7000,
    8000,  9000,  10000, 11000, 12000, 14000, 16000, 18000, 20000};

double frequency_hz(int index);

/// A biquad `H(z) = (b0 + b1 z⁻¹ + b2 z⁻²) / (1 + a1 z⁻¹ + a2 z⁻²)`.
struct Coeffs {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    /// |H| in dB at `hz` (for tests and docs).
    double magnitude_db(double hz, double sample_rate) const;
};

/// Transposed direct form II state.
struct State {
    float z1 = 0, z2 = 0;
    float tick(const Coeffs& c, float x) {
        const float y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return y;
    }
};

/// First-order shelves: a boost (`gain_db` > 0) has its pole at `hz` (bilinear,
/// K = tan(π f / fs)); a cut is the exact inverse of the boost by the same amount. The shape
/// of the MU2000 part EQ and multi EQ band 1 [meas], and of the 2006LE effect EQ designer.
Coeffs low_shelf1(double hz, double gain_db, double sample_rate);
Coeffs high_shelf1(double hz, double gain_db, double sample_rate);
/// The product of two first-order sections (b2 = a2 = 0) as one biquad.
Coeffs cascade1(const Coeffs& x, const Coeffs& y);
/// Peaking band, RBJ cookbook, with the XG Q (value / 10) as the bandwidth of the boost (or of
/// the cut): RBJ Q = q / max(A, 1/A), A = 10^(dB/40). Fits the MU2000 multi EQ to 0.03 dB.
Coeffs peak(double hz, double q, double gain_db, double sample_rate);
/// High-pass, RBJ cookbook, 12 dB/oct.
Coeffs high_pass2(double hz, double q, double sample_rate);

// ── Part / drum-note EQ and HPF ─────────────────────────────────────────────────────────

/// The EQ / HPF parameters of one voice, XG values.
struct VoiceEqSettings {
    std::uint8_t bass_gain = 64, treble_gain = 64;
    std::uint8_t bass_freq = 0x0C, treble_freq = 0x36;
    std::uint8_t hpf = 64;
    /// The MU2000's gain strength (`mu_shelf_db`) instead of the XG nominal ±12 dB.
    bool mu_strength = false;
    bool operator==(const VoiceEqSettings&) const = default;
    bool shelves_flat() const { return bass_gain == 64 && treble_gain == 64; }
    bool hpf_off() const { return hpf <= 64; }
};

/// A melodic voice of `part`: the part EQ and HPF. A drum voice (`drum` = its drum-setup note,
/// or nullptr for the kit defaults): the note EQ, and the HPF part + note − 64 (MU2000).
VoiceEqSettings voice_settings(const midi::Part& part, const midi::xg::DrumSetup* drum, bool is_drum);
/// `voice_settings`, or flat when `off` (DLL-exact mode: neither DLL renders them);
/// `mu_strength`: with the MU2000's gain strength (`Synth::Options::mu_part_eq`).
inline VoiceEqSettings voice_settings(bool off, bool mu_strength, const midi::Part& part,
                                      const midi::xg::DrumSetup* drum, bool is_drum) {
    if (off) return VoiceEqSettings{};
    VoiceEqSettings s = voice_settings(part, drum, is_drum);
    s.mu_strength = mu_strength;
    return s;
}

/// The shelf gain in dB of a part / drum-note EQ gain byte (0x34..0x4C): XG nominal
/// (value − 64), or with `mu_strength` the MU2000's: it acts in steps of four values,
/// n = ⌊(value − 64) / 4⌋, n = −3 … 3 → −2.095, −1.334, −0.573, 0, +0.762, +1.526, +2.286 dB
/// [meas: S-MU2000, every value of both bands of the part EQ, ±12 / ±6 at band frequencies
/// 80 Hz … 16 kHz (within ±0.04 dB of these), and the drum-note EQ; the shelf shape is
/// unchanged].
double shelf_db(std::uint8_t gain, bool mu_strength);

/// HPF cutoff in Hz for an XG HPF value > 64: 29.2 Hz · 2^((v − 64) · 24/256), the MU2000's
/// 24 filter-register units per step at 256 units per octave [meas: 0x50 / 0x60 / 0x7F →
/// 83 / 225 / 1811 Hz on S-MU2000].
double hpf_hz(int value);
/// Resonance of the part HPF (MU2000 fit: Q 1.02 … 1.2).
inline constexpr double kHpfQ = 1.0;

/// Superset smoothing of EQ changes during playback: the crossfade from the old to the new
/// filter (part / drum-note EQ and HPF, multi EQ).
inline constexpr double kChangeFadeSeconds = 0.010;
/// The crossfade length in samples at `sample_rate` (at least 1).
std::uint32_t change_fade_samples(float sample_rate);

class VoiceEq {
public:
    /// A new voice: clear the filter state and take `s` (no crossfade).
    void start(const VoiceEqSettings& s, float sample_rate);
    /// Take `s` (redesigns only on a change). A change while a filter runs, or that starts or
    /// stops one, crossfades from the old filter over `kChangeFadeSeconds`.
    void update(const VoiceEqSettings& s, float sample_rate);
    bool active() const { return cur_.on() || fade_ > 0; }
    /// Filter `n` mono samples in place; nothing while flat.
    void process(float* x, std::size_t n) {
        if (fade_ > 0) {
            process_fading(x, n);
            return;
        }
        if (cur_.shelves_on)
            for (std::size_t i = 0; i < n; ++i) x[i] = cur_.shelves_state.tick(cur_.shelves, x[i]);
        if (cur_.hpf_on)
            for (std::size_t i = 0; i < n; ++i) x[i] = cur_.hpf_state.tick(cur_.hpf, x[i]);
    }

private:
    struct Path {
        bool shelves_on = false, hpf_on = false;
        Coeffs shelves{}, hpf{};
        State shelves_state{}, hpf_state{};
        bool on() const { return shelves_on || hpf_on; }
        float tick(float x) {
            if (shelves_on) x = shelves_state.tick(shelves, x);
            if (hpf_on) x = hpf_state.tick(hpf, x);
            return x;
        }
    };
    void process_fading(float* x, std::size_t n);

    VoiceEqSettings s_{};
    Path cur_{}, old_{};
    std::uint32_t fade_ = 0, fade_len_ = 0;
};

// ── Multi EQ ────────────────────────────────────────────────────────────────────────────

/// The XG multi EQ on a stereo output. Band 1 shelf: first-order low shelf at the band
/// frequency; band 5 shelf: two first-order high shelves of half the gain each (one biquad);
/// peaks: `peak`. All fitted to the MU2000 (docs/ENGINE.md). Flat bands are skipped.
class MultiEqStage {
public:
    explicit MultiEqStage(float sample_rate = 44100.0f) : sample_rate_(sample_rate) {}
    /// Take the RAM (redesigns only on a change).
    void update(const midi::xg::MultiEq& ram);
    bool active() const { return cur_.count > 0 || fade_ > 0; }
    /// A change after the first update crossfades from the old setting over
    /// `kChangeFadeSeconds`.
    void process(float* left, float* right, std::size_t n);

    /// The coefficients of band `k` of `ram` (identity when flat).
    static Coeffs design(const midi::xg::MultiEq& ram, std::size_t k, float sample_rate);

private:
    /// The active bands of one EQ setting and their state.
    struct Chain {
        std::array<Coeffs, midi::xg::MultiEq::kBands> coeffs{};
        std::array<std::array<State, 2>, midi::xg::MultiEq::kBands> state{};
        std::array<std::uint8_t, midi::xg::MultiEq::kBands> bands{};
        std::size_t count = 0;
    };
    void process_fading(float* left, float* right, std::size_t n);

    float sample_rate_;
    midi::xg::MultiEq ram_{};
    bool fresh_ = true;
    Chain cur_{}, old_{};
    std::uint32_t fade_ = 0, fade_len_ = 0;
};

} // namespace hvyxg::eq
