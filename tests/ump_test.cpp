// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Universal MIDI Packets in (Synth::ump) and the 32-bit integer render, as an embedded host
// (Hira-V's midi::Instrument) uses them: each must sound exactly like the byte / float API.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "test_support.hpp"

using namespace hvyxg;

namespace {

/// A UMP 7-bit SysEx packet (type 3) of up to six data bytes.
std::array<std::uint32_t, 2> sysex_packet(std::uint32_t group, std::uint32_t status, std::span<const std::uint8_t> d) {
    std::uint8_t b[6] = {};
    for (std::size_t i = 0; i < d.size() && i < 6; ++i) b[i] = d[i];
    return {0x30000000u | group << 24 | status << 20 | static_cast<std::uint32_t>(d.size()) << 16 | b[0] << 8 | b[1],
            static_cast<std::uint32_t>(b[2]) << 24 | b[3] << 16 | b[4] << 8 | b[5]};
}

/// The SysEx body (between F0 and F7) as UMP packets: one complete packet, or start / continue / end.
std::vector<std::array<std::uint32_t, 2>> sysex_packets(std::uint32_t group, std::span<const std::uint8_t> body) {
    std::vector<std::array<std::uint32_t, 2>> out;
    if (body.size() <= 6) {
        out.push_back(sysex_packet(group, 0, body));
        return out;
    }
    for (std::size_t at = 0; at < body.size(); at += 6) {
        const std::size_t n = std::min<std::size_t>(6, body.size() - at);
        const std::uint32_t st = at == 0 ? 1 : at + n == body.size() ? 3 : 2;
        out.push_back(sysex_packet(group, st, body.subspan(at, n)));
    }
    return out;
}

std::vector<float> render(Synth& s, std::size_t frames) {
    std::vector<float> out(2 * frames);
    s.render(out.data(), frames);
    return out;
}

Synth make(const std::shared_ptr<const Bank>& bank) {
    Synth::Settings st;
    st.ports = 2;
    return Synth(bank, st);
}

} // namespace

TEST_CASE("ump: channel voice messages and SysEx play as the same bytes") {
    HVYXG_NEED_TABLES();
    const auto bank = test::bank(true);
    if (!bank) return;
    Synth a = make(bank), b = make(bank), c = make(bank);  // c: without the SysEx
    // XG reverb type Room 1 (02 01 00 = 02 00), a SysEx of 9 data bytes: three UMP packets.
    const std::vector<std::uint8_t> body = {0x43, 0x10, 0x4C, 0x02, 0x01, 0x00, 0x02, 0x00};
    std::vector<std::uint8_t> sx = {0xF0};
    sx.insert(sx.end(), body.begin(), body.end());
    sx.push_back(0xF7);
    a.midi(sx);
    for (const auto& p : sysex_packets(0, body)) b.ump(p);
    // Program change, a controller, notes on two ports at sample offsets.
    const std::uint8_t prog[] = {0xC0, 24}, cc[] = {0xB0, 91, 100}, on1[] = {0x90, 60, 100}, on2[] = {0x91, 67, 90};
    a.midi(prog);
    a.midi(cc);
    a.midi(on1, 10);
    a.midi(on2, 300, 1);
    b.ump(std::array<std::uint32_t, 1>{0x20C01800u});
    b.ump(std::array<std::uint32_t, 1>{0x20B05B64u});
    b.ump(std::array<std::uint32_t, 1>{0x20903C64u}, 10);
    b.ump(std::array<std::uint32_t, 1>{0x2191435Au}, 300);
    for (const auto& w : {0x20C01800u, 0x20B05B64u}) c.ump(std::array<std::uint32_t, 1>{w});
    c.ump(std::array<std::uint32_t, 1>{0x20903C64u}, 10);
    c.ump(std::array<std::uint32_t, 1>{0x2191435Au}, 300);
    const std::vector<float> rb = render(b, 22050);
    CHECK(render(a, 22050) == rb);
    CHECK(render(c, 22050) != rb);  // the SysEx took effect
    // A System Reset is an XG System On.
    static constexpr std::uint8_t kXgOn[] = {0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7};
    a.midi(kXgOn);
    b.ump(std::array<std::uint32_t, 1>{0x10FF0000u});
    a.midi(on1);
    b.ump(std::array<std::uint32_t, 1>{0x20903C64u});
    CHECK(render(a, 22050) == render(b, 22050));
}

TEST_CASE("ump: a continuation without a start and MIDI 2.0 messages are ignored") {
    HVYXG_NEED_TABLES();
    const auto bank = test::bank(true);
    if (!bank) return;
    Synth a = make(bank), b = make(bank);
    const std::uint8_t d[] = {0x43, 0x10, 0x4C};
    b.ump(sysex_packet(0, 2, d));                                         // continue, no start
    b.ump(sysex_packet(0, 3, d));                                         // end, no start
    b.ump(std::array<std::uint32_t, 2>{0x40903C00u, 0xFFFF0000u});         // MIDI 2.0 note on
    b.ump(std::array<std::uint32_t, 1>{0x20F80000u});                     // not a channel message
    const std::uint8_t on[] = {0x90, 60, 100};
    a.midi(on);
    b.ump(std::array<std::uint32_t, 1>{0x20903C64u});
    CHECK(render(a, 8192) == render(b, 8192));
}

TEST_CASE("ump: the 32-bit integer render is the float render at a 16-bit scale") {
    HVYXG_NEED_TABLES();
    const auto bank = test::bank(true);
    if (!bank) return;
    Synth a = make(bank), b = make(bank);
    const std::uint8_t on[] = {0x90, 48, 127}, on2[] = {0x90, 55, 127};
    for (Synth* s : {&a, &b}) {
        s->midi(on);
        s->midi(on2);
    }
    // An odd length, longer than the conversion's chunk.
    const std::size_t n = 44100 + 77;
    const std::vector<float> f = render(a, n);
    std::vector<std::int32_t> i(2 * n);
    b.render(i.data(), n);
    std::size_t same = 0;
    bool loud = false;
    for (std::size_t k = 0; k < f.size(); ++k) {
        same += i[k] == static_cast<std::int32_t>(std::lrint(f[k] * 32768.0f));
        loud = loud || std::abs(i[k]) > 1000;
    }
    CHECK(same == f.size());
    CHECK(loud);
}
