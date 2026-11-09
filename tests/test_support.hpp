// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// Shared helpers of the test suite: bank paths from the environment and a small synth rig.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "bank_impl.hpp"
#include "engine_tables.hpp"
#include "hvyxg/bank.hpp"
#include "hvyxg/engine_tables.hpp"
#include "hvyxg/synth.hpp"

namespace test {

struct BankFiles {
    std::string bin, data;
};

/// Root of the local, git-ignored asset tree (`roms/`, `tbl/`, `dll/`, `midi/`; docs/TESTING.md):
/// $HVYXG_ASSETS, else `_untracked/` in the source tree.
inline std::optional<std::filesystem::path> asset_root() {
    namespace fs = std::filesystem;
    const char* e = std::getenv("HVYXG_ASSETS");
    const fs::path p = e != nullptr ? fs::path(e) : fs::path(HVYXG_SOURCE_DIR) / "_untracked";
    if (fs::is_directory(p)) return p;
    return std::nullopt;
}

/// The engine tables from your DLLs, loaded once: every PE file under $HVYXG_DLLS, else under
/// asset_root()/dll (an empty object when there are none).
inline const std::shared_ptr<const hvyxg::EngineTables>& tables() {
    static const std::shared_ptr<const hvyxg::EngineTables> t = [] {
        namespace fs = std::filesystem;
        std::optional<fs::path> dir;
        if (const char* e = std::getenv("HVYXG_DLLS")) dir = fs::path(e);
        else if (const auto root = asset_root()) dir = *root / "dll";
        std::vector<fs::path> dlls;
        if (dir && fs::is_directory(*dir)) dlls = hvyxg::find_dlls(*dir);
        return std::shared_ptr<const hvyxg::EngineTables>(hvyxg::load_engine_tables(dlls));
    }();
    return t;
}

/// True when both engines' tables are there.
inline bool tables_ready() {
    return hvyxg::engine_tables_ready(*tables(), hvyxg::Model::Syxg50) &&
           hvyxg::engine_tables_ready(*tables(), hvyxg::Model::Syxg2006le);
}

/// The fx part of the test tables (internal classes take it).
inline const hvyxg::fx::Tables& fx_tables() { return tables()->fx; }

/// XG2.0 insertion RAM whose type defaults come from the test tables.
inline hvyxg::midi::xg::EffectInsertions insertions() {
    hvyxg::midi::xg::EffectInsertions a;
    for (auto& e : a) e = hvyxg::midi::xg::EffectInsertion(tables().get());
    return a;
}

/// BankOptions with the test tables.
inline hvyxg::BankOptions bank_options(hvyxg::BankOptions o = {}) {
    o.tables = tables();
    return o;
}

/// The file or folder named by the environment variable `var`; when it is unset, `rel` under
/// asset_root() (if given). Empty when neither exists.
inline std::string asset(const char* var, const char* rel = nullptr) {
    namespace fs = std::filesystem;
    if (const char* e = std::getenv(var)) return fs::exists(e) ? std::string(e) : std::string();
    if (rel == nullptr) return {};
    const auto root = asset_root();
    if (!root || !fs::exists(*root / rel)) return {};
    return (*root / rel).string();
}

/// An asset pair (table + wave data); nullopt unless both exist.
inline std::optional<BankFiles> asset_pair(const char* bin_var, const char* bin_rel, const char* data_var,
                                           const char* data_rel) {
    std::string bin = asset(bin_var, bin_rel);
    std::string data = asset(data_var, data_rel);
    if (bin.empty() || data.empty()) return std::nullopt;
    return BankFiles{std::move(bin), std::move(data)};
}

/// The S-YXG50 4 MB bank (HVYXG_TBL_BIN / HVYXG_TBL_DATA, else `roms/yxg50/`) or the 2006LE
/// bank (HVYXG_2006LE_BIN / HVYXG_2006LE_DATA, else `roms/yxg2006le/`).
inline std::optional<BankFiles> bank_files(bool syxg50) {
    if (syxg50)
        return asset_pair("HVYXG_TBL_BIN", "roms/yxg50/sxgbin41.tbl", "HVYXG_TBL_DATA", "roms/yxg50/sxgwave4.tbl");
    return asset_pair("HVYXG_2006LE_BIN", "roms/yxg2006le/SXGBNW6L.TBL", "HVYXG_2006LE_DATA",
                      "roms/yxg2006le/SXGDAT6L.TBL");
}

/// The bank, loaded once per process; nullptr (and a message) when the variables are unset.
inline std::shared_ptr<const hvyxg::Bank> bank(bool syxg50) {
    static std::shared_ptr<const hvyxg::Bank> cache[2];
    auto& b = cache[syxg50 ? 0 : 1];
    if (b) return b;
    const auto f = bank_files(syxg50);
    if (!f) {
        if (syxg50) MESSAGE("HVYXG_TBL_BIN / HVYXG_TBL_DATA not set: skipped");
        else MESSAGE("HVYXG_2006LE_BIN / HVYXG_2006LE_DATA not set: skipped");
        return nullptr;
    }
    b = hvyxg::load_bank(f->bin, f->data, hvyxg::Model::Auto, test::bank_options());
    return b;
}

inline bool have_banks() { return bank(true) && bank(false); }

/// A synth on a shared bank, with XG System On sent.
struct Rig {
    hvyxg::Synth synth;

