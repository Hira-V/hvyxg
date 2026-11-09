// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Writes the seed corpora of the fuzz targets into DIR/<target>/ (all synthetic: banks built
// from scratch, hand-written MIDI). usage: hvyxg_fuzz_seeds DIR
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#include "fuzz_support.hpp"

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

namespace {

void write(const fs::path& dir, const std::string& name, const Bytes& b) {
    fs::create_directories(dir);
    std::ofstream(dir / name, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

Bytes bank_input(std::initializer_list<std::uint8_t> head, const Bytes& bin, const Bytes& wave) {
    Bytes b(head);
    b.push_back(static_cast<std::uint8_t>(bin.size()));
    b.push_back(static_cast<std::uint8_t>(bin.size() >> 8));
    b.push_back(static_cast<std::uint8_t>(bin.size() >> 16));
    b.insert(b.end(), bin.begin(), bin.end());
    b.insert(b.end(), wave.begin(), wave.end());
    return b;
}

void vlq(Bytes& b, std::uint32_t v) {
    Bytes t{static_cast<std::uint8_t>(v & 0x7F)};
    while (v >>= 7) t.insert(t.begin(), static_cast<std::uint8_t>(0x80 | (v & 0x7F)));
    b.insert(b.end(), t.begin(), t.end());
}

Bytes smf(std::uint16_t format, std::uint16_t division, const std::vector<Bytes>& tracks) {
    Bytes b{'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, static_cast<std::uint8_t>(format), 0, static_cast<std::uint8_t>(tracks.size()),
            static_cast<std::uint8_t>(division >> 8), static_cast<std::uint8_t>(division)};
    for (const Bytes& t : tracks) {
        b.insert(b.end(), {'M', 'T', 'r', 'k'});
        const auto n = static_cast<std::uint32_t>(t.size());
        b.insert(b.end(), {static_cast<std::uint8_t>(n >> 24), static_cast<std::uint8_t>(n >> 16), static_cast<std::uint8_t>(n >> 8),
                           static_cast<std::uint8_t>(n)});
        b.insert(b.end(), t.begin(), t.end());
    }
    return b;
}

Bytes track(std::initializer_list<std::pair<std::uint32_t, Bytes>> events) {
    Bytes t;
    for (const auto& [dt, e] : events) {
        vlq(t, dt);
        t.insert(t.end(), e.begin(), e.end());
    }
    return t;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s DIR\n", argv[0]);
        return 2;
    }
    const fs::path root = argv[1];
    const Bytes b50 = fuzz::syxg50_bin(false), big = fuzz::syxg50_bin(true), w50 = fuzz::syxg50_wave();
    const Bytes ble = fuzz::syxg2006le_bin(), wle = fuzz::syxg2006le_wave();
    for (std::uint8_t s : {0x00, 0x01, 0x42, 0x8A})
        write(root / "fuzz_syxg50_bank", "seed" + std::to_string(s), bank_input({s}, b50, w50));
    for (std::uint8_t s : {0x00, 0x02, 0x81})
        write(root / "fuzz_syxg2006le_bank", "seed" + std::to_string(s), bank_input({s}, ble, wle));
    write(root / "fuzz_mu_bank", "big", bank_input({0x00, 0x00}, big, w50));
    write(root / "fuzz_mu_bank", "big-basic", bank_input({0x05, 0x08}, big, w50));
    write(root / "fuzz_mu_bank", "classic", bank_input({0x02, 0x01}, b50, w50));
    write(root / "fuzz_mu_bank", "forced-big", bank_input({0x03, 0x02}, b50, w50));

    // MIDI programs (fuzz_support.cpp `play`): op % 10 picks the kind.
    const std::vector<std::pair<std::string, Bytes>> progs = {
        {"notes", {0x00, 0x00, 0x00, 0x10, 0xC0, 0x00, 0x00, 0x90, 60, 100, 0x01, 0x99, 36, 100, 0x08, 200, 0x00, 0x80, 60, 0, 0x08, 100}},
        {"xg-param", {0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x7E, 0x00, 0x02, 0x00, 0x08, 0x00, 0x07, 0x00, 0x00, 0x90, 64, 90,
                      0x02, 0x00, 0x02, 0x01, 0x40, 0x05, 0x00, 0x02, 0x00, 0x02, 0x01, 0x5A, 0x01, 0x08, 0xFF}},
        {"insertion", {0x00, 0x00, 0x02, 0x00, 0x03, 0x03, 0x00, 0x00, 0x05, 0x00, 0x02, 0x00, 0x03, 0x03, 0x00, 0x0C, 0x00, 0x00,
                       0x00, 0x90, 50, 127, 0x08, 0xFF, 0x08, 0xFF}},
        {"bulk", {0x01, 0x00, 0x03, 20, 0x00, 0x08, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
                  0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x00, 0x10, 60, 100, 0x08, 50}},
        {"gs", {0x00, 0x01, 0x04, 0x10, 0x00, 0x7F, 0x01, 0x00, 0x04, 0x10, 0x11, 0x15, 0x01, 0x01, 0x00, 0x19, 0x50, 0x70,
                0x08, 0x50, 0x84, 0x10, 0x41, 0x01, 0x30, 0x02, 0x04, 0x05}},
        {"universal", {0x00, 0x00, 0x05, 0x7F, 0x02, 0x09, 0x01, 0x85, 0x7F, 0x04, 0x04, 0x01, 0x00, 0x40, 0x00, 0x10, 70, 80, 0x08, 30}},
        {"raw", {0x02, 0x00, 0x06, 5, 0xF0, 0x43, 0x10, 0x4C, 0xF7, 0x86, 0x10, 0x00, 3, 0x90, 64, 64, 0x08, 0x20, 0x09, 0xF9, 0x08, 0x10}},
        {"stream", {0x00, 0x01, 0x07, 12, 0x90, 60, 100, 62, 100, 0xF8, 64, 100, 0xF0, 0x7E, 0x7F, 0x08, 50, 0x07, 3, 0x09, 0x01, 0xF7}},
        {"mu", {0x04, 0x02, 0x00, 0x30, 0x00, 0x00, 0x00, 0x10, 0x20, 0x01, 0x00, 0x10, 0xC0, 0x05, 0x01, 0x10, 0x90, 0x40, 0x7F, 0x08, 0x80}},
        {"poly", {0x0C, 0x00, 0x01, 0x10, 0x90, 40, 100, 0x01, 0x11, 0x90, 52, 100, 0x01, 0x12, 0x90, 64, 100, 0x01, 0x13, 0x90, 76, 100,
                  0x08, 0x40, 0x01, 0x30, 0xB0, 120, 0x00}},
    };
    for (const auto& [name, p] : progs) write(root / "fuzz_midi", name, p);
    const std::vector<std::pair<std::string, Bytes>> streams = {
        {"running", {0x00, 0x00, 0x87, 0x90, 60, 100, 64, 100, 67}, },
        {"split-sysex", {0x00, 0x01, 0x05, 0xF0, 0x43, 0x10, 0x4C, 0x00, 0x86, 0x00, 0x7E, 0x00, 0xF7, 0x00, 0x83, 0xC0, 0x05, 0x10}},
        {"realtime", {0x00, 0x00, 0x88, 0x90, 0xF8, 60, 0xFE, 100, 0xB0, 0x07, 0x40}},
        {"garbage", {0x01, 0x02, 0x8A, 0x45, 0xF7, 0xF0, 0x90, 0xF3, 60, 0xF1, 0x12, 0x80, 0x40}},
    };
    for (const auto& [name, p] : streams) write(root / "fuzz_midi_stream", name, p);

    const Bytes on{0x90, 60, 100}, off{0x80, 60, 0};
    write(root / "fuzz_smf", "format0", smf(0, 480, {track({{0, {0xFF, 0x51, 3, 0x07, 0xA1, 0x20}}, {0, on}, {480, {60, 0}},
                                                            {0, {0xF0, 3, 0x7E, 0x7F, 0xF7}}, {10, {0xFF, 0x2F, 0}}})}));
    write(root / "fuzz_smf", "format1", smf(1, 96, {track({{0, {0xFF, 0x51, 3, 0x03, 0xD0, 0x90}}, {200, {0xFF, 0x51, 3, 0x0F, 0x42, 0x40}}}),
                                                     track({{0, {0xC0, 5}}, {0, on}, {96, off}, {0, {0xF7, 2, 0x43, 0x12}}, {0, {0xFF, 0x2F, 0}}})}));
    write(root / "fuzz_smf", "smpte", smf(0, 0xE728, {track({{0, on}, {40, off}, {0, {0xFF, 0x2F, 0}}})}));

    // MU ROM detection: mode byte, then files [u16 len][bytes].
    Bytes mu{0x20};
    for (int f = 0; f < 3; ++f) {
        mu.push_back(64);
        mu.push_back(0);
        for (int i = 0; i < 64; ++i) mu.push_back(static_cast<std::uint8_t>(i * (f + 3)));
    }
    write(root / "fuzz_mu_rom", "files", mu);
    mu[0] = 0x81;
    write(root / "fuzz_mu_rom", "decode", mu);
    return 0;
}
