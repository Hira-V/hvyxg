// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// S-YXG50 voices rendered from the real bank (HVYXG_TBL_BIN / HVYXG_TBL_DATA = sxgbin41.tbl /
// sxgwave4.tbl; skipped when unset). Expected values are the DLL's.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include "hvyxg/bank.hpp"
#include "syxg50/bank_data.hpp"
#include "syxg50/bin_table.hpp"
#include "bank_io.hpp"
#include "test_support.hpp"

using namespace hvyxg;

namespace {

bool have_bank() {
    const auto f = test::bank_files(true);
    if (!f) {
        MESSAGE("HVYXG_TBL_BIN / HVYXG_TBL_DATA not set: skipped");
        return false;
    }
    return f->bin.find("41") != std::string::npos;
}

struct Rig : test::Rig {
    Rig() : test::Rig(test::bank(true), 64) {}
};

/// Fundamental from the autocorrelation peak (parabolic interpolation) of channel 0,
/// searched between 2 and 20 ms.
double frequency(const std::vector<float>& s) {
    const std::size_t n = s.size() / 2;
    const auto ac = [&](std::size_t lag) {
        double a = 0;
        for (std::size_t i = 0; i + lag < n; ++i) a += static_cast<double>(s[2 * i]) * s[2 * (i + lag)];
        return a;
    };
    std::size_t best = 88;
    double bv = -1e30;
    for (std::size_t lag = 88; lag < 882; ++lag) {
        const double v = ac(lag);
        if (v > bv) {
            bv = v;
            best = lag;
        }
    }
    // The first peak within 1 % of the best (avoid octave errors).
    for (std::size_t lag = 88; lag < best; ++lag)
        if (ac(lag) > 0.99 * bv && ac(lag) >= ac(lag - 1) && ac(lag) >= ac(lag + 1)) {
            best = lag;
            break;
        }
    const double a = ac(best - 1), b = ac(best), c = ac(best + 1);
    return 44100.0 / (static_cast<double>(best) + 0.5 * (a - c) / (a - 2 * b + c));
}

double rms(const std::vector<float>& s) {
    double a = 0;
    for (std::size_t i = 0; i < s.size(); i += 2) a += static_cast<double>(s[i]) * s[i];
    return std::sqrt(a / static_cast<double>(s.size() / 2));
}

} // namespace

/// The bin and wave files in memory with BankMemory's padding (BinTable reads them in place).
struct TableFiles {
    std::vector<std::uint8_t> bin, wave;
    explicit TableFiles(const test::BankFiles& f)
        : bin(read_file(f.bin, "bin table")), wave(read_file(f.data, "wave table")) {
        bin.resize(bin.size() + BankMemory::kPad);
        wave.resize(wave.size() + BankMemory::kPad);
    }
    syxg50::BinTable parse() {
        syxg50::BinTable t;
        Error e;
        REQUIRE(syxg50::BinTable::parse(BankMemory{bin, wave}, t, e));
        return t;
    }
};

TEST_CASE("syxg50_bank_program_lookup") {
    if (!have_bank()) return;
    using syxg50::BankData;
    using syxg50::BankMode;
    const auto files = *test::bank_files(true);
    TableFiles tf(files);
    BankData bank(tf.parse());
    const std::uint8_t* piano = bank.program(BankMode::Xg, 0, 0, 0);
    REQUIRE(piano != nullptr);
    // XG MSB 1 is unlisted: the dummy row 0 record (voice level 0, key range 60-60).
    const std::uint8_t* dummy = bank.program(BankMode::Xg, 1, 0, 0);
    REQUIRE(dummy != nullptr);
    CHECK(dummy[0] == 0);
    CHECK(BankData::element(dummy, 0)[1] == 60);
    // GM mode ignores bank select; GS with an unlisted MSB plays the capital tone.
    CHECK(bank.program(BankMode::Gm, 8, 0, 0) == bank.program(BankMode::Gm, 0, 0, 0));
    CHECK(bank.program(BankMode::Gs, 12, 0, 0) == bank.program(BankMode::Gs, 0, 0, 0));
    // Reed Organ XG 0/0/20: element level 100, voice level 77.
    const std::uint8_t* organ = bank.program(BankMode::Xg, 0, 0, 20);
    REQUIRE(organ != nullptr);
    CHECK(organ[0] == 77);
    // XG Standard Kit kick: own wave, root 76, XG coarse −4 → −1628 cents relative to the wave.
    const std::uint8_t* kick = bank.drum_entry(bank.drum_kit(BankMode::Xg, 127, 0, 0), 36);
    REQUIRE(kick != nullptr);
    CHECK(kick[16] == 0xff);
    CHECK(kick[18] == 76);
}

