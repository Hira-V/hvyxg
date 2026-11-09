// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Links the core alone (hvyxg_core_check: no exceptions, no RTTI, no files) against every entry
// point an embedded host uses, so a core source that still needs a hosted one fails the build.
// Run with a DLL, a bin and a wave file it loads them and renders a second; without, it only links.
#include <cstdio>
#include <vector>

#include "hvyxg/bank.hpp"
#include "hvyxg/engine_tables.hpp"
#include "hvyxg/mu_rom.hpp"
#include "hvyxg/synth.hpp"

int main(int argc, char** argv) {
    std::vector<unsigned char> mem(hvyxg::engine_tables_size() + hvyxg::kEngineTablesAlign);
    void* at = mem.data() + (hvyxg::kEngineTablesAlign - reinterpret_cast<std::uintptr_t>(mem.data()) % hvyxg::kEngineTablesAlign);
    hvyxg::EngineTables* t = hvyxg::init_engine_tables(at);
    const auto read = [](const char* p) {
        std::vector<std::uint8_t> b;
        if (FILE* f = std::fopen(p, "rb")) {
            for (int c; (c = std::fgetc(f)) != EOF;) b.push_back(static_cast<std::uint8_t>(c));
            std::fclose(f);
        }
        return b;
    };
    std::vector<std::uint8_t> dll, bin, wave;
    if (argc == 4) {
        dll = read(argv[1]);
        bin = read(argv[2]);
        wave = read(argv[3]);
    }
    hvyxg::load_engine_tables(*t, dll);
    bin.resize(bin.size() + hvyxg::BankMemory::kPad);
    wave.resize(wave.size() + hvyxg::BankMemory::kPad);
    hvyxg::Error e;
    hvyxg::MuTables mu;
    std::span<const std::uint8_t> none;
    hvyxg::convert_mu_roms(std::span(&none, 1), hvyxg::MuRomModel::Auto, *t, mu, e);
    hvyxg::BankOptions o;
    o.tables = std::shared_ptr<const hvyxg::EngineTables>(std::shared_ptr<void>{}, t);  // not owned
    const auto bank = hvyxg::load_bank(hvyxg::BankMemory{bin, wave}, hvyxg::Model::Auto, o, e);
    if (!bank) {
        std::printf("%s\n", argc == 4 ? e.message.c_str() : "linked");
        return argc == 4 ? 1 : 0;
    }
    hvyxg::Synth::Settings st;
    st.sample_rate = 48000.0f;
    hvyxg::Synth s(bank, st);
    s.ump(std::array<std::uint32_t, 1>{0x20903C64u});
    std::vector<std::int32_t> out(2 * 48000);
    s.render(out.data(), 48000);
    std::int32_t peak = 0;
    for (const auto v : out) peak = std::max(peak, v < 0 ? -v : v);
    std::printf("rendered, peak %d\n", static_cast<int>(peak));
    return peak > 0 ? 0 : 1;
}
