// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// The built-in MU ROM converter (hvyxg/mu_rom.hpp, docs/ENGINE.md "MU ROMs"): its Python
// semantics helpers, ROM recognition errors, and — with your own ROMs — byte identity with
// SXG-Create-NF's tables and with renders from them.
//
//   HVYXG_MU_ROMS        ROM files / directories, separated by ':' (the MU1000 set, and the
//                        MU2000 set for the MU2000 case)
//   HVYXG_MU_BIN / _DATA SXG-Create-NF's SXGMU1KX.TBL / .UPCM made from the same MU1000 ROMs
//   HVYXG_MU2000_BIN / _DATA   the same for the MU2000 (SXGMU2KX.TBL / .UPCM), optional
// Cases skip when the variables are not set.
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "hvyxg/bank.hpp"
#include "bank_io.hpp"
#include "hvyxg/mu_rom.hpp"
#include "syxg50/mu_rom/py.hpp"
#include "hvyxg/synth.hpp"
#include "test_support.hpp"

using namespace hvyxg;
namespace fs = std::filesystem;

namespace {

std::vector<fs::path> rom_paths() {
    std::vector<fs::path> out;
    if (std::getenv("HVYXG_MU_ROMS") == nullptr) {
        // Unset: the asset tree's ROM folder (searched recursively), if there is one.
        if (const std::string dir = test::asset("HVYXG_MU_ROMS", "roms"); !dir.empty()) out.emplace_back(dir);
        return out;
    }
    std::string s = std::getenv("HVYXG_MU_ROMS");
    std::size_t at = 0;
    while (at <= s.size()) {
        const std::size_t c = s.find(':', at);
        const std::string part = s.substr(at, c == std::string::npos ? std::string::npos : c - at);
        if (!part.empty() && fs::exists(part)) out.emplace_back(part);
        if (c == std::string::npos) break;
        at = c + 1;
    }
    return out;
}

std::optional<std::pair<std::string, std::string>> env_pair(const char* bin, const char* data) {
    // Defaults in the asset tree: SXG-Create-NF's output for the model the variables name.
    const bool mu2000 = std::string(bin).find("MU2000") != std::string::npos;
    const auto f = test::asset_pair(bin, mu2000 ? "tbl/mu2000/SXGMU2KX.TBL" : "tbl/mu1000/SXGMU1KX.TBL", data,
                                    mu2000 ? "tbl/mu2000/SXGMU2KX.UPCM" : "tbl/mu1000/SXGMU1KX.UPCM");
    if (!f) return std::nullopt;
    return std::make_pair(f->bin, f->data);
}

/// Converted once per process and model.
const MuTables* converted(MuRomModel model) {
    static std::map<MuRomModel, MuTables> cache;
    if (const auto it = cache.find(model); it != cache.end()) return &it->second;
    const auto roms = rom_paths();
    if (roms.empty()) return nullptr;
    MuRomSet set;
    try {
        set = find_mu_roms(MuRomOptions{.roms = roms, .model = model, .cache_dir = {}});
    } catch (const BankError& e) {
        MESSAGE("no ROM set for this model in HVYXG_MU_ROMS: " << e.what());
        return nullptr;
    }
    return &cache.emplace(model, convert_mu_roms(set, *test::tables())).first->second;
}

std::vector<float> render(Synth::Options o) {
    Synth s(std::move(o));
    const std::vector<std::vector<std::uint8_t>> msgs{
        {0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7},
        {0xC0, 48}, {0x90, 60, 100}, {0x90, 64, 90},           // strings (multisample pages)
        {0xB1, 0, 0}, {0xB1, 32, 0}, {0xC1, 0}, {0x91, 67, 110},  // piano
        {0x99, 36, 100}, {0x99, 38, 100}, {0x99, 42, 90},       // drums
        {0xB2, 0, 64}, {0xC2, 0}, {0x92, 72, 100},             // SFX bank
    };
    for (const auto& m : msgs) s.midi(m);
    std::vector<float> buf(2 * 44100);
    s.render(buf.data(), 44100);
    return buf;
}

void check_identical(const MuTables& t, const std::string& bin, const std::string& data) {
    const auto ref_tbl = read_file(bin, "reference table");
    const auto ref_upcm = read_file(data, "reference wave file");
    CHECK(t.tbl.size() == ref_tbl.size());
    CHECK(t.upcm.size() == ref_upcm.size());
    CHECK(t.tbl == ref_tbl);
    CHECK(t.upcm == ref_upcm);
}

fs::path temp_dir(const char* name) {
    // Unique per run, so that concurrent test runs (ctest -j, several build trees) don't share
    // or delete each other's folders.
    static const std::string run = std::to_string(std::random_device{}());
    const fs::path p = fs::temp_directory_path() / ("hvyxg-mu-rom-test-" + run + "-" + name);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

} // namespace

TEST_CASE("mu rom: Python arithmetic helpers") {
    using namespace syxg50::mu_rom;
    // round(): half to even; round(x, n): the exact binary value decides.
    CHECK(py_round(0.5) == 0);
    CHECK(py_round(1.5) == 2);
    CHECK(py_round(-2.5) == -2);
    CHECK(py_round_nd(2.675, 2) == 2.67);
    CHECK(py_round_nd(0.125, 2) == 0.12);
    CHECK(py_round_nd(-0.04, 1) == 0.0);
    // sum() of Python 3.12 is compensated (Neumaier).
    CHECK(py_sum({1e100, 1.0, -1e100}) == 1.0);
    CHECK(py_sum(std::vector<double>(10, 0.1)) == 1.0);
    // Integer true division is correctly rounded, also above 2^53.
    CHECK(py_truediv(7, 2) == 3.5);
    const u128 big = (static_cast<u128>(1) << 60) + 1;
    CHECK(py_truediv(big, 3) == static_cast<double>(big) / 3.0);
    CHECK(floordiv(-7, 2) == -4);
    CHECK(ceildiv(7, 2) == 4);
}

TEST_CASE("mu rom: clear errors for missing and unrecognised ROMs") {
    const fs::path dir = temp_dir("errors");
    CHECK_THROWS_AS(find_mu_roms(MuRomOptions{}), BankError);
    CHECK_THROWS_AS(find_mu_roms(MuRomOptions{.roms = {dir / "nope"}, .model = MuRomModel::Auto, .cache_dir = {}}), BankError);
    try {
        find_mu_roms(MuRomOptions{.roms = {dir}, .model = MuRomModel::Auto, .cache_dir = {}});
        FAIL("no error");
    } catch (const BankError& e) {
        CHECK(std::string(e.what()).find("no ROM-sized files") != std::string::npos);
    }
    {
        std::ofstream f(dir / "junk.bin", std::ios::binary);
        const std::vector<char> zeros(1 << 20, 0);
        f.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    }
    try {
        find_mu_roms(MuRomOptions{.roms = {dir}, .model = MuRomModel::Auto, .cache_dir = {}});
        FAIL("no error");
    } catch (const BankError& e) {
        const std::string w = e.what();
        CHECK(w.find("no complete") != std::string::npos);
        CHECK(w.find("junk.bin") != std::string::npos);
    }
    fs::remove_all(dir);
}

TEST_CASE("mu rom: MU1000 tables are byte-identical to SXG-Create-NF's") {
    HVYXG_NEED_TABLES();
    const auto ref = env_pair("HVYXG_MU_BIN", "HVYXG_MU_DATA");
    const MuTables* t = ref ? converted(MuRomModel::Mu1000) : nullptr;
    if (t == nullptr) {
        MESSAGE("HVYXG_MU_ROMS (MU1000 set) / HVYXG_MU_BIN / HVYXG_MU_DATA not set: skipped");
        return;
    }
    CHECK(t->tbl_name == "SXGMU1KX.TBL");
    CHECK(t->upcm_name == "SXGMU1KX.UPCM");
    check_identical(*t, ref->first, ref->second);
}

TEST_CASE("mu rom: MU2000 tables are byte-identical to SXG-Create-NF's") {
    HVYXG_NEED_TABLES();
    const auto ref = env_pair("HVYXG_MU2000_BIN", "HVYXG_MU2000_DATA");
    const MuTables* t = ref ? converted(MuRomModel::Mu2000) : nullptr;
    if (t == nullptr) {
        MESSAGE("HVYXG_MU_ROMS (MU2000 set) / HVYXG_MU2000_BIN / HVYXG_MU2000_DATA not set: skipped");
        return;
    }
    check_identical(*t, ref->first, ref->second);
}

TEST_CASE("mu rom: renders from the ROMs equal renders from SXG-Create-NF's tables") {
    HVYXG_NEED_TABLES();
    const auto ref = env_pair("HVYXG_MU_BIN", "HVYXG_MU_DATA");
    const auto roms = rom_paths();
    if (!ref || roms.empty() || !converted(MuRomModel::Mu1000)) {
        MESSAGE("HVYXG_MU_ROMS / HVYXG_MU_BIN / HVYXG_MU_DATA not set: skipped");
        return;
    }
    const fs::path cache = temp_dir("cache");
    for (const MuVoiceMap map : {MuVoiceMap::Default, MuVoiceMap::Basic}) {
        for (const bool exact : {false, true}) {
            Synth::Options files;
            files.tables = test::tables();
            files.bin_file = ref->first;
            files.data_file = ref->second;
            files.enable_mu = true;
            files.mu_voice_map = map;
            files.dll_exact = exact;
            Synth::Options from_roms = files;
            from_roms.bin_file.clear();
            from_roms.data_file.clear();
            from_roms.enable_mu = false;  // implied by an MU bank found under bank_paths
            from_roms.bank_paths = roms;
            from_roms.mu_rom_model = MuRomModel::Mu1000;
            from_roms.mu_cache = cache;  // first pass writes the cache, later ones read it
            const auto a = render(files);
            const auto b = render(from_roms);
            CHECK(a == b);
            double peak = 0;
            for (const float v : a) peak = std::max(peak, static_cast<double>(std::abs(v)));
            CHECK(peak > 0.01);
        }
    }
    // The cache holds SXG-Create-NF's tables too.
    std::size_t files_in_cache = 0;
    for (const auto& e : fs::directory_iterator(cache)) {
        ++files_in_cache;
        if (e.path().extension() == ".TBL") CHECK(read_file(e.path(), "cache") == read_file(ref->first, "reference"));
    }
    CHECK(files_in_cache == 2);
    fs::remove_all(cache);
}

TEST_CASE("mu rom: a joined program ROM file (concatenated or interleaved) is the same set") {
    HVYXG_NEED_TABLES();
    const auto root = test::asset_root();
    if (!root || !fs::exists(*root / "roms/mu1000/mu1000-v2.01-h.bin") || !fs::exists(*root / "roms/mu2000/xv364a0.ic49")) {
        MESSAGE("skipped: needs roms/mu1000 (h / l) and the wave ROMs in roms/mu2000");
        return;
    }
    const auto h = read_file(*root / "roms/mu1000/mu1000-v2.01-h.bin", "h");
    const auto l = read_file(*root / "roms/mu1000/mu1000-v2.01-l.bin", "l");
    // Build each joined form from the pair, in both orders.
    const auto join = [](const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b, MuProgramForm form) {
        std::vector<std::uint8_t> out;
        if (form == MuProgramForm::Concatenated) {
            out = a;
            out.insert(out.end(), b.begin(), b.end());
            return out;
        }
        const bool swap = form == MuProgramForm::InterleavedSwapped;
        for (std::size_t i = 0; i + 1 < a.size(); i += 2)
            for (const auto* w : {&a, &b})
                if (swap) out.insert(out.end(), {(*w)[i + 1], (*w)[i]});
                else out.insert(out.end(), {(*w)[i], (*w)[i + 1]});
        return out;
    };
    const fs::path dir = temp_dir("joined");
    const fs::path file = dir / "program.bin";
    const MuRomSet pair = find_mu_roms(MuRomOptions{.roms = {*root / "roms/mu1000", *root / "roms/mu2000"},
                                                    .model = MuRomModel::Mu1000, .cache_dir = {}});
    for (const MuProgramForm form : {MuProgramForm::Concatenated, MuProgramForm::Interleaved, MuProgramForm::InterleavedSwapped})
        for (const bool low_first : {false, true}) {
            CAPTURE(static_cast<int>(form));
            CAPTURE(low_first);
            const auto bytes = low_first ? join(l, h, form) : join(h, l, form);
            std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                        static_cast<std::streamsize>(bytes.size()));
            const MuRomSet set = find_mu_roms(MuRomOptions{.roms = {dir, *root / "roms/mu2000"}, .model = MuRomModel::Mu1000,
                                                           .cache_dir = {}});
            CHECK(set.program_form == form);
            CHECK(set.low_first == low_first);
            CHECK(set.program.size() == 1);
            CHECK(set.crcs == pair.crcs);  // the pair's CRCs: the same cache key
            if (form == MuProgramForm::Interleaved && low_first) {
                // Converted, the joined file gives the pair's tables byte for byte.
                const MuTables* ref = converted(MuRomModel::Mu1000);
                REQUIRE(ref != nullptr);
                const MuTables t = convert_mu_roms(set, *test::tables());
                CHECK(t.tbl == ref->tbl);
                CHECK(t.upcm == ref->upcm);
            }
        }
    fs::remove_all(dir);
}

