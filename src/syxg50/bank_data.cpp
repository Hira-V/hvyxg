// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "syxg50/bank_data.hpp"

namespace hvyxg::syxg50 {

namespace {

std::uint16_t u16le(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t be24(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) << 16 | static_cast<std::uint32_t>(p[1]) << 8 | p[2];
}

std::uint32_t be32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) << 24 | be24(p + 1);
}

std::uint32_t u32le(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

/// Size of the prevoice record at `p` (`2 + 78 × elements`), 0 when it does not fit.
std::size_t record_size(std::span<const std::uint8_t> seg, std::size_t off) {
    if (off + 2 > seg.size()) return 0;
    const std::size_t size = 2 + kElementSize * ((seg[off + 1] & 2) ? 2 : 1);
    return off + size <= seg.size() ? size : 0;
}

} // namespace

BankMode bank_mode(ResetMode mode) {
    switch (mode) {
    case ResetMode::GS: return BankMode::Gs;
    case ResetMode::XG:
    case ResetMode::GM2: return BankMode::Xg;
    case ResetMode::GM: return BankMode::Gm;
    }
    return BankMode::Xg;
}

BankData::BankData(BinTable tbl, MuTable mu) : tbl_(std::move(tbl)), mu_(mu) {
    for (std::size_t i = 0; i < 4; ++i) {
        rows_[i] = seg(i);
        rows_[4 + i] = seg(7 + i);
    }
    if (mu_.on && tbl_.appendix != 0) {
        // SXG-Create-NF voice-map appendix: +4 default map, +5 MSB 48 row present, +8 the MU
        // Basic drum program rows (GS, XG, SFX, GM2) and voice bank rows (GS, MSB, XG, GM2),
        // +0x408 the MSB 48 row (the same in both maps). The patched DLL points its eight row
        // pointers at the appendix rows when MU Basic is on (docs/ENGINE.md, "MU banks").
        const std::span<const std::uint8_t> a =
            tbl_.bin.subspan(tbl_.appendix, BinTable::kAppendixSize);
        if (mu_.basic)
            for (std::size_t i = 0; i < 8; ++i) rows_[i] = a.subspan(8 + 128 * i, 128);
        if (a[5] == 1) msb48_ = a.subspan(0x408, 128);
    } else {
        mu_.basic = false;
    }
    // 16-bit samples are offset binary too: pad with silence (0x8080 ≈ 0x8000 is close
    // enough; the pad is only read by the interpolator past the last sample). An embedded wave
    // the bin goes on after reads the bin's bytes there instead.
    std::fill(tbl_.wave_pad.begin(), tbl_.wave_pad.end(), std::uint8_t{0x80});
    wave_ = std::span(tbl_.wave.data(), tbl_.wave.size() + BankMemory::kPad);
}

std::span<const std::uint8_t> BankData::seg(std::size_t i) const {
    return tbl_.bin.subspan(tbl_.seg_offset[i], tbl_.seg_length[i]);
}

const std::uint8_t* BankData::program(BankMode mode, std::uint8_t msb, std::uint8_t lsb,
                                      std::uint8_t prog) const {
    msb &= 0x7f;
    lsb &= 0x7f;
    prog &= 0x7f;
    const auto row_of = [&](std::span<const std::uint8_t> t, std::uint8_t i) -> int {
        return i < t.size() ? t[i] : 0;
    };
    std::span<const std::uint8_t> table = seg(12);
    int row = 0;
    switch (mode) {
    case BankMode::Xg:
        if (msb == 121) {
            row = row_of(bank_row(3), lsb);
        } else if (msb == 48 && !msb48_.empty() && msb48_[lsb] != 0xff) {
            // SXG-Create-NF MSB 48 patch: the bank of each LSB, used as it is.
            row = msb48_[lsb];
        } else {
            row = row_of(bank_row(1), msb);
            if (row == 1) row = row_of(bank_row(2), lsb);
        }
        break;
    case BankMode::Gs:
        // An unlisted MSB (seg7 = 0xff) plays the capital tone [meas] (docs: GS and GM modes).
        row = row_of(bank_row(0), msb);
        if (row == 0xff) row = 0;
        table = seg(11);
        break;
    case BankMode::Gm:
        // GM ignores bank select and plays the XG capital set [meas].
        row = row_of(bank_row(2), 0);
        break;
    }
    if (mu_.big) {
        // Big layout: a u32 byte offset from the start of voice bank A.
        const std::size_t at = (static_cast<std::size_t>(row) * 128 + prog) * 4;
        if (at + 4 > table.size()) return nullptr;
        return prevoice_at(u32le(table.data() + at));
    }
    const std::size_t at = (static_cast<std::size_t>(row) * 128 + prog) * 2;
    if (at + 2 > table.size()) return nullptr;
    return prevoice(u16le(table.data() + at));
}

const std::uint8_t* BankData::prevoice(std::uint16_t idx) const {
    const bool ext = idx >= 0x8000;
    const std::span<const std::uint8_t> s = seg(ext ? 14 : 13);
    const std::size_t off = 2 * static_cast<std::size_t>(ext ? idx - 0x8000 : idx);
    return record_size(s, off) ? s.data() + off : nullptr;
}