TEST_CASE("syxg50_render_reed_organ") {
    HVYXG_NEED_TABLES();
    if (!have_bank()) return;
    Rig rig;
    rig.program(0, 20);
    rig.cc(0, 7, 127);
    rig.note_on(0, 69, 127);
    rig.render(22050);
    const auto s = rig.render(22050);
    // Key 69: 439.96 Hz [meas].
    CHECK(frequency(s) == doctest::Approx(439.96).epsilon(0.001));
    const double a127 = rms(s);
    CHECK(a127 > 0.0);
    // Volume 100: Tlev[60] vs Tlev[77] = 6 × 0.75 dB → lin(0xc000)/lin(0) = 0.6250 at the gain;
    // the measured gains were 0.1719 vs 0.2812 (ratio 0.611).
    rig.cc(0, 7, 100);
    rig.render(4410);
    const double a100 = rms(rig.render(22050));
    CHECK(a100 / a127 == doctest::Approx(0.1719 / 0.2812).epsilon(0.01));
}

TEST_CASE("syxg50_render_kick_pitch_independent_of_key_shift") {
    HVYXG_NEED_TABLES();
    if (!have_bank()) return;
    Rig rig;
    rig.note_on(9, 36, 127);
    const auto s = rig.render(4096);
    CHECK(rms(s) > 0.01);
}

TEST_CASE("syxg50_render_element_key_on_delay") {
    HVYXG_NEED_TABLES();
    if (!have_bank()) return;
    // XG 0/64/18: element 1 velocity 93-127, element 2 velocity 1-127 with byte 68 = 1
    // (key-on delay 20 → keyed by the first 10-ms tick). At velocity 64 only the
    // delayed element plays. The first tick is due at 441 samples; the DLL runs a block's events,
    // then the ticks due by the block's end, then renders it, so that tick runs before the
    // block 384..511 and the voice starts at 384 [meas: syxg50.dll, first sound at 384].
    const auto first_sound = [](const std::vector<float>& s) {
        for (std::size_t i = 0; i < s.size() / 2; ++i)
            if (s[2 * i] != 0.0f || s[2 * i + 1] != 0.0f) return i;
        return s.size();
    };
    const auto setup = [](Rig& rig) {
        rig.cc(0, 0, 0);
        rig.cc(0, 32, 64);
        rig.program(0, 18);
    };
    {
        Rig rig;
        setup(rig);
        rig.note_on(0, 60, 64);
        const auto s = rig.render(2048);
        CHECK(first_sound(s) == 384);
    }
    {
        // Velocity 127: element 1 (no delay) sounds at once.
        Rig rig;
        setup(rig);
        rig.note_on(0, 60, 127);
        CHECK(first_sound(rig.render(2048)) == 0);
    }
    {
        // A note-off before the delayed key-on cancels the element.
        Rig rig;
        setup(rig);
        rig.note_on(0, 60, 64);
        rig.render(128);
        rig.note_off(0, 60);
        CHECK(first_sound(rig.render(4096)) == 4096 * 2);
    }
}

TEST_CASE("syxg50_bank_zone_low_key") {
    if (!have_bank()) return;
    // Zone byte 14 is the zone's lowest key (MUTable's "note limit low"): the previous zone's
    // highest key + 1, 0 for a chain's first zone. The DLL never reads it.
    const auto files = *test::bank_files(true);
    TableFiles tf(files);
    const auto t = tf.parse();
    const auto s15 = t.bin.subspan(t.seg_offset[15], t.seg_length[15]);
    const auto s16 = t.bin.subspan(t.seg_offset[16], t.seg_length[16]);
    int zones = 0;
    for (std::size_t w = 0; w + 1 < s15.size(); w += 2) {
        auto off = static_cast<std::size_t>(s15[w] | (s15[w + 1] << 8));
        int prev = -1;
        while (off + 16 <= s16.size()) {
            CHECK(s16[off + 14] == prev + 1);
            ++zones;
            prev = s16[off + 15] & 0x7f;
            if (s16[off + 15] & 0x80) break;
            off += 16;
        }
    }
    CHECK(zones > 1000);
}

