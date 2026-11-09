// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "fuzz_support.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "dll/tables.hpp"
#include "bank_io.hpp"

namespace fuzz {

namespace {

using Bytes = std::vector<std::uint8_t>;

void put16le(Bytes& b, std::size_t at, std::uint32_t v) {
    b[at] = static_cast<std::uint8_t>(v);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}
void put32le(Bytes& b, std::size_t at, std::uint32_t v) {
    put16le(b, at, v & 0xFFFF);
    put16le(b, at + 2, v >> 16);
}
void put24be(Bytes& b, std::size_t at, std::uint32_t v) {
    b[at] = static_cast<std::uint8_t>(v >> 16);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8);
    b[at + 2] = static_cast<std::uint8_t>(v);
}
void put32be(Bytes& b, std::size_t at, std::uint32_t v) {
    b[at] = static_cast<std::uint8_t>(v >> 24);
    put24be(b, at + 1, v & 0xFFFFFF);
}
void append(Bytes& out, const Bytes& in) { out.insert(out.end(), in.begin(), in.end()); }

// ---- S-YXG50 ----

/// A 78-byte element: wave `wave`, full key / velocity range, a plain organ-like envelope.
Bytes syxg50_element(std::uint8_t wave, std::uint8_t key_lo, std::uint8_t key_hi) {
    Bytes e(78, 64);
    e[0] = wave;
    e[1] = key_lo, e[2] = key_hi, e[3] = 1, e[4] = 127;
    e[5] = 1, e[6] = 0, e[7] = 31, e[8] = 0, e[9] = 0, e[10] = 8, e[11] = 0, e[12] = 0;
    e[15] = 0, e[16] = 60, e[17] = 1, e[21] = 60;
    for (std::size_t i = 22; i < 26; ++i) e[i] = 40;
    e[31] = 8, e[32] = 0, e[33] = 110;
    e[34] = 20, e[35] = 50, e[36] = 80, e[37] = 110;
    e[45] = 60;
    for (std::size_t i = 46; i < 50; ++i) e[i] = 40;
    e[55] = 120;
    e[56] = 10, e[57] = 50, e[58] = 90, e[59] = 120;
    e[64] = 0, e[65] = 7, e[67] = 60, e[68] = 0;
    e[69] = 63, e[70] = 30, e[71] = 10, e[72] = 40, e[73] = 120, e[74] = 100, e[75] = 0, e[76] = 4;
    return e;
}

/// A 30-byte classic drum entry: the prevoice of seg6[0], or (own) its own one-shot wave.
Bytes syxg50_drum_entry(bool own, bool big) {
    Bytes d(30, 0);
    const std::array<std::uint8_t, 16> setup = {60, 64, 110, 0, 64, 127, 127, 127, 0, 0, 1, 64, 64, 64, 64, 64};
    std::copy(setup.begin(), setup.end(), d.begin());
    if (!big) {
        d[16] = own ? 0xFF : 0, d[17] = 0, d[18] = 60;
        // One-shot 8-bit: 512 samples before byte 3072.
        d[19] = 0x02, d[20] = 0x00, d[22] = 0, d[23] = 0;
        put24be(d, 24, 3072);
        d[27] = 0x80;
    } else {
        d[10] = 60;
        if (own) d[17] = 0xFF;
        else put16le(d, 16, 0);
        put24be(d, 18, 512);
        put24be(d, 21, 0);
        put32be(d, 24, 3072 | 0x80000000u);  // 8-bit (top bits = format)
    }
    return d;
}

/// A 16-byte wave zone.
Bytes syxg50_zone(bool big, std::uint32_t pre, std::uint32_t loop_len, std::uint32_t loop_start, bool pcm8,
                  std::uint8_t low, std::uint8_t high, bool last) {
    Bytes z(16, 0);
    z[0] = 0, z[1] = 60, z[2] = 0;
    put24be(z, 3, pre);
    put24be(z, 6, loop_len);
    if (big) {
        put32be(z, 9, loop_start);
        z[13] = pcm8 ? 0x80 : 0;
    } else {
        put24be(z, 9, loop_start);
        z[12] = pcm8 ? 0x80 : 0;
    }
    z[14] = low;
    z[15] = static_cast<std::uint8_t>(high | (last ? 0x80 : 0));
    return z;
}

} // namespace

