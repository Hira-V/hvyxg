// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Reverb and chorus DSP units: impulse responses, decay times, LFO rate and depth, and the
// sample-rate scaling.
#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "fx/fx_dsp.hpp"
#include "fx/fx_control.hpp"
#include "test_support.hpp"

using namespace hvyxg::fx;

namespace {

struct Ir {
    std::vector<float> l, r;
};

Ir reverb_ir(std::uint8_t algorithm, float rate, float seconds) {
    EffectControl ctrl(test::fx_tables());
    ctrl.set_reverb_algorithm(algorithm);
    ctrl.set_reverb_return(64, 64);
    ReverbUnit unit(rate);
    unit.load(ctrl);
    const auto n = static_cast<std::size_t>(seconds * rate);
    Ir ir{std::vector<float>(n), std::vector<float>(n)};
    ir.l[0] = 1.0f;
    for (std::size_t i = 0; i < n; i += 64) {
        const std::size_t k = std::min<std::size_t>(64, n - i);
        unit.process(ir.l.data() + i, ir.r.data() + i, k);
    }
    return ir;
}

std::size_t first_arrival(const std::vector<float>& x) {
    for (std::size_t i = 0; i < x.size(); ++i)
        if (std::fabs(x[i]) > 1e-9f) return i;
    return x.size();
}

// T30 from the Schroeder integral of L² + R² (−5 … −35 dB).
double t30(const Ir& ir, float rate) {
    std::vector<double> e(ir.l.size() + 1, 0.0);
    for (std::size_t i = ir.l.size(); i-- > 0;)
        e[i] = e[i + 1] + double(ir.l[i]) * ir.l[i] + double(ir.r[i]) * ir.r[i];
    const auto at = [&](double db) {
        for (std::size_t i = 0; i < ir.l.size(); ++i)
            if (10 * std::log10(e[i] / e[0]) <= db) return double(i) / rate;
        return double(ir.l.size()) / rate;
    };
    return 2.0 * (at(-35.0) - at(-5.0));
}

} // namespace

TEST_CASE("fx_reverb_impulse_response") {
    HVYXG_NEED_TABLES();
    const Ir hall = reverb_ir(1, 44100.0f, 4.0f);
    // The early-reflection tap after the initial delay (8 → 12.7 ms = 560 samples).
    CHECK(first_arrival(hall.l) == 560);
    CHECK(first_arrival(hall.r) == 560);
    // DLL renders: Hall 1 T30 1.66 (2006LE) … 1.69 s (S-YXG50); Room 1 0.48 … 0.54 s;
    // Plate 2.09 … 2.23 s; Tunnel 3.8 … 4.3 s.
    CHECK(t30(hall, 44100.0f) == doctest::Approx(1.68).epsilon(0.08));
    CHECK(t30(reverb_ir(3, 44100.0f, 2.0f), 44100.0f) == doctest::Approx(0.51).epsilon(0.12));
    CHECK(t30(reverb_ir(8, 44100.0f, 5.0f), 44100.0f) == doctest::Approx(2.16).epsilon(0.1));
    CHECK(t30(reverb_ir(10, 44100.0f, 8.0f), 44100.0f) == doctest::Approx(4.05).epsilon(0.12));
}

TEST_CASE("fx_reverb_sample_rate_scaling") {
    HVYXG_NEED_TABLES();
    const Ir a = reverb_ir(1, 44100.0f, 4.0f);
    const Ir b = reverb_ir(1, 48000.0f, 4.0f);
    // Same delays in milliseconds: 560 samples at 44.1 kHz → 609.5 at 48 kHz.
    CHECK(std::abs(double(first_arrival(b.l)) - 609.5) <= 1.0);
    CHECK(t30(b, 48000.0f) == doctest::Approx(t30(a, 44100.0f)).epsilon(0.03));
}

TEST_CASE("fx_chorus_lfo_rate_and_depth") {
    HVYXG_NEED_TABLES();
    // Chorus 1, LFO 64 (2.69 Hz), depth 20 (160 samples p-p), delay offset 64 (6.4 ms).
    for (const float rate : {44100.0f, 48000.0f}) {
        EffectControl ctrl(test::fx_tables());
        ctrl.set_chorus_algorithm(1);
        ctrl.set_parameter(Block::Chorus, 0, 64);
        ctrl.set_parameter(Block::Chorus, 1, 20);
        ctrl.set_parameter(Block::Chorus, 2, 64);  // no feedback
        ctrl.set_parameter(Block::Chorus, 3, 64);
        ChorusUnit unit(rate);
        unit.load(ctrl);
        const auto spacing = static_cast<std::size_t>(512 * rate / 44100.0f);
        const auto n = static_cast<std::size_t>(2.0f * rate);
        std::vector<float> l(n), r(n);
        for (std::size_t i = 0; i < n; i += spacing) l[i] = 1.0f;
        for (std::size_t i = 0; i < n; i += 64) unit.process(l.data() + i, r.data() + i, std::min<std::size_t>(64, n - i));
        std::vector<double> delay;
        for (std::size_t i = 0; i + spacing <= n; i += spacing) {
            std::size_t best = i;
            for (std::size_t k = i; k < i + spacing; ++k)
                if (std::fabs(l[k]) > std::fabs(l[best])) best = k;
            delay.push_back(double(best - i) * 1000.0 / rate);  // ms
        }
        double lo = 1e9, hi = -1e9, mean = 0;
        for (double d : delay) {
            lo = std::min(lo, d);
            hi = std::max(hi, d);
            mean += d;
        }
        mean /= double(delay.size());
        // 160 samples at 44.1 kHz = 3.63 ms peak to peak, above the 6.4 ms offset.
        CHECK(hi - lo == doctest::Approx(3.63).epsilon(0.06));
        CHECK(lo == doctest::Approx(6.45).epsilon(0.05));
        int crossings = 0;
        for (std::size_t k = 1; k < delay.size(); ++k)
            if ((delay[k - 1] - mean) * (delay[k] - mean) < 0) ++crossings;
        CHECK(crossings / 2.0 / 2.0 == doctest::Approx(2.69).epsilon(0.1));
    }
}

TEST_CASE("fx_celeste_three_taps") {
    HVYXG_NEED_TABLES();
    EffectControl ctrl(test::fx_tables());
    ctrl.set_chorus_algorithm(4);  // Celeste 1: taps at 0°, +120°, −120°
    ctrl.set_parameter(Block::Chorus, 1, 0);  // no modulation
    ChorusUnit unit(44100.0f);
    unit.load(ctrl);
    std::vector<float> l(512), r(512);
    l[0] = 1.0f;
    unit.process(l.data(), r.data(), 256);
    unit.process(l.data() + 256, r.data() + 256, 256);
    int pulses = 0;
    for (std::size_t i = 1; i + 1 < l.size(); ++i)
        if (std::fabs(l[i]) > 0.1f && std::fabs(l[i]) >= std::fabs(l[i - 1]) &&
            std::fabs(l[i]) >= std::fabs(l[i + 1]))
            ++pulses;
    CHECK(pulses == 2);  // the main tap and the third tap 64 samples later
}
