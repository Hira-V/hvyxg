// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "eq.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

#include "midi/part.hpp"

namespace hvyxg::eq {

namespace {

constexpr double kPi = std::numbers::pi;

double nyquist_clamp(double hz, double sample_rate) { return std::clamp(hz, 1.0, 0.45 * sample_rate); }

/// First-order shelf boost (g ≥ 1) or its inverse: numerator / denominator pairs of
/// `n0 + n1 z⁻¹`, `d0 + d1 z⁻¹`.
Coeffs shelf1(double hz, double gain_db, double sample_rate, bool high) {
    const double k = std::tan(kPi * nyquist_clamp(hz, sample_rate) / sample_rate);
    const double g = std::pow(10.0, std::abs(gain_db) / 20.0);
    double n0, n1;
    const double d0 = 1 + k, d1 = k - 1;  // pole at hz
    if (high) {
        n0 = k + g;
        n1 = k - g;
    } else {
        n0 = 1 + k * g;
        n1 = k * g - 1;
    }
    Coeffs c;
    if (gain_db >= 0) {
        c.b0 = static_cast<float>(n0 / d0);
        c.b1 = static_cast<float>(n1 / d0);
        c.a1 = static_cast<float>(d1 / d0);
    } else {
        c.b0 = static_cast<float>(d0 / n0);
        c.b1 = static_cast<float>(d1 / n0);
        c.a1 = static_cast<float>(n1 / n0);
    }
    return c;
}

int db_of(std::uint8_t gain) { return static_cast<int>(gain) - 64; }

/// An XG EQ gain byte: 0x34..0x4C; anything else (2006LE kit entries without XG2.0 data hold
/// 0) is 0 dB.
std::uint8_t gain_or_flat(std::uint8_t v) { return v >= 0x34 && v <= 0x4C ? v : 64; }

} // namespace

double shelf_db(std::uint8_t gain, bool mu_strength) {
    const int db = db_of(gain);
    if (!mu_strength) return db;
    static constexpr std::array<double, 7> kMu = {-2.095, -1.334, -0.573, 0.0, 0.762, 1.526, 2.286};
    const int n = std::clamp(db >= 0 ? db / 4 : -((-db + 3) / 4), -3, 3);  // ⌊db / 4⌋
    return kMu[static_cast<std::size_t>(n + 3)];
}

double frequency_hz(int index) {
    return kFrequencyHz[static_cast<std::size_t>(std::clamp(index, 0, 60))];
}

double Coeffs::magnitude_db(double hz, double sample_rate) const {
    const std::complex<double> z1 = std::polar(1.0, -2 * kPi * hz / sample_rate);
    const std::complex<double> z2 = z1 * z1;
    const auto h = (static_cast<double>(b0) + static_cast<double>(b1) * z1 + static_cast<double>(b2) * z2) /
                   (1.0 + static_cast<double>(a1) * z1 + static_cast<double>(a2) * z2);
    return 20 * std::log10(std::abs(h));
}

Coeffs low_shelf1(double hz, double gain_db, double sample_rate) {
    return shelf1(hz, gain_db, sample_rate, false);
}

Coeffs high_shelf1(double hz, double gain_db, double sample_rate) {
    return shelf1(hz, gain_db, sample_rate, true);
}

Coeffs cascade1(const Coeffs& x, const Coeffs& y) {
    Coeffs c;
    c.b0 = x.b0 * y.b0;
    c.b1 = x.b0 * y.b1 + x.b1 * y.b0;
    c.b2 = x.b1 * y.b1;
    c.a1 = x.a1 + y.a1;
    c.a2 = x.a1 * y.a1;
    return c;
}

Coeffs peak(double hz, double q, double gain_db, double sample_rate) {
    const double a = std::pow(10.0, gain_db / 40.0);
    const double w = 2 * kPi * nyquist_clamp(hz, sample_rate) / sample_rate;
    const double alpha = std::sin(w) / (2 * std::max(q, 0.01) / std::max(a, 1 / a));
    const double cw = std::cos(w);
    const double a0 = 1 + alpha / a;
    Coeffs c;
    c.b0 = static_cast<float>((1 + alpha * a) / a0);
    c.b1 = static_cast<float>(-2 * cw / a0);
    c.b2 = static_cast<float>((1 - alpha * a) / a0);
    c.a1 = static_cast<float>(-2 * cw / a0);
    c.a2 = static_cast<float>((1 - alpha / a) / a0);
    return c;
}

Coeffs high_pass2(double hz, double q, double sample_rate) {
    const double w = 2 * kPi * nyquist_clamp(hz, sample_rate) / sample_rate;
    const double alpha = std::sin(w) / (2 * q);
    const double cw = std::cos(w);
    const double a0 = 1 + alpha;
    Coeffs c;
    c.b0 = static_cast<float>((1 + cw) / 2 / a0);
    c.b1 = static_cast<float>(-(1 + cw) / a0);
    c.b2 = c.b0;
    c.a1 = static_cast<float>(-2 * cw / a0);
    c.a2 = static_cast<float>((1 - alpha) / a0);
    return c;
}

double hpf_hz(int value) { return 29.2 * std::exp2((value - 64) * (24.0 / 256.0)); }

// ── Voice EQ ────────────────────────────────────────────────────────────────────────────

VoiceEqSettings voice_settings(const midi::Part& part, const midi::xg::DrumSetup* drum, bool is_drum) {
    VoiceEqSettings s;
    const int part_hpf = part.ram_ext ? part.ram_ext->hpf_cutoff : 64;
    if (!is_drum) {
        const midi::xg::MultiPart& r = *part.ram;
        s.bass_gain = gain_or_flat(r.eq_bass_gain);
        s.treble_gain = gain_or_flat(r.eq_treble_gain);
        s.bass_freq = r.eq_bass_freq;
        s.treble_freq = r.eq_treble_freq;
        s.hpf = static_cast<std::uint8_t>(part_hpf);
        return s;
    }
    // MU2000 firmware (measured in S-MU2000): a drum note takes its own EQ and
    // not the part's; the HPF is part + note − 64.
    int note_hpf = 64;
    if (drum) {
        s.bass_gain = gain_or_flat(drum->eq_bass_gain);
        s.treble_gain = gain_or_flat(drum->eq_treble_gain);
        if (drum->eq_bass_freq >= 4 && drum->eq_bass_freq <= 40) s.bass_freq = drum->eq_bass_freq;
        if (drum->eq_treble_freq >= 28 && drum->eq_treble_freq <= 58) s.treble_freq = drum->eq_treble_freq;
        // A kit entry without XG2.0 data (bass frequency and HPF both 0; no 2006LE entry has
        // bass frequency 0 with an HPF) has no note HPF; a written HPF (SysEx 3n rr 50, NRPN
        // 24 rr) applies.
        if (drum->eq_bass_freq != 0 || drum->hpf_cutoff_freq != 0) note_hpf = drum->hpf_cutoff_freq;
    }
    s.hpf = static_cast<std::uint8_t>(std::clamp(part_hpf + note_hpf - 64, 0, 127));
    return s;
}

std::uint32_t change_fade_samples(float sample_rate) {
    return std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(kChangeFadeSeconds * sample_rate)));
}

