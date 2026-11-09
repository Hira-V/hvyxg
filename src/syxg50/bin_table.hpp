// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// The S-YXG50 bin table (sxgbin41.tbl / sxgbin21.tbl) and its wave table
// (cross-checked against SXG-Create-NF's table writer, THIRD_PARTY.md):
//
// - 0x00: version text ("MU50 4MB V2.0"), 0x10: wave file name, 0x1F: flags
//   (bit 0 = the wave table is stored plain, bit 7 = the wave is embedded in this file at
//   u32 @0x10, size u32 @0x14);
// - 0x20: 17 little-endian u32 segment lengths; the segments follow one another from 0x64.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "bank_impl.hpp"

namespace hvyxg::syxg50 {

struct BinTable {
    static constexpr std::size_t kSegments = 17;
    static constexpr std::size_t kHeaderSize = 0x64;
    static constexpr std::uint8_t kWaveKey = 0x5D;

    /// The bin file (the caller's memory; the S-YXG50 bin table is not scrambled).
    std::span<const std::uint8_t> bin;
    /// The plain wave bytes (descrambled in place when the header says so): the wave file, or
    /// the part of the bin file that holds it. At least BankMemory::kPad more bytes follow it
    /// in the caller's memory.
    std::span<const std::uint8_t> wave;
    /// The kPad bytes after the wave when they are spare (the end of a buffer), else empty
    /// (an embedded wave the bin file goes on after: the bytes after it are the bin's).
    std::span<std::uint8_t> wave_pad;
    std::array<std::size_t, kSegments> seg_offset{};
    std::array<std::size_t, kSegments> seg_length{};
    /// Header text (version) and wave file name.
    std::string version;
    std::string wave_name;
    /// SXG-Create-NF voice-map appendix ('MAP2', 8 + 9 × 128 bytes right after the last
    /// segment; the DLL ignores data there), or 0 when there is none.
    std::size_t appendix = 0;
    static constexpr std::size_t kAppendixSize = 8 + 9 * 128;

    /// The table looks like SXG-Create-NF's big layout (MU100 and later): 32-bit wave-zone
    /// offsets that all land on 16-byte zone records, and 32-bit program-map entries.
    bool looks_big() const;

    /// The bin header is an S-YXG50 one: long enough, and the 17 segments fit the file.
    static bool check_header(std::span<const std::uint8_t> bin);
    /// Validate and split the bin table into `table`; take the wave table from `memory.wave`
    /// (or from the bin when embedded) and descramble it in place if needed. False and `error`
    /// for a malformed table.
    static bool parse(const BankMemory& memory, BinTable& table, Error& error);

    std::span<const std::uint8_t> segment(std::size_t i) const {
        return bin.subspan(seg_offset[i], seg_length[i]);
    }
};

} // namespace hvyxg::syxg50