std::vector<std::uint8_t> syxg50_bin(bool big) {
    std::array<Bytes, 17> seg;
    for (std::size_t i = 0; i < 4; ++i) seg[i] = Bytes(128, 0);  // drum kit tables: kit 0
    // seg4: kit 0, note → byte offset into seg5 (0xffff = no sound).
    seg[4] = Bytes(256, 0xFF);
    put16le(seg[4], 2 * 36, 0);
    put16le(seg[4], 2 * 38, 30);
    put16le(seg[4], 2 * 42, 0);
    append(seg[5], syxg50_drum_entry(false, big));
    append(seg[5], syxg50_drum_entry(true, big));
    // seg13: prevoices. rec0 at 0 (one element), rec1 at 80 (two, key split at 60).
    Bytes rec0{100, 1};
    append(rec0, syxg50_element(0, 0, 127));
    Bytes rec1{90, 3};
    append(rec1, syxg50_element(0, 0, 60));
    append(rec1, syxg50_element(1, 61, 127));
    append(seg[13], rec0);
    append(seg[13], rec1);
    // seg14: one more prevoice (classic index 0x8000; big: offset from the start of seg13).
    Bytes rec2{80, 1};
    append(rec2, syxg50_element(1, 0, 127));
    seg[14] = rec2;
    const std::uint32_t rec1_off = 80, rec2_off = static_cast<std::uint32_t>(seg[13].size());
    // seg6: drum key → prevoice (byte offset into seg13; big: u32).
    seg[6] = Bytes(big ? 4 : 2, 0);
    // seg7..10: GS MSB rows, XG MSB rows (1 = use seg9[LSB]), XG LSB rows, GM2 LSB rows.
    seg[7] = Bytes(128, 0xFF);
    seg[7][0] = 0;
    seg[8] = Bytes(128, 0);
    seg[8][0] = 1;
    seg[8][64] = 1;
    seg[9] = Bytes(128, 1);
    seg[10] = Bytes(128, 1);
    // seg11 (GS, 1 row) and seg12 (XG, 2 rows: row 0 dummy, row 1 the capital set).
    const std::size_t w = big ? 4 : 2;
    seg[11] = Bytes(128 * w, 0);
    seg[12] = Bytes(2 * 128 * w, 0);
    for (std::size_t p = 0; p < 128; ++p) {
        if (big) {
            const std::uint32_t off = p % 3 == 0 ? 0 : p % 3 == 1 ? rec1_off : rec2_off;
            put32le(seg[12], 4 * (128 + p), off);
            put32le(seg[11], 4 * p, p % 2 ? rec1_off : 0);
        } else {
            const std::uint32_t idx = p % 3 == 0 ? 0 : p % 3 == 1 ? rec1_off / 2 : 0x8000;
            put16le(seg[12], 2 * (128 + p), idx);
            put16le(seg[11], 2 * p, p % 2 ? rec1_off / 2 : 0);
        }
    }
    // seg16: zones. Wave 0: 16-bit looped below key 60, 8-bit looped above; wave 1: 16-bit
    // one-shot. Wave data (syxg50_wave): 16-bit sine in bytes 0..2047, 8-bit in 2048..4095.
    append(seg[16], syxg50_zone(big, 256, 512, 512, false, 0, 60, false));
    append(seg[16], syxg50_zone(big, 256, 1024, 2304, true, 61, 127, true));
    append(seg[16], syxg50_zone(big, 1000, 0, 2048, false, 0, 127, true));
    // seg15: wave index → zone byte offset (big: u32, at least two entries).
    seg[15] = Bytes(2 * w, 0);
    if (big) put32le(seg[15], 4, 32);
    else put16le(seg[15], 2, 32);

    Bytes bin(0x64, 0);
    const char* version = big ? "SXG FUZZ BIG" : "SXG FUZZ SEED";
    std::memcpy(bin.data(), version, std::strlen(version));
    std::memcpy(bin.data() + 0x10, "fuzzwave.tbl", 12);
    bin[0x1F] = 0x01;  // plain wave
    for (std::size_t k = 0; k < 17; ++k) {
        put32le(bin, 0x20 + 4 * k, static_cast<std::uint32_t>(seg[k].size()));
    }
    for (const Bytes& s : seg) append(bin, s);
    if (big) {
        // SXG-Create-NF MAP2 appendix: +4 default map (native), +5 MSB 48 row present, +8 the
        // MU Basic rows, +0x408 the MSB 48 row.
        Bytes a(8 + 9 * 128, 0);
        std::memcpy(a.data(), "MAP2", 4);
        a[4] = 0;
        a[5] = 1;
        for (std::size_t i = 0; i < 8; ++i)
            for (std::size_t k = 0; k < 128; ++k) a[8 + 128 * i + k] = i == 5 ? 0 : i == 6 ? 1 : 0;
        a[8 + 128 * 5 + 0] = 1;  // MSB 0 → seg9 (LSB) rows
        for (std::size_t k = 0; k < 128; ++k) a[0x408 + k] = k < 4 ? 1 : 0xFF;
        append(bin, a);
    }
    return bin;
}

