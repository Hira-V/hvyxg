// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "syxg50/bin_table.hpp"

#include <algorithm>

#include "bank_io.hpp"
#include "hvyxg/types.hpp"

namespace hvyxg::syxg50 {

namespace {

std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 |
           static_cast<std::uint32_t>(b[at + 2]) << 16 | static_cast<std::uint32_t>(b[at + 3]) << 24;
}

std::string text(std::span<const std::uint8_t> b, std::size_t at, std::size_t n) {
    std::string s;
    for (std::size_t i = at; i < at + n && b[i] != 0; ++i) s.push_back(static_cast<char>(b[i]));
    return s;
}

/// Sum of the segment lengths, or ~0 when the header is too short.
std::uint64_t segment_bytes(std::span<const std::uint8_t> bin) {
    if (bin.size() < BinTable::kHeaderSize) return ~std::uint64_t{0};
    std::uint64_t sum = 0;
    for (std::size_t k = 0; k < BinTable::kSegments; ++k) sum += u32(bin, 0x20 + 4 * k);
    return sum;
}

bool fail(Error& error, ErrorKind kind, const std::string& what) {
    error = {kind, "S-YXG50 bin table: " + what};
    return false;
}

} // namespace

bool BinTable::check_header(std::span<const std::uint8_t> bin) {
    if (bin.size() < kHeaderSize) return false;
    // The version text starts with a printable character ("MU50 4MB V2.0", SXG-Create-NF:
    // "S-YXG50…"), and the DLL's accepts the table when its segments fit.
    if (bin[0] < 0x20 || bin[0] > 0x7E) return false;
    if (std::equal(bin.begin(), bin.begin() + 8, "UTG VPRM")) return false;
    return kHeaderSize + segment_bytes(bin) <= bin.size();
}

bool BinTable::parse(const BankMemory& memory, BinTable& t, Error& error) {
    if (memory.bin.size() < BankMemory::kPad) return fail(error, BankError::Kind::Header, "no room for the bin table's padding");
    const std::span<std::uint8_t> bin_file = memory.bin.first(memory.bin.size() - BankMemory::kPad);
    t = BinTable{};
    t.bin = bin_file;
    const std::span<const std::uint8_t> b = t.bin;
    if (b.size() < kHeaderSize) return fail(error, BankError::Kind::Header, "file too short for the header");
    if (!check_header(b)) return fail(error, BankError::Kind::Header, "bad header (segments do not fit the file)");
    t.version = text(b, 0, 16);
    std::size_t at = kHeaderSize;
    for (std::size_t k = 0; k < kSegments; ++k) {
        t.seg_offset[k] = at;
        t.seg_length[k] = u32(b, 0x20 + 4 * k);
        at += t.seg_length[k];
    }
    // The engine reads the kit tables (0-3), the drum keys (4-6), the program maps (7-12),
    // the prevoices (13) and the wave zones (15, 16).
    for (const std::size_t k : {0u, 1u, 4u, 5u, 7u, 8u, 9u, 11u, 12u, 13u, 15u, 16u})
        if (t.seg_length[k] == 0) return fail(error, BankError::Kind::Table, "segment " + std::to_string(k) + " missing");
    if (t.seg_length[12] % 256 != 0 || t.seg_length[11] % 256 != 0)
        return fail(error, BankError::Kind::Table, "program tables are not whole rows");
    if (t.seg_length[16] % 16 != 0) return fail(error, BankError::Kind::Table, "wave zones are not whole records");
    // SXG-Create-NF's second voice map (MU Basic) and MSB 48 row, behind the last segment.
    if (at + kAppendixSize <= b.size() && std::equal(b.begin() + static_cast<std::ptrdiff_t>(at),
                                                     b.begin() + static_cast<std::ptrdiff_t>(at + 4), "MAP2"))
        t.appendix = at;

    const std::uint8_t flags = b[0x1F];
    if (flags & 0x80) {
        // Embedded wave: offset and size at 0x10 / 0x14 instead of the file name.
        const std::uint64_t off = u32(b, 0x10), size = u32(b, 0x14);
        if (off + size > b.size()) return fail(error, BankError::Kind::Table, "embedded wave outside the file");
        const std::span<std::uint8_t> w = bin_file.subspan(static_cast<std::size_t>(off), static_cast<std::size_t>(size));
        if (!(flags & 0x01)) descramble_wave(kWaveKey, w);
        t.wave = w;
        // At the end of the file the padding is the buffer's; inside, the bin goes on after it.
        if (off + size == b.size()) t.wave_pad = memory.bin.last(BankMemory::kPad);
    } else {
        t.wave_name = text(b, 0x10, 15);
        if (memory.wave.size() < BankMemory::kPad) return fail(error, BankError::Kind::Table, "empty wave table");
        const std::span<std::uint8_t> w = memory.wave.first(memory.wave.size() - BankMemory::kPad);
        // Bit 0 set: the wave is stored plain (both shipped banks); else descramble it as the
        // DLL does (key 0x5D).
        if (!(flags & 0x01)) descramble_wave(kWaveKey, w);
        t.wave = w;
        t.wave_pad = memory.wave.last(BankMemory::kPad);
    }
    if (t.wave.empty()) return fail(error, BankError::Kind::Table, "empty wave table");
    return true;
}

bool BinTable::looks_big() const {
    // Big layout (SXG-Create-NF makeSYXG50.py): seg15 holds u32 offsets into seg16 (entry 768,
    // when present, is the page-2 start in voice bank B), seg11 / seg12 u32 voice offsets. In
    // the classic layout seg15 holds u16 offsets, which read as u32 pairs land far outside the
    // zone table.
    const std::size_t n15 = seg_length[15], n16 = seg_length[16];
    if (n15 % 4 != 0 || n15 < 8 || seg_length[12] % 512 != 0 || seg_length[11] % 512 != 0) return false;
    const std::span<const std::uint8_t> s = segment(15);
    for (std::size_t i = 0; i < n15 / 4; ++i) {
        if (i == 768) continue;
        const std::uint32_t o = u32(s, 4 * i);
        if (o % 16 != 0 || o >= n16) return false;
    }
    return true;
}

} // namespace hvyxg::syxg50