TEST_CASE("syxg50_render_gs_modulation_wheel_depth") {
    HVYXG_NEED_TABLES();
    if (!have_bank()) return;
    // GS mode reads the controller vibrato depth through its own PMD table: piano
    // note 67 with MW 64 swings 66.9 cents in GS mode against 43.1 in XG mode (DLL renders,
    // spectral peak track; the autocorrelation track here reads both swings wider).
    const auto swing = [](bool gs) {
        Rig rig;
        if (gs) rig.sysex({0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7});
        rig.cc(0, 91, 0);
        rig.cc(0, 1, 64);
        rig.render(4410);
        rig.note_on(0, 67, 100);
        rig.render(22050);
        double lo = 1e9, hi = 0;
        for (int i = 0; i < 80; ++i) {
            const double f = frequency(rig.render(551));
            lo = std::min(lo, f);
            hi = std::max(hi, f);
        }
        return 1200.0 * std::log2(hi / lo);
    };
    const double xg = swing(false), gs = swing(true);
    CHECK(gs / xg > 1.3);
    CHECK(gs / xg < 1.8);
}

namespace {

/// One render from power-on with every message queued at its frame (as the reference renders:
/// XG System On at 0, the part set up at 2000 with the effect sends at 0).
struct Timed {
    std::uint32_t frame;
    std::vector<std::uint8_t> msg;
};

std::vector<float> play(std::uint8_t program, std::vector<Timed> evs, std::size_t frames) {
    test::Rig rig(test::bank(true));
    const std::vector<Timed> setup{{2000, {0xB0, 91, 0}}, {2000, {0xB0, 93, 0}}, {2000, {0xB0, 94, 0}},
                                   {2000, {0xB0, 0, 0}},  {2000, {0xB0, 32, 0}}, {2000, {0xC0, program}}};
    evs.insert(evs.begin(), setup.begin(), setup.end());
    for (const auto& e : evs) rig.synth.midi(e.msg, e.frame);
    return rig.render(frames);
}

/// The first frame where two renders differ, or SIZE_MAX.
std::size_t first_difference(const std::vector<float>& a, const std::vector<float>& b) {
    for (std::size_t i = 0; i < std::min(a.size(), b.size()) / 2; ++i)
        if (std::abs(a[2 * i] - b[2 * i]) > 1e-6f || std::abs(a[2 * i + 1] - b[2 * i + 1]) > 1e-6f) return i;
    return SIZE_MAX;
}

} // namespace

TEST_CASE("syxg50_lfo_registers_at_level_round_robin") {
    HVYXG_NEED_TABLES();
    if (!have_bank()) return;
    // The control layer writes the LFO registers (speed, PMD, AMD / FMD) only at the note-on and at
    // the voice's turn in the level round robin, every 8 ticks. Reed Organ, key
    // 60 at frame 4410 (voice 0): the vibrato of a mod wheel move starts at frame 31745 for any
    // CC1 frame in 30000..31234 [meas: syxg50.dll, first differing sample].
    const std::vector<Timed> note{{4410, {0x90, 60, 100}}, {64410, {0x80, 60, 0}}};
    const auto base = play(20, note, 40000);
    for (std::uint32_t at : {30000u, 30077u, 31234u}) {
        auto evs = note;
        evs.push_back({at, {0xB0, 1, 127}});
        CHECK(first_difference(base, play(20, evs, 40000)) == 31745);
    }
    // After one earlier note the key takes voice 1 from the free-voice FIFO (not the lowest
    // free voice 0), whose turn is one tick later: 32129 [meas].
    auto pre = note;
    pre.insert(pre.begin(), {{2500, {0x90, 50, 100}}, {2600, {0x80, 50, 0}}});
    const auto pre_base = play(20, pre, 40000);
    pre.push_back({30000, {0xB0, 1, 127}});
    CHECK(first_difference(pre_base, play(20, pre, 40000)) == 32129);
    // The part vibrato rate is live (at the same turn): MW 64 from 10000, CC76 127
    // at 30000 → 31873; the part vibrato depth is latched at the note-on [meas].
    auto mw = note;
    mw.push_back({10000, {0xB0, 1, 64}});
    const auto mw_base = play(20, mw, 40000);
    auto rate = mw;
    rate.push_back({30000, {0xB0, 76, 127}});
    CHECK(first_difference(mw_base, play(20, rate, 40000)) == 31873);
    auto depth = mw;
    depth.push_back({30000, {0xB0, 77, 127}});
    CHECK(first_difference(mw_base, play(20, depth, 40000)) == SIZE_MAX);
    // Element vibrato (violin, delay 6 LFO ticks, fade 2/tick) with part vibrato rate 30: the
    // first change is at 10497, where the faded depth first reaches the chip [meas]. (Before,
    // updating the depth every block gave 0.45 waveform correlation with the DLL.)
    const std::vector<Timed> vnote{{4410, {0x90, 69, 100}}, {34410, {0x80, 69, 0}}};
    auto v30 = vnote;
    v30.push_back({3000, {0xB0, 76, 30}});
    CHECK(first_difference(play(40, vnote, 20000), play(40, v30, 20000)) == 10497);
}