std::vector<std::uint8_t> syxg50_wave() {
    Bytes w(4096);
    for (std::size_t i = 0; i < 1024; ++i) {
        const int s = static_cast<int>(std::lround(12000.0 * std::sin(6.283185307179586 * static_cast<double>(i) / 64.0)));
        put16le(w, 2 * i, static_cast<std::uint32_t>(s + 0x8000));
    }
    for (std::size_t i = 0; i < 2048; ++i)
        w[2048 + i] = static_cast<std::uint8_t>(0x80 + std::lround(90.0 * std::sin(6.283185307179586 * static_cast<double>(i) / 32.0)));
    return w;
}

// ---- S-YXG2006LE (src/syxg2006le/tbl/voice_table.hpp) ----

namespace {

Bytes le_element(std::uint16_t sample, bool refs) {
    Bytes e(12, 0);
    put16le(e, 4, 0xFFFF);  // no extension
    put16le(e, 6, sample);
    put16le(e, 8, 0xFFFF);  // no pan curve
    // A: LFO.
    Bytes a(16, 0);
    a[0] = 1, a[1] = 75;
    a[4] = 20;
    append(e, a);
    if (refs) {
        e.push_back(0x80), e.push_back(0), e.push_back(0), e.push_back(0);  // B → element 0
    } else {
        Bytes b(28, 0);
        b[0] = 60, b[1] = 64;
        b[4] = 60, b[5] = 60, b[6] = 60, b[7] = 60;
        put16le(b, 18, 0xFFFF);
        append(e, b);
    }
    // C: amp EG (attenuations), no curves.
    Bytes c(36, 0);
    c[1] = 0x70, c[2] = 60, c[3] = 40, c[4] = 0, c[5] = 70, c[6] = 50;
    c[7] = 200, c[8] = 0, c[9] = 8, c[10] = 16;
    for (std::size_t i = 12; i < 30; i += 2) put16le(c, i, 0xFFFF);
    c[30] = 0x11, c[31] = 0x11;
    if (!refs) put16le(c, 16, 0);  // level velocity curve: seg5 row 0
    append(e, c);
    if (refs) {
        e.push_back(0x80), e.push_back(0), e.push_back(0), e.push_back(0);  // D → element 0
    } else {
        Bytes d(84, 0);
        d[1] = 64;
        d[4] = 60, d[5] = 60, d[6] = 60, d[7] = 60, d[8] = 60;
        for (std::size_t i = 22; i < 32; i += 2) put16le(d, i, 0xFFFF);
        put16le(d, 38, 0xFFFF);
        for (const std::size_t f : {44u, 64u}) {
            d[f] = 0, d[f + 1] = 40;
            put16le(d, f + 2, 15000);
            d[f + 4] = 0x30;
            for (std::size_t i = 10; i < 18; i += 2) put16le(d, f + i, 0xFFFF);
        }
        append(e, d);
    }
    return e;
}

Bytes le_voice(std::uint8_t flags, std::uint16_t kit, const std::vector<std::vector<std::array<std::uint8_t, 5>>>& groups) {
    // zone = {key_high, key_low, condition, element, pan}
    Bytes v(16, 0);
    v[1] = flags;
    v[2] = static_cast<std::uint8_t>(groups.size());
    put16le(v, 4, 0xFFFF);
    put16le(v, 6, kit);
    for (const auto& g : groups) {
        Bytes h(12, 0);
        h[1] = static_cast<std::uint8_t>(g.size());
        h[3] = 60, h[7] = 64;
        put16le(h, 8, 0xFFFF);
        append(v, h);
        for (const auto& z : g) {
            Bytes zb(16, 0);
            zb[0] = z[0], zb[1] = z[1], zb[2] = 127, zb[3] = 1, zb[5] = z[4], zb[6] = z[2];
            put32le(zb, 12, z[3]);
            append(v, zb);
        }
    }
    return v;
}

/// A segment that starts with a u32 offset table (relative to the file) of its records.
Bytes record_segment(std::size_t at, const std::vector<Bytes>& recs) {
    Bytes s(4 * recs.size(), 0);
    std::size_t off = at + s.size();
    for (std::size_t i = 0; i < recs.size(); ++i) {
        put32le(s, 4 * i, static_cast<std::uint32_t>(off));
        off += recs[i].size();
    }
    for (const Bytes& r : recs) append(s, r);
    return s;
}

} // namespace

