// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Master volume: the universal real-time master volume (F0 7F 7F 04 01 ll mm F7) and the XG
// System master volume (F0 43 10 4C 00 00 04 vv F7). The expected gains and timings were
// measured from renders of syxg50.dll and S-YXG2006LE.DLL: a
// sustained drawbar organ (program 16, key 60, velocity 127, volume / expression 127, reverb
// and chorus sends 0) divided by the same render without the master volume message
// (docs/ENGINE.md, "Master volume"). They need the banks (skipped otherwise); the front-end
// rules (MSB only, last message wins, resets) are in front_end_test.cpp.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "test_support.hpp"

using namespace hvyxg;

namespace {

std::vector<std::uint8_t> gm_master(int v) {
    return {0xF0, 0x7F, 0x7F, 0x04, 0x01, static_cast<std::uint8_t>(v & 0x7F),
            static_cast<std::uint8_t>(v >> 7 & 0x7F), 0xF7};
}
std::vector<std::uint8_t> xg_master(int v) {
    return {0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x04, static_cast<std::uint8_t>(v), 0xF7};
}

/// The test tone of the DLL measurements, the note struck at frame 13230.
struct Tone : test::Rig {
    std::vector<float> left;
    explicit Tone(bool syxg50) : test::Rig(test::bank(syxg50), 64) {
        cc(0, 91, 0);
        cc(0, 93, 0);
        cc(0, 7, 127);
        cc(0, 11, 127);
        program(0, 16);
        run(13230);
        note_on(0, 60, 127);
    }
    void midi(const std::vector<std::uint8_t>& m) { synth.midi(m); }
    void run(std::size_t frames) {
        const auto st = render(frames);
        for (std::size_t i = 0; i < frames; ++i) left.push_back(st[2 * i]);
    }
};

/// Power ratio of x over ref in [a, b), dB.
double ratio_db(const std::vector<float>& x, const std::vector<float>& ref, std::size_t a, std::size_t b) {
    double sx = 0, sr = 0;
    for (std::size_t i = a; i < b; ++i) {
        sx += static_cast<double>(x[i]) * x[i];
        sr += static_cast<double>(ref[i]) * ref[i];
    }
    return 10 * std::log10((sx + 1e-300) / sr);
}

/// The level after `msgs` at frame 22050, from 9000 frames on (past the S-YXG50's 80 ms
/// level update and the level glide down to silence), relative to the plain tone.
double level_db(bool syxg50, const std::vector<std::vector<std::uint8_t>>& msgs) {
    Tone ref(syxg50), t(syxg50);
    ref.run(22050 - 13230);
    t.run(22050 - 13230);
    for (const auto& m : msgs) t.midi(m);
    ref.run(17640);
    t.run(17640);
    return ratio_db(t.left, ref.left, 22050 + 9000, 22050 + 17640);
}

/// The first frame (16-frame resolution) at which t departs from ref by more than 0.02 dB.
std::size_t first_change(const std::vector<float>& t, const std::vector<float>& ref, std::size_t from) {
    for (std::size_t a = from; a + 16 <= t.size(); a += 16)
        if (std::abs(ratio_db(t, ref, a, a + 16)) > 0.02) return a;
    return t.size();
}

// MSB → measured gain (dB) of the universal real-time master volume (LSB 0).
constexpr std::pair<int, double> kGain2006le[] = {
    {126, -0.285},  {121, -0.579},  {120, -0.884},  {100, -3.807}, {96, -4.722},  {90, -6.021},
    {65, -11.765},  {64, -12.041},  {63, -12.041},  {40, -19.590}, {32, -24.082}, {16, -35.848},
    {8, -47.363},   {3, -62.429},   {2, -68.095},   {1, -999.0},   {0, -999.0}};
constexpr std::pair<int, double> kGainSyxg50[] = {
    {126, -1.023},  {121, -1.023},  {120, -1.584},  {100, -4.278}, {96, -5.105},  {90, -6.021},
    {65, -12.041},  {64, -12.041},  {63, -13.064},  {40, -20.245}, {32, -25.105}, {16, -35.208},
    {8, -47.250},   {3, -63.728},   {2, -75.769},   {1, -75.769},  {0, -999.0}};

void check_gain(double got, double want) {
    if (want < -900) CHECK(got < -200.0);  // silent
    else CHECK(std::abs(got - want) < 0.002);
}

} // namespace

TEST_CASE("master_volume_gain_2006le") {
    HVYXG_NEED_TABLES();
    if (!test::bank(false)) return;
    for (const auto& [msb, want] : kGain2006le) {
        CAPTURE(msb);
        check_gain(level_db(false, {gm_master(msb << 7)}), want);
    }
    // LSB ignored; XG System master volume identical.
    check_gain(level_db(false, {gm_master(0x207F)}), -12.041);
    check_gain(level_db(false, {gm_master(0x3F7F)}), -0.285);
    check_gain(level_db(false, {xg_master(64)}), -12.041);
    // Last message wins.
    check_gain(level_db(false, {xg_master(100), gm_master(0x3FFF)}), 0.0);
    check_gain(level_db(false, {gm_master(96 << 7), xg_master(100)}), -3.807);
}

TEST_CASE("master_volume_gain_syxg50") {
    HVYXG_NEED_TABLES();
    if (!test::bank(true)) return;
    for (const auto& [msb, want] : kGainSyxg50) {
        CAPTURE(msb);
        check_gain(level_db(true, {gm_master(msb << 7)}), want);
    }
    check_gain(level_db(true, {gm_master(0x207F)}), -12.041);
    check_gain(level_db(true, {gm_master(0x3F7F)}), -1.023);
    check_gain(level_db(true, {xg_master(63)}), -13.064);
    check_gain(level_db(true, {xg_master(100), gm_master(0x3FFF)}), 0.0);
    check_gain(level_db(true, {gm_master(96 << 7), xg_master(100)}), -4.278);
}

TEST_CASE("master_volume_timing") {
    HVYXG_NEED_TABLES();
    // Master volume 100 sent at frame 26460: the S-YXG2006LE applies it from the start of the
    // engine block holding it; the S-YXG50 at the voice's next level round-robin slot, i.e.
    // the tick ≡ 0 (mod 8) at 28224, from the start of the block running it (DLL: first
    // changed 16-frame chunk at 28164).
    for (const bool syxg50 : {false, true}) {
        if (!test::bank(syxg50)) continue;
        CAPTURE(syxg50);
        Tone ref(syxg50), t(syxg50);
        ref.run(26460 - 13230);
        t.run(26460 - 13230);
        t.midi(xg_master(100));
        ref.run(4000);
        t.run(4000);
        const std::size_t at = first_change(t.left, ref.left, 26000);
        if (syxg50) {
            CHECK(at >= 28100);
            CHECK(at < 28228);
        } else {
            CHECK(at >= 26400);
            CHECK(at < 26460 + 64);
        }
    }
}
