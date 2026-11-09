// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Superset EQs (eq.hpp): the multi EQ 02 40, the part EQ 08 nn 72..77, the part HPF 0A nn 20
// and the drum-note EQ / HPF. Filter responses, MU2000 type presets, the RAM, the drum rules,
// and that the defaults leave the output bit-identical.
#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "eq.hpp"
#include "midi/part.hpp"
#include "midi/xg_ram.hpp"
#include "test_support.hpp"

using namespace hvyxg;

namespace {

constexpr double kFs = 44100.0;

double db(const eq::Coeffs& c, double hz) { return c.magnitude_db(hz, kFs); }

/// First-order shelf with its pole at wc: |(jw + G wc) / (jw + wc)| in dB, analog.
double analog_low_shelf_db(double f, double fc, double g) {
    const double x = f / fc;
    return 10 * std::log10((x * x + g * g) / (x * x + 1));
}

} // namespace

TEST_CASE("eq: XG EQ frequency table") {
    CHECK(eq::frequency_hz(0) == 20);
    CHECK(eq::frequency_hz(0x0C) == 80);
    CHECK(eq::frequency_hz(0x22) == 1000);
    CHECK(eq::frequency_hz(0x36) == 10000);
    CHECK(eq::frequency_hz(60) == 20000);
    CHECK(eq::frequency_hz(99) == 20000);
}

TEST_CASE("eq: first-order shelves") {
    for (double gain : {12.0, 6.0, 1.0}) {
        const double g = std::pow(10.0, gain / 20);
        const eq::Coeffs lo = eq::low_shelf1(200, gain, kFs);
        CHECK(db(lo, 1) == doctest::Approx(gain).epsilon(0.001));
        CHECK(db(lo, 20000) == doctest::Approx(0).epsilon(0.01));
        // Pole at the band frequency (bilinear, so the analog curve holds well below fs / 2).
        CHECK(db(lo, 200) == doctest::Approx(analog_low_shelf_db(200, 200, g)).epsilon(0.002));
        const eq::Coeffs hi = eq::high_shelf1(2000, gain, kFs);
        CHECK(db(hi, 5) == doctest::Approx(0).epsilon(0.001));
        CHECK(db(hi, 22049) == doctest::Approx(gain).epsilon(0.001));
    }
    // A cut is the exact inverse of the boost.
    for (double f : {30.0, 200.0, 1000.0, 8000.0}) {
        CHECK(db(eq::low_shelf1(500, -9, kFs), f) == doctest::Approx(-db(eq::low_shelf1(500, 9, kFs), f)).epsilon(1e-4));
        CHECK(db(eq::high_shelf1(500, -9, kFs), f) == doctest::Approx(-db(eq::high_shelf1(500, 9, kFs), f)).epsilon(1e-4));
    }
    // Two first-order sections as one biquad.
    const eq::Coeffs a = eq::low_shelf1(100, 6, kFs), b = eq::high_shelf1(5000, -4, kFs);
    const eq::Coeffs ab = eq::cascade1(a, b);
    for (double f : {50.0, 700.0, 9000.0})
        CHECK(db(ab, f) == doctest::Approx(db(a, f) + db(b, f)).epsilon(1e-4));
}

TEST_CASE("eq: peaking band") {
    for (double q : {0.1, 0.7, 2.0, 12.0}) {
        for (double gain : {12.0, -12.0, 3.0}) {
            const eq::Coeffs p = eq::peak(1000, q, gain, kFs);
            CHECK(db(p, 1000) == doctest::Approx(gain).epsilon(0.001));
            // Boost and cut by the same amount are inverses.
            const eq::Coeffs m = eq::peak(1000, q, -gain, kFs);
            for (double f : {100.0, 700.0, 1400.0, 9000.0})
                CHECK(db(p, f) + db(m, f) == doctest::Approx(0).epsilon(1e-3));
        }
    }
    // Wider for a smaller XG Q: at one octave away, Q 0.7 keeps much more than Q 12.
    CHECK(db(eq::peak(1000, 0.7, 12, kFs), 2000) > 6.0);
    CHECK(db(eq::peak(1000, 12, 12, kFs), 2000) < 0.5);
}