std::vector<std::uint8_t> syxg2006le_bin() {
    Bytes f(0x40, 0);
    const char kText[] = "UTG VPRM 06 07 28 15 28";
    std::memcpy(f.data(), kText, sizeof kText);
    std::array<std::uint32_t, 10> off{};
    off.fill(0xFFFFFFFF);
    // seg0: [msb][lsb] → bank id.
    off[0] = static_cast<std::uint32_t>(f.size());
    Bytes s0(128 * 128, 0xFF);
    for (std::size_t l = 0; l < 128; ++l) {
        s0[0 * 128 + l] = 0;
        s0[119 * 128 + l] = 0;
        s0[127 * 128 + l] = 1;
        s0[126 * 128 + l] = 1;
        s0[118 * 128 + l] = 1;
    }
    append(f, s0);
    // seg1: [bank][program] → voice.
    off[1] = static_cast<std::uint32_t>(f.size());
    Bytes s1(2 * 2 * 128, 0);
    for (std::size_t p = 0; p < 128; ++p) {
        put16le(s1, 2 * p, p % 2);
        put16le(s1, 2 * (128 + p), p % 4 == 3 ? 0xFFFF : 2);
    }
    append(f, s1);
    // seg2: voices.
    off[2] = static_cast<std::uint32_t>(f.size());
    const std::vector<Bytes> voices = {
        le_voice(0, 0xFFFF, {{{60, 0, 0, 0, 64}, {127, 61, 0, 1, 40}}}),
        le_voice(0, 0xFFFF, {{{127, 0, 0, 1, 80}}, {{127, 0, 1, 0, 64}}}),
        le_voice(1, 0, {{{127, 0, 0, 0, 64}, {50, 30, 0, 1, 64}}}),
    };
    append(f, record_segment(f.size(), voices));
    // seg3: elements (element 1 references element 0's blocks B and D).
    off[3] = static_cast<std::uint32_t>(f.size());
    append(f, record_segment(f.size(), {le_element(0, false), le_element(1, true)}));
    // seg4: velocity remap rows; seg5: curves.
    off[4] = static_cast<std::uint32_t>(f.size());
    for (std::size_t r = 0; r < 2; ++r)
        for (std::size_t v = 0; v < 128; ++v) f.push_back(static_cast<std::uint8_t>(r ? 127 - v / 2 : v));
    off[5] = static_cast<std::uint32_t>(f.size());
    for (std::size_t r = 0; r < 2; ++r)
        for (std::size_t v = 0; v < 128; ++v) f.push_back(static_cast<std::uint8_t>(r ? (v / 4) : static_cast<std::uint8_t>(v - 64)));
    // seg6: sample headers (16-bit; sample data in syxg2006le_wave: 4096 samples).
    off[6] = static_cast<std::uint32_t>(f.size());
    for (const auto& [start_sample, pre, len] : {std::array<std::uint32_t, 3>{1024, 512, 256}, {3000, 1000, 64}}) {
        Bytes h(20, 0);
        h[0] = 60, h[5] = 3;
        put32le(h, 8, start_sample * 2 / 4);
        put32le(h, 12, pre);
        put32le(h, 16, len);
        append(f, h);
    }
    // seg8: drum setup, 79 notes of kit 0.
    off[8] = static_cast<std::uint32_t>(f.size());
    for (std::size_t n = 0; n < 79; ++n) {
        Bytes d(24, 64);
        d[0] = 0, d[1] = 127, d[2] = static_cast<std::uint8_t>(n % 3);
        append(f, d);
    }
    for (std::size_t k = 0; k < 10; ++k) put32le(f, 0x18 + 4 * k, off[k]);
    return f;
}

