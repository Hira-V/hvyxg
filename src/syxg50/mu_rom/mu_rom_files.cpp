// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Translated from SXG-Create-NF, Copyright (c) 2026 Soundshock, BSD-3-Clause (THIRD_PARTY.md)
// MU ROM sets from files (hosted builds): the search by CRC-32 through files and directories,
// the conversion of a set found there, and the optional table cache.
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>

#include "engine_tables.hpp"
#include "bank_impl.hpp"
#include "bank_io.hpp"
#include "hvyxg/engine_tables.hpp"
#include "hvyxg/mu_rom.hpp"
#include "syxg50/bin_table.hpp"
#include "syxg50/mu_rom/py.hpp"
#include "syxg50/mu_rom/rom_sets.hpp"

namespace hvyxg {

namespace {

namespace fs = std::filesystem;
using namespace mu_rom_sets;
using syxg50::mu_rom::crc32;

/// Bump when the converter's output could change (it is part of the cache key).
constexpr const char* kConverterVersion = "sxg-create-nf-0086663/1";

std::string hex(std::uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08X", v);
    return b;
}

bool plausible_size(std::uintmax_t n) {
    // Known MU ROMs are 1, 2, 4 or 8 MB (plus 512 KB / 16 MB for safety).
    for (std::uintmax_t s = 512 * 1024; s <= 16 * 1024 * 1024; s *= 2)
        if (n == s) return true;
    return false;
}

std::uint32_t file_crc(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw BankError(BankError::Kind::Io, "MU ROMs: cannot open " + p.string());
    std::vector<char> buf(1 << 20);
    std::uint32_t crc = 0;
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto n = static_cast<std::size_t>(in.gcount());
        if (n == 0) break;
        crc = crc32(reinterpret_cast<const std::uint8_t*>(buf.data()), n, crc);
    }
    return crc;
}

Bytes read_rom(const fs::path& p) { return read_file(p, "MU ROM"); }

/// The high and low halves of a set's program ROM, from two files or one joined file.
std::pair<Bytes, Bytes> program_halves(const MuRomSet& set) {
    if (set.program_form == MuProgramForm::Pair) return {read_rom(set.program.at(0)), read_rom(set.program.at(1))};
    auto [a, b] = split_program(read_rom(set.program.at(0)), set.program_form);
    if (set.low_first) std::swap(a, b);
    return {std::move(a), std::move(b)};
}

std::vector<std::uint8_t> read_cached(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return {};
    return read_file(p, "MU table cache");
}

void write_atomic(const fs::path& p, const std::vector<std::uint8_t>& data) {
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw BankError(BankError::Kind::Io, "MU table cache: cannot write " + tmp.string());
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) throw BankError(BankError::Kind::Io, "MU table cache: write error in " + tmp.string());
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) throw BankError(BankError::Kind::Io, "MU table cache: cannot rename " + tmp.string() + ": " + ec.message());
}

} // namespace

