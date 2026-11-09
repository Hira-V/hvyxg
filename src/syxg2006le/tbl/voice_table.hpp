// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// S-YXG2006LE voice table (SXGBNW6L.TBL) and sample decoding.
//
// The format as the S-YXG2006LE DLL reads it. The voice table is stored in the clear; the wave
// table must first go through `yxg2006le::decrypt`.
//
// Layout: "UTG VPRM ..." text header (24 bytes), then ten u32 segment offsets at 0x18
// (relative to the file start; 0xFFFFFFFF = absent):
//   seg0  [msb][lsb] → bank id (u8, 0xFF = none)
//   seg1  [bank][program] → voice index (u16, 0xFFFF = none)
//   seg2  voice index → u32 offset table, then variable-size voice records
//   seg3  u32 offset table, then variable-size element / key-zone records
//   seg4  128-byte velocity remap rows (4; voice u16 @ 4)
//   seg5  128-byte key / velocity scaling curves (curve_value)
//   seg6  20-byte sample headers
//   seg7  36-byte entries indexed from 0x6000 (absent here)
//   seg8  24-byte drum setup entries, 79 notes (13..91) per kit
//   seg9  u32 offset table of variable records (absent here)

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "syxg2006le/tbl/param_blocks.hpp"

namespace hvyxg::syxg2006le::tbl {

/// `v & 0x7F` read as 7-bit two's complement (DLL tables).
std::int8_t sext7(std::uint8_t v);

/// Sample encodings (header byte 5 & 7; the DLL reads each its own way).
enum class SampleFormat : std::uint8_t {
    Dpcm8 = 0,   ///< 8-bit companded DPCM with a 2-pole predictor
    Pcm8 = 1,    ///< 8-bit signed
    Pcm12 = 2,   ///< 12-bit packed, 2 samples per 3 bytes
    Pcm16 = 3,   ///< 16-bit signed little-endian; every sample in SXGBNW6L
    Pcm24 = 4,   ///< 24-bit in 4-byte slots
    Pcm32 = 5,
};

/// One 20-byte seg6 entry, decoded as the DLL does.
struct SampleHeader {
    std::uint8_t root_key = 0;   ///< byte 0 & 0x7F
    bool fixed_pitch = false;    ///< byte 0 bit 7: no key tracking, only the sample tune
    /// byte 1 & 0x7F as 7-bit signed semitones, added to the note pitch: the
    /// stored audio sits `-coarse - fine` from root_key at 44.1 kHz (measured to < 0.1 cent)
    std::int8_t coarse = 0;
    std::int8_t fine = 0;        ///< byte 2 & 0x7F as 7-bit signed, × 25/32 cents
    /// byte 3 & 0x7F as 7-bit signed: extra tune of s7 × 75/64 cents applied only while the
    /// play position is ≥ 0 (inside the loop)
    std::int8_t loop_fine = 0;
    std::uint8_t level = 0;      ///< byte 4: level attenuation offset, 1 unit = 1/16 octave
    SampleFormat format = SampleFormat::Pcm16;
    std::uint8_t predictor = 0;  ///< byte 7 & 0x1F; & 3 selects the DPCM coefficient pair
    bool reverse_loop = false;   ///< byte 6 bit 6: playback starts at loop_length - 1
    bool flag_b6 = false;        ///< byte 6 bit 7: selects the alternative block renderer
    std::uint32_t loop_start_byte = 0; ///< u32 @ 8, ×4: byte offset of sample index 0 (loop start)
    std::uint32_t pre_loop = 0;  ///< u32 @ 12: samples before the loop start (play from -pre_loop)
    std::uint32_t loop_length = 0; ///< u32 @ 16: samples from the loop start to the loop end

    /// coarse·100 + trunc(fine·25/32): the sample's term of the pitch sum, in cents.
    int tune_cents() const { return coarse * 100 + fine * 25 / 32; }

