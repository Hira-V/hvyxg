// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "syxg2006le/tbl/voice_table.hpp"

#include <algorithm>
#include <cstring>

#include "syxg2006le/tbl/header.hpp"
#include "syxg2006le/tbl/decrypt.hpp"

namespace hvyxg::syxg2006le::tbl {

namespace {

constexpr std::uint32_t kAbsent = 0xFFFFFFFF;
constexpr std::size_t kSampleHeaderSize = 20;
constexpr std::size_t kDrumEntrySize = 24;
constexpr std::size_t kDrumNotes = 79; // notes 13..91
constexpr std::size_t kRowSize = 128;
constexpr std::array<std::uint8_t, kRowSize> kZeroRow{};

std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t at) {
    std::uint32_t v = 0;
    std::memcpy(&v, b.data() + at, 4); // little-endian host
    return v;
}

/// A segment that starts with a table of u32 record offsets (relative to the file); the
/// table ends where the first record begins.
/// Empty, with `error` set, when the table is malformed.
std::vector<std::uint32_t> record_offsets(std::span<const std::uint8_t> file, std::uint32_t seg, const char*& error) {
    if (seg + 4 > file.size()) {
        error = "record table outside the file";
        return {};
    }
    const std::uint32_t first = u32(file, seg);
    if (first <= seg || first > file.size() || (first - seg) % 4 != 0) {
        error = "malformed record table";
        return {};
    }
    std::vector<std::uint32_t> offs((first - seg) / 4);
    for (std::size_t i = 0; i < offs.size(); ++i) offs[i] = u32(file, seg + 4 * i);
    return offs;
}

} // namespace

std::int8_t sext7(std::uint8_t v) {
    const int x = v & 0x7F;
    return static_cast<std::int8_t>(x >= 64 ? x - 128 : x);
}

SampleHeader SampleHeader::from_bytes(std::span<const std::uint8_t, 20> b) {
    SampleHeader h;
    h.root_key = b[0] & 0x7F;
    h.fixed_pitch = b[0] & 0x80;
    h.coarse = sext7(b[1]);
    h.fine = sext7(b[2]);
    h.loop_fine = sext7(b[3]);
    h.level = b[4];
    h.format = static_cast<SampleFormat>(b[5] & 7);
    h.predictor = b[7] & 0x1F;
    h.reverse_loop = (b[6] >> 6) & 1;
    h.flag_b6 = (b[6] >> 7) & 1;
    h.loop_start_byte = u32(b, 8) * 4;
    h.pre_loop = u32(b, 12);
    h.loop_length = u32(b, 16);
    return h;
}

VoiceTable::VoiceTable(std::span<const std::uint8_t> file) : file_(file) {
    if (file.size() < 0x40 || !check_header(file.first(8))) {
        fail("not an S-YXG2006LE voice table (missing \"UTG VPRM\")");
        return;
    }
    for (std::size_t k = 0; k < kSegments; ++k) {
        offsets_[k] = u32(file, 0x18 + 4 * k);
        if (offsets_[k] != kAbsent && offsets_[k] > file.size()) {
            fail("segment offset outside the file");
            return;
        }
    }
    for (const std::size_t k : {0u, 1u, 2u, 3u, 6u})
        if (offsets_[k] == kAbsent) {
            fail("required segment missing");
            return;
        }
    if (offsets_[0] + 128 * 128 > file.size()) {
        fail("bank map truncated");
        return;
    }
    const char* e = nullptr;
    voice_offsets_ = record_offsets(file, offsets_[2], e);
    if (!e) element_offsets_ = record_offsets(file, offsets_[3], e);
    if (e) {
        voice_offsets_.clear();
        element_offsets_.clear();
        fail(e);
    }
}

void VoiceTable::fail(const char* what) const {
    if (error_ == nullptr) error_ = what;
}

std::optional<std::span<const std::uint8_t>> VoiceTable::segment(std::size_t k) const {
    if (k >= kSegments || offsets_[k] == kAbsent) return std::nullopt;
    // A segment ends at the next segment start (segments are stored in ascending order).
    std::size_t end = file_.size();
    for (const std::uint32_t o : offsets_)
        if (o != kAbsent && o > offsets_[k]) end = std::min<std::size_t>(end, o);
    return file_.subspan(offsets_[k], end - offsets_[k]);
}

