// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "syxg2006le/bank_data.hpp"

#include <algorithm>
#include <cstring>

namespace hvyxg::syxg2006le {

using namespace tbl;

BankData::BankData(std::span<const std::uint8_t> bin, std::span<const std::uint8_t> wave)
    : bin_(bin),
      // Little-endian samples, read in place (the loader checks the alignment).
      wave_(reinterpret_cast<const std::int16_t*>(wave.data()), wave.size() / 2),
      table_(bin_) {
    voices_.reserve(table_.voice_count());
    if (table_.error()) return;
    for (std::size_t v = 0; v < table_.voice_count() && !table_.error(); ++v) voices_.push_back(table_.voice(v));
    if (table_.error()) return;
    elements_.reserve(table_.element_count());
    const auto wave_len = static_cast<std::int64_t>(wave_.size());
    for (std::size_t e = 0; e < table_.element_count(); ++e) {
        ElementData d;
        d.element = table_.element(e);
        if (table_.error()) return;
        d.lfo = d.element.lfo();
        d.peg = d.element.pitch();
        d.amp = d.element.amp();
        d.filter = d.element.filter();
        if (d.element.sample < table_.sample_count()) {
            d.sample = table_.sample_header(d.element.sample);
            d.sample_base = d.sample.loop_start_byte / 2;
            d.playable = d.sample.format == SampleFormat::Pcm16 && d.sample.loop_length > 0 &&
                         d.sample_base - static_cast<std::int64_t>(d.sample.pre_loop) >= 0 &&
                         d.sample_base + d.sample.loop_length <= wave_len;
        }
        elements_.push_back(d);
    }
    if (const auto s4 = table_.segment(4)) seg4_rows_ = s4->size() / 128;
    if (const auto s5 = table_.segment(5)) seg5_rows_ = s5->size() / 128;
}

std::optional<std::uint16_t> BankData::find_voice(std::uint8_t msb, std::uint8_t lsb,
                                                  std::uint8_t program) const {
    const auto bank = table_.bank_id(msb, lsb);
    if (!bank) return std::nullopt;
    return table_.voice_index(*bank, program);
}

int BankData::curve(std::uint16_t row, int index) const {
    if (row == kNoCurve || row >= seg5_rows_) return 0;
    return curve_value(table_.seg5_row(row)[static_cast<std::size_t>(std::clamp(index, 0, 127))]);
}

int BankData::raw_curve(std::uint16_t row, int index) const {
    if (row == kNoCurve || row >= seg5_rows_) return 0;
    return static_cast<std::int8_t>(table_.seg5_row(row)[static_cast<std::size_t>(index & 0x7F)]);
}

std::optional<std::uint8_t> BankData::velocity_remap(std::uint16_t row,
                                                     std::uint8_t velocity) const {
    if (row == kNoCurve || row >= seg4_rows_) return std::nullopt;
    return table_.seg4_row(row)[velocity & 0x7F];
}

} // namespace hvyxg::syxg2006le