TEST_CASE("syxg50_pitch_and_cutoff_at_tick") {
    HVYXG_NEED_TABLES();
    if (!have_bank()) return;
    // Pitch and cutoff registers are written at the note-on and by the 10-ms tick,
    // so a bend or CC74 in the block 19968..20095 takes effect with the tick before the block at
    // 20224 [meas: syxg50.dll, first differing sample 20225 for each].
    const std::vector<Timed> note{{4410, {0x90, 60, 100}}, {64410, {0x80, 60, 0}}};
    const auto base = play(20, note, 24000);
    for (std::uint32_t at : {20077u, 20300u}) {
        auto evs = note;
        evs.push_back({at, {0xE0, 0x60, 0x5D}});
        CHECK(first_difference(base, play(20, evs, 24000)) == 20225);
    }
    auto cutoff = note;
    cutoff.push_back({20077, {0xB0, 74, 20}});
    CHECK(first_difference(base, play(20, cutoff, 24000)) == 20225);
}

TEST_CASE("syxg50_voice_pool") {
    HVYXG_NEED_TABLES();
    if (!have_bank()) return;
    // Sustain pedal down on a releasing poly voice: its release rate becomes at most the D2
    // rate, written to the chip at the voice's next level round-robin turn. Piano key 60 at 4410 (voice 0), note-off at 20000; CC64 127 at 21000 →
    // first change 21121, at 21300 or 24000 → 24577 [meas: syxg50.dll, first differing sample].
    const std::vector<Timed> note{{4410, {0x90, 60, 100}}, {20000, {0x80, 60, 0}}};
    const auto base = play(0, note, 30000);
    for (const auto& [at, expect] : {std::pair{21000u, 21121u}, {21300u, 24577u}, {24000u, 24577u}}) {
        auto evs = note;
        evs.push_back({at, {0xB0, 64, 127}});
        CHECK(first_difference(base, play(0, evs, 30000)) == expect);
    }
    // The drum part's note assign after a reset: multi in XG mode (the part template), single
    // after a GS reset, so a second crash cymbal hit cuts the first only in GS.
    // Residual of (hit 1 + hit 2) − hit 1 − hit 2 after the second hit, relative to the pair:
    // −25.9 dB XG, −5.6 dB GS [meas: syxg50.dll, the same for hvyxg].
    const auto drums = [](bool gs, std::vector<std::uint32_t> hits) {
        test::Rig rig(test::bank(true));
        if (gs) rig.sysex({0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7});
        for (std::uint8_t c : {91, 93, 94}) rig.synth.midi(std::vector<std::uint8_t>{0xB9, c, 0}, 2000);
        for (std::uint32_t h : hits) {
            rig.synth.midi(std::vector<std::uint8_t>{0x99, 49, 100}, h);
            rig.synth.midi(std::vector<std::uint8_t>{0x89, 49, 0}, h + 500);
        }
        return rig.render(60000);
    };
    for (bool gs : {false, true}) {
        const auto a = drums(gs, {3000}), b = drums(gs, {9000}), ab = drums(gs, {3000, 9000});
        double r = 0, e = 0;
        for (std::size_t i = 2 * 9000; i < ab.size(); ++i) {
            const double d = static_cast<double>(ab[i]) - a[i] - b[i];
            r += d * d;
            e += static_cast<double>(ab[i]) * ab[i];
        }
        const double db = 10 * std::log10(r / e);
        CHECK(db == doctest::Approx(gs ? -5.6 : -25.9).epsilon(0.02));
    }
}