std::optional<std::uint8_t> VoiceTable::bank_id(std::uint8_t msb, std::uint8_t lsb) const {
    const std::uint8_t b = file_[offsets_[0] + (msb & 0x7F) * 128u + (lsb & 0x7F)];
    if (b == 0xFF) return std::nullopt;
    return b;
}

std::optional<std::uint16_t> VoiceTable::voice_index(std::uint8_t bank, std::uint8_t program) const {
    const std::size_t at = offsets_[1] + 2 * (bank * 128u + (program & 0x7F));
    if (at + 2 > offsets_[2]) return std::nullopt;
    const auto v = static_cast<std::uint16_t>(file_[at] | (file_[at + 1] << 8));
    if (v == 0xFFFF || v >= voice_offsets_.size()) return std::nullopt;
    return v;
}

namespace {
/// Empty, with `error` set, for a bad index or a record outside its segment.
std::span<const std::uint8_t> record(std::span<const std::uint8_t> file,
                                     const std::vector<std::uint32_t>& offs, std::size_t index,
                                     std::size_t end, const char*& error) {
    if (index >= offs.size()) {
        error = "record index out of range";
        return {};
    }
    const std::size_t begin = offs[index];
    const std::size_t next = index + 1 < offs.size() ? offs[index + 1] : end;
    // Records lie inside their segment, in ascending order.
    if (begin > next || next > end || end > file.size()) {
        error = "record outside its segment";
        return {};
    }
    return file.subspan(begin, next - begin);
}
} // namespace

std::span<const std::uint8_t> VoiceTable::voice_record(std::size_t index) const {
    const auto seg = segment(2);
    if (!seg) return {};
    const char* e = nullptr;
    const auto r = record(file_, voice_offsets_, index, offsets_[2] + seg->size(), e);
    if (e) fail(e);
    return r;
}

std::span<const std::uint8_t> VoiceTable::element_record(std::size_t index) const {
    const auto seg = segment(3);
    if (!seg) return {};
    const char* e = nullptr;
    const auto r = record(file_, element_offsets_, index, offsets_[3] + seg->size(), e);
    if (e) fail(e);
    return r;
}

Voice VoiceTable::voice(std::size_t index) const {
    const auto r = voice_record(index);
    Voice v;
    if (r.size() < 16) {
        fail("voice record too short");
        return v;
    }
    std::copy_n(r.begin(), 16, v.header.begin());
    v.type = r[0];
    v.flags = r[1];
    v.velocity_curve = static_cast<std::uint16_t>(r[4] | (r[5] << 8));
    v.drum_kit = static_cast<std::uint16_t>(r[6] | (r[7] << 8));
    v.level = r[14];
    v.damper_mode = r[15] >> 4;
    std::size_t p = 0x10;
    for (std::size_t g = 0; g < r[2]; ++g) {
        if (p + 12 > r.size()) {
            fail("voice group truncated");
            return v;
        }
        Group grp;
        std::copy_n(r.begin() + static_cast<std::ptrdiff_t>(p), 12, grp.raw.begin());
        grp.transpose = sext7(r[p + 2]);
        grp.center_key = static_cast<std::int8_t>(r[p + 3]);
        grp.key_track = r[p + 4] & 7;
        grp.layer = r[p];
        grp.fine = sext7(r[p + 5]);
        grp.level = r[p + 6];
        grp.pan = r[p + 7] & 0x7F;
        grp.pitch_curve = static_cast<std::uint16_t>(r[p + 8] | (r[p + 9] << 8));
        const std::size_t zones = r[p + 1];
        p += 12;
        if (p + 16 * zones > r.size()) {
            fail("voice zones truncated");
            return v;
        }
        for (std::size_t z = 0; z < zones; ++z, p += 16) {
            Zone zn;
            std::copy_n(r.begin() + static_cast<std::ptrdiff_t>(p), 16, zn.raw.begin());
            zn.key_high = r[p];
            zn.key_low = r[p + 1];
            zn.vel_high = r[p + 2];
            zn.vel_low = r[p + 3];
            zn.level = r[p + 4];
            zn.pan = r[p + 5] & 0x7F;
            zn.condition = r[p + 6];
            zn.element = u32(r, p + 12);
            grp.zones.push_back(zn);
        }
        v.groups.push_back(std::move(grp));
    }
    return v;
}

