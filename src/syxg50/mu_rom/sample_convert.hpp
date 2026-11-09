// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Translated from SXG-Create-NF, Copyright (c) 2026 Soundshock, BSD-3-Clause (THIRD_PARTY.md)
#pragma once
// Sample conversion of SXG-Create-NF's utility/SampleConvert.py (BSD-3-Clause, Copyright 2026
// Soundshock; its DPCM decoder follows MAME, BSD-3-Clause, Olivier Galibert): MU90-family
// 8 / 12 / 16-bit and DPCM samples → the S-YXG50's unsigned 8 / 16-bit formats, with the
// reversed, high-passed and EG-baked variants, and the wave file layout (SampleMonster).

#include <cstdint>
#include <span>
#include <tuple>
#include <vector>

#include "syxg50/mu_rom/model.hpp"

namespace hvyxg::syxg50::mu_rom {

/// SampleConvert.ConvertSample (MU90 source, S-YXG50 target). Throws ConvertError where the
/// Python code raises.
Bytes convert_sample(const Sample& s, std::span<const std::uint8_t> waverom);

/// SampleConvert.SampleMonster: lays the converted samples out in the new wave file.
class SampleMonster {
public:
    SampleMonster(std::span<const std::uint8_t> waverom, bool big) : waverom_(waverom), big_(big) {}
    /// Returns (new loop address + 1 sample, target format) and queues the sample.
    std::pair<std::int64_t, Fmt> feed(const SampleP& s);
    /// The wave file.
    Bytes generate();
    std::size_t queued() const { return queue_.size(); }

private:
    struct Item {
        std::int64_t at;
        SampleP sample;
        std::int64_t new_loop;
    };
    std::span<const std::uint8_t> waverom_;
    bool big_;
    std::vector<Item> queue_;
    std::int64_t rom_end_ = 0;
    std::int64_t adpcm_reserve_ = 0x400000;
};

} // namespace hvyxg::syxg50::mu_rom
