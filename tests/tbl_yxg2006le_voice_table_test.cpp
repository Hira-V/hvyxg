// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// S-YXG2006LE voice table parser and sample decoding.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "syxg2006le/tbl/decrypt.hpp"
#include "syxg2006le/tbl/voice_table.hpp"
#include "test_support.hpp"

using namespace hvyxg::syxg2006le::tbl;

namespace {

std::vector<std::uint8_t> slurp(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

/// Second-difference energy / signal energy (log10): smooth audio is well below 0.
double roughness(const std::vector<float>& x) {
    double d = 0, e = 0;
    for (std::size_t i = 2; i < x.size(); ++i) {
        const double s = x[i] - 2.0 * x[i - 1] + x[i - 2];
        d += s * s;
        e += static_cast<double>(x[i]) * x[i];
    }
    return e > 0 ? std::log10(d / e) : 0.0;
}

} // namespace

TEST_CASE("yxg2006le_voice_table_rejects_other_files") {
    std::vector<std::uint8_t> junk(0x100, 0);
    CHECK(VoiceTable{junk}.error() != nullptr);
}

TEST_CASE("yxg2006le_sample_header_fields") {
    const std::array<std::uint8_t, 20> raw = {0x80 | 60, 0x81, 0x3A, 0x7F, 2, 0x03, 0xC0, 0x1F,
                                              0x10, 0, 0, 0,  // loop start 0x10 × 4
                                              5, 0, 0, 0,     // pre-loop
                                              9, 0, 0, 0};    // loop length
    const auto h = SampleHeader::from_bytes(raw);
    CHECK(h.root_key == 60);
    CHECK(h.fixed_pitch);
    CHECK(h.coarse == 1); // 0x81 & 0x7F
    CHECK(h.fine == 58);
    CHECK(h.tune_cents() == 100 + 45); // 58 · 25 / 32 = 45.3 → 45
    CHECK(h.loop_fine == -1);
    CHECK(h.level == 2);
    CHECK(h.format == SampleFormat::Pcm16);
    CHECK(h.reverse_loop);
    CHECK(h.flag_b6);
    CHECK(h.predictor == 0x1F);
    CHECK(h.loop_start_byte == 0x40);
    CHECK(h.pre_loop == 5);
    CHECK(h.loop_length == 9);
}

TEST_CASE("yxg2006le_engine_conversions") {
    HVYXG_NEED_TABLES();
    // Rate table: the filter-EG step measured on the harp (rate 35) and the PEG glides (18, 22).
    CHECK(eg_increment(test::tables()->le, 35, 44100.0) == 176);
    CHECK(eg_increment(test::tables()->le, 18, 44100.0) == 40);
    CHECK(eg_increment(test::tables()->le, 22, 44100.0) == 56);
    CHECK(eg_increment(test::tables()->le, 127, 44100.0) == 520223);
    // Level units → gain (piecewise-linear 2^-x): exact at multiples of 16 units.
    CHECK(attenuation_gain(0) == doctest::Approx(1.0).epsilon(1e-4));
    CHECK(attenuation_gain(16) == doctest::Approx(0.5).epsilon(1e-4));
    CHECK(attenuation_gain(24) == doctest::Approx(0.375).epsilon(1e-4));
    // Pan: centre = 8 + 8 + 8 = 24 units on both sides, hard left mutes the right output.
    CHECK(pan_attenuation(test::tables()->le, 64) == 8);
    CHECK(pan_attenuation(test::tables()->le, 1) == 0);
    CHECK(pan_attenuation(test::tables()->le, 127) == 255);
    // LFO: speed index 75 → v 509 → 5.131 Hz; measured 5.1306 / 10.012 / 1.2622 Hz.
    CHECK(lfo_speed_value(test::tables()->le, 75) == 509);
    CHECK(lfo_speed_value(test::tables()->le, 200) == 1016);
    CHECK(lfo_increment(509, 44100.0) == 7808);
    CHECK(lfo_hz(509, 44100.0) == doctest::Approx(5.1310).epsilon(1e-4));
    CHECK(lfo_hz(1000, 44100.0) == doctest::Approx(10.0096).epsilon(1e-4));
    CHECK(lfo_hz(130, 44100.0) == doctest::Approx(1.2617).epsilon(1e-4));
    CHECK(lfo_hz(509, 48000.0) == doctest::Approx(5.13).epsilon(1e-2));
    // Filter: 14550 cents at 44.1 kHz → f = 288/4096 (measured resonant voice), floor 9600 cents.
    CHECK(filter_log_cutoff(14550, 44100.0) == 794624);
    CHECK(filter_coefficient(filter_log_cutoff(14550, 44100.0)) == doctest::Approx(288.0 / 4096));
    CHECK(filter_coefficient(filter_log_cutoff(9000, 44100.0)) == doctest::Approx(1.0 / 256));
    CHECK(filter_coefficient(filter_log_cutoff(19200, 44100.0)) == doctest::Approx(1.0));
    CHECK(filter_log_cutoff(13800, 48000.0) == 13800 * 4096 / 75 - 8012);
    CHECK(filter_damping(16) == doctest::Approx(1.5));
    CHECK(filter_damping(32) == doctest::Approx(1.0));
    CHECK(filter_damping(96) == doctest::Approx(0.25));
    CHECK(filter_damping(140) == doctest::Approx(52.0 / 512));
}

TEST_CASE("yxg2006le_group_key_tracking") {
    HVYXG_NEED_TABLES();
    Group g;
    g.center_key = 60;
    g.key_track = 0;
    CHECK(group_key(test::tables()->le, g, 72).first == 72);
    g.key_track = 5; // fixed
    CHECK(group_key(test::tables()->le, g, 72).first == 60);
    g.key_track = 1; // 50 %: 12 keys above → 6 above
    CHECK(group_key(test::tables()->le, g, 72) == std::pair<std::uint8_t, std::uint8_t>{66, 0});
    CHECK(group_key(test::tables()->le, g, 61) == std::pair<std::uint8_t, std::uint8_t>{60, 50}); // 0.5 key
    CHECK(group_key(test::tables()->le, g, 59) == std::pair<std::uint8_t, std::uint8_t>{59, 50}); // -0.5 → 59 + 50 c
    g.key_track = 0;
    g.transpose = -12;
    CHECK(group_key(test::tables()->le, g, 5).first == 0); // clamped
}

TEST_CASE("yxg2006le_decode_sample_indexing") {
    // 16-bit: sample i lives at loop_start + 2i, including negative (pre-loop) indices.
    std::vector<std::uint8_t> wave(64, 0);
    for (int i = -2; i < 3; ++i) {
        const auto v = static_cast<std::int16_t>(1000 * (i + 10));
        wave[static_cast<std::size_t>(32 + 2 * i)] = static_cast<std::uint8_t>(v & 0xFF);
        wave[static_cast<std::size_t>(32 + 2 * i + 1)] = static_cast<std::uint8_t>(v >> 8);
    }
    SampleHeader h;
    h.format = SampleFormat::Pcm16;
    h.loop_start_byte = 32;
    h.pre_loop = 2;
    h.loop_length = 3;
    const auto s = decode_sample(wave, h);
    REQUIRE(s.has_value());
    REQUIRE(s->size() == 5);
    for (int k = 0; k < 5; ++k) CHECK((*s)[static_cast<std::size_t>(k)] == doctest::Approx(1000 * (k + 8) / 32768.0));

    // Past the end of the wave table → nullopt
    h.loop_length = 100;
    CHECK_FALSE(decode_sample(wave, h).has_value());
}

TEST_CASE("yxg2006le_real_voice_table") {
    HVYXG_NEED_TABLES();
    // Set HVYXG_2006LE_BIN (SXGBNW6L.TBL) and HVYXG_2006LE_DATA (SXGDAT6L.TBL) to run.
    const auto files = test::bank_files(false);
    if (!files) {
        MESSAGE("HVYXG_2006LE_BIN / HVYXG_2006LE_DATA not set: skipped");
        return;
    }
    const auto file = slurp(files->bin.c_str());
    const VoiceTable vt(file);
    CHECK(vt.voice_count() == 754);
    CHECK(vt.element_count() == 6048);
    CHECK(vt.sample_count() == 1989);
    CHECK(vt.bank_id(0, 0) == 0);
    CHECK(vt.voice_index(0, 0) == 402); // GM program 0
    CHECK(vt.voice_index(0, 2) == 180);
    CHECK_FALSE(vt.bank_id(0, 2).has_value()); // unmapped bank LSB
    CHECK(vt.drum_entry(0, 36).has_value());

    // Every voice and element record parses (records tile their segments exactly).
    for (std::size_t i = 0; i < vt.voice_count(); ++i) CHECK_NOTHROW(vt.voice(i));
    for (std::size_t i = 0; i < vt.element_count(); ++i) CHECK_NOTHROW(vt.element(i));

    // GM piano: one group of 16 key splits; middle C (60) → zone 59-62 → sample with root 60.
    const Voice piano = vt.voice(402);
    REQUIRE(piano.groups.size() == 1);
    CHECK(piano.groups[0].zones.size() == 16);
    const auto hits = find_zones(test::tables()->le, piano, 60, 100);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].zone->key_low == 59);
    CHECK(hits[0].zone->key_high == 62);
    const Element el = vt.element(hits[0].zone->element);
    CHECK(el.sample == 1238);
    CHECK(vt.sample_header(el.sample).root_key == 60);
    const SampleHeader sh = vt.sample_header(el.sample);
    CHECK(sh.coarse == -11);
    CHECK(sh.fine == 58);
    CHECK(sh.tune_cents() == -1055); // measured: piano note 60 renders at 261.39 Hz
    CHECK(sh.level == 4);
    CHECK(piano.damper_mode == 3);
    CHECK_FALSE(piano.is_drum());
    CHECK(piano.level == 13);

    // Element 3713 parameter blocks.
    const AmpEgBlock amp = el.amp();
    CHECK(amp.attack_rate == 0x80); // instant attack
    CHECK(amp.decay1_rate == 44);
    CHECK(amp.decay2_rate == 36);
    CHECK(amp.decay3_rate == 0);    // hold level_3
    CHECK(amp.release_rate == 64);  // measured ≈ 129 dB/s
    CHECK(amp.damper_release_rate == 37);
    CHECK(amp.level_1 == 0);
    CHECK(amp.level_2 == 15);
    CHECK(amp.level_3 == 255);
    CHECK(amp.level_key_curve == 0x316);
    CHECK(amp.level_velocity_curve == 0x26);
    CHECK(curve_value(vt.seg5_row(0x26)[63]) == -16); // velocity 64: 32 units ≈ 12 dB
    const FilterBlock flt = el.filter();
    CHECK(flt.filter[0].type == 0);
    CHECK(flt.filter[0].cutoff == 18740);
    CHECK(flt.filter[0].resonance == 0x20);
    CHECK(flt.rate[0] == 16);
    CHECK(flt.level[1] == -4143);
    CHECK_FALSE(el.pitch().active());
    CHECK(el.pitch().coarse == 0);
    CHECK(el.lfo().key_sync);

    // Element 0: the shared default blocks.
    const Element e0 = vt.element(0);
    CHECK(e0.lfo().wave == 6);
    CHECK(e0.lfo().speed == 75);
    CHECK(e0.amp().attack_rate == 0x7B);
    CHECK(e0.filter().filter[0].cutoff == 17006);
    CHECK(e0.filter().level[0] == 2100);
    CHECK(e0.pitch().rate_key_center == 60);
    for (const auto& b : el.blocks) CHECK_FALSE(b.empty());
    CHECK_FALSE(vt.drum_entry(0, 12).has_value());

    // Measured filter voice (MSB 0 / LSB 19 / program 39 = voice 162): note 60 → element 1347,
    // LPF 13425 cents + key curve 0x0d (15 at key 60, scale 16 → +1125) = 14550, resonance 96.
    CHECK(vt.voice_index(vt.bank_id(0, 19).value(), 39) == 162);
    const Element bass = vt.element(1347);
    const FilterEntry lpf = bass.filter().filter[0];
    CHECK(lpf.type == 0);
    CHECK(lpf.cutoff == 13425);
    CHECK(lpf.resonance == 96);
    CHECK(lpf.cutoff_key_curve == 0x0d);
    CHECK(curve_value(vt.seg5_row(0x0d)[60]) * ((lpf.scale_a >> 4) + 1) * 75 / 16 == 1125);
    CHECK_FALSE(bass.filter().eg_active());
    CHECK(filter_coefficient(filter_log_cutoff(13425 + 1125, 44100.0)) == doctest::Approx(288.0 / 4096));
    CHECK(filter_damping(lpf.resonance) == doctest::Approx(0.25));
    // Its LFO: speed index 75 → 5.131 Hz (measured on the organ, same block layout).
    CHECK(lfo_hz(lfo_speed_value(test::tables()->le, bass.lfo().speed), 44100.0) == doctest::Approx(5.131).epsilon(1e-3));

    auto wave = slurp(files->data.c_str());
    decrypt(wave);
    // Decisive addressing check: pre-loop and loop parts are contiguous, so the step across
    // the loop start is no bigger than the steps around it. Attack transients (pre-loop) are
    // often noise-like, so smoothness is only expected of the sustained loop parts.
    int decoded = 0, joins = 0, continuous = 0, loops = 0, smooth_loops = 0;
    for (std::size_t i = 0; i < vt.sample_count(); ++i) {
        const auto h = vt.sample_header(i);
        CHECK(h.format == SampleFormat::Pcm16);
        const auto s = decode_sample(wave, h);
        if (!s) continue;
        ++decoded;
        if (h.pre_loop >= 4 && h.loop_length >= 4) {
            const std::size_t z = h.pre_loop; // index of sample 0 (loop start)
            const float step = std::fabs((*s)[z] - (*s)[z - 1]);
            const float around = (std::fabs((*s)[z - 1] - (*s)[z - 2]) + std::fabs((*s)[z + 1] - (*s)[z])) / 2;
            ++joins;
            continuous += step <= 5 * around + 1e-4f;
        }
        if (h.loop_length >= 256) {
            ++loops;
            smooth_loops += roughness({s->begin() + h.pre_loop, s->end()}) < -1.0;
        }
    }
    MESSAGE("decoded " << decoded << ", continuous at loop start " << continuous << "/" << joins << ", smooth loops "
                       << smooth_loops << "/" << loops);
    CHECK(decoded > 1800);
    CHECK(continuous > joins * 95 / 100);
    CHECK(smooth_loops > loops * 7 / 10);
}
