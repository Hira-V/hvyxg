// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// S-YXG50 voice laws: tables and integer laws, no bank needed.

#include <doctest/doctest.h>

#include <cmath>

#include "syxg50/chip.hpp"
#include "syxg50/dll_tables.hpp"
#include "syxg50/laws.hpp"
#include "test_support.hpp"

using namespace hvyxg::syxg50;

TEST_CASE("syxg50_lin_law") {
    CHECK(lin(0) == doctest::Approx(1.0f));
    CHECK(lin(0x10000) == doctest::Approx(0.5f).epsilon(1e-4));
    CHECK(lin(0x8000) == doctest::Approx(0.75f).epsilon(1e-4)); // piecewise linear, not 0.707
    CHECK(lin(0x80000) == doctest::Approx(1.0f / 256.0f).epsilon(1e-4)); // -48 dB voice end
    CHECK(lin(kAttenOff) == 0.0f);
    for (std::int32_t x : {0x1000, 0x23456, 0x80000, 0xab000})
        CHECK(std::abs(inv_lin(lin(x)) - x) <= 0x400);
}

TEST_CASE("syxg50_rate_tables") {
    HVYXG_NEED_TABLES();
    // TA/TB/TC at 44.1 kHz: 2^26/TA, 2^27/TB, 2^27/TC (docs: Time tables).
    const RateTables rt(test::tables()->syxg50, 44100.0f);
    CHECK(test::tables()->syxg50.kAttackTime[0] == 867329.0f);
    CHECK(test::tables()->syxg50.kDecayTime[32] == 524160.0f);
    CHECK(rt.attack[127] == static_cast<int>(67108864.0f / 67.0f));
    CHECK(rt.decay[32] == 256);
    // LFO: byte 7 = 31 → s 47 → period TB[79] = 8736 samples (5.048 Hz) [meas].
    CHECK(test::tables()->syxg50.kLfoSpeed[31] == 47);
    CHECK(test::tables()->syxg50.kDecayTime[32 + 47] == 8736.0f);
    // The rates scale with the output rate.
    const RateTables r48(test::tables()->syxg50, 48000.0f);
    CHECK(r48.decay[32] == static_cast<int>(256.0f * 44100.0f / 48000.0f));
}

TEST_CASE("syxg50_level_chain") {
    HVYXG_NEED_TABLES();
    // Tlev: [128]=0, [101]=5, [77]=11, [64]=16, [39]=27, [13]=52, [0]=127.
    CHECK(test::tables()->syxg50.kLevelAtten[128] == 0);
    CHECK(test::tables()->syxg50.kLevelAtten[101] == 5);
    CHECK(test::tables()->syxg50.kLevelAtten[77] == 11);
    CHECK(test::tables()->syxg50.kLevelAtten[64] == 16);
    CHECK(test::tables()->syxg50.kLevelAtten[0] == 127);
    // Reed Organ (L0 77) [meas]: vol/expr/vel → level register.
    const auto reg = [](int vol, int expr, int vel) {
        const int g = law::part_gain(vol, expr, 127);
        const int l = std::max(1, (g * 77) >> 7);
        return law::level_register(test::tables()->syxg50, l, 0, law::velocity_atten(test::tables()->syxg50, 0, vel));
    };
    CHECK(reg(127, 127, 127) == 22);
    CHECK(reg(100, 127, 127) == 34);
    CHECK(reg(64, 127, 127) == 54);
    CHECK(reg(127, 127, 100) == 32);
    CHECK(reg(127, 64, 127) == 54);
    CHECK(reg(127, 127, 64) == 52);
    CHECK(lin(0x1000 * 22 + 0x4000 * 2) == doctest::Approx(0.2812f).epsilon(1e-3));
    CHECK(law::part_gain(0, 127, 127) == 0);
    CHECK(law::part_gain(1, 1, 127) == 1);
}

TEST_CASE("syxg50_velocity_and_pan") {
    HVYXG_NEED_TABLES();
    CHECK(law::velocity_sense(100, 64, 64) == 100);
    CHECK(law::velocity_sense(100, 127, 64) == 127);
    CHECK(law::velocity_sense(100, 64, 0) == 1);
    CHECK(law::velocity_offset(64, 0) == 64);
    CHECK(law::velocity_offset(100, 2) == 107);
    // Curve 1 (drums) is concave.
    CHECK(test::tables()->syxg50.kVelocityCurves[128 + 64] == 72);
    // Centre pan: 2 steps (1.5 dB each) on both sides; hard left mutes the right.
    CHECK(law::pan_atten(test::tables()->syxg50, 64, 64).left == 2);
    CHECK(law::pan_atten(test::tables()->syxg50, 64, 64).right == 2);
    CHECK(law::pan_atten(test::tables()->syxg50, 127, 64).left == 15);
    CHECK(law::pan_atten(test::tables()->syxg50, 127, 64).right == 0);
    CHECK(law::send_atten(test::tables()->syxg50, 127) == 0);
    CHECK(law::send_atten(test::tables()->syxg50, 64) == 16);
    CHECK(law::send_atten(test::tables()->syxg50, 0) == 255);
    CHECK(test::tables()->syxg50.kChorusPanAtten[64] == 16);
}