void VoiceEq::start(const VoiceEqSettings& s, float sample_rate) {
    *this = VoiceEq{};
    update(s, sample_rate);
    fade_ = 0;
}

void VoiceEq::update(const VoiceEqSettings& s, float sample_rate) {
    if (s == s_) return;
    const Path was = cur_;
    s_ = s;
    const double bass_db = shelf_db(s.bass_gain, s.mu_strength);
    const double treble_db = shelf_db(s.treble_gain, s.mu_strength);
    cur_.shelves_on = bass_db != 0.0 || treble_db != 0.0;
    cur_.hpf_on = !s.hpf_off();
    if (cur_.shelves_on) {
        const Coeffs lo = low_shelf1(frequency_hz(s.bass_freq), bass_db, sample_rate);
        const Coeffs hi = high_shelf1(frequency_hz(s.treble_freq), treble_db, sample_rate);
        cur_.shelves = cascade1(lo, hi);
        if (!was.shelves_on) cur_.shelves_state = State{};
    }
    if (cur_.hpf_on) {
        cur_.hpf = high_pass2(hpf_hz(s.hpf), kHpfQ, sample_rate);
        if (!was.hpf_on) cur_.hpf_state = State{};
    }
    // Smoothing (superset): the old filter keeps running on the same input and fades out
    // while the new one fades in. A change during a fade restarts it from the filter heard
    // last (the old path's remaining weight is small by then).
    if (was.on() || cur_.on()) {
        old_ = fade_ > 0 && !was.on() ? old_ : was;
        fade_len_ = fade_ = change_fade_samples(sample_rate);
    }
}