TEST_CASE("eq: part HPF") {
    CHECK(eq::hpf_hz(0x50) == doctest::Approx(82.6).epsilon(0.01));
    CHECK(eq::hpf_hz(0x60) == doctest::Approx(233.6).epsilon(0.01));
    CHECK(eq::hpf_hz(0x7F) == doctest::Approx(1745).epsilon(0.01));
    const double fc = eq::hpf_hz(0x7F);
    const eq::Coeffs h = eq::high_pass2(fc, eq::kHpfQ, kFs);
    CHECK(db(h, fc) == doctest::Approx(0).epsilon(0.01));         // Q 1: 0 dB at fc
    CHECK(db(h, fc / 4) == doctest::Approx(-23.7).epsilon(0.02));  // 12 dB/oct
    CHECK(db(h, 4 * fc) == doctest::Approx(0.27).epsilon(0.1));

    eq::VoiceEq v;
    eq::VoiceEqSettings s;
    v.start(s, 44100.0f);
    CHECK_FALSE(v.active());
    s.hpf = 0;  // below the voice's own HPF: none in either bank
    v.update(s, 44100.0f);
    CHECK_FALSE(v.active());
    s.hpf = 65;
    v.update(s, 44100.0f);
    CHECK(v.active());
}

TEST_CASE("eq: voice EQ is transparent while flat and filters otherwise") {
    std::vector<float> x(256), y;
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = std::sin(0.01f * static_cast<float>(i * i));
    eq::VoiceEq v;
    v.start(eq::VoiceEqSettings{}, 44100.0f);
    y = x;
    v.process(y.data(), y.size());
    CHECK(y == x);
    // DC through a +12 dB bass shelf at 80 Hz settles at ×3.98.
    eq::VoiceEqSettings s;
    s.bass_gain = 0x4C;
    v.update(s, 44100.0f);
    std::vector<float> dc(44100, 0.25f);
    v.process(dc.data(), dc.size());
    CHECK(dc.back() == doctest::Approx(0.25 * std::pow(10.0, 12.0 / 20)).epsilon(0.001));
}

TEST_CASE("eq: multi EQ RAM, ranges and MU2000 type presets") {
    midi::xg::MultiEq m;
    CHECK(m.flat());
    CHECK(m.band(0).freq == 0x0C);
    CHECK(m.band(1).freq == 0x1C);
    CHECK(m.band(2).freq == 0x22);
    CHECK(m.band(3).freq == 0x2E);
    CHECK(m.band(4).freq == 0x34);
    for (std::size_t k = 0; k < 5; ++k) {
        CHECK(m.band(k).gain == 0x40);
        CHECK(m.band(k).q == 7);
    }
    CHECK_FALSE(m.band(0).peak);
    CHECK(m.band(2).peak);

    CHECK(m.write(0x01, 0x7F));
    CHECK(m[0x01] == 0x4C);  // +12 dB
    CHECK(m.write(0x02, 0));
    CHECK(m[0x02] == 4);     // band 1: 32 Hz..2 kHz
    CHECK(m.write(0x12, 0x7F));
    CHECK(m[0x12] == 58);    // band 5: 500 Hz..16 kHz
    CHECK(m.write(0x0A, 0));
    CHECK(m[0x0A] == 14);    // bands 2..4: 100 Hz..10 kHz
    CHECK(m.write(0x03, 0));
    CHECK(m[0x03] == 1);     // Q 0.1..12.0
    CHECK(m.write(0x14, 5));
    CHECK(m[0x14] == 1);
    CHECK_FALSE(m.write(0x08, 1));  // reserved
    CHECK_FALSE(m.write(0x15, 1));

    // Rock (read back from the MU2000 firmware); a type write also resets both shapes.
    CHECK(m.write(0x00, 3));
    const std::array<std::uint8_t, 0x15> rock = {3,    0x47, 0x10, 7, 0, 0x44, 0x14, 7, 0, 0x3C, 0x24,
                                                5,    0,    0x44, 0x29, 10, 0, 0x42, 0x32, 7, 0};
    CHECK(m.bytes == rock);
    CHECK(m.write(0x00, 9));
    CHECK(m[0x00] == 4);  // classic
    CHECK(m.write(0x00, 0));
    CHECK(m == midi::xg::MultiEq{});
}

