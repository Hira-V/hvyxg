// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Translated from SXG-Create-NF, Copyright (c) 2026 Soundshock, BSD-3-Clause (THIRD_PARTY.md)
// MU ROM sets: recognition of ROM images in memory (CRC-32 lists of SXG-Create-NF
// utility/dataCRCs.py) and conversion (convert.py's MU1000 / MU128 / MU2000 path). The search
// through files and the table cache are in mu_rom_files.cpp.
#include "hvyxg/mu_rom.hpp"

#include <algorithm>
#include <cstdio>
#include <map>

#include "engine_tables.hpp"
#include "fail.hpp"
#include "hvyxg/engine_tables.hpp"
#include "syxg50/mu_rom/rom_sets.hpp"
#if HVYXG_MU_CONVERTER
#include "syxg50/mu_rom/decode.hpp"
#include "syxg50/mu_rom/make_table.hpp"
#endif

#if HVYXG_MU_CONVERTER

namespace hvyxg::mu_rom_sets {

using namespace syxg50::mu_rom;

const char* model_name(MuRomModel m) {
    switch (m) {
    case MuRomModel::Mu1000: return "MU1000";
    case MuRomModel::Mu2000: return "MU2000";
    case MuRomModel::Mu128: return "MU128";
    default: return "auto";
    }
}

std::pair<Bytes, Bytes> split_program(std::span<const std::uint8_t> b, MuProgramForm form) {
    if (form == MuProgramForm::InterleavedSwapped) return mu2000_flash_split(b);
    Bytes x, y;
    if (form == MuProgramForm::Concatenated) {
        x.assign(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(b.size() / 2));
        y.assign(b.begin() + static_cast<std::ptrdiff_t>(b.size() / 2), b.end());
    } else {
        for (std::size_t i = 0; i + 3 < b.size(); i += 4) {
            x.insert(x.end(), {b[i], b[i + 1]});
            y.insert(y.end(), {b[i + 2], b[i + 3]});
        }
    }
    return {std::move(x), std::move(y)};
}

MuTables convert_set(const SetSpec& spec, std::span<const std::uint8_t> high, std::span<const std::uint8_t> low,
                     std::span<const std::span<const std::uint8_t>> waves, const EngineTables& tables) {
    Bytes image;
    {
        const Bytes prg = interleave16(high, low);
        image = dejumble(prg);
    }
    Engine engine = Engine::Mu1000;
    if (spec.model == MuRomModel::Mu2000) {
        engine = Engine::Mu2000;
        mu2000_scaling(image);
    } else if (spec.model == MuRomModel::Mu128) {
        engine = Engine::Mu128;
    }
    Bytes waverom;
    for (std::size_t i = 0; i + 1 < waves.size(); i += 2) {
        const Bytes w = interleave16(waves[i], waves[i + 1]);
        waverom.insert(waverom.end(), w.begin(), w.end());
    }
    elemreduce::EnvIds ids;
    Table table = create_table(engine, image, false, waverom, ids);
    Table alt = create_table(engine, image, true, waverom, ids);
    syxg50::mu_rom::Tables t = make_syxg50(tables.syxg50, table, alt, waverom, spec.version, spec.upcm_name);
    return MuTables{std::move(t.tbl), std::move(t.upcm), spec.tbl_name, spec.upcm_name};
}

} // namespace hvyxg::mu_rom_sets