std::vector<std::uint8_t> syxg2006le_wave() {
    Bytes w(2 * 4096);
    for (std::size_t i = 0; i < 4096; ++i) {
        const int s = static_cast<int>(std::lround(9000.0 * std::sin(6.283185307179586 * static_cast<double>(i) / 50.0)));
        put16le(w, 2 * i, static_cast<std::uint16_t>(static_cast<std::int16_t>(s)));
    }
    // Encrypt: the inverse of tbl::decrypt (descramble with key 0x5B, then swap byte pairs).
    for (std::size_t i = 0; i + 1 < w.size(); i += 2) std::swap(w[i], w[i + 1]);
    std::uint8_t key = 0x5B;
    for (std::size_t i = 0; i < w.size(); ++i) {
        const std::uint8_t x = static_cast<std::uint8_t>((w[i] << 4) | (w[i] >> 4));
        w[i] = static_cast<std::uint8_t>(x ^ key ^ (i & 0xFF));
        key = static_cast<std::uint8_t>(~key);
    }
    return w;
}

// ---- Engine tables ----

std::shared_ptr<const hvyxg::EngineTables> need_tables() {
    static const std::shared_ptr<const hvyxg::EngineTables> t = [] {
        std::filesystem::path dir;
        if (const char* e = std::getenv("HVYXG_DLLS")) dir = e;
#ifdef HVYXG_SOURCE_DIR
        // The test binaries: the asset tree's dll/ folder, as tests/test_support.hpp.
        else if (const char* a = std::getenv("HVYXG_ASSETS")) dir = std::filesystem::path(a) / "dll";
        else dir = std::filesystem::path(HVYXG_SOURCE_DIR) / "_untracked" / "dll";
#endif
        std::vector<std::filesystem::path> dlls;
        if (!dir.empty() && std::filesystem::is_directory(dir)) dlls = hvyxg::find_dlls(dir);
        return std::shared_ptr<const hvyxg::EngineTables>(hvyxg::load_engine_tables(dlls));
    }();
    if (!hvyxg::engine_tables_ready(*t, hvyxg::Model::Syxg50) || !hvyxg::engine_tables_ready(*t, hvyxg::Model::Syxg2006le)) {
        std::fprintf(stderr, "the engines need their tables: set HVYXG_DLLS to a folder with syxg50.dll and "
                             "the S-YXG2006LE DLL (docs/TESTING.md)\n");
        std::exit(2);
    }
    return t;
}

// ---- Banks for the MIDI targets ----

std::shared_ptr<const hvyxg::Bank> bank(int which) {
    const auto tables = need_tables();
    static std::shared_ptr<const hvyxg::Bank> cache[3];
    auto& b = cache[which];
    if (b) return b;
    static const char* const kVars[3][2] = {
        {"HVYXG_TBL_BIN", "HVYXG_TBL_DATA"}, {"HVYXG_2006LE_BIN", "HVYXG_2006LE_DATA"}, {"HVYXG_MU_BIN", "HVYXG_MU_DATA"}};
    hvyxg::BankOptions bo;
    bo.tables = tables;
    bo.enable_mu = which == 2;
    const char* bin = std::getenv(kVars[which][0]);
    const char* data = std::getenv(kVars[which][1]);
    if (bin && data) {
        b = hvyxg::load_bank(bin, data, hvyxg::Model::Auto, bo);
    } else if (which == 1) {
        b = hvyxg::load_bank(syxg2006le_bin(), syxg2006le_wave(), hvyxg::Model::Auto, bo);
    } else {
        b = hvyxg::load_bank(syxg50_bin(which == 2), syxg50_wave(), hvyxg::Model::Auto, bo);
    }
    return b;
}