TEST_CASE("eq: multi EQ band responses") {
    midi::xg::MultiEq m;
    // Band 3 peak +12 dB at 1 kHz.
    m.write(0x09, 0x4C);
    CHECK(eq::MultiEqStage::design(m, 2, 44100.0f).magnitude_db(1000, kFs) == doctest::Approx(12).epsilon(0.001));
    // Band 1 shelf −6 dB at 200 Hz: −6 dB at DC, the inverse first-order shelf.
    m.write(0x01, 0x3A);
    m.write(0x02, 0x14);
    const eq::Coeffs b1 = eq::MultiEqStage::design(m, 0, 44100.0f);
    CHECK(b1.magnitude_db(1, kFs) == doctest::Approx(-6).epsilon(0.001));
    CHECK(b1.magnitude_db(200, kFs) == doctest::Approx(-analog_low_shelf_db(200, 200, std::pow(10.0, 0.3))).epsilon(0.002));
    // Band 1 as a peak: +/−6 dB at 200 Hz exactly.
    m.write(0x04, 1);
    CHECK(eq::MultiEqStage::design(m, 0, 44100.0f).magnitude_db(200, kFs) == doctest::Approx(-6).epsilon(0.001));
    // Band 5 shelf +10 dB at 4 kHz: two half-gain first-order shelves.
    m.write(0x11, 0x4A);
    m.write(0x12, 0x2E);
    const eq::Coeffs b5 = eq::MultiEqStage::design(m, 4, 44100.0f);
    CHECK(b5.magnitude_db(22049, kFs) == doctest::Approx(10).epsilon(0.001));
    CHECK(b5.magnitude_db(4000, kFs) == doctest::Approx(2 * eq::high_shelf1(4000, 5, kFs).magnitude_db(4000, kFs)).epsilon(1e-3));
    // Flat bands are identity.
    const eq::Coeffs b2 = eq::MultiEqStage::design(m, 1, 44100.0f);
    CHECK(b2.b0 == 1.0f);
    CHECK(b2.a1 == 0.0f);

    // The stage on a block: inactive (and untouched output) while flat.
    eq::MultiEqStage stage(44100.0f);
    stage.update(midi::xg::MultiEq{});
    CHECK_FALSE(stage.active());
    stage.update(m);
    CHECK(stage.active());
}

TEST_CASE("eq: jazz preset response") {
    midi::xg::MultiEq m;
    m.write(0x00, 1);
    // Sum of the five bands at a few points (dB), from the design itself; checks the bands
    // add as a cascade and the preset reaches its low / high shelves.
    double lo = 0, hi = 0;
    for (std::size_t k = 0; k < 5; ++k) {
        lo += eq::MultiEqStage::design(m, k, 44100.0f).magnitude_db(5, kFs);
        hi += eq::MultiEqStage::design(m, k, 44100.0f).magnitude_db(21000, kFs);
    }
    CHECK(lo == doctest::Approx(-6).epsilon(0.05));  // band 1 −6 dB shelf (+ bands 2..4 → 0)
    CHECK(hi == doctest::Approx(-6).epsilon(0.1));   // band 5 −6 dB shelf
}

TEST_CASE("eq: part EQ / HPF settings and the drum rules") {
    midi::xg::MultiPart ram(0);
    midi::xg::MultiPartExt ext;
    midi::Part part;
    part.ram = &ram;
    part.ram_ext = &ext;
    CHECK(eq::voice_settings(part, nullptr, false) == eq::VoiceEqSettings{});
    ram.write(0x72, 0x7F);
    ram.write(0x76, 0x28);
    ext.write(0x20, 100);
    const eq::VoiceEqSettings melodic = eq::voice_settings(part, nullptr, false);
    CHECK(melodic.bass_gain == 0x4C);
    CHECK(melodic.bass_freq == 0x28);
    CHECK(melodic.hpf == 100);
    // A drum note takes its own EQ (not the part's) and HPF part + note − 64 (MU2000).
    midi::xg::DrumSetup d;
    d.write(0x21, 0x40 - 6);
    d.write(0x50, 84);
    const eq::VoiceEqSettings drum = eq::voice_settings(part, &d, true);
    CHECK(drum.bass_gain == 64);
    CHECK(drum.treble_gain == 0x3A);
    CHECK(drum.treble_freq == 0x36);
    CHECK(drum.hpf == 120);
    // No drum entry: the kit defaults.
    CHECK(eq::voice_settings(part, nullptr, true).bass_gain == 64);
    CHECK(eq::voice_settings(part, nullptr, true).hpf == 100);
    // 2006LE kit entries without XG2.0 data (EQ / HPF bytes 0) are flat, HPF = the part's.
    midi::xg::DrumSetup z;
    z.eq_bass_gain = z.eq_treble_gain = z.eq_bass_freq = z.eq_treble_freq = z.hpf_cutoff_freq = 0;
    const eq::VoiceEqSettings zs = eq::voice_settings(part, &z, true);
    CHECK(zs.shelves_flat());
    CHECK(zs.hpf == 100);
    ext.reset();
    CHECK(eq::voice_settings(part, &z, true) == eq::VoiceEqSettings{});
}