Element VoiceTable::element(std::size_t index) const {
    const auto r = element_record(index);
    Element e;
    if (r.size() < 12 + 4 * 4) {
        fail("element record too short");
        return e;
    }
    std::copy_n(r.begin(), 12, e.header.begin());
    e.extension = static_cast<std::uint16_t>(r[4] | (r[5] << 8));
    e.sample = static_cast<std::uint16_t>(r[6] | (r[7] << 8));
    e.level = r[0];
    e.delay = static_cast<std::uint16_t>((r[2] | (r[3] << 8)) & 0x7FF);
    e.pan_curve = static_cast<std::uint16_t>(r[8] | (r[9] << 8));
    e.pan_curve_depth = r[10] >> 4;
    for (std::size_t b = 0; b < 4; ++b) {
        // Walk to block b in the record that holds it, following references (bounded: the
        // DLL loops until it finds an inline block).
        std::span<const std::uint8_t> rec = r;
        for (int hops = 0;; ++hops) {
            if (hops > 64) {
                fail("element block reference loop");
                return e;
            }
            std::size_t p = 12;
            for (std::size_t k = 0; k < b && p < rec.size(); ++k) p += (rec[p] & 0x80) ? 4 : Element::kBlockSizes[k];
            if (p + 4 > rec.size()) {
                fail("element block truncated");
                return e;
            }
            if (rec[p] & 0x80) { // big-endian reference, bit 31 = "see element N"
                const std::uint32_t target =
                    ((rec[p] & 0x7Fu) << 24) | (rec[p + 1] << 16) | (rec[p + 2] << 8) | rec[p + 3];
                if (target >= element_offsets_.size()) {
                    fail("bad element reference");
                    return e;
                }
                rec = element_record(target);
                if (rec.size() < 12) {
                    fail("element block truncated");
                    return e;
                }
                continue;
            }
            if (p + Element::kBlockSizes[b] > rec.size()) {
                fail("element block truncated");
                return e;
            }
            e.blocks[b] = rec.subspan(p, Element::kBlockSizes[b]);
            break;
        }
    }
    return e;
}

std::pair<std::uint8_t, std::uint8_t> group_key(const dll::Tables& tb, const Group& g, std::uint8_t key) {
    const int center = g.center_key;
    int k = key;
    int cents = 0;
    if (g.key_track == 5) {
        k = center;
    } else if (g.key_track != 0) {
        const int d = static_cast<std::int8_t>(key - center);
        const int scaled = tb.kKeyTrackScale[g.key_track] * d;
        if (d >= 0) {
            k = center + static_cast<std::int8_t>(scaled / 100);
            cents = scaled % 100;
        } else {
            const int q = -scaled / 100, r = -scaled % 100;
            k = r ? center - (q + 1) : center - q;
            cents = r ? 100 - r : 0;
        }
    }
    const int zone_key = std::clamp(k + g.transpose, 0, 127);
    return {static_cast<std::uint8_t>(zone_key), static_cast<std::uint8_t>(cents)};
}

std::vector<ZoneMatch> find_zones(const dll::Tables& tb, const Voice& voice, std::uint8_t key, std::uint8_t velocity,
                                  std::uint8_t condition) {
    std::vector<ZoneMatch> out;
    for (const Group& g : voice.groups) {
        const auto [zone_key, cents] = group_key(tb, g, key);
        const auto played = static_cast<std::uint8_t>(std::clamp(key + g.transpose, 0, 127));
        for (const Zone& z : g.zones) {
            const bool in_range = velocity >= z.vel_low && velocity <= z.vel_high &&
                                  zone_key >= z.key_low && zone_key <= z.key_high;
            const bool triggers = condition == 0 ? z.condition == 0 : (z.condition & condition) != 0;
            if (in_range && triggers) out.push_back({&g, &z, zone_key, played, cents});
        }
    }
    return out;
}

