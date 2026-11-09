// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// MU ROM set detection (find_mu_roms: directories, CRC-32 recognition, error reporting) on
// fuzzed files, and the ROM decoders that run before any CRC check could matter: the
// S-MU2000 flash split, the 16-bit interleave / dejumble and the table decoder on a fuzzed
// program image (they must throw, not crash).
// Input: [mode] then files [u16 len] [bytes…] (at most 4).
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "fuzz_support.hpp"
#include "hvyxg/mu_rom.hpp"
#include "syxg50/mu_rom/decode.hpp"
#include "syxg50/mu_rom/py.hpp"

namespace fs = std::filesystem;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    fuzz::Input in(data, size);
    const std::uint8_t mode = in.u8();
    static const fs::path dir = [] {
        const fs::path d = fs::temp_directory_path() / ("hvyxg-fuzz-mu-" + std::to_string(::getpid()));
        fs::create_directories(d);
        return d;
    }();
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) fs::remove_all(e.path(), ec);
    std::vector<std::vector<std::uint8_t>> files;
    hvyxg::MuRomOptions o;
    o.model = static_cast<hvyxg::MuRomModel>(mode % 4);
    for (int i = 0; i < 4 && !in.empty(); ++i) {
        const auto b = in.take(in.u16() % 4096);
        files.emplace_back(b.begin(), b.end());
        const fs::path p = dir / ((mode & 0x10) ? "sub" : "") / ("rom" + std::to_string(i) + ".bin");
        fs::create_directories(p.parent_path(), ec);
        std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        if (mode & 0x20) o.roms.push_back(p);
    }
    if (!(mode & 0x20) || o.roms.empty()) o.roms.push_back(dir);
    if (mode & 0x40) o.roms.push_back(dir / "missing");
    try {
        (void)hvyxg::find_mu_roms(o);
    } catch (const hvyxg::BankError&) {
    }
    using namespace hvyxg::syxg50::mu_rom;
    if (files.empty()) return 0;
    try {
        const auto [hi, lo] = mu2000_flash_split(files[0]);
        (void)hi;
        (void)lo;
    } catch (const ConvertError&) {
    } catch (const std::out_of_range&) {
    }
    try {
        Bytes image = dejumble(interleave16(files[0], files.size() > 1 ? files[1] : files[0]));
        if (mode & 0x80) {
            elemreduce::EnvIds ids;
            const Engine engine = static_cast<Engine>((mode >> 2) % 3);
            if (engine == Engine::Mu2000) mu2000_scaling(image);
            (void)create_table(engine, image, (mode & 1) != 0, files.size() > 2 ? files[2] : files[0], ids);
        }
    } catch (const ConvertError&) {
    } catch (const std::out_of_range&) {
    }
    return 0;
}
