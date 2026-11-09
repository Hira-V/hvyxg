// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Standard MIDI File reader of the tools (tools/render/lib/midifile: Hira-V's, copied): any bytes
// either open into well-formed UMP events in time order or are refused, never read past.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "lib/midi.hpp"
#include "lib/midifile.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::int32_t err = 0;
    const std::unique_ptr<midifile::Song> song(midifile::open(data, static_cast<std::uint32_t>(size), &err));
    if (!song) return 0;
    std::uint64_t last = 0;
    for (std::uint32_t i = 0; i < song->count(); ++i) {
        const midifile::Event& e = song->event(i);
        const std::uint32_t t = midi::ump_type(e.ump[0]);
        if (e.us < last || (t != 2 && t != 3) || e.words != midi::ump_words(e.ump[0]) || song->port(i) > 15) {
            std::fprintf(stderr, "bad event %u: %llu us, type %u, %u words\n", i, static_cast<unsigned long long>(e.us), t, e.words);
            std::abort();
        }
        last = e.us;
    }
    if (song->length_us() < last) std::abort();
    return 0;
}
