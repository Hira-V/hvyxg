// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Bank tables embedded in a plugin DLL (hvyxg/plugin_tables.hpp): a synthetic PE image built
// here, and, when present, an SXG-Create-NF `--embed` plugin (HVYXG_MU_PLUGIN, else
// `tbl/mu1000-embed/syxg1000.dll` in the asset tree) against the same tables written as files
// (HVYXG_MU_BIN / HVYXG_MU_DATA).
#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "bank_io.hpp"
#include "hvyxg/plugin_tables.hpp"
#include "hvyxg/synth.hpp"
#include "hvyxg/types.hpp"
#include "test_support.hpp"

using namespace hvyxg;

namespace {

using Bytes = std::vector<std::uint8_t>;

void put16(Bytes& b, std::size_t o, std::uint32_t v) {
    if (b.size() < o + 2) b.resize(o + 2);
    b[o] = static_cast<std::uint8_t>(v);
    b[o + 1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(Bytes& b, std::size_t o, std::uint32_t v) {
    put16(b, o, v & 0xFFFF);
    put16(b, o + 2, v >> 16);
}

/// A minimal S-YXG50 bin table header (no segments) naming `wave` at 0x10.
Bytes fake_bin(const std::string& wave, std::uint8_t fill) {
    Bytes b(0x64 + 32, fill);
    std::memset(b.data(), 0, 0x64);
    std::memcpy(b.data(), "MU TEST", 7);
    std::memcpy(b.data() + 0x10, wave.data(), wave.size());
    b[0x1F] = 0x01;  // plain wave
    return b;
}

/// A PE32 image with one .rsrc section: RT_RCDATA resources `rc` (name, data) and, when
/// `strings` is not empty, one RT_STRING block holding them.
Bytes make_pe(const std::vector<std::pair<std::string, Bytes>>& rc, const std::vector<std::string>& strings = {}) {
    constexpr std::uint32_t kRva = 0x1000, kRaw = 0x200;
    Bytes r;  // the resource section, offsets relative to its start
    const bool str = !strings.empty();
    const std::size_t ntypes = str ? 2 : 1;
    // Root directory → type entries (6 RT_STRING, 10 RT_RCDATA), sorted by id.
    auto dir = [&](std::size_t at, std::size_t named, std::size_t ids) {
        if (r.size() < at + 16) r.resize(at + 16);
        put16(r, at + 12, static_cast<std::uint32_t>(named));
        put16(r, at + 14, static_cast<std::uint32_t>(ids));
    };
    std::size_t at = 0;
    dir(at, 0, ntypes);
    const std::size_t root_entries = 16;
    std::size_t next = root_entries + 8 * ntypes;
    // RT_STRING: type dir → id 1 → language → data.
    std::size_t str_type = 0, rc_type = 0;
    if (str) {
        str_type = next;
        next += 16 + 8;
        put32(r, root_entries, 6);
        put32(r, root_entries + 4, 0x80000000u | static_cast<std::uint32_t>(str_type));
    }
    rc_type = next;
    next += 16 + 8 * rc.size();
    put32(r, root_entries + 8 * (ntypes - 1), 10);
    put32(r, root_entries + 8 * (ntypes - 1) + 4, 0x80000000u | static_cast<std::uint32_t>(rc_type));
    // Language directories and data entries.
    std::vector<std::size_t> lang(rc.size() + 1), data(rc.size() + 1);
    for (std::size_t i = 0; i < rc.size() + (str ? 1 : 0); ++i) {
        lang[i] = next;
        next += 16 + 8;
        data[i] = next;
        next += 16;
    }
    // Names (counted UTF-16).
    std::vector<std::size_t> name(rc.size());
    for (std::size_t i = 0; i < rc.size(); ++i) {
        name[i] = next;
        next += 2 + 2 * rc[i].first.size();
    }
    // Payloads.
    Bytes strblock;
    if (str) {
        for (int k = 0; k < 16; ++k) {
            const std::string s = k < static_cast<int>(strings.size()) ? strings[static_cast<std::size_t>(k)] : "";
            put16(strblock, strblock.size(), static_cast<std::uint32_t>(s.size()));
            for (const char c : s) put16(strblock, strblock.size(), static_cast<std::uint8_t>(c));
        }
    }
    std::vector<std::size_t> payload(rc.size() + 1);
    for (std::size_t i = 0; i < rc.size() + (str ? 1 : 0); ++i) {
        next = (next + 7) & ~std::size_t{7};
        payload[i] = next;
        next += i < rc.size() ? rc[i].second.size() : strblock.size();
    }
    r.resize(next);
    dir(rc_type, rc.size(), 0);
    for (std::size_t i = 0; i < rc.size(); ++i) {
        put32(r, rc_type + 16 + 8 * i, 0x80000000u | static_cast<std::uint32_t>(name[i]));
        put32(r, rc_type + 16 + 8 * i + 4, 0x80000000u | static_cast<std::uint32_t>(lang[i]));
        put16(r, name[i], static_cast<std::uint32_t>(rc[i].first.size()));
        for (std::size_t k = 0; k < rc[i].first.size(); ++k)
            put16(r, name[i] + 2 + 2 * k, static_cast<std::uint8_t>(rc[i].first[k]));
        std::memcpy(r.data() + payload[i], rc[i].second.data(), rc[i].second.size());
    }
    if (str) {
        dir(str_type, 0, 1);
        put32(r, str_type + 16, 1);
        put32(r, str_type + 20, 0x80000000u | static_cast<std::uint32_t>(lang[rc.size()]));
        std::memcpy(r.data() + payload[rc.size()], strblock.data(), strblock.size());
    }
    for (std::size_t i = 0; i < rc.size() + (str ? 1 : 0); ++i) {
        dir(lang[i], 0, 1);
        put32(r, lang[i] + 16, 0x409);
        put32(r, lang[i] + 20, static_cast<std::uint32_t>(data[i]));
        put32(r, data[i], kRva + static_cast<std::uint32_t>(payload[i]));
        put32(r, data[i] + 4, static_cast<std::uint32_t>(i < rc.size() ? rc[i].second.size() : strblock.size()));
    }

    Bytes pe(kRaw, 0);
    pe[0] = 'M';
    pe[1] = 'Z';
    put32(pe, 0x3c, 0x40);
    put32(pe, 0x40, 0x00004550);
    put16(pe, 0x44, 0x14c);
    put16(pe, 0x46, 1);     // sections
    put16(pe, 0x54, 224);   // optional header size
    const std::size_t opt = 0x58;
    put16(pe, opt, 0x10b);
    put32(pe, opt + 92, 16);
    put32(pe, opt + 96 + 16, kRva);
    put32(pe, opt + 96 + 20, static_cast<std::uint32_t>(r.size()));
    const std::size_t sec = opt + 224;
    std::memcpy(pe.data() + sec, ".rsrc", 5);
    put32(pe, sec + 8, static_cast<std::uint32_t>(r.size()));
    put32(pe, sec + 12, kRva);
    put32(pe, sec + 16, static_cast<std::uint32_t>(r.size()));
    put32(pe, sec + 20, kRaw);
    pe.resize(kRaw);
    pe.insert(pe.end(), r.begin(), r.end());
    return pe;
}

} // namespace

TEST_CASE("plugin tables: RCDATA resources of a synthetic PE") {
    const Bytes bin = fake_bin("SXGTEST.UPCM", 0xAB);
    const Bytes wave(1000, 0x5A);
    const Bytes pe = make_pe({{"SXGTEST.TBL", bin}, {"SXGTEST.UPCM", wave}, {"OTHER", Bytes{1, 2, 3}}});
    const auto res = pe_rcdata(pe);
    REQUIRE(res.size() == 3);
    CHECK(res[0].name == "SXGTEST.TBL");
    CHECK(res[2].data == Bytes{1, 2, 3});
    const PluginTables t = read_plugin_tables(pe);
    CHECK(t.bin_name == "SXGTEST.TBL");
    CHECK(t.wave_name == "SXGTEST.UPCM");
    CHECK(t.bin == bin);
    CHECK(t.wave == wave);
}

TEST_CASE("plugin tables: the string table picks among several tables; names are case-blind") {
    const Bytes bin41 = fake_bin("SXGWAVE4.TBL", 1), bin21 = fake_bin("sxgwave2.tbl", 2);
    const Bytes w4(64, 4), w2(64, 2);
    const Bytes pe = make_pe({{"SXGBIN21.TBL", bin21}, {"SXGBIN41.TBL", bin41}, {"SXGWAVE2.TBL", w2}, {"SXGWAVE4.TBL", w4}},
                             {"S-YXG50", "sxgbin41.tbl"});
    CHECK(pe_strings(pe) == std::vector<std::string>{"S-YXG50", "sxgbin41.tbl"});
    const PluginTables t = read_plugin_tables(pe);
    CHECK(t.bin_name == "SXGBIN41.TBL");
    CHECK(t.wave == w4);
    // Without a string table the first table wins; its wave is matched case-blind.
    const PluginTables u = read_plugin_tables(make_pe({{"SXGBIN21.TBL", bin21}, {"SXGWAVE2.TBL", w2}}));
    CHECK(u.wave == w2);
}

TEST_CASE("plugin tables: malformed input is refused") {
    CHECK_THROWS_AS(read_plugin_tables(Bytes{'M', 'Z'}), BankError);
    CHECK_THROWS_AS(read_plugin_tables(Bytes(4096, 0)), BankError);
    // No bank table, or a missing wave.
    CHECK_THROWS_AS(read_plugin_tables(make_pe({{"X", Bytes{1}}})), BankError);
    CHECK_THROWS_AS(read_plugin_tables(make_pe({{"A.TBL", fake_bin("B.UPCM", 0)}})), BankError);
    // Truncated: every prefix of a valid image throws or reads cleanly, never out of bounds.
    const Bytes pe = make_pe({{"SXGTEST.TBL", fake_bin("SXGTEST.UPCM", 3)}, {"SXGTEST.UPCM", Bytes(100, 7)}});
    for (std::size_t n = 0; n < pe.size(); n += 7) {
        const Bytes cut(pe.begin(), pe.begin() + static_cast<std::ptrdiff_t>(n));
        CHECK_THROWS_AS(read_plugin_tables(cut), BankError);
    }
}

TEST_CASE("plugin tables: an SXG-Create-NF --embed plugin holds the same tables as its files") {
    HVYXG_NEED_TABLES();
    const std::string dll = test::asset("HVYXG_MU_PLUGIN", "tbl/mu1000-embed/syxg1000.dll");
    const auto files = test::asset_pair("HVYXG_MU_BIN", "tbl/mu1000/SXGMU1KX.TBL", "HVYXG_MU_DATA",
                                        "tbl/mu1000/SXGMU1KX.UPCM");
    if (dll.empty() || !files) {
        MESSAGE("HVYXG_MU_PLUGIN (an SXG-Create-NF --embed plugin) or the MU tables not found: skipped");
        return;
    }
    const PluginTables t = read_plugin_tables(std::filesystem::path(dll));
    CHECK(t.bin == read_file(files->bin, "bin"));
    CHECK(t.wave == read_file(files->data, "wave"));
    // And a synth reads it as it reads the files.
    Synth::Options a, b;
    a.tables = b.tables = test::tables();
    a.plugin_file = dll;
    a.enable_mu = true;
    b.bin_file = files->bin;
    b.data_file = files->data;
    b.enable_mu = true;
    Synth sa(a), sb(b);
    for (Synth* s : {&sa, &sb}) {
        const std::vector<std::uint8_t> on{0x90, 60, 100};
        s->midi(on);
    }
    std::vector<float> x(2 * 22050), y(2 * 22050);
    sa.render(x.data(), 22050);
    sb.render(y.data(), 22050);
    CHECK(x == y);
}
