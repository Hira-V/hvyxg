// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// The loaded S-YXG2006LE bank in the form the voice engine reads it: the voice table with
// every voice and element record parsed once at load (no parsing on the audio thread), and
// the decrypted 16-bit wave data.

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "syxg2006le/tbl/param_blocks.hpp"
#include "syxg2006le/tbl/voice_table.hpp"

namespace hvyxg::syxg2006le {

/// An element record with its four parameter blocks and its sample header decoded.
struct ElementData {
    syxg2006le::tbl::Element element;
    syxg2006le::tbl::LfoBlock lfo;
    syxg2006le::tbl::PitchEgBlock peg;
    syxg2006le::tbl::AmpEgBlock amp;
    syxg2006le::tbl::FilterBlock filter;
    syxg2006le::tbl::SampleHeader sample;
    /// Index of sample index 0 (the loop start) in `BankData::wave()`; playable only when the
    /// sample is 16-bit and lies inside the wave table.
    std::int64_t sample_base = 0;
    bool playable = false;
};

class BankData {
public:
    /// Parse a voice table and a decrypted wave table (the caller's memory, 2-byte aligned; read
    /// in place). A malformed voice table sets `error()`; the object is then unusable.
    BankData(std::span<const std::uint8_t> bin, std::span<const std::uint8_t> wave);
    BankData(const BankData&) = delete;
    BankData& operator=(const BankData&) = delete;

    const syxg2006le::tbl::VoiceTable& table() const { return table_; }
    /// The voice table's first problem, or nullptr.
    const char* error() const { return table_.error(); }
    const syxg2006le::tbl::Voice& voice(std::size_t i) const { return voices_[i]; }
    std::size_t voice_count() const { return voices_.size(); }
    const ElementData* element(std::size_t i) const {
        return i < elements_.size() ? &elements_[i] : nullptr;
    }
    std::span<const std::int16_t> wave() const { return wave_; }

    /// seg0 / seg1: (bank MSB, LSB, program) → voice index, without fallbacks.
    std::optional<std::uint16_t> find_voice(std::uint8_t msb, std::uint8_t lsb,
                                            std::uint8_t program) const;

    /// seg5 curve value `T[row][index]`; 0 without a curve.
    int curve(std::uint16_t row, int index) const;
    /// seg5 row `row` at `index` as a raw signed byte (the group pitch curve, in cents); 0
    /// without that row.
    int raw_curve(std::uint16_t row, int index) const;
    /// seg4 velocity remap row `row` at `velocity`, or nullopt without that row.
    std::optional<std::uint8_t> velocity_remap(std::uint16_t row, std::uint8_t velocity) const;

private:
    std::span<const std::uint8_t> bin_;
    std::span<const std::int16_t> wave_;
    syxg2006le::tbl::VoiceTable table_;
    std::vector<syxg2006le::tbl::Voice> voices_;
    std::vector<ElementData> elements_;
    std::size_t seg4_rows_ = 0;
    std::size_t seg5_rows_ = 0;
};

} // namespace hvyxg::syxg2006le