std::span<const std::uint8_t> BankData::voices() const {
    return tbl_.bin.subspan(tbl_.seg_offset[13], tbl_.seg_length[13] + tbl_.seg_length[14]);
}

const std::uint8_t* BankData::prevoice_at(std::size_t seg13_offset) const {
    const std::span<const std::uint8_t> s = mu_.big ? voices() : seg(13);
    return record_size(s, seg13_offset) ? s.data() + seg13_offset : nullptr;
}

const std::uint8_t* BankData::zone(const std::uint8_t* e, int key6) const {
    const std::span<const std::uint8_t> s15 = seg(15);
    const std::span<const std::uint8_t> s16 = seg(16);
    std::size_t off;
    if (mu_.big) {
        // SXG-Create-NF "Full" patch: u32 offsets, up to 768 multisamples in three pages of
        // 256. An element in voice bank B adds 256 when the table has more than 256 entries,
        // and another 256 when it lies at or after the page-2 start (entry 768, an offset
        // from bank B) and the table has more than 512 entries.
        std::size_t wave = e[0];
        const std::size_t n = s15.size() / 4;
        const std::uint8_t* bank_b = seg(14).data();
        if (n > 256 && e >= bank_b) {
            wave += 256;
            if (n > 512 && s15.size() >= 769 * 4 && e >= bank_b + u32le(s15.data() + 768 * 4)) wave += 256;
        }
        if (4 * wave + 4 > s15.size()) return nullptr;
        off = u32le(s15.data() + 4 * wave);
    } else {
        const std::uint8_t wave = e[0];
        if (2u * wave + 2 > s15.size()) return nullptr;
        off = u16le(s15.data() + 2 * wave);
    }
    if (off + kZoneSize > s16.size()) return nullptr;
    // step while the zone ends below the key and is not the last one.
    while ((s16[off + 15] & 0x7f) < key6 && (s16[off + 15] & 0x80) == 0 &&
           off + 2 * kZoneSize <= s16.size())
        off += kZoneSize;
    return s16.data() + off;
}

WaveRef BankData::zone_wave(const std::uint8_t* z) const {
    // Big layout: the address is a u32 at +9 and the format moves to +0x0D.
    if (mu_.big)
        return {.loop_start = be32(z + 9), .pre = be24(z + 3), .loop_len = be24(z + 6), .format = z[13]};
    return {.loop_start = be24(z + 9), .pre = be24(z + 3), .loop_len = be24(z + 6),
            .format = z[12]};
}

std::size_t BankData::drum_kit(BankMode mode, std::uint8_t msb, std::uint8_t lsb,
                               std::uint8_t prog) const {
    prog &= 0x7f;
    std::span<const std::uint8_t> t = drum_row(0);
    if (mode == BankMode::Xg) {
        if (msb == 126) {
            if (lsb != 0) return 20;
            t = drum_row(2);
        } else if (msb == 120) {
            t = drum_row(3);
        } else {
            t = drum_row(1);
        }
    }
    return prog < t.size() ? t[prog] : 0;
}

const std::uint8_t* BankData::drum_entry(std::size_t kit, std::uint8_t note) const {
    const std::span<const std::uint8_t> s4 = seg(4);
    const std::span<const std::uint8_t> s5 = seg(5);
    const std::size_t at = kit * 256 + 2 * (note & 0x7f);
    if (at + 2 > s4.size()) return nullptr;
    const std::size_t off = u16le(s4.data() + at);
    if (off == 0xffff || off + kDrumEntrySize > s5.size()) return nullptr;
    return s5.data() + off;
}

const std::uint8_t* BankData::drum_prevoice(const std::uint8_t* entry) const {
    const std::span<const std::uint8_t> s6 = seg(6);
    if (mu_.big) {
        // Big layout: a u16 ext-voice index at +0x10, u32 offsets from voice bank A.
        const std::size_t idx = u16le(entry + 16);
        if (4 * idx + 4 > s6.size()) return nullptr;
        return prevoice_at(u32le(s6.data() + 4 * idx));
    }
    if (2u * entry[17] + 2 > s6.size()) return nullptr;
    return prevoice_at(u16le(s6.data() + 2 * entry[17]));
}

WaveRef BankData::drum_wave(const std::uint8_t* e) const {
    // Big layout: start (+0x12) and loop length (+0x15) 24-bit, the address 30 bits at +0x18
    // with the format in its top two bits.
    if (mu_.big)
        return {.loop_start = be32(e + 24) & 0x3fffffffu, .pre = be24(e + 18), .loop_len = be24(e + 21),
                .format = static_cast<std::uint8_t>(e[24] & 0xc0)};
    return {.loop_start = be24(e + 24),
            .pre = static_cast<std::uint32_t>(e[19] << 8 | e[20]),
            .loop_len = static_cast<std::uint32_t>(e[22] << 8 | e[23]),
            .format = e[27]};
}

} // namespace hvyxg::syxg50
