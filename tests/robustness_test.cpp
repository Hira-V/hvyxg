// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Robustness: malformed banks, SMF and ROM images must fail cleanly (or play safely), never
// crash or read out of bounds. Each case is a bug the fuzzers found (fuzz/README.md); the
// banks are the synthetic ones of fuzz/fuzz_support.cpp, so these run without bank files.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "fuzz_support.hpp"
#include "hvyxg/bank.hpp"
#include "lib/midifile.hpp"
#include "hvyxg/synth.hpp"
#include "syxg50/mu_rom/decode.hpp"
#include "syxg50/mu_rom/py.hpp"
#include "test_support.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;

std::uint32_t u32le(const Bytes& b, std::size_t at) {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = v << 8 | b[at + static_cast<std::size_t>(i)];
    return v;
}
void put32le(Bytes& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
}

/// Render `frames` after `msgs`; returns the output.
std::vector<float> play(const std::shared_ptr<const hvyxg::Bank>& bank, const std::vector<Bytes>& msgs, std::size_t frames) {
    hvyxg::Synth::Options o;
    o.tables = test::tables();
    o.bank = bank;
    hvyxg::Synth s(o);
    for (const Bytes& m : msgs) s.midi(m);
    std::vector<float> out(2 * frames);
    s.render(out.data(), frames);
    return out;
}

double rms(const std::vector<float>& v) {
    double e = 0;
    for (const float x : v) e += static_cast<double>(x) * x;
    return std::sqrt(e / static_cast<double>(v.size()));
}

} // namespace

TEST_CASE("synthetic banks load and play on every engine") {
    HVYXG_NEED_TABLES();
    for (int which = 0; which < 3; ++which) {
        CAPTURE(which);
        const auto bank = fuzz::bank(which);
        REQUIRE(bank);
        const auto out = play(bank, {{0xC0, 0}, {0x90, 60, 100}, {0x99, 36, 100}}, 4096);
        CHECK(rms(out) > 0.01);
    }
}

TEST_CASE("SMF: an empty escaped SysEx (F7 00) carries no message") {
    // Found by fuzz_smf in hvyxg's former reader: it became an empty message, which the renderer
    // indexed. The tools read with midifile now (tools/render/lib): the escape is skipped.
    const Bytes smf = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96,  //
                       'M', 'T', 'r', 'k', 0, 0, 0, 11,                     //
                       0x00, 0xF7, 0x00,                                    // empty escape
                       0x00, 0x90, 60, 100,                                 //
                       0x00, 0xFF, 0x2F, 0x00};
    std::int32_t err = 0;
    const std::unique_ptr<midifile::Song> song(midifile::open(smf.data(), static_cast<std::uint32_t>(smf.size()), &err));
    REQUIRE(song);
    REQUIRE(song->count() == 1);
    CHECK(song->event(0).ump[0] == 0x20903C64u);
}

TEST_CASE("SMF: SMPTE division with zero ticks per frame is refused") {
    const Bytes smf = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0xE7, 0x00,  //
                       'M', 'T', 'r', 'k', 0, 0, 0, 8, 0x00, 0x90, 60, 100, 0x00, 0xFF, 0x2F, 0x00};
    std::int32_t err = 0;
    CHECK_FALSE(midifile::open(smf.data(), static_cast<std::uint32_t>(smf.size()), &err));
    CHECK(err == midifile::BAD);
}

TEST_CASE("S-YXG2006LE: record offsets outside their segment are a BankError") {
    // Found by fuzz_syxg2006le_bank: the seg2 / seg3 offset tables were not checked, so a
    // record span could run past the file.
    for (const std::size_t seg : {2u, 3u}) {
        CAPTURE(seg);
        for (const std::uint32_t bad : {0xFFFFFF00u, 0x40u}) {
            Bytes bin = fuzz::syxg2006le_bin();
            const std::uint32_t table = u32le(bin, 0x18 + 4 * seg);
            put32le(bin, table + 4, bad);  // the second record's offset
            CHECK_THROWS_AS(hvyxg::load_bank(bin, fuzz::syxg2006le_wave(), hvyxg::Model::Auto, test::bank_options()), hvyxg::BankError);
        }
    }
}

TEST_CASE("S-YXG2006LE: a group pitch curve row the bank lacks plays as no curve") {
    HVYXG_NEED_TABLES();
    // Found by fuzz_syxg2006le_bank: the row was read with seg5_row, which threw
    // std::out_of_range out of Synth::midi at the note-on.
    const auto with_curve = [](std::uint16_t row) {
        Bytes bin = fuzz::syxg2006le_bin();
        const std::uint32_t voice0 = u32le(bin, u32le(bin, 0x18 + 4 * 2));
        bin[voice0 + 16 + 8] = static_cast<std::uint8_t>(row);
        bin[voice0 + 16 + 9] = static_cast<std::uint8_t>(row >> 8);
        return hvyxg::load_bank(bin, fuzz::syxg2006le_wave(), hvyxg::Model::Auto, test::bank_options());
    };
    const std::vector<Bytes> notes = {{0xC0, 0}, {0x90, 40, 100}, {0x90, 70, 100}};
    const auto none = play(with_curve(0xFFFF), notes, 2048);
    std::vector<float> missing;
    CHECK_NOTHROW(missing = play(with_curve(0x7FFF), notes, 2048));
    CHECK(missing == none);
    CHECK(play(with_curve(1), notes, 2048) != none);  // a real row does act
}