TEST_CASE("syxg50_pitch_laws") {
    HVYXG_NEED_TABLES();
    // Key tracking: 100 % plays the key; 50 % halves the distance to the centre, cents remain.
    CHECK(law::key_track(test::tables()->syxg50, 61, 0, 60).key == 61);
    CHECK(law::key_track(test::tables()->syxg50, 61, 1, 60).key == 60);
    CHECK(law::key_track(test::tables()->syxg50, 61, 1, 60).cents == 50);
    CHECK(law::key_track(test::tables()->syxg50, 59, 1, 60).key == 59);
    CHECK(law::key_track(test::tables()->syxg50, 59, 1, 60).cents == 50);
    CHECK(law::key_track(test::tables()->syxg50, 10, 5, 60).key == 60);
    // Bend: ±200 cents at the default range, +100 at 0x3000 [meas].
    CHECK(law::bend_cents(0x42, 0x3fff) == 200);
    CHECK(law::bend_cents(0x42, 0x3000) == 100);
    CHECK(law::bend_cents(0x42, 0) == -200);
    CHECK(law::bend_cents(0x4c, 0x3fff) == 1200);
    // MW pitch +24 semitones at MW 64 → +1207 cents (measured 1204.9).
    CHECK(law::ctrl_pitch(0x58, 64) == 1207);
    CHECK(law::detune_cents(0x80) == 0);
    // PEG: range 2 → 18.75 cents per level step (¼-cent units).
    CHECK(law::peg_level(-1, 0, 2) == -75);
    CHECK(law::peg_part_level(63, 0) == 2400); // part PEG initial level 127 → +600 cents
}

TEST_CASE("syxg50_filter_laws") {
    HVYXG_NEED_TABLES();
    CHECK(test::tables()->syxg50.kCutoff[0] == 1536);
    CHECK(test::tables()->syxg50.kCutoff[64] == 3237);
    CHECK(test::tables()->syxg50.kCutoff[127] == 4095);
    CHECK(law::cutoff_register(0x700) == 1);
    CHECK(law::cutoff_register(0xc00) == 0x400);
    CHECK(law::cutoff_register(0x1200) == 0x7ff);
    // Part resonance: additive below 64, max() above.
    CHECK(law::part_resonance(10, 74) == 10);
    CHECK(law::part_resonance(10, 96) == 32);
    CHECK(law::part_resonance(10, 32) == 0);
    // FEG: 32 cutoff units (= 128 ¼ units) per level step.
    CHECK(law::feg_level(65, 0) == 128);
    CHECK(test::tables()->syxg50.kFegStep[31] == 88);
    CHECK(test::tables()->syxg50.kPegStep[31] == 84);
}

TEST_CASE("syxg50_mutable_cross_check_tables") {
    HVYXG_NEED_TABLES();
    // Element byte 68 = key-on delay: keyed at the ceil(t/20)-th 10-ms tick.
    const auto ticks = [](int t) { return (t + 19) / 20; };
    CHECK(ticks(test::tables()->syxg50.kKeyOnDelay[0]) == 0);
    CHECK(ticks(test::tables()->syxg50.kKeyOnDelay[1]) == 1);
    CHECK(ticks(test::tables()->syxg50.kKeyOnDelay[2]) == 2);
    CHECK(ticks(test::tables()->syxg50.kKeyOnDelay[3]) == 4);
    CHECK(ticks(test::tables()->syxg50.kKeyOnDelay[10]) == 64);
    // Element byte 15: 0 = 100 % … 5 = fixed (MUTable lists the reverse order).
    CHECK(test::tables()->syxg50.kKeyTrack[0] == 100);
    CHECK(test::tables()->syxg50.kKeyTrack[5] == 0);
    // Tlev is the 40·log law ("y ≈ (x/127)²" in MUTable), 0.75 dB units, within 1.5 units.
    for (int l = 1; l <= 128; ++l) {
        const double expect = -40.0 * std::log10(l / 128.0) / 0.75;
        CHECK(std::abs(test::tables()->syxg50.kLevelAtten[static_cast<std::size_t>(l)] - expect) < 1.5);
    }
}
