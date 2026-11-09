// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Full XG on the S-YXG2006LE bank (`Synth::Options::full_xg_2006le`, docs/ENGINE.md "Full XG on
// the S-YXG2006LE"), the 2006LE's 0A nn 40..45 level controls and the smoothing of EQ changes.
// The bank cases need the banks (skipped otherwise).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "eq.hpp"
#include "test_support.hpp"

using namespace hvyxg;

namespace {

Synth::Options opts(bool syxg50, bool full, bool exact = false) {
    Synth::Options o;
    o.tables = test::tables();
    o.bank = test::bank(syxg50);
    o.full_xg_2006le = full;
    o.dll_exact = exact;
    return o;
}

/// An organ chord on part 1 with the variation in insertion connection on it (`msb` / `lsb`),
/// reverb and chorus sends 0; the level in dB from 0.6 s to 1.6 s.
double insertion_level(bool syxg50, bool full, std::uint8_t msb) {
    test::Rig r(opts(syxg50, full));
    r.program(0, 16);
    r.cc(0, 91, 0);
    r.cc(0, 93, 0);
    r.xg({0x02, 0x01, 0x40, msb, 0x00});
    r.xg({0x02, 0x01, 0x5A, 0x00});
    r.xg({0x02, 0x01, 0x5B, 0x00});
    for (const std::uint8_t k : {48, 55, 60, 64}) r.note_on(0, k, 100);
    const auto x = r.render_left(1.6);
    return test::rms_db(x, static_cast<std::size_t>(0.6 * 44100));
}

std::vector<float> render_probe(const Synth::Options& o, void (*setup)(test::Rig&), double seconds = 1.0) {
    test::Rig r(o);
    setup(r);
    return r.render(static_cast<std::size_t>(seconds * 44100));
}

} // namespace

TEST_CASE("full_xg_2006le_distortion_drive_follows_the_syxg50") {
    HVYXG_NEED_TABLES();
    if (!test::have_banks()) return;
    // Relative to Thru: S-YXG50 −7.8 / −6.7 / −5.2 dB (distortion, overdrive, amp simulator);
    // the 2006LE's own clip level (+24 dB above full scale) +5.5 / +6.0 / +7.8 dB; with full XG
    // the S-YXG50's clip level (+12 dB): −6.2 / −5.2 / −3.7 dB, the rest is the two organs.
    for (const std::uint8_t msb : {0x49, 0x4A, 0x4B}) {
        CAPTURE(msb);
        const double s50 = insertion_level(true, false, msb) - insertion_level(true, false, 0x40);
        const double le = insertion_level(false, false, msb) - insertion_level(false, false, 0x40);
        const double full = insertion_level(false, true, msb) - insertion_level(false, true, 0x40);
        CHECK(le > 4.0);
        CHECK(full < -3.0);
        CHECK(std::abs(full - s50) < 2.0);
        // The S-YXG50 bank ignores the option.
        CHECK(insertion_level(true, true, msb) == insertion_level(true, false, msb));
    }
}

TEST_CASE("full_xg_2006le_variation_parameter_ranges") {
    HVYXG_NEED_TABLES();
    if (!test::have_banks()) return;
    // Delay L,C,R / L,R / Echo / Cross Delay in system connection, delay times (parameters 1..3,
    // 14 bit, 0.1 ms) at the 2006LE's top and 5 ms above it: its limit tables end at 7150 / 3550
    // (Echo, Cross Delay), the S-YXG50's at 7200 / 3600.
    for (const std::uint8_t msb : {0x05, 0x06, 0x07, 0x08}) {
        CAPTURE(msb);
        const auto probe = [&](bool full, int d) {
            test::Rig r(opts(false, full));
            r.program(0, 0);
            r.cc(0, 91, 0);
            r.cc(0, 93, 0);
            r.cc(0, 94, 127);
            r.xg({0x02, 0x01, 0x40, msb, 0x00});
            r.xg({0x02, 0x01, 0x5A, 0x01});
            for (int k = 0; k < 3; ++k)
                r.xg({0x02, 0x01, static_cast<std::uint8_t>(0x42 + 2 * k), static_cast<std::uint8_t>(d >> 7),
                      static_cast<std::uint8_t>(d & 0x7F)});
            r.note_on(0, 60, 100);
            r.render(4410);
            r.note_off(0, 60);
            return r.render(44100);
        };
        const int top = msb <= 0x06 ? 7150 : 3550;
        CHECK(probe(false, top) == probe(false, top + 50));
        CHECK(probe(true, top) != probe(true, top + 50));
        CHECK(probe(true, top) == probe(false, top));
    }
}

