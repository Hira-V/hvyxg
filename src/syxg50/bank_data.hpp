// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// Read access to an S-YXG50 bank by its record layouts: program and
// drum-kit lookup, prevoice records, elements, wave zones and drum entries, all as raw bytes.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "syxg50/bin_table.hpp"
#include "hvyxg/types.hpp"

namespace hvyxg::syxg50 {

inline constexpr std::size_t kElementSize = 78;
inline constexpr std::size_t kZoneSize = 16;
inline constexpr std::size_t kDrumEntrySize = 30;

/// The tone generator's view of a program / drum-key mode (engine `0x63ae`, set by the reset
/// messages): GS, XG (also GM2) or GM.
enum class BankMode : std::uint8_t { Gs, Xg, Gm };

BankMode bank_mode(ResetMode mode);

/// A wave as the chip plays it (zone bytes 3-12, drum entry bytes 19-27).
struct WaveRef {
    /// Byte offset of the loop start in the wave data.
    std::size_t loop_start = 0;
    /// Samples played before the loop start.
    std::uint32_t pre = 0;
    /// Loop length in samples; 0 = one-shot (playback ends at the loop start).
    std::uint32_t loop_len = 0;
    /// Format byte: bit 7 = 8-bit, bits 0-5 = loop length fraction in 1/64 sample.
    std::uint8_t format = 0;

    bool pcm8() const { return (format & 0x80) != 0; }
};

/// How an SXG-Create-NF MU bank is read (docs/ENGINE.md, "MU banks").
struct MuTable {
    /// The bank is played as the SXG-Create-NF patched DLL plays it.
    bool on = false;
    /// Big layout ("Full" patch): 32-bit voice / ext-voice offsets, three multisample pages,
    /// 32-bit sample addresses, 24-bit drum start / loop fields, drum root key at +0x0A.
    bool big = false;
    /// Play the voice map of the appendix (MU Basic) instead of the table's own rows.
    bool basic = false;
};

class BankData {
public:
    explicit BankData(BinTable tbl, MuTable mu = {});
    BankData(const BankData&) = delete;
    BankData& operator=(const BankData&) = delete;

    const MuTable& mu() const { return mu_; }

    /// Prevoice record (`[0]` voice level, `[1]` element mask, elements from `[2]`) for a
    /// program, or nullptr when the bank has no such program.
    const std::uint8_t* program(BankMode mode, std::uint8_t msb, std::uint8_t lsb,
                                std::uint8_t prog) const;
    /// Prevoice record by its program-table index (`< 0x8000` seg13, else seg14).
    const std::uint8_t* prevoice(std::uint16_t idx) const;
    /// Prevoice record at a seg13 byte offset (drum keys, seg6). Big layout: an offset from
    /// voice bank A that may run on into bank B (seg14 follows seg13).
    const std::uint8_t* prevoice_at(std::size_t seg13_offset) const;
    /// Number of elements of a prevoice record.
    static int element_count(const std::uint8_t* pv) { return (pv[1] & 2) ? 2 : 1; }
    /// Element `i` of a prevoice record.
    static const std::uint8_t* element(const std::uint8_t* pv, int i) {
        return pv + 2 + kElementSize * static_cast<std::size_t>(i);
    }

    /// The wave zone of element `e` (wave index `e[0]`) for zone-select key `key6`
    /// nullptr when the index is out of range. Big layout: the element's
    /// position in voice bank B selects multisample page 1 or 2.
    const std::uint8_t* zone(const std::uint8_t* e, int key6) const;
    WaveRef zone_wave(const std::uint8_t* z) const;

    /// Drum kit number.
    std::size_t drum_kit(BankMode mode, std::uint8_t msb, std::uint8_t lsb,
                         std::uint8_t prog) const;
    /// The 30-byte drum entry of a kit's note, or nullptr (`0xffff` = no sound).
    const std::uint8_t* drum_entry(std::size_t kit, std::uint8_t note) const;
    /// The drum entry plays its own wave (classic `entry[16] == 0xff`, big `entry[17]`).
    bool drum_own_wave(const std::uint8_t* entry) const { return entry[mu_.big ? 17 : 16] == 0xff; }
    /// The drum entry's root key (classic `entry[18]`, big `entry[10]`).
    int drum_root_key(const std::uint8_t* entry) const { return entry[mu_.big ? 10 : 18]; }
    /// The prevoice record a drum entry plays (not `drum_own_wave`).
    const std::uint8_t* drum_prevoice(const std::uint8_t* entry) const;
    WaveRef drum_wave(const std::uint8_t* entry) const;

    /// Wave data and BankMemory::kPad bytes after it (silence when they are the buffer's spare
    /// bytes), so the interpolator may read past the end.
    std::span<const std::uint8_t> wave() const { return wave_; }

private:
    std::span<const std::uint8_t> seg(std::size_t i) const;
    /// Drum program rows (GS, XG, SFX, GM2 = seg0..3) and voice bank rows (GS, MSB, XG, GM2 =
    /// seg7..10), or the MU Basic rows of the appendix.
    std::span<const std::uint8_t> drum_row(std::size_t i) const { return rows_[i]; }
    std::span<const std::uint8_t> bank_row(std::size_t i) const { return rows_[4 + i]; }
    /// Big layout: voice banks A and B as one range (offsets are from the start of A).
    std::span<const std::uint8_t> voices() const;

    BinTable tbl_;
    MuTable mu_;
    std::array<std::span<const std::uint8_t>, 8> rows_;
    /// MU: bank index per LSB for XG MSB 48 (appendix +0x408, 0xFF = none), or empty.
    std::span<const std::uint8_t> msb48_;
    std::span<const std::uint8_t> wave_;
};

} // namespace hvyxg::syxg50