namespace {

/// A low saw-lead note (C2, 2 s) on part 1 after `setup`, left channel.
template <class F>
std::vector<float> saw_note(std::shared_ptr<const Bank> b, F setup) {
    test::Rig r(b);
    r.cc(0, 91, 0);
    r.cc(0, 93, 0);
    r.program(0, 81);
    setup(r);
    r.note_on(0, 36, 100);
    return r.render_left(1.0);
}

double band_rms_db(const std::vector<float>& x, double lo_hz, double hi_hz) {
    // Energy of the Hann-windowed DFT bins in [lo, hi] over 8192 samples from 0.1 s.
    constexpr std::size_t n = 8192, start = 4410;
    const double pi = 3.141592653589793;
    double s = 0;
    const auto k0 = static_cast<std::size_t>(lo_hz * n / 44100.0);
    const auto k1 = static_cast<std::size_t>(hi_hz * n / 44100.0);
    for (std::size_t k = k0; k <= k1; ++k) {
        double re = 0, im = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const double w = 0.5 - 0.5 * std::cos(2 * pi * static_cast<double>(i) / n);
            const double ph = 2 * pi * static_cast<double>(k * i % n) / n;
            re += w * x[start + i] * std::cos(ph);
            im -= w * x[start + i] * std::sin(ph);
        }
        s += re * re + im * im;
    }
    return 10 * std::log10(s + 1e-30);
}

} // namespace

TEST_CASE("eq: rendered through both engines") {
    HVYXG_NEED_TABLES();
    for (bool syxg50 : {true, false}) {
        const auto b = test::bank(syxg50);
        if (!b) continue;
        CAPTURE(syxg50);
        const auto ref = saw_note(b, [](test::Rig&) {});
        // Default transparency: writing every default value again changes nothing.
        const auto same = saw_note(b, [](test::Rig& r) {
            r.xg({0x02, 0x40, 0x00, 0x00});
            r.xg({0x08, 0x00, 0x72, 0x40});
            r.xg({0x08, 0x00, 0x73, 0x40});
            r.xg({0x08, 0x00, 0x76, 0x0C});
            r.xg({0x0A, 0x00, 0x20, 0x40});
            r.xg({0x0A, 0x00, 0x20, 0x00});  // below the voice's own HPF: none
        });
        CHECK(same == ref);
        // Part HPF at its maximum (≈ 1.7 kHz) takes the low partials away.
        const auto hpf = saw_note(b, [](test::Rig& r) { r.xg({0x0A, 0x00, 0x20, 0x7F}); });
        CHECK(band_rms_db(hpf, 60, 400) < band_rms_db(ref, 60, 400) - 20);
        // Part bass +12 dB at 200 Hz raises the lows; a write to another part does nothing.
        const auto bass = saw_note(b, [](test::Rig& r) {
            r.xg({0x08, 0x00, 0x72, 0x4C});
            r.xg({0x08, 0x00, 0x76, 0x14});
        });
        CHECK(band_rms_db(bass, 60, 150) > band_rms_db(ref, 60, 150) + 8);
        const auto other = saw_note(b, [](test::Rig& r) { r.xg({0x08, 0x01, 0x72, 0x4C}); });
        CHECK(other == ref);
        // Multi EQ: Rock lifts the lows; band 5 −12 dB shelf at 2 kHz cuts the highs.
        const auto rock = saw_note(b, [](test::Rig& r) { r.xg({0x02, 0x40, 0x00, 0x03}); });
        CHECK(band_rms_db(rock, 60, 150) > band_rms_db(ref, 60, 150) + 4);
        const auto dark = saw_note(b, [](test::Rig& r) {
            r.xg({0x02, 0x40, 0x11, 0x34});
            r.xg({0x02, 0x40, 0x12, 0x28});
        });
        CHECK(band_rms_db(dark, 5000, 12000) < band_rms_db(ref, 5000, 12000) - 9);
        // XG System On brings the flat EQ back.
        const auto reset = saw_note(b, [](test::Rig& r) {
            r.xg({0x02, 0x40, 0x00, 0x03});
            r.xg({0x08, 0x00, 0x72, 0x4C});
            r.xg({0x0A, 0x00, 0x20, 0x7F});
            r.sysex({0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7});
            r.cc(0, 91, 0);
            r.cc(0, 93, 0);
            r.program(0, 81);
        });
        CHECK(reset == ref);
    }
}