    explicit Rig(std::shared_ptr<const hvyxg::Bank> b, std::uint16_t polyphony = 0)
        : Rig(options(std::move(b), polyphony)) {}
    explicit Rig(hvyxg::Synth::Options o) : synth(std::move(o)) {
        sysex({0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7});
    }
    static hvyxg::Synth::Options options(std::shared_ptr<const hvyxg::Bank> b, std::uint16_t polyphony) {
        hvyxg::Synth::Options o;
        o.tables = test::tables();
        o.bank = std::move(b);
        o.max_polyphony = polyphony;
        return o;
    }
    void send(std::initializer_list<std::uint8_t> m) {
        const std::vector<std::uint8_t> v(m);
        synth.midi(v);
    }
    void sysex(std::initializer_list<std::uint8_t> m) { send(m); }
    /// XG parameter change `F0 43 10 4C hh mm ll dd… F7`.
    void xg(std::initializer_list<std::uint8_t> addr_data) {
        std::vector<std::uint8_t> v{0xF0, 0x43, 0x10, 0x4C};
        v.insert(v.end(), addr_data);
        v.push_back(0xF7);
        synth.midi(v);
    }
    void cc(std::uint8_t ch, std::uint8_t cc, std::uint8_t v) { send({static_cast<std::uint8_t>(0xB0 | ch), cc, v}); }
    void program(std::uint8_t ch, std::uint8_t p) { send({static_cast<std::uint8_t>(0xC0 | ch), p}); }
    void note_on(std::uint8_t ch, std::uint8_t key, std::uint8_t vel) {
        send({static_cast<std::uint8_t>(0x90 | ch), key, vel});
    }
    void note_off(std::uint8_t ch, std::uint8_t key) { send({static_cast<std::uint8_t>(0x80 | ch), key, 0}); }
    void bend(std::uint8_t ch, std::uint16_t v) {
        send({static_cast<std::uint8_t>(0xE0 | ch), static_cast<std::uint8_t>(v & 0x7F),
              static_cast<std::uint8_t>(v >> 7)});
    }
    /// Render `frames` interleaved stereo frames.
    std::vector<float> render(std::size_t frames) {
        std::vector<float> buf(2 * frames);
        synth.render(buf.data(), frames);
        return buf;
    }
    /// Render `seconds`, returning the left channel.
    std::vector<float> render_left(double seconds) {
        const auto st = render(static_cast<std::size_t>(seconds * 44100.0));
        std::vector<float> left(st.size() / 2);
        for (std::size_t i = 0; i < left.size(); ++i) left[i] = st[2 * i];
        return left;
    }
};

inline double rms(const std::vector<float>& x, std::size_t a = 0, std::size_t b = 0) {
    if (b == 0) b = x.size();
    double s = 0;
    for (std::size_t i = a; i < b; ++i) s += static_cast<double>(x[i]) * x[i];
    return std::sqrt(s / static_cast<double>(b - a));
}

inline double rms_db(const std::vector<float>& x, std::size_t a = 0) {
    double s = 0;
    for (std::size_t i = a; i < x.size(); ++i) s += double(x[i]) * x[i];
    return 10 * std::log10(s / double(x.size() - a) + 1e-30);
}

} // namespace test

/// Skip a test case that needs the engine tables when the DLLs are not available.
#define HVYXG_NEED_TABLES()                                                                         \
    do {                                                                                            \
        if (!::test::tables_ready()) {                                                              \
            MESSAGE("skipped: needs the engine tables (the DLLs in _untracked/dll or $HVYXG_DLLS)"); \
            return;                                                                                 \
        }                                                                                           \
    } while (false)
