// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Effect control: coefficient words and delay addresses against the DLLs' own register
// images (dumped from syxg50.dll and S-YXG2006LE.DLL; the two agree word for word).
#include <doctest/doctest.h>

#include <cmath>

#include "fx/fx_dsp.hpp"
#include "fx/fx_control.hpp"
#include "fx/fx_tables.hpp"
#include "test_support.hpp"

using namespace hvyxg::fx;
using hvyxg::fx::chorus_algorithm;
using hvyxg::fx::reverb_algorithm;

namespace {

EffectControl hall1() {
    EffectControl ctrl(test::fx_tables());
    ctrl.set_reverb_algorithm(1);
    ctrl.set_reverb_return(64, 64);
    ctrl.set_parameter(Block::Reverb, 12, 52);  // XG reset image
    return ctrl;
}

} // namespace

TEST_CASE("fx_type_maps") {
    HVYXG_NEED_TABLES();
    CHECK(reverb_algorithm(test::fx_tables(), 0x01, 0x00) == 1);
    CHECK(reverb_algorithm(test::fx_tables(), 0x01, 0x07) == 17);
    CHECK(reverb_algorithm(test::fx_tables(), 0x02, 0x05) == 13);
    CHECK(reverb_algorithm(test::fx_tables(), 0x04, 0x07) == 18);
    CHECK(reverb_algorithm(test::fx_tables(), 0x10, 0x00) == 9);   // White Room
    CHECK(reverb_algorithm(test::fx_tables(), 0x11, 0x03) == 10);  // Tunnel, any LSB
    CHECK(reverb_algorithm(test::fx_tables(), 0x12, 0x00) == 0);   // Canyon: not supported
    CHECK(reverb_algorithm(test::fx_tables(), 0x13, 0x00) == 12);  // Basement
    CHECK(reverb_algorithm(test::fx_tables(), 0x01, 0x20) == 1);   // LSB > 9 → 0
    CHECK(chorus_algorithm(test::fx_tables(), 0x41, 0x08) == 12);  // Chorus 4
    CHECK(chorus_algorithm(test::fx_tables(), 0x42, 0x08) == 13);  // Celeste 4
    CHECK(chorus_algorithm(test::fx_tables(), 0x43, 0x07) == 20);
    CHECK(chorus_algorithm(test::fx_tables(), 0x44, 0x00) == 0);   // Symphonic as chorus: off
    CHECK(chorus_algorithm(test::fx_tables(), 0x48, 0x00) == 0);   // Phaser as chorus: off
    CHECK(is_celeste(4));
    CHECK(is_celeste(13));
    CHECK_FALSE(is_celeste(7));
}

TEST_CASE("fx_hall1_register_image") {
    HVYXG_NEED_TABLES();
    const EffectControl ctrl = hall1();
    // Input HPF 90 Hz +2 dB, LPF 5.6 kHz.
    CHECK(ctrl.word(46) == 160);
    CHECK(ctrl.word(45) == 864);
    CHECK(ctrl.word(47) == 505);
    CHECK(ctrl.word(48) == 38);
    CHECK(ctrl.word(49) == 38);
    CHECK(ctrl.word(50) == 208);
    // Comb {in, fir, fir} for reverb time 18.
    CHECK(ctrl.word(91) == 77);
    CHECK(ctrl.word(90) == 172);
    CHECK(ctrl.word(89) == 172);
    CHECK(ctrl.word(93) == 183);
    CHECK(ctrl.word(99) == 192);
    CHECK(ctrl.word(102) == 199);
    // All-passes and the output all-pass with return 64 / centre pan.
    CHECK(ctrl.word(41) == 337);
    CHECK(ctrl.word(44) == 939);
    CHECK(ctrl.word(56) == 358);
    CHECK(ctrl.word(59) == 934);
    CHECK(ctrl.word(72) == 960);
    CHECK(ctrl.word(71) == 155);
    CHECK(ctrl.word(84) == 960);
    CHECK(ctrl.word(83) == 155);
    // ER / reverb balance 52.
    CHECK(ctrl.word(27) == 152);
    CHECK(ctrl.word(28) == 336);
    CHECK(ctrl.word(51) == 0);
    // Addresses (44.1 kHz samples).
    CHECK(ctrl.addr(17) == 18427);
    CHECK(ctrl.addr(63) == 18987);  // initial delay 8 → 560 samples
    CHECK(ctrl.addr(8) == 18991);   // rev delay 0 → 4 samples
    CHECK(ctrl.addr(11) - ctrl.addr(16) == 1255);
    CHECK(ctrl.addr(13) - ctrl.addr(21) == 976);
    CHECK(ctrl.addr(12) - ctrl.addr(31) == 4570);
    CHECK(ctrl.addr(28) - ctrl.addr(33) == 3920);
    CHECK(ctrl.addr(30) - ctrl.addr(34) == 3295);
    CHECK(ctrl.addr(32) - ctrl.addr(35) == 2894);
    CHECK(ctrl.addr(0) == 7387);
    CHECK(ctrl.addr(4) == 7744);
    CHECK(ctrl.addr(23) == 3608);
}