TEST_CASE("MU ROM decoder: images too short throw instead of reading past the end") {
    // Found by fuzz_mu_rom: mu2000_scaling read the first voice byte before any bounds check.
    using namespace hvyxg::syxg50::mu_rom;
    Bytes tiny(8, 0xFF);
    CHECK_THROWS_AS(mu2000_scaling(tiny), ConvertError);
}

TEST_CASE("S-YXG50 EG: a release from the target level (zero slope) is defined") {
    HVYXG_NEED_TABLES();
    // Found by fuzz_midi on the SXG-Create-NF MU1000 bank and by the test suite under UBSan:
    // a zero EG slope made the step a float to int conversion of -inf (undefined behaviour;
    // only a sanitizer build sees it). The fuzz input, replayed on that bank.
    const auto f = test::asset_pair("HVYXG_MU_BIN", "tbl/mu1000/SXGMU1KX.TBL", "HVYXG_MU_DATA", "tbl/mu1000/SXGMU1KX.UPCM");
    if (!f) {
        MESSAGE("SXG-Create-NF MU1000 bank not found: skipped");
        return;
    }
    hvyxg::BankOptions bo;
    bo.enable_mu = true;
    const auto bank = hvyxg::load_bank(f->bin, f->data, hvyxg::Model::Auto, test::bank_options(bo));
    static const std::uint8_t kInput[] = {
        0x04, 0x35, 0x07, 0xB6, 0x5D, 0x5D, 0x5D, 0x5D, 0x5D, 0x5D, 0x5D, 0x5D, 0x5D, 0x5D, 0x5D, 0x5D,
        0x5D, 0x5D, 0x5D, 0x5D, 0x5D, 0x5D, 0x5D, 0x5D, 0x5D, 0x5D, 0x5D, 0x5D, 0x5D, 0x5D, 0x5D, 0x5D,
        0xC0, 0x64, 0x01, 0x99, 0x24, 0x01, 0x00, 0x03, 0x5D, 0x5D, 0x5D, 0x64, 0x24, 0x2F, 0x07, 0x24,
        0x80, 0xF6, 0xF6, 0xF6, 0xF6, 0xF6, 0xF7, 0xFF, 0xFF, 0xFF, 0x99, 0x24, 0x01, 0x00, 0x03, 0x64,
        0x24, 0x64, 0x27, 0x00, 0x2F, 0x02, 0x34, 0x3C, 0x14, 0x00, 0x08, 0x00, 0x03, 0x13, 0x14, 0x00
    };
    fuzz::Input in(kInput, sizeof kInput);
    const std::uint8_t config = in.u8();
    (void)in.u8();  // bank selector of fuzz_midi: the MU bank
    hvyxg::Synth synth(fuzz::options(config, bank));
    CHECK_NOTHROW(fuzz::play(synth, in));
}

TEST_CASE("polyphony above kMaxPolyphony is an error") {
    HVYXG_NEED_TABLES();
    for (int which = 0; which < 3; ++which) {
        CAPTURE(which);
        hvyxg::Synth::Options o;
        o.tables = test::tables();
        o.bank = fuzz::bank(which);
        o.max_polyphony = hvyxg::Synth::kMaxPolyphony;
        hvyxg::Synth s(o);
        std::vector<float> buf(2 * 256);
        s.render(buf.data(), 256);
        o.max_polyphony = hvyxg::Synth::kMaxPolyphony + 1;
        CHECK_THROWS_AS(hvyxg::Synth{o}, std::invalid_argument);
    }
}

TEST_CASE("Room reverbs with a long time in the smallest room stay inside the comb table") {
    HVYXG_NEED_TABLES();
    // Found by fuzz_midi: White Room / Tunnel / Basement with reverb time >= 53 and dimension 0
    // computed comb gain rows past the 208-row table (up to 235 within the XG ranges) and read
    // past it (only a sanitizer build sees it). The rows are now held at the last one.
    for (const std::uint8_t type : {0x10, 0x11, 0x13}) {
        CAPTURE(type);
        const auto xg = [](std::uint8_t lo, std::uint8_t v) { return Bytes{0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, lo, v, 0xF7}; };
        std::vector<Bytes> msgs = {Bytes{0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x00, type, 0x00, 0xF7}, xg(0x02, 69)};
        for (const std::uint8_t p : {0x07, 0x08, 0x09, 0x0A}) msgs.push_back(xg(p, 0));  // width, height, depth, wall
        msgs.push_back({0xB0, 91, 127});
        msgs.push_back({0xC0, 0});
        msgs.push_back({0x90, 60, 127});
        const auto a = play(fuzz::bank(0), msgs, 8192);
        CHECK(std::all_of(a.begin(), a.end(), [](float x) { return std::isfinite(x); }));
        CHECK(play(fuzz::bank(0), msgs, 8192) == a);
    }
}