MuRomSet find_mu_roms(const MuRomOptions& o) {
    if (o.roms.empty()) throw BankError(BankError::Kind::Io, "MU ROMs: no files or directories given");
    std::vector<fs::path> files;
    for (const fs::path& p : o.roms) {
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            std::vector<fs::path> found;
            for (fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end; it != end;
                 it.increment(ec)) {
                if (ec) break;
                std::error_code e2;
                if (it->is_regular_file(e2) && plausible_size(it->file_size(e2))) found.push_back(it->path());
            }
            std::sort(found.begin(), found.end());
            files.insert(files.end(), found.begin(), found.end());
        } else if (fs::is_regular_file(p, ec)) {
            files.push_back(p);
        } else {
            throw BankError(BankError::Kind::Io, "MU ROMs: no such file or directory: " + p.string());
        }
    }
    if (files.empty())
        throw BankError(BankError::Kind::Io, "MU ROMs: no ROM-sized files (512 KB, 1, 2, 4, 8 or 16 MB) found in the given directories");
    std::map<std::uint32_t, fs::path> by_crc;
    std::vector<std::pair<fs::path, std::uint32_t>> all;
    for (const fs::path& f : files) {
        const std::uint32_t c = file_crc(f);
        all.emplace_back(f, c);
        by_crc.emplace(c, f);  // the first file with that CRC
    }
    std::set<std::uint32_t> known;
    for (const SetSpec& s : kSpecs) {
        for (const RomId& r : s.program) known.insert(r.crc);
        for (const RomId& r : s.wave) known.insert(r.crc);
    }
    for (const RomId& u : kUnsupported) known.insert(u.crc);
    // Joined program ROMs: a file of no known CRC whose halves, split as concatenated,
    // interleaved or interleaved byte-swapped (in either order), are a known high / low pair.
    struct Joined {
        fs::path file;
        MuProgramForm form;
        bool low_first;
    };
    std::map<std::uint32_t, Joined> joined;  // by the high half's CRC
    for (const auto& [f, c] : all) {
        if (known.count(c)) continue;
        std::error_code ec;
        const auto size = fs::file_size(f, ec);
        if (ec || size % 4 != 0 || size < (1u << 20)) continue;
        const Bytes b = read_rom(f);
        for (const MuProgramForm form : {MuProgramForm::Concatenated, MuProgramForm::Interleaved,
                                         MuProgramForm::InterleavedSwapped}) {
            const auto [x, y] = split_program(b, form);
            const std::uint32_t cx = crc32(x.data(), x.size()), cy = crc32(y.data(), y.size());
            for (const SetSpec& s : kSpecs) {
                if (s.program.size() != 2) continue;
                const std::uint32_t h = s.program[0].crc, l = s.program[1].crc;
                if (cx == h && cy == l) joined.emplace(h, Joined{f, form, false});
                else if (cx == l && cy == h) joined.emplace(h, Joined{f, form, true});
            }
        }
    }
    const auto have = [&](const RomId& r) { return by_crc.count(r.crc) != 0; };
    std::string missing_msg;
    for (const MuRomModel want : {MuRomModel::Mu1000, MuRomModel::Mu2000, MuRomModel::Mu128}) {
        if (o.model != MuRomModel::Auto && o.model != want) continue;
        for (const SetSpec& s : kSpecs) {
            if (s.model != want) continue;
            const auto j = joined.find(s.program[0].crc);
            const bool from_joined = !std::all_of(s.program.begin(), s.program.end(), have) && j != joined.end();
            const bool prg = from_joined || std::all_of(s.program.begin(), s.program.end(), have);
            const bool any_prg = prg || std::any_of(s.program.begin(), s.program.end(), have);
            const bool wav = std::all_of(s.wave.begin(), s.wave.end(), have);
            if (prg && wav) {
                MuRomSet set;
                set.model = s.model;
                set.name = s.name;
                if (from_joined) {
                    set.program.push_back(j->second.file);
                    set.program_form = j->second.form;
                    set.low_first = j->second.low_first;
                }
                for (const RomId& r : s.program) {
                    if (!from_joined) set.program.push_back(by_crc.at(r.crc));
                    set.crcs.push_back(r.crc);
                }
                for (const RomId& r : s.wave) {
                    set.wave.push_back(by_crc.at(r.crc));
                    set.crcs.push_back(r.crc);
                }
                return set;
            }
            if (any_prg) {
                std::string miss;
                for (const RomId& r : s.program)
                    if (!have(r)) miss += std::string(miss.empty() ? "" : ", ") + "program ROM " + r.name + " (CRC32 " + hex(r.crc) + ")";
                for (const RomId& r : s.wave)
                    if (!have(r)) miss += std::string(miss.empty() ? "" : ", ") + "wave ROM " + r.name + " (CRC32 " + hex(r.crc) + ")";
                missing_msg += std::string(missing_msg.empty() ? "" : "; ") + s.name + " is missing " + miss;
            }
        }
    }
    if (!missing_msg.empty()) throw BankError(BankError::Kind::Io, "MU ROMs: incomplete set: " + missing_msg +
                                                 " (give all ROM files, or the directories that hold them)");
    std::string msg = "MU ROMs: no complete ";
    msg += o.model == MuRomModel::Auto ? std::string("MU1000 / MU2000 / MU128") : std::string(model_name(o.model));
    msg += " ROM set found";
    std::string unsup;
    for (const auto& [f, c] : all)
        for (const RomId& u : kUnsupported)
            if (u.crc == c) unsup += std::string(unsup.empty() ? "" : ", ") + u.name + " (" + f.filename().string() + ")";
    if (!unsup.empty()) msg += "; recognised but not supported: " + unsup;
    std::string unknown;
    std::size_t n_unknown = 0;
    for (const auto& [f, c] : all)
        if (!known.count(c)) {
            if (n_unknown++ < 8) unknown += std::string(unknown.empty() ? "" : ", ") + f.filename().string() + " (CRC32 " + hex(c) + ")";
        }
    if (n_unknown) msg += "; unrecognised files: " + unknown + (n_unknown > 8 ? ", …" : "");
    std::string found;
    for (const auto& [f, c] : all)
        if (known.count(c) && std::none_of(kUnsupported.begin(), kUnsupported.end(), [&](const RomId& u) { return u.crc == c; }))
            found += std::string(found.empty() ? "" : ", ") + f.filename().string();
    if (!found.empty()) msg += "; recognised: " + found;
    throw BankError(BankError::Kind::Io, msg);
}

