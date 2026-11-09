// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Synth::midi (channel messages, XG / GS / universal SysEx, malformed messages, frame
// offsets, ports) and rendering. Input: [config] [bank] [program…] (fuzz_support.cpp, `play`).
#include <cstddef>
#include <cstdint>

#include "fuzz_support.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    fuzz::Input in(data, size);
    const std::uint8_t config = in.u8();
    const int which = in.u8() % 3;
    hvyxg::Synth synth(fuzz::options(config, fuzz::bank(which)));
    fuzz::play(synth, in);
    return 0;
}