namespace hvyxg {

using namespace mu_rom_sets;

bool convert_mu_roms(std::span<const std::span<const std::uint8_t>> images, MuRomModel model,
                     const EngineTables& tables, MuTables& out, Error& error) {
    using syxg50::mu_rom::crc32;
    // The drum velocity conversion reads the S-YXG50 FEG velocity curve from the DLL.
    if (!check_engine_tables(tables, Model::Syxg50, error)) return false;
    std::vector<std::uint32_t> crc(images.size());
    for (std::size_t i = 0; i < images.size(); ++i) crc[i] = crc32(images[i].data(), images[i].size());
    const auto find = [&](std::uint32_t c) -> const std::span<const std::uint8_t>* {
        for (std::size_t i = 0; i < images.size(); ++i)
            if (crc[i] == c) return &images[i];
        return nullptr;
    };
    const auto known = [&](std::uint32_t c) {
        for (const SetSpec& s : kSpecs) {
            for (const RomId& r : s.program)
                if (r.crc == c) return true;
            for (const RomId& r : s.wave)
                if (r.crc == c) return true;
        }
        return std::any_of(kUnsupported.begin(), kUnsupported.end(), [&](const RomId& u) { return u.crc == c; });
    };
    // Joined program ROMs: an image of no known CRC whose halves, split as concatenated,
    // interleaved or interleaved byte-swapped (in either order), are a known high / low pair.
    struct Joined {
        std::size_t image;
        MuProgramForm form;
        bool low_first;
    };
    std::map<std::uint32_t, Joined> joined;  // by the high half's CRC
    for (std::size_t i = 0; i < images.size(); ++i) {
        if (known(crc[i]) || images[i].size() % 4 != 0 || images[i].size() < (1u << 20)) continue;
        for (const MuProgramForm form : {MuProgramForm::Concatenated, MuProgramForm::Interleaved,
                                         MuProgramForm::InterleavedSwapped}) {
            const auto [x, y] = split_program(images[i], form);
            const std::uint32_t cx = crc32(x.data(), x.size()), cy = crc32(y.data(), y.size());
            for (const SetSpec& s : kSpecs) {
                const std::uint32_t h = s.program[0].crc, l = s.program[1].crc;
                if (cx == h && cy == l) joined.emplace(h, Joined{i, form, false});
                else if (cx == l && cy == h) joined.emplace(h, Joined{i, form, true});
            }
        }
    }
    std::string missing;
    for (const MuRomModel want : {MuRomModel::Mu1000, MuRomModel::Mu2000, MuRomModel::Mu128}) {
        if (model != MuRomModel::Auto && model != want) continue;
        for (const SetSpec& s : kSpecs) {
            if (s.model != want) continue;
            const auto* hi = find(s.program[0].crc);
            const auto* lo = find(s.program[1].crc);
            const auto j = joined.find(s.program[0].crc);
            std::array<std::span<const std::uint8_t>, 4> waves;
            bool wav = true;
            for (std::size_t k = 0; k < s.wave.size(); ++k) {
                const auto* w = find(s.wave[k].crc);
                if (w) waves[k] = *w;
                else wav = false;
            }
            const bool pair = hi && lo;
            if ((pair || j != joined.end()) && wav) {
#if HVYXG_EXCEPTIONS
                try {
#endif
                    if (pair) {
                        out = convert_set(s, *hi, *lo, waves, tables);
                    } else {
                        auto [a, b] = split_program(images[j->second.image], j->second.form);
                        if (j->second.low_first) std::swap(a, b);
                        out = convert_set(s, a, b, waves, tables);
                    }
#if HVYXG_EXCEPTIONS
                } catch (const std::exception& e) {
                    error = {ErrorKind::Table, std::string("MU ROMs: ") + s.name + ": conversion failed: " + e.what()};
                    return false;
                }
#endif
                return true;
            }
            if (hi || lo || j != joined.end())
                missing += std::string(missing.empty() ? "" : "; ") + s.name + " is incomplete";
        }
    }
    error = {ErrorKind::Io, missing.empty() ? std::string("MU ROMs: no complete ") +
                                                  (model == MuRomModel::Auto ? "MU1000 / MU2000 / MU128" : model_name(model)) +
                                                  " ROM set among the images"
                                            : "MU ROMs: " + missing};
    return false;
}

} // namespace hvyxg

#else  // !HVYXG_MU_CONVERTER

namespace hvyxg {

bool convert_mu_roms(std::span<const std::span<const std::uint8_t>>, MuRomModel, const EngineTables&, MuTables&,
                     Error& error) {
    error = {ErrorKind::Io, "MU ROMs: the converter needs 128-bit integers (__int128, or HVYXG_INT128_HEADER); "
                            "convert elsewhere (render_yxg --mu-cache, or SXG-Create-NF) and give the .TBL / .UPCM tables"};
    return false;
}

} // namespace hvyxg

#endif
