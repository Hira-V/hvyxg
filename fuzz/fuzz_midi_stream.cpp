// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Synth::midi_stream: raw byte streams (running status, real-time bytes, SysEx split across
// calls, stray data / status bytes) with renders in between.
// Input: [config] [bank] then chunks [len | flags] [bytes…].
#include <cstddef>
#include <cstdint>

#include "fuzz_support.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    fuzz::Input in(data, size);
    const std::uint8_t config = in.u8();
    const int which = in.u8() % 3;
    hvyxg::Synth synth(fuzz::options(config, fuzz::bank(which)));
    std::size_t budget = 32768;
    while (!in.empty()) {
        const std::uint8_t h = in.u8();
        const auto chunk = in.take(h & 0x3F);
        synth.midi_stream(chunk, (h & 0x40) ? 100 : 0, (h & 0x80) ? 1 : 0);
        if (h & 0x80 && budget >= 200) {
            fuzz::render(synth, 200);
            budget -= 200;
        }
    }
    fuzz::render(synth, 1024);
    return 0;
}