TEST_CASE("mu roms: images in memory convert to the same tables as the files") {
    HVYXG_NEED_TABLES();
    const auto roms = rom_paths();
    if (roms.empty()) {
        MESSAGE("HVYXG_MU_ROMS not set: skipped");
        return;
    }
    for (const MuRomModel model : {MuRomModel::Mu1000, MuRomModel::Mu2000}) {
        const MuTables* want = converted(model);
        if (!want) continue;
        const MuRomSet set = find_mu_roms(MuRomOptions{.roms = roms, .model = model, .cache_dir = {}});
        // The set's files and a stray file, in a shuffled order, as an embedded host reads them.
        std::vector<std::vector<std::uint8_t>> files;
        for (auto it = set.wave.rbegin(); it != set.wave.rend(); ++it) files.push_back(read_file(*it, "rom"));
        files.push_back(std::vector<std::uint8_t>(1 << 20, 0x5A));
        for (const auto& p : set.program) files.push_back(read_file(p, "rom"));
        std::vector<std::span<const std::uint8_t>> images(files.begin(), files.end());
        MuTables got;
        Error e;
        REQUIRE(convert_mu_roms(images, model, *test::tables(), got, e));
        CHECK(got.tbl == want->tbl);
        CHECK(got.upcm == want->upcm);
        CHECK(got.tbl_name == want->tbl_name);
        // Without the wave ROMs: an error, not a crash.
        images.erase(images.begin(), images.begin() + 2);
        MuTables none;
        CHECK_FALSE(convert_mu_roms(images, model, *test::tables(), none, e));
        CHECK(e.kind == ErrorKind::Io);
    }
}