    static SampleHeader from_bytes(std::span<const std::uint8_t, 20> b);
};


/// One 16-byte key / velocity zone of a voice group (matched).
struct Zone {
    std::uint8_t key_high = 127, key_low = 0, vel_high = 127, vel_low = 0;
    std::uint8_t level = 0;       ///< byte 4: level attenuation offset, 1/16 octave
    std::uint8_t pan = 64;        ///< byte 5 & 0x7F: pan, 64 = centre, relative
    std::uint8_t condition = 0;   ///< byte 6: trigger mask (0 = normal note-on)
    std::uint32_t element = 0;    ///< u32 @ 12: element record index (seg3)
    std::array<std::uint8_t, 16> raw{};
};

/// A group of zones sharing key tracking (12-byte header + zones).
struct Group {
    std::int8_t transpose = 0;    ///< byte 2 & 0x7F as 7-bit signed semitones
    std::int8_t center_key = 60;  ///< byte 3
    std::uint8_t key_track = 0;   ///< byte 4 & 7: 0 = key, 5 = fixed center, 1-4 = 50/20/10/5 %
    std::uint8_t layer = 0;       ///< byte 0: compared across voices for legato / takeover
    std::int8_t fine = 0;         ///< byte 5 & 0x7F as 7-bit signed, × 25/32 cents
    std::uint8_t level = 0;       ///< byte 6: level attenuation offset, 1/16 octave
    std::uint8_t pan = 64;        ///< byte 7 & 0x7F: pan offset, 64 = centre
    /// u16 @ 8: seg5 row indexed by the scale key, raw signed cents added to pitch; 0xFFFF = none
    std::uint16_t pitch_curve = 0xFFFF;
    std::array<std::uint8_t, 12> raw{};
    std::vector<Zone> zones;
};

/// A voice record (seg2): 16-byte header, then `groups`.
struct Voice {
    std::uint8_t type = 0;        ///< byte 0 (3 and 4 use the element's extension index)
    std::uint8_t flags = 0;       ///< byte 1: bit 0 = drum voice (drum setup params apply)
    std::uint16_t velocity_curve = 0xFFFF; ///< u16 @ 4: seg4 velocity remap row
    std::uint16_t drum_kit = 0xFFFF; ///< u16 @ 6: seg8 drum setup kit
    std::uint8_t level = 0;       ///< byte 14: level attenuation offset, 1/16 octave
    /// byte 15 >> 4: damper mode of the amp EG: 3 = release rate switches at
    /// pedal 64, other modes interpolate C[5]..C[6] by the pedal value (half damper)
    std::uint8_t damper_mode = 0;
    bool is_drum() const { return flags & 1; }
    std::array<std::uint8_t, 16> header{};
    std::vector<Group> groups;
};

/// An element record (seg3): 12-byte header + four parameter blocks. Each block is stored
/// inline or as a 4-byte big-endian reference (bit 31 set) to another element record whose
/// block is used instead (resolved here, do).
struct Element {
    static constexpr std::array<std::size_t, 4> kBlockSizes = {16, 28, 36, 84};
    std::uint16_t extension = 0xFFFF; ///< u16 @ 4: seg9 record (voice types 3 / 4)
    std::uint16_t sample = 0;         ///< u16 @ 6: sample header index (seg6)
    std::uint8_t level = 0;           ///< byte 0: level attenuation offset
    std::uint16_t delay = 0;          ///< u16 @ 2 & 0x7FF: key-on delay in 10 ms units, ≤ 2000
    std::uint16_t pan_curve = 0xFFFF; ///< u16 @ 8: seg5 pan curve by scale key
    std::uint8_t pan_curve_depth = 0; ///< byte 10 >> 4: pan curve scale (term = T[b]·(d+1) >> 4)
    std::array<std::uint8_t, 12> header{};
    std::array<std::span<const std::uint8_t>, 4> blocks; ///< resolved parameter blocks

    LfoBlock lfo() const { return LfoBlock::from_bytes(blocks[0].first<16>()); }
    PitchEgBlock pitch() const { return PitchEgBlock::from_bytes(blocks[1].first<28>()); }
    AmpEgBlock amp() const { return AmpEgBlock::from_bytes(blocks[2].first<36>()); }
    FilterBlock filter() const { return FilterBlock::from_bytes(blocks[3].first<84>()); }
};

/// A zone that answers a note: what reports per match.
struct ZoneMatch {
    const Group* group = nullptr;
    const Zone* zone = nullptr;
    std::uint8_t zone_key = 0;  ///< key after key tracking + transpose (used for matching)
    std::uint8_t played_key = 0; ///< played key + transpose
    std::uint8_t cents = 0;      ///< key-tracking remainder in cents
};

/// the effective key a group matches zones with, plus the cents remainder.
std::pair<std::uint8_t, std::uint8_t> group_key(const dll::Tables& tb, const Group& g, std::uint8_t key);
/// every zone of `voice` that answers (key, velocity, condition).
std::vector<ZoneMatch> find_zones(const dll::Tables& tb, const Voice& voice, std::uint8_t key, std::uint8_t velocity,
                                  std::uint8_t condition = 0);

class VoiceTable {
public:
    /// Parse (and validate) a voice table image; the bytes must outlive this object. A
    /// malformed table sets `error()` (as does a malformed record read later); after an error
    /// use nothing but `error()`.
    explicit VoiceTable(std::span<const std::uint8_t> file);
    /// The first problem found, or nullptr.
    const char* error() const { return error_; }

    static constexpr std::size_t kSegments = 10;

    std::optional<std::uint8_t> bank_id(std::uint8_t msb, std::uint8_t lsb) const;
    std::optional<std::uint16_t> voice_index(std::uint8_t bank, std::uint8_t program) const;
    /// Raw voice / element records (variable length).
    std::size_t voice_count() const { return voice_offsets_.size(); }
    std::span<const std::uint8_t> voice_record(std::size_t index) const;
    std::size_t element_count() const { return element_offsets_.size(); }
    std::span<const std::uint8_t> element_record(std::size_t index) const;

    Voice voice(std::size_t index) const;
    Element element(std::size_t index) const;

    std::size_t sample_count() const;
    SampleHeader sample_header(std::size_t index) const;
    /// 24-byte drum setup entry for drum kit `kit`, note 13..91.
    std::optional<std::span<const std::uint8_t, 24>> drum_entry(std::size_t kit, std::uint8_t note) const;
    /// 128-byte row of seg4 / seg5.
    std::span<const std::uint8_t, 128> seg4_row(std::size_t row) const;
    std::span<const std::uint8_t, 128> seg5_row(std::size_t row) const;

    std::optional<std::span<const std::uint8_t>> segment(std::size_t k) const;

private:
    std::span<const std::uint8_t> file_;
    std::array<std::uint32_t, kSegments> offsets_{};
    std::vector<std::uint32_t> voice_offsets_, element_offsets_;
    mutable const char* error_ = nullptr;
    void fail(const char* what) const;
};

/// Decode a sample from the decrypted wave table into floats in [-1, 1), from index
/// -pre_loop to loop_length - 1. Returns nullopt for formats not implemented yet (DPCM needs
/// per-voice clamp limits; 24/32-bit unseen) or if the sample runs past the wave table.
std::optional<std::vector<float>> decode_sample(std::span<const std::uint8_t> wave,
                                                const SampleHeader& h);

} // namespace hvyxg::syxg2006le::tbl