TEST_CASE("full_xg_2006le_xg_parameters_after_a_gs_reset") {
    HVYXG_NEED_TABLES();
    if (!test::have_banks()) return;
    // GS reset, then the XG part volume 08 00 0B 20: the 2006LE ignores it, full XG obeys it as
    // the S-YXG50 does (−12 dB at volume 32 against 100 with the XG volume law).
    const auto level = [](bool full) {
        test::Rig r(opts(false, full));
        r.sysex({0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7});
        r.render(4410);
        r.cc(0, 91, 0);
        r.cc(0, 93, 0);
        r.xg({0x08, 0x00, 0x0B, 0x20});
        r.note_on(0, 60, 100);
        return test::rms_db(r.render_left(1.0), 4410);
    };
    const double le = level(false), full = level(true);
    CHECK(full < le - 10.0);
}

TEST_CASE("full_xg_2006le_off_with_dll_exact_and_on_the_syxg50") {
    HVYXG_NEED_TABLES();
    if (!test::have_banks()) return;
    const auto setup = [](test::Rig& r) {
        r.sysex({0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7});
        r.xg({0x08, 0x00, 0x0B, 0x20});
        r.xg({0x02, 0x01, 0x40, 0x49, 0x00});
        r.xg({0x02, 0x01, 0x5A, 0x00});
        r.xg({0x02, 0x01, 0x5B, 0x00});
        r.note_on(0, 60, 100);
        r.note_on(0, 67, 100);
    };
    CHECK(render_probe(opts(false, true, true), setup) == render_probe(opts(false, false, true), setup));
    CHECK(render_probe(opts(true, true), setup) == render_probe(opts(true, false), setup));
    CHECK(render_probe(opts(false, true), setup) != render_probe(opts(false, false), setup));
}

TEST_CASE("syxg2006le_0a_level_controls") {
    HVYXG_NEED_TABLES();
    if (!test::bank(false)) return;
    // S-YXG2006LE.DLL (organ note on part 1, each segment after XG On):
    // 0A 00 40 (MW) 127 with MW 0 and 0 with MW 127 are silent; 32 with MW 127 and 96 with MW 0
    // are −11.8 / −12.0 dB; 64 changes nothing; the same for PB (41, |bend| >> 6), CAT (42),
    // PAT (43, per key), AC1 (44), AC2 (45). hvyxg matches every segment within 0.1 dB.
    const auto level = [](std::uint8_t addr, std::uint8_t depth, std::uint8_t source, bool exact) {
        test::Rig r(opts(false, false, exact));
        r.program(0, 16);
        r.xg({0x0A, 0x00, addr, depth});
        switch (addr) {
        case 0x40: r.cc(0, 1, source); break;
        case 0x41: r.bend(0, source ? 0x0000 : 0x2000); break;  // |bend| >> 6: full down = 127
        case 0x42: r.send({0xD0, source}); break;
        case 0x43: r.send({0xA0, 60, source}); break;
        case 0x44: r.cc(0, 16, source); break;
        default: r.cc(0, 17, source); break;
        }
        r.note_on(0, 60, 100);
        return test::rms_db(r.render_left(1.0), 4410);
    };
    const double plain = level(0x40, 64, 0, false);
    for (const std::uint8_t a : {0x40, 0x42, 0x43, 0x44, 0x45}) {
        CAPTURE(a);
        CHECK(level(a, 127, 0, false) < -150.0);
        CHECK(level(a, 0, 127, false) < -150.0);
        CHECK(std::abs(level(a, 32, 127, false) - plain + 11.8) < 0.6);
        CHECK(std::abs(level(a, 96, 0, false) - plain + 11.8) < 0.6);
        CHECK(std::abs(level(a, 127, 127, false) - plain) < 1.0);  // DLL: +0.3 … +0.6 dB
        CHECK(std::abs(level(a, 0, 0, false) - plain) < 0.01);
        CHECK(level(a, 127, 0, true) < -150.0);  // DLL behaviour: also in DLL-exact mode
    }
    // Bend: the term reads |bend| >> 6, so a centred bend counts as 0 and full down as 127.
    CHECK(level(0x41, 127, 0, false) < -150.0);
    CHECK(level(0x41, 0, 127, false) < -150.0);
    CHECK(std::abs(level(0x41, 0, 0, false) - plain) < 0.01);
}