void VoiceEq::process_fading(float* x, std::size_t n) {
    const float step = 1.0f / static_cast<float>(fade_len_);
    for (std::size_t i = 0; i < n; ++i) {
        const float in = x[i];
        const float y_new = cur_.tick(in);
        if (fade_ == 0) {
            x[i] = y_new;
            continue;
        }
        const float y_old = old_.tick(in);
        const float w = static_cast<float>(fade_) * step;  // weight of the old path
        x[i] = y_new + (y_old - y_new) * w;
        --fade_;
    }
}

// ── Multi EQ ────────────────────────────────────────────────────────────────────────────

Coeffs MultiEqStage::design(const midi::xg::MultiEq& ram, std::size_t k, float sample_rate) {
    const midi::xg::MultiEq::Band b = ram.band(k);
    const int db = db_of(b.gain);
    if (db == 0) return Coeffs{};
    const double hz = frequency_hz(b.freq);
    if (b.peak) return peak(hz, b.q / 10.0, db, sample_rate);
    if (k == 0) return low_shelf1(hz, db, sample_rate);
    const Coeffs half = high_shelf1(hz, db / 2.0, sample_rate);
    return cascade1(half, half);
}

void MultiEqStage::update(const midi::xg::MultiEq& ram) {
    if (!fresh_ && ram == ram_) return;
    const bool first = fresh_;
    const Chain was = cur_;
    std::array<bool, midi::xg::MultiEq::kBands> on{};
    for (std::size_t i = 0; i < was.count; ++i) on[was.bands[i]] = true;
    fresh_ = false;
    ram_ = ram;
    cur_.count = 0;
    for (std::size_t k = 0; k < midi::xg::MultiEq::kBands; ++k) {
        if (db_of(ram.band(k).gain) == 0) continue;
        cur_.coeffs[k] = design(ram, k, sample_rate_);
        if (!on[k]) cur_.state[k] = {};
        cur_.bands[cur_.count++] = static_cast<std::uint8_t>(k);
    }
    // Smoothing (superset), as `VoiceEq::update`: not for the first setting.
    if (!first && (was.count > 0 || cur_.count > 0)) {
        old_ = fade_ > 0 && was.count == 0 ? old_ : was;
        fade_len_ = fade_ = change_fade_samples(sample_rate_);
    }
}

void MultiEqStage::process(float* left, float* right, std::size_t n) {
    if (fade_ > 0) {
        process_fading(left, right, n);
        return;
    }
    for (std::size_t i = 0; i < cur_.count; ++i) {
        const std::size_t k = cur_.bands[i];
        const Coeffs& c = cur_.coeffs[k];
        State& sl = cur_.state[k][0];
        State& sr = cur_.state[k][1];
        for (std::size_t j = 0; j < n; ++j) {
            left[j] = sl.tick(c, left[j]);
            right[j] = sr.tick(c, right[j]);
        }
    }
}

void MultiEqStage::process_fading(float* left, float* right, std::size_t n) {
    const auto run = [](Chain& ch, float& l, float& r) {
        for (std::size_t i = 0; i < ch.count; ++i) {
            const std::size_t k = ch.bands[i];
            l = ch.state[k][0].tick(ch.coeffs[k], l);
            r = ch.state[k][1].tick(ch.coeffs[k], r);
        }
    };
    const float step = 1.0f / static_cast<float>(fade_len_);
    for (std::size_t j = 0; j < n; ++j) {
        float nl = left[j], nr = right[j];
        run(cur_, nl, nr);
        if (fade_ == 0) {
            left[j] = nl;
            right[j] = nr;
            continue;
        }
        float ol = left[j], orr = right[j];
        run(old_, ol, orr);
        const float w = static_cast<float>(fade_) * step;
        left[j] = nl + (ol - nl) * w;
        right[j] = nr + (orr - nr) * w;
        --fade_;
    }
}

} // namespace hvyxg::eq