hvyxg::Synth::Options options(std::uint8_t c, std::shared_ptr<const hvyxg::Bank> b) {
    hvyxg::Synth::Options o;
    o.bank = std::move(b);
    o.dll_exact = (c & 0x01) != 0;
    o.ports = 1 + ((c >> 1) & 1) * 3;
    static constexpr std::uint16_t kPoly[4] = {0, 1, 8, 300};
    o.max_polyphony = kPoly[(c >> 2) & 3];
    o.interpolation = static_cast<hvyxg::Interpolation>(((c >> 4) & 3) % 3);
    o.steal_fade_ms = (c & 0x40) ? 20.0f : 0.0f;
    o.device_id = (c & 0x80) ? 0 : -1;
    return o;
}

void render(hvyxg::Synth& s, std::size_t frames) {
    static float buf[2 * 1024];
    while (frames > 0) {
        const std::size_t n = std::min<std::size_t>(frames, 1024);
        s.render(buf, n);
        for (std::size_t i = 0; i < 2 * n; ++i)
            if (!std::isfinite(buf[i])) {
                std::fprintf(stderr, "non-finite output sample\n");
                std::abort();
            }
        frames -= n;
    }
}

void probe_bank(const std::shared_ptr<const hvyxg::Bank>& b, std::uint8_t seed) {
    need_tables();
    hvyxg::Synth::Options o;
    o.bank = b;
    o.max_polyphony = 16;
    o.dll_exact = (seed & 0x80) != 0;
    hvyxg::Synth s(o);
    const auto send = [&](std::initializer_list<std::uint8_t> m) { s.midi(std::vector<std::uint8_t>(m)); };
    // Reset (XG, GM, GS by the seed), then programs on a few banks, notes over the key range.
    if ((seed & 3) == 1) send({0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7});
    if ((seed & 3) == 2) send({0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7});
    static constexpr std::uint8_t kMsb[] = {0, 127, 126, 64, 121, 48, 120, 8};
    for (std::uint8_t ch = 0; ch < 8; ++ch) {
        const std::uint8_t msb = kMsb[(ch + seed) & 7];
        send({static_cast<std::uint8_t>(0xB0 | ch), 0, msb});
        send({static_cast<std::uint8_t>(0xB0 | ch), 32, static_cast<std::uint8_t>((seed >> 3) & 3)});
        send({static_cast<std::uint8_t>(0xC0 | ch), static_cast<std::uint8_t>((ch * 37 + seed) & 0x7F)});
    }
    send({0xB9, 0, 127});
    send({0xC9, 0});
    for (std::uint8_t k = 0; k < 128; k += 9) {
        const auto ch = static_cast<std::uint8_t>((k / 9) % 10);
        send({static_cast<std::uint8_t>(0x90 | ch), k, static_cast<std::uint8_t>(1 + (k * 13 + seed) % 127)});
    }
    send({0x99, 36, 100});
    send({0x99, 38, 100});
    send({0x99, 42, 1});
    render(s, 1500);
    send({0xB0, 1, 127});
    send({0xE0, 0x7F, 0x7F});
    send({0xB0, 64, 127});
    render(s, 600);
    send({0xB0, 123, 0});
    for (std::uint8_t ch = 0; ch < 16; ++ch) send({static_cast<std::uint8_t>(0xB0 | ch), 120, 0});
    render(s, 300);
}