TEST_CASE("syxg50_tuning_and_commits") {
    HVYXG_NEED_TABLES();
    if (!have_bank()) return;
    // Pitch and cutoff are committed by the tick only while an envelope or portamento moves or
    // a part message has marked the voice. The XG master tune is taken at the
    // note-on: a change never reaches a sounding note, even when a later CC1
    // makes the tick commit; the GM2 master fine / coarse tuning is live.
    // Reed Organ key 60 at 4410 [meas: syxg50.dll, first differing sample].
    const std::vector<Timed> note{{4410, {0x90, 60, 100}}, {60000, {0x80, 60, 0}}};
    const std::vector<std::uint8_t> tune{0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x00, 0xF7};
    const auto base = play(20, note, 40000);
    auto mt = note;
    mt.push_back({20000, tune});
    CHECK(first_difference(base, play(20, mt, 40000)) == SIZE_MAX);
    auto mw = note;
    mw.push_back({30000, {0xB0, 1, 0}});
    auto mt_mw = mw;
    mt_mw.push_back({20000, tune});
    CHECK(first_difference(play(20, mw, 40000), play(20, mt_mw, 40000)) == SIZE_MAX);
    auto early = note;
    early.push_back({3000, tune});
    CHECK(first_difference(base, play(20, early, 40000)) == 4358);
    for (std::uint8_t b : {0x03, 0x04}) {
        auto gm2 = note;
        gm2.push_back({20000, {0xF0, 0x7F, 0x7F, 0x04, b, 0x00, static_cast<std::uint8_t>(b == 3 ? 0x50 : 0x42), 0xF7}});
        CHECK(first_difference(base, play(20, gm2, 40000)) == 20225);
    }
}

TEST_CASE("syxg50_kit_on_normal_part_uses_drum_setup_2") {
    HVYXG_NEED_TABLES();
    if (!have_bank()) return;
    // A normal part with an XG kit bank plays with drum setup 2:
    // its drum NRPNs edit that setup, which a part in mode 3 shares; a part in mode 2 does not
    // [meas: syxg50.dll cutoff register 0x262f instead of 0x27ff on channels 9, 2 and the
    // mode 3 part, unchanged on channel 10].
    const auto render = [](bool nrpn, std::uint8_t ch) {
        test::Rig rig(test::bank(true));
        for (std::uint8_t c = 0; c < 16; ++c)
            for (std::uint8_t cc : {91, 93, 94})
                rig.synth.midi(std::vector<std::uint8_t>{static_cast<std::uint8_t>(0xB0 | c), cc, 0}, 10);
        for (std::uint8_t c : {std::uint8_t{8}, std::uint8_t{1}}) {
            rig.synth.midi(std::vector<std::uint8_t>{static_cast<std::uint8_t>(0xB0 | c), 0, 127}, 20);
            rig.synth.midi(std::vector<std::uint8_t>{static_cast<std::uint8_t>(0xC0 | c), 30}, 20);
        }
        rig.synth.midi(std::vector<std::uint8_t>{0xF0, 0x43, 0x10, 0x4C, 0x08, 0x03, 0x07, 0x03, 0xF7}, 30);
        rig.synth.midi(std::vector<std::uint8_t>{0xB3, 0, 127}, 40);
        rig.synth.midi(std::vector<std::uint8_t>{0xC3, 30}, 40);
        if (nrpn)
            for (const auto& m : {std::vector<std::uint8_t>{0xB8, 99, 0x14}, std::vector<std::uint8_t>{0xB8, 98, 36},
                                  std::vector<std::uint8_t>{0xB8, 6, 30}})
                rig.synth.midi(m, 500);
        rig.synth.midi(std::vector<std::uint8_t>{static_cast<std::uint8_t>(0x90 | ch), 36, 100}, 4410);
        rig.synth.midi(std::vector<std::uint8_t>{static_cast<std::uint8_t>(0x80 | ch), 36, 0}, 9000);
        return rig.render(12000);
    };
    for (std::uint8_t ch : {8, 1, 3}) CHECK(first_difference(render(false, ch), render(true, ch)) != SIZE_MAX);
    CHECK(first_difference(render(false, 9), render(true, 9)) == SIZE_MAX);
}

