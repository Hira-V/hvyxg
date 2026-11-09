// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// XG2.0 insertion effects (hvyxg superset): routing through the chain pre-buses, level
// neutrality against the uninserted part, a decoded variation-type kernel in a chain, and the
// XG2.0-only designs (compressor, noise gate, ring modulator, lo-fi).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include "hvyxg/bank.hpp"
#include "fx/fx_insertion.hpp"
#include "fx/fx_system.hpp"
#include "renderer.hpp"
#include "syxg50/effects.hpp"
#include "midi/part.hpp"
#include "midi/xg_ram.hpp"
#include "test_support.hpp"

using namespace hvyxg;
using fx::Xg20Effect;

namespace {

constexpr double kPi = 3.14159265358979323846;

double rms_db(const std::vector<float>& x, std::size_t a = 0) {
    double s = 0;
    for (std::size_t i = a; i < x.size(); ++i) s += double(x[i]) * x[i];
    return 10 * std::log10(s / double(x.size() - a) + 1e-30);
}

// Amplitude of frequency f in x (Goertzel).
double tone(const std::vector<float>& x, double f, double fs) {
    const double w = 2 * kPi * f / fs, c = 2 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (float v : x) {
        const double s = v + c * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    return 2 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / double(x.size());
}

std::vector<float> run(Xg20Effect& fx, const std::vector<float>& in) {
    std::vector<float> l = in, r = in;
    for (std::size_t i = 0; i < in.size(); i += 64)
        fx.process(l.data() + i, r.data() + i, std::min<std::size_t>(64, in.size() - i));
    return l;
}

std::vector<float> sine(double f, double amp, std::size_t n) {
    std::vector<float> x(n);
    for (std::size_t i = 0; i < n; ++i) x[i] = static_cast<float>(amp * std::sin(2 * kPi * f * double(i) / 44100.0));
    return x;
}

} // namespace

TEST_CASE("fx_insertion_ram_defaults") {
    HVYXG_NEED_TABLES();
    // XG2.0 default: Distortion with the DLLs' variation defaults, part off; a type write
    // reloads the type's defaults (Delay L,C,R in the word layout) and counts.
    midi::xg::EffectInsertion e{test::tables().get()};
    CHECK(e.type_msb() == 0x49);
    CHECK(e.part() == 0x7F);
    CHECK(e.param(0) == fx::variation_defaults(test::fx_tables(), {2, 5})[0]);
    const auto w = e.type_writes;
    e.write(0x00, 0x05);
    CHECK(e.type_writes == w + 1);
    CHECK(e[0x30] * 128 + e[0x31] == 3333);
    CHECK(e.param(0) == 3333);
    e.write(0x00, 0x53);
    CHECK(e[0x04] == 100);  // compressor threshold −27 dB
}

TEST_CASE("fx_insertion_chain_delay_lcr") {
    HVYXG_NEED_TABLES();
    // A Delay L,C,R block on part 1: the chain's dry pair comes back with its taps, scaled by
    // the part's volume / pan law; no chain means no change.
    midi::xg::MultiPart mp(0);
    midi::Part part;
    part.ram = &mp;
    midi::xg::EffectData eff{test::tables().get()};
    midi::xg::EffectInsertions ins = test::insertions();
    midi::xg::System sys;
    ModuleContext ctx;
    ctx.tables = test::tables().get();
    ctx.shared.effect1 = &eff;
    ctx.shared.effect_insertion = &ins;
    ctx.shared.system = &sys;
    ctx.shared.parts = &part;
    ctx.shared.part_count = 1;
    auto fx = syxg50::make_system_effects(ctx);
    Buses b;
    fx->prepare(b);
    CHECK(b.chains == 0);
    CHECK(b.chain_of(0) == -1);

    ins[3].write(0x00, 0x05);
    ins[3].write(0x0C, 0x00);
    std::vector<float> out;
    for (int blk = 0; blk < 200; ++blk) {
        b.clear();
        fx->prepare(b);
        REQUIRE(b.chain_of(0) == 0);
        if (blk == 0) b.chain_l[0][0] = 0.5f;
        fx->process(b, 128);
        out.insert(out.end(), b.dry_r.begin(), b.dry_r.end());
    }
    // R tap at 166.7 ms (A2d − A3a = 7351 samples), wet 62/512 (insertion halving).
    double peak = 0;
    std::size_t at = 0;
    for (std::size_t i = 1000; i < out.size(); ++i)
        if (std::fabs(out[i]) > peak) {
            peak = std::fabs(out[i]);
            at = i;
        }
    CHECK(at == 7351);
    CHECK(peak > 0.5 * 62.0 / 512 * 0.3);
}

TEST_CASE("fx_insertion_compressor_and_gate") {
    Xg20Effect c(test::fx_tables(), 44100.0f);
    auto p = *fx::xg20_insertion_defaults(0x53, 0);
    p[4] = 127;  // output level 0 dB
    c.set(Xg20Effect::Kind::Compressor, p);
    // 0 dBFS sine, threshold −27 dB, ratio 5: ≈ 21.6 dB of gain reduction.
    const auto y = run(c, sine(1000, 1.0, 44100));
    const double gr = 20 * std::log10(tone(std::vector<float>(y.begin() + 22050, y.end()), 1000, 44100));
    // (The peak detector releases between the sine's peaks: ≈ 1.6 dB less reduction than the
    // static 21.6 dB.)
    CHECK(gr == doctest::Approx(-20.0).epsilon(0.05));
    // Below the threshold: unity.
    Xg20Effect c2(test::fx_tables(), 44100.0f);
    c2.set(Xg20Effect::Kind::Compressor, p);
    const auto y2 = run(c2, sine(1000, 0.01, 44100));
    CHECK(20 * std::log10(tone(std::vector<float>(y2.begin() + 22050, y2.end()), 1000, 44100) / 0.01) ==
          doctest::Approx(0.0).epsilon(0.01));

    Xg20Effect g(test::fx_tables(), 44100.0f);
    auto pg = *fx::xg20_insertion_defaults(0x54, 0);
    pg[3] = 127;
    g.set(Xg20Effect::Kind::NoiseGate, pg);
    // −60 dB (below the −45 dB threshold) stays shut; −20 dB opens.
    CHECK(rms_db(run(g, sine(500, 0.001, 44100)), 22050) < -120);
    CHECK(rms_db(run(g, sine(500, 0.1, 44100)), 22050) == doctest::Approx(20 * std::log10(0.1 / std::sqrt(2.0))).epsilon(0.01));
}

TEST_CASE("fx_insertion_ring_mod_and_lofi") {
    HVYXG_NEED_TABLES();
    Xg20Effect rm(test::fx_tables(), 44100.0f);
    rm.set(Xg20Effect::Kind::RingMod, *fx::xg20_insertion_defaults(0x71, 0));
    // 300 Hz × 1000.7 Hz carrier (dry/wet 127): sidebands at 700.7 / 1300.7 Hz, no 300 Hz.
    const auto y = run(rm, sine(300, 0.5, 44100));
    const double lo = tone(y, 700.7, 44100), hi = tone(y, 1300.7, 44100), in = tone(y, 300, 44100);
    CHECK(lo == doctest::Approx(0.25).epsilon(0.1));
    CHECK(hi == doctest::Approx(0.25).epsilon(0.1));
    CHECK(in < 0.01);

    Xg20Effect lf(test::fx_tables(), 44100.0f);
    auto p = *fx::xg20_insertion_defaults(0x5E, 0);
    p[0] = 4;   // 8.8 kHz: a new sample every 5 input samples
    p[4] = 0;   // no filter
    p[2] = 6;   // 0 dB
    lf.set(Xg20Effect::Kind::LoFi, p);
    const auto z = run(lf, sine(440, 0.5, 4410));
    std::size_t changes = 0;
    for (std::size_t i = 1; i < z.size(); ++i) changes += z[i] != z[i - 1];
    CHECK(changes == doctest::Approx(4410 / 5).epsilon(0.02));
}

namespace {

struct SongRig : test::Rig {
    explicit SongRig(bool syxg50) : test::Rig(test::bank(syxg50), 64) {}
    std::vector<float> note() {
        render(88200);  // let the effect ramps settle
        note_on(0, 60, 100);
        auto n = render(44100);
        note_off(0, 60);
        return n;
    }
    void sysex(std::initializer_list<std::uint8_t> addr_data) { xg(addr_data); }
};

// Path setups: 0 dry only, 1 reverb only, 2 chorus only (system connection so that the part
// dry level applies).
void path(SongRig& r, int which) {
    r.sysex({0x02, 0x01, 0x5A, 0x01});
    r.cc(0, 91, static_cast<std::uint8_t>(which == 1 ? 127 : 0));
    r.cc(0, 93, static_cast<std::uint8_t>(which == 2 ? 127 : 0));
    if (which != 0) r.sysex({0x08, 0x00, 0x11, 0x00});  // part dry level 0
}

} // namespace

TEST_CASE("fx_insertion_routing_with_banks") {
    HVYXG_NEED_TABLES();
    if (!test::have_banks()) return;
    for (const bool syxg50 : {true, false}) {
        CAPTURE(syxg50);
        // A Thru block changes nothing (the chain is skipped): bit-identical.
        {
            SongRig a(syxg50), b(syxg50);
            b.sysex({0x03, 0x00, 0x00, 0x40, 0x00});
            b.sysex({0x03, 0x00, 0x0C, 0x00});
            CHECK(a.note() == b.note());
        }
        // A flat 2-band EQ is level-neutral on the dry, reverb and chorus paths.
        for (int which : {0, 1, 2}) {
            CAPTURE(which);
            SongRig a(syxg50), b(syxg50);
            path(a, which);
            path(b, which);
            b.sysex({0x03, 0x00, 0x00, 0x4D, 0x00});
            b.sysex({0x03, 0x00, 0x03, 0x40});
            b.sysex({0x03, 0x00, 0x05, 0x40});
            b.sysex({0x03, 0x00, 0x0C, 0x00});
            const double ref = test::rms_db(a.note()), ins = test::rms_db(b.note());
            CHECK(std::fabs(ins - ref) < 0.3);
        }
    }
}

TEST_CASE("fx_insertion_pan_zero_is_left") {
    HVYXG_NEED_TABLES();
    // CC10 = 0 on an inserted melodic part pans the chain output hard left, as the part's own
    // voices are panned [meas: S-MU2000, distortion block on an organ chord: left only].
    if (!test::have_banks()) return;
    for (const bool syxg50 : {true, false}) {
        CAPTURE(syxg50);
        SongRig r(syxg50);
        r.cc(0, 91, 0);
        r.cc(0, 93, 0);
        r.cc(0, 10, 0);
        r.sysex({0x03, 0x00, 0x00, 0x49, 0x00});
        r.sysex({0x03, 0x00, 0x0C, 0x00});
        const auto st = r.note();
        double el = 0, er = 0;
        for (std::size_t i = 0; i < st.size(); i += 2) {
            el += double(st[i]) * st[i];
            er += double(st[i + 1]) * st[i + 1];
        }
        CHECK(el > 1e-3);
        CHECK(10 * std::log10(er / el + 1e-30) < -40);
    }
}