void play(hvyxg::Synth& s, Input& in) {
    std::size_t budget = 48000;  // rendered frames per input
    std::vector<std::uint8_t> m;
    m.reserve(512);
    const auto port = [&](std::uint8_t b) { return static_cast<std::size_t>(b & 7); };  // may exceed `ports`
    while (!in.empty()) {
        const std::uint8_t op = in.u8();
        m.clear();
        switch (op % 10) {
        case 0:
        case 1: {  // channel message
            const auto st = static_cast<std::uint8_t>(0x80 | in.u8());
            m.push_back(st);
            m.push_back(in.u8() & 0x7F);
            if ((st & 0xF0) != 0xC0 && (st & 0xF0) != 0xD0) m.push_back(in.u8() & 0x7F);
            s.midi(m, 0, op & 0x80 ? port(op >> 4) : 0);
            break;
        }
        case 2: {  // XG parameter change F0 43 1n 4C hh mm ll dd… F7
            m = {0xF0, 0x43, static_cast<std::uint8_t>(0x10 | (in.u8() & 0x0F)), 0x4C};
            const std::size_t n = 3 + in.u8() % 6;
            for (std::size_t i = 0; i < n; ++i) m.push_back(in.u8() & 0x7F);
            m.push_back(0xF7);
            s.midi(m);
            break;
        }
        case 3: {  // XG bulk dump F0 43 0n 4C bh bl hh mm ll data… cs F7 (and other Yamaha models)
            const std::uint8_t model = (op & 0x80) ? in.u8() : 0x4C;
            const std::size_t n = in.u8() % 140;
            m = {0xF0, 0x43, static_cast<std::uint8_t>(in.u8() & 0x0F), model,
                 static_cast<std::uint8_t>((n >> 7) & 0x7F), static_cast<std::uint8_t>(n & 0x7F)};
            if (op & 0x40) m[4] = in.u8() & 0x7F, m[5] = in.u8() & 0x7F;  // lying byte count
            std::uint8_t sum = 0;
            for (std::size_t i = 0; i < 3 + n; ++i) {
                const std::uint8_t b = in.u8() & 0x7F;
                m.push_back(b);
                sum = static_cast<std::uint8_t>(sum + b);
            }
            m.push_back(static_cast<std::uint8_t>((0x80 - (sum + m[4] + m[5])) & 0x7F));
            m.push_back(0xF7);
            s.midi(m);
            break;
        }
        case 4: {  // GS DT1 F0 41 dev 42 12 a1 a2 a3 data… cs F7
            m = {0xF0, 0x41, static_cast<std::uint8_t>(in.u8() & 0x1F), 0x42, 0x12, 0x40};
            if (op & 0x80) m[5] = in.u8() & 0x7F;
            m.push_back(in.u8() & 0x7F);
            m.push_back(in.u8() & 0x7F);
            const std::size_t n = 1 + in.u8() % 32;
            for (std::size_t i = 0; i < n; ++i) m.push_back(in.u8() & 0x7F);
            std::uint8_t sum = 0;
            for (std::size_t i = 5; i < m.size(); ++i) sum = static_cast<std::uint8_t>(sum + m[i]);
            m.push_back(static_cast<std::uint8_t>((0x80 - sum) & 0x7F));
            m.push_back(0xF7);
            s.midi(m);
            break;
        }
        case 5: {  // universal SysEx (GM / GM2 on, master volume, …) or any raw SysEx
            m = {0xF0, static_cast<std::uint8_t>((op & 0x80) ? 0x7F : 0x7E), static_cast<std::uint8_t>(in.u8() & 0x7F)};
            const std::size_t n = in.u8() % 12;
            for (std::size_t i = 0; i < n; ++i) m.push_back(in.u8() & 0x7F);
            m.push_back(0xF7);
            s.midi(m);
            break;
        }
        case 6: {  // raw bytes as one message (malformed on purpose), with a frame offset
            const std::uint32_t at = (op & 0x80) ? in.u16() : 0;
            const auto raw = in.take(in.u8() % 24);
            s.midi(raw, at, port(op >> 4));
            break;
        }
        case 7: {  // raw byte stream
            const auto raw = in.take(in.u8() % 64);
            s.midi_stream(raw, (op & 0x80) ? in.u8() : 0, port(op >> 4));
            break;
        }
        case 8: {  // render
            const std::size_t n = std::min<std::size_t>(budget, 1 + static_cast<std::size_t>(in.u8()) * 23);
            render(s, n);
            budget -= n;
            break;
        }
        case 9:
            if ((op & 0xF0) == 0xF0) s.reset();
            else if (budget > 0) {
                render(s, 1);
                --budget;
            }
            break;
        }
    }
    render(s, std::min<std::size_t>(budget, 2048));
}

} // namespace fuzz