TEST_CASE("syxg50_drum_attack_offset_clamp") {
    HVYXG_NEED_TABLES();
    // (own-wave drum AEG, XG): a kit attack rate plus the drum-setup
    // offset (30 rr 0D) of 94 or more gives max(94, the kit's rate), so a raised offset on a
    // key whose rate is already 94 or more changes nothing [meas: offsets 0x50 / 0x60 / 0x7F on
    // every key of kits 0, 25, 40, 127, −107 dB against the DLL; before: −7 dB].
    if (!have_bank()) return;
    const auto hit = [](std::uint8_t key, std::uint8_t offset) {
        Rig r;
        r.cc(9, 91, 0);
        r.xg({0x30, key, 0x0D, offset});
        r.note_on(9, key, 100);
        return r.render(22050);
    };
    for (const std::uint8_t key : {std::uint8_t{40}, std::uint8_t{20}, std::uint8_t{23}}) {
        const auto base = hit(key, 0x40);
        CHECK(rms(base) > 1e-4);
        CHECK(hit(key, 0x60) == base);
        CHECK(hit(key, 0x7F) == base);
        CHECK(hit(key, 0x20) != base);
    }
}

TEST_CASE("syxg50_part_kill_and_reset_state") {
    HVYXG_NEED_TABLES();
    // syxg50.dll: a reset and the part kill (mono / poly, part
    // mode, variation connection / insertion-part writes) set the portamento source to key 60
    // and clear the mono key list; CC120 only silences the voices [meas:
    // portamento after a reset, CC120, 02 01 5B and CC126; CC120 in mono mode: −113 … −120 dB].
    if (!have_bank()) return;
    {
        // The first note after the reset glides up from key 60 (261.6 Hz) toward key 72.
        Rig r;
        r.program(0, 16);
        r.cc(0, 91, 0);
        r.cc(0, 65, 127);
        r.cc(0, 5, 100);
        r.note_on(0, 72, 100);
        const auto s = r.render(4410);
        CHECK(frequency(s) < 300.0);
    }
    const auto retrigger = [](bool cc120) {
        Rig r;
        r.program(0, 16);
        r.cc(0, 91, 0);
        r.cc(0, 126, 1);  // mono
        r.note_on(0, 60, 100);
        r.render(4096);
        r.note_on(0, 64, 100);
        r.render(4096);
        if (cc120) r.cc(0, 120, 0);
        else r.xg({0x08, 0x00, 0x05, 0x00});  // mono again: the part kill
        r.render(22050);
        r.note_off(0, 64);  // 60 is still held: CC120 kept the list, so it plays again
        return rms(r.render(8820));
    };
    CHECK(retrigger(true) > 1e-3);
    CHECK(retrigger(false) < 1e-6);
}

TEST_CASE("syxg50_mono_cuts_the_previous_note_only") {
    HVYXG_NEED_TABLES();
    // a mono note-on cuts (rate 0x5a) only the previous mono note's
    // voices: its keyed voices while keys are held or the sustain pedal is down, its release
    // tail when no key is held [meas: legato, detached, sustain and mixed sequences on strings
    // and piano, −110 … −122 dB; before: −3 … −17 dB on the detached / mixed ones].
    if (!have_bank()) return;
    const auto run = [](bool mono) {
        Rig r;
        r.program(0, 48);  // strings: a long release
        r.cc(0, 91, 0);
        if (mono) r.cc(0, 126, 1);
        r.note_on(0, 60, 100);
        r.render(8192);
        r.note_off(0, 60);
        r.render(2048);
        r.note_on(0, 72, 100);
        return r.render(8192);
    };
    const auto mono = run(true), poly = run(false);
    // The previous note's tail is cut in mono mode, so less sound than in poly mode.
    CHECK(rms(mono) < 0.95 * rms(poly));
}

TEST_CASE("syxg50_eg_time_changes_reach_sounding_voices") {
    HVYXG_NEED_TABLES();
    // (CC72 / CC73, 08 nn 1C / 1A, the EG NRPNs): the
    // release rate of the voices already sounding is computed again, also during the release,
    // and a filter EG in its attack / release stage takes the new rate [meas: strings and saw
    // lead, CC72 held / in the tail, 08 nn 1C, CC73 in the attack: −112 … −129 dB; before:
    // −16 … −40 dB].
    if (!have_bank()) return;
    const auto tail = [](int when) {  // 0: no change, 1: before the note-off, 2: after it
        Rig r;
        r.program(0, 48);
        r.cc(0, 91, 0);
        r.note_on(0, 60, 100);
        r.render(22050);
        if (when == 1) r.cc(0, 72, 20);
        r.render(4410);
        r.note_off(0, 60);
        if (when == 2) r.xg({0x08, 0x00, 0x1C, 20});
        r.render(8820);
        return rms(r.render(8820));
    };
    const double none = tail(0);
    CHECK(tail(1) < 0.5 * none);
    CHECK(tail(2) < 0.5 * none);
}