TEST_CASE("fx_reverb_parameters") {
    HVYXG_NEED_TABLES();
    EffectControl ctrl = hall1();
    // Initial delay 63 → 4379 samples (XG table #5, 99.3 ms).
    ctrl.set_parameter(Block::Reverb, 2, 63);
    CHECK(ctrl.addr(63) - ctrl.addr(17) == 4379);
    // Values outside the limit table are clamped (LPF 34..60).
    ctrl.set_parameter(Block::Reverb, 4, 10);
    CHECK(ctrl.parameter(Block::Reverb, 4) == 34);
    // High damp and dry/wet are not used by the system reverb.
    CHECK_FALSE(ctrl.set_parameter(Block::Reverb, 13, 3));
    CHECK_FALSE(ctrl.set_parameter(Block::Reverb, 9, 10));
    // ER / reverb balance (docs: 1 → 152/0, 96 → 36/512, 127 → 0/512).
    ctrl.set_parameter(Block::Reverb, 12, 1);
    CHECK(ctrl.word(27) == 152);
    CHECK(ctrl.word(28) == 0);
    ctrl.set_parameter(Block::Reverb, 12, 96);
    CHECK(ctrl.word(27) == 36);
    CHECK(ctrl.word(28) == 512);
    ctrl.set_parameter(Block::Reverb, 12, 127);
    CHECK(ctrl.word(27) == 0);
    // Feedback ±0.97.
    ctrl.set_parameter(Block::Reverb, 14, 127);
    CHECK(decode_word(ctrl.word(51), test::fx_tables().kCoefFormat[51]) == doctest::Approx(0.9609375));
    // Density 0: no all-passes; the output path is scaled by 0.5.
    ctrl.set_parameter(Block::Reverb, 11, 0);
    CHECK(ctrl.word(44) == 128);
    CHECK(ctrl.word(42) == 0);
    CHECK(ctrl.word(71) == 0);
    // Return 127, then pan hard left (logged register writes).
    ctrl.set_parameter(Block::Reverb, 11, 3);
    ctrl.set_reverb_return(64, 127);
    CHECK(ctrl.word(72) == 896);
    CHECK(ctrl.word(71) == 309);
    ctrl.set_reverb_return(1, 127);
    CHECK(ctrl.word(72) == 843);
    CHECK(ctrl.word(71) == 437);
    CHECK(ctrl.word(84) == 0);
    CHECK(ctrl.word(83) == 0);
}

TEST_CASE("fx_room_reverb_dimensions") {
    HVYXG_NEED_TABLES();
    EffectControl ctrl(test::fx_tables());
    ctrl.set_reverb_algorithm(9);  // White Room
    // Width 0: the shortest comb 1 (values cross-checked by the register sweep).
    ctrl.set_parameter(Block::Reverb, 5, 0);
    CHECK(ctrl.word(90) == 253);
    CHECK(ctrl.word(91) == 16);
    CHECK(ctrl.addr(12) - ctrl.addr(31) == 63);
}