TEST_CASE("eq: MU2000 part EQ strength (Synth::Options::mu_part_eq)") {
    HVYXG_NEED_TABLES();
    // S-MU2000 (genuine MU2000 firmware), every gain value of both bands: steps of four values.
    const double mu[25] = {-2.095, -2.095, -2.095, -2.095, -1.334, -1.334, -1.334, -1.334, -0.573,
                           -0.573, -0.573, -0.573, 0,      0,      0,      0,      0.762,  0.762,
                           0.762,  0.762,  1.526,  1.526,  1.526,  1.526,  2.286};
    for (int v = 0x34; v <= 0x4C; ++v) {
        CAPTURE(v);
        CHECK(eq::shelf_db(static_cast<std::uint8_t>(v), false) == v - 64);
        CHECK(eq::shelf_db(static_cast<std::uint8_t>(v), true) == doctest::Approx(mu[v - 0x34]));
    }
    // The shape stays: a bass shelf at 1 kHz reaches its plateau far below.
    eq::VoiceEq e;
    eq::VoiceEqSettings s;
    s.bass_freq = 0x22;
    s.bass_gain = 0x4C;
    s.mu_strength = true;
    e.start(s, 44100.0f);
    CHECK(e.active());
    std::vector<float> x(44100, 0.0f);
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(std::sin(2 * 3.141592653589793 * 30.0 * i / 44100.0));
    e.process(x.data(), x.size());
    CHECK(20 * std::log10(test::rms(x, 22050) * std::sqrt(2.0)) == doctest::Approx(2.286).epsilon(0.01));
    // +1 … +3 is flat on the MU2000: nothing is processed.
    s.bass_gain = 0x43;
    e.start(s, 44100.0f);
    CHECK_FALSE(e.active());

    const auto b = test::bank(true);
    if (!b) return;
    const auto render = [&](bool mu_eq, bool exact, std::uint8_t gain) {
        hvyxg::Synth::Options o;
        o.tables = test::tables();
        o.bank = b;
        o.mu_part_eq = mu_eq;
        o.dll_exact = exact;
        test::Rig r(o);
        r.cc(0, 91, 0);
        r.cc(0, 93, 0);
        r.program(0, 81);
        r.xg({0x08, 0x00, 0x76, 0x22});
        r.xg({0x08, 0x00, 0x72, gain});
        r.note_on(0, 36, 100);
        return r.render_left(1.0);
    };
    const auto ref = render(false, false, 0x40);
    CHECK(render(true, false, 0x40) == ref);
    CHECK(render(true, false, 0x43) == ref);
    const double nominal = band_rms_db(render(false, false, 0x4C), 60, 150) - band_rms_db(ref, 60, 150);
    const double mu12 = band_rms_db(render(true, false, 0x4C), 60, 150) - band_rms_db(ref, 60, 150);
    CHECK(nominal > 10);
    CHECK(mu12 == doctest::Approx(2.286).epsilon(0.05));
    // Ignored with dll_exact (no part EQ at all).
    CHECK(render(true, true, 0x4C) == render(false, true, 0x40));
}

TEST_CASE("eq: drum note EQ and HPF") {
    HVYXG_NEED_TABLES();
    for (bool syxg50 : {true, false}) {
        const auto b = test::bank(syxg50);
        if (!b) continue;
        CAPTURE(syxg50);
        const auto kick = [&](auto setup) {
            test::Rig r(b);
            r.cc(9, 91, 0);
            r.cc(9, 93, 0);
            setup(r);
            r.note_on(9, 36, 110);
            return r.render_left(0.6);
        };
        const auto ref = kick([](test::Rig&) {});
        // The part EQ does not reach drum notes (MU2000).
        CHECK(kick([](test::Rig& r) { r.xg({0x08, 0x09, 0x72, 0x34}); }) == ref);
        // The note HPF does (setup 1 = part mode 2 of part 10), and adds to the part HPF.
        const auto hpf = kick([](test::Rig& r) { r.xg({0x30, 36, 0x50, 0x7F}); });
        CHECK(band_rms_db(hpf, 30, 200) < band_rms_db(ref, 30, 200) - 15);
        const auto sum = kick([](test::Rig& r) {
            r.xg({0x30, 36, 0x50, 0x60});
            r.xg({0x0A, 0x09, 0x20, 0x5F});  // 0x60 + 0x5F − 64 = 0x7F
        });
        CHECK(sum == hpf);
        // The note's bass EQ.
        const auto boom = kick([](test::Rig& r) {
            r.xg({0x30, 36, 0x20, 0x4C});
            r.xg({0x30, 36, 0x24, 0x14});
        });
        CHECK(band_rms_db(boom, 30, 150) > band_rms_db(ref, 30, 150) + 6);
    }
}