TEST_CASE("eq_changes_crossfade") {
    // A change of a running voice EQ (or one that switches it on / off) crossfades over 10 ms;
    // the first settings of a voice apply at once.
    constexpr float kFs = 44100.0f;
    const std::size_t fade = eq::change_fade_samples(kFs);
    CHECK(fade == 441);
    std::vector<float> sine(8820);
    for (std::size_t i = 0; i < sine.size(); ++i) sine[i] = static_cast<float>(0.5 * std::sin(2 * 3.141592653589793 * 60.0 * i / kFs));
    eq::VoiceEqSettings boost, cut;
    boost.bass_gain = 0x4C;
    cut.bass_gain = 0x34;

    // Reference: the boost from the start, unsmoothed.
    eq::VoiceEq ref;
    ref.start(boost, kFs);
    std::vector<float> a = sine;
    ref.process(a.data(), a.size());
    eq::VoiceEq v;
    v.start(boost, kFs);
    std::vector<float> b = sine;
    v.process(b.data(), b.size());
    CHECK(a == b);

    // Boost → cut halfway: the largest step between neighbouring samples stays that of the
    // signal (a hard switch jumps by ≈ 1.6 at 60 Hz).
    eq::VoiceEq s;
    s.start(boost, kFs);
    std::vector<float> y = sine;
    s.process(y.data(), 4410);
    s.update(cut, kFs);
    CHECK(s.active());
    s.process(y.data() + 4410, 4410);
    float max_step = 0;
    for (std::size_t i = 4000; i < 5400; ++i) max_step = std::max(max_step, std::abs(y[i] - y[i - 1]));
    CHECK(max_step < 0.05f);
    // After the fade the output is the cut alone: the level of the steady cut.
    eq::VoiceEq c;
    c.start(cut, kFs);
    std::vector<float> yc = sine;
    c.process(yc.data(), yc.size());
    CHECK(std::abs(20 * std::log10(test::rms(y, 7000, 8820) / test::rms(yc, 7000, 8820))) < 0.01);

    // Switching off fades out and then stops processing.
    s.update(eq::VoiceEqSettings{}, kFs);
    CHECK(s.active());
    std::vector<float> z(fade, 0.1f);
    s.process(z.data(), z.size());
    CHECK_FALSE(s.active());

    // The multi EQ stage: no fade at its first setting, a fade after a later change.
    midi::xg::MultiEq m;
    m.write(0x01, 0x4C);
    eq::MultiEqStage st(kFs);
    st.update(m);
    CHECK(st.active());
    std::vector<float> l = sine, r = sine;
    st.process(l.data(), r.data(), 4410);
    m.write(0x01, 0x34);
    st.update(m);
    st.process(l.data() + 4410, r.data() + 4410, 4410);
    float step = 0;
    for (std::size_t i = 4000; i < 5400; ++i) step = std::max(step, std::abs(l[i] - l[i - 1]));
    CHECK(step < 0.05f);
}