TEST_CASE("fx_chorus1_register_image") {
    HVYXG_NEED_TABLES();
    EffectControl ctrl(test::fx_tables());
    ctrl.set_chorus_algorithm(1);
    CHECK(ctrl.word(9) == 48);    // LFO 6 → 0.252 Hz
    CHECK(ctrl.word(26) == 108);  // depth 54 → 432 samples p-p
    CHECK(ctrl.word(40) == 108);
    CHECK(ctrl.word(37) == 512);  // R at 180°
    CHECK(ctrl.word(66) == 76);   // feedback 77 → +0.148
    CHECK(ctrl.word(79) == 76);
    CHECK(ctrl.word(80) == 512);  // mono input: L line = L + R
    CHECK(ctrl.word(96) == 504);  // 63/64
    CHECK(ctrl.word(105) == 504);
    CHECK(ctrl.addr(27) == 1);
    CHECK(ctrl.addr(9) == 756);   // delay offset 106 → 755 samples
    CHECK(ctrl.addr(22) == 0);
    CHECK(ctrl.addr(14) == 756);
    // Input EQ: low shelf with the fixed +3 dB, high shelf at 0 dB.
    CHECK(ctrl.word(74) == 181);
    CHECK(ctrl.word(73) == 855);
    CHECK(ctrl.word(75) == 477);
    CHECK(ctrl.word(77) == 128);
    CHECK(ctrl.word(76) == 954);
    CHECK(ctrl.word(78) == 280);
    // LFO table (docs: 64 → 448 = 2.69 Hz, 127 → 886 = 39.7 Hz).
    ctrl.set_parameter(Block::Chorus, 0, 64);
    CHECK(ctrl.word(9) == 448);
    CHECK(decode_word(ctrl.word(9), 2) * 22050.0f == doctest::Approx(2.69165).epsilon(1e-4));
    ctrl.set_parameter(Block::Chorus, 0, 127);
    CHECK(ctrl.word(9) == 886);
    // Stereo input mode moves the R line 4096 samples away.
    ctrl.set_parameter(Block::Chorus, 14, 1);
    CHECK(ctrl.word(80) == 0);
    CHECK(ctrl.addr(22) == 4096);
    CHECK(ctrl.addr(14) == 4096 + 1 + 754);
}

TEST_CASE("fx_celeste_and_flanger") {
    HVYXG_NEED_TABLES();
    EffectControl cel(test::fx_tables());
    cel.set_chorus_algorithm(4);  // Celeste 1
    CHECK(cel.word(97) == 336);   // 42/64 on both taps
    CHECK(cel.word(96) == 336);
    CHECK(cel.addr(19) - cel.addr(9) == 64);
    EffectControl fl(test::fx_tables());
    fl.set_chorus_algorithm(7);  // Flanger 1
    CHECK(fl.word(26) == 14);    // depth 14 → v·128/127, no doubling
    CHECK(fl.word(37) == 512);   // phase diff 4 → 180°
    CHECK(fl.word(80) == 0);     // stereo input
    CHECK(fl.word(96) == 312);   // 39/64
    fl.set_parameter(Block::Chorus, 3, 100);
    CHECK(fl.parameter(Block::Chorus, 3) == 63);  // flanger delay offset limit
    fl.set_parameter(Block::Chorus, 13, 64);
    CHECK(fl.word(37) == 0);
}

TEST_CASE("fx_filter_design_follows_the_sample_rate") {
    HVYXG_NEED_TABLES();
    // The DLLs keep a tan(πf/fs) table per rate; the LPF pole for 5.6 kHz at 48 kHz.
    EffectControl ctrl(test::fx_tables(), 48000.0f);
    ctrl.set_reverb_algorithm(1);
    const double k = std::tan(3.14159265358979 * 5600.0 / 48000.0);
    CHECK(ctrl.word(50) == static_cast<int>(512.0 * (1.0 - k) / (1.0 + k) + 0.5));
    CHECK(ctrl.word(50) != hall1().word(50));
}