MuTables convert_mu_roms(const MuRomSet& set, const EngineTables& tables) {
    // The drum velocity conversion reads the S-YXG50 FEG velocity curve from the DLL.
    require_engine_tables(tables, Model::Syxg50);
    const SetSpec* spec = nullptr;
    for (const SetSpec& s : kSpecs)
        if (s.name == set.name) spec = &s;
    if (spec == nullptr) throw BankError(BankError::Kind::Io, "MU ROMs: unknown set " + set.name);
    try {
        std::vector<Bytes> wave_bytes;
        for (const fs::path& p : set.wave) wave_bytes.push_back(read_rom(p));
        std::vector<std::span<const std::uint8_t>> waves(wave_bytes.begin(), wave_bytes.end());
        const auto [hi, lo] = program_halves(set);
        return convert_set(*spec, hi, lo, waves, tables);
    } catch (const syxg50::mu_rom::ConvertError& e) {
        throw BankError(BankError::Kind::Table, "MU ROMs: " + set.name + ": conversion failed: " + e.what());
    } catch (const std::out_of_range& e) {
        throw BankError(BankError::Kind::Table, "MU ROMs: " + set.name + ": conversion failed: " + e.what());
    }
}

std::shared_ptr<const Bank> load_mu_roms(const MuRomOptions& o, const BankOptions& bank_options) {
    const MuRomSet set = find_mu_roms(o);
    BankOptions bo = bank_options;
    bo.enable_mu = true;
    std::vector<std::uint8_t> tbl, upcm;
    fs::path cache_tbl, cache_upcm;
    if (!o.cache_dir.empty()) {
        std::string key = kConverterVersion;
        for (const std::uint32_t c : set.crcs) key += hex(c);
        const std::uint32_t k = crc32(reinterpret_cast<const std::uint8_t*>(key.data()), key.size());
        const std::string stem = std::string(model_name(set.model)) + "-" + hex(k);
        cache_tbl = o.cache_dir / (stem + ".TBL");
        cache_upcm = o.cache_dir / (stem + ".UPCM");
        tbl = read_cached(cache_tbl);
        upcm = read_cached(cache_upcm);
        if (tbl.empty() || upcm.empty() || !syxg50::BinTable::check_header(tbl)) {
            tbl.clear();
            upcm.clear();
        }
    }
    if (tbl.empty()) {
        if (!bo.tables) throw BankError(BankError::Kind::Header, "no engine tables (BankOptions::tables)");
        MuTables t = convert_mu_roms(set, *bo.tables);
        tbl = std::move(t.tbl);
        upcm = std::move(t.upcm);
        if (!o.cache_dir.empty()) {
            std::error_code ec;
            fs::create_directories(o.cache_dir, ec);
            write_atomic(cache_upcm, upcm);
            write_atomic(cache_tbl, tbl);
        }
    }
    return load_bank(std::move(tbl), std::move(upcm), Model::Syxg50, bo);
}

} // namespace hvyxg