std::size_t VoiceTable::sample_count() const { return segment(6)->size() / kSampleHeaderSize; }

SampleHeader VoiceTable::sample_header(std::size_t index) const {
    if (index >= sample_count()) {
        fail("sample header index out of range");
        return {};
    }
    return SampleHeader::from_bytes(
        segment(6)->subspan(index * kSampleHeaderSize).first<kSampleHeaderSize>());
}

std::optional<std::span<const std::uint8_t, 24>> VoiceTable::drum_entry(std::size_t kit,
                                                                       std::uint8_t note) const {
    const auto seg = segment(8);
    if (!seg || note < 13 || note > 91) return std::nullopt;
    const std::size_t at = (kit * kDrumNotes + (note - 13u)) * kDrumEntrySize;
    if (at + kDrumEntrySize > seg->size()) return std::nullopt;
    return seg->subspan(at).first<kDrumEntrySize>();
}

std::span<const std::uint8_t, 128> VoiceTable::seg4_row(std::size_t row) const {
    const auto seg = segment(4);
    if (!seg || (row + 1) * kRowSize > seg->size()) {
        fail("seg4 row out of range");
        return kZeroRow;
    }
    return seg->subspan(row * kRowSize).first<kRowSize>();
}

std::span<const std::uint8_t, 128> VoiceTable::seg5_row(std::size_t row) const {
    const auto seg = segment(5);
    if (!seg || (row + 1) * kRowSize > seg->size()) {
        fail("seg5 row out of range");
        return kZeroRow;
    }
    return seg->subspan(row * kRowSize).first<kRowSize>();
}

std::optional<std::vector<float>> decode_sample(std::span<const std::uint8_t> wave,
                                                const SampleHeader& h) {
    const auto first = -static_cast<std::int64_t>(h.pre_loop);
    const auto last = static_cast<std::int64_t>(h.loop_length) - 1;
    const auto base = static_cast<std::int64_t>(h.loop_start_byte);
    const auto in_range = [&](std::int64_t lo, std::int64_t hi) {
        return lo >= 0 && hi <= static_cast<std::int64_t>(wave.size());
    };
    std::vector<float> out;
    out.reserve(static_cast<std::size_t>(last - first + 1));
    switch (h.format) {
    case SampleFormat::Pcm16:
        if (!in_range(base + 2 * first, base + 2 * last + 2)) return std::nullopt;
        for (std::int64_t i = first; i <= last; ++i) {
            const auto p = static_cast<std::size_t>(base + 2 * i);
            out.push_back(static_cast<float>(static_cast<std::int16_t>(wave[p] | (wave[p + 1] << 8))) /
                          32768.0f);
        }
        return out;
    case SampleFormat::Pcm8:
        if (!in_range(base + first, base + last + 1)) return std::nullopt;
        for (std::int64_t i = first; i <= last; ++i)
            out.push_back(static_cast<float>(static_cast<std::int8_t>(wave[static_cast<std::size_t>(base + i)])) /
                          128.0f);
        return out;
    case SampleFormat::Pcm12: {
        // Index i → bytes base + 3·(i >> 1) (arithmetic shift, as the DLL's `sar`).
        const auto group = [](std::int64_t i) { return i >= 0 ? i / 2 : -((-i + 1) / 2); };
        if (!in_range(base + 3 * group(first), base + 3 * group(last) + 3)) return std::nullopt;
        for (std::int64_t i = first; i <= last; ++i) {
            const auto g = static_cast<std::size_t>(base + 3 * group(i));
            out.push_back(static_cast<float>(unpack12(wave.subspan(g, 3), static_cast<std::size_t>(i & 1))) /
                          32768.0f);
        }
        return out;
    }
    default: return std::nullopt;
    }
}

} // namespace hvyxg::syxg2006le::tbl
