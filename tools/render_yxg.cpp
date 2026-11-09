// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// render_yxg: a Standard MIDI File through hvyxg into a WAV file.
//
//   render_yxg [options] SONG.mid OUT.wav      (render_yxg --help: every option)
//
// Also the shortest example of the library: hvyxg::Synth behind Hira-V's instrument interface
// (midi::Instrument: UMP messages at sample offsets, then the block), as render/render_host.hpp
// plays one. `YxgInstrument` is the whole of the glue; everything else here is options.
#include <cstdlib>
#include <memory>
#include <string>

#include "hvyxg/hvyxg.hpp"
#include "render/render_host.hpp"

namespace {

/// hvyxg::Synth as a midi::Instrument.
class YxgInstrument final : public render::Instrument {
public:
    explicit YxgInstrument(hvyxg::Synth::Options o)
        : info_{"XG", static_cast<std::uint32_t>(o.ports), midi::MIDI1}, synth_(std::move(o)) {}

    // One UMP message at its sample in the block about to be rendered: MIDI 1.0 channel voice
    // messages, 7-bit SysEx packets, System Reset; the group is the port.
    bool event(std::uint32_t offset, const std::uint32_t* ump, std::uint32_t words) override {
        synth_.ump(std::span(ump, words), offset);
        return true;  // the synth holds every message for its block
    }
    // The block at a 16-bit sample's scale (Hira-V's), or as float (nothing lost).
    void render(std::int32_t* out, std::uint32_t frames) override { synth_.render(out, frames); }
    bool render_float(float* out, std::uint32_t frames) override {
        synth_.render(out, frames);
        return true;
    }
    void reset() override { synth_.reset(); }
    const midi::InstrumentInfo& info() const override { return info_; }
    // The DLLs' block is 128 frames (S-YXG50; the 2006LE's 64 divides it).
    std::uint32_t block() const override { return 128; }
    std::string describe() const override {
        const bool le = synth_.model() == hvyxg::Model::Syxg2006le;
        return std::string(le ? "S-YXG2006LE" : synth_.bank().mu() ? "S-YXG50, MU bank" : "S-YXG50") + ", " +
               std::to_string(synth_.bank().default_polyphony()) + "-voice DLL model";
    }

private:
    midi::InstrumentInfo info_;
    hvyxg::Synth synth_;
};

class Yxg final : public render::Tool {
public:
    Yxg() {
        if (const char* e = std::getenv("HVYXG_TBL_BIN")) o_.bin_file = e;
        if (const char* e = std::getenv("HVYXG_TBL_DATA")) o_.data_file = e;
    }
    const char* name() const override { return "render_yxg"; }
    const char* version() const override { return HVYXG_VERSION_STRING; }
    const char* about() const override { return "a Standard MIDI File through hvyxg (S-YXG50 / S-YXG2006LE / MU banks)"; }
    std::uint32_t default_rate() const override { return 44100; }

    std::vector<render::Option> options() const override {
        return {
            {"--bin", "FILE", "bank: bin / voice table (default $HVYXG_TBL_BIN)"},
            {"--data", "FILE", "bank: wave / data table (default $HVYXG_TBL_DATA)"},
            {"--plugin", "DLL", "a DLL with the engine tables (repeatable); when no bank is given otherwise, one that embeds a bank supplies it"},
            {"--plugin-path", "DIR", "every DLL under DIR (recursively), as --plugin"},
            {"--bank-path", "PATH", "where the bank is found by content (repeatable): stock or MU tables, MU ROMs"},
            {"--model", "auto|syxg50|2006le", "the engine (default: from the bank)"},
            {"--polyphony", "N", "voices, at most 1024 (default: the DLL's)"},
            {"--steal-fade", "MS", "a stolen voice fades out over MS ms (default 0: the DLL's fast steal)"},
            {"--interp", "linear|hermite|lanczos", "sample interpolation (default linear, the DLLs')"},
            {"--enable-mu", nullptr, "an SXG-Create-NF MU bank, played as its patched DLL does"},
            {"--mu-layout", "auto|classic|big", "force the MU table layout"},
            {"--mu-voice-map", "default|native|basic", "MU100 / MU128 / MU1000 voice map"},
            {"--mu-model", "auto|mu1000|mu2000|mu128", "which MU bank, when --bank-path holds several"},
            {"--mu-cache", "DIR", "keep converted MU ROM tables in DIR"},
            {"--dll-exact", nullptr, "no superset behaviour: follow the reference DLL"},
            {"--mu-part-eq", nullptr, "part / drum-note EQ with the MU2000's gain strength"},
            {"--2006full", nullptr, "S-YXG2006LE bank: full XG instead of the DLL's XGlite subset"},
        };
    }

    bool option(const std::string& n, const std::string& v, std::string& err) override {
        const auto choose = [&](std::initializer_list<const char*> names, int& out) {
            int i = 0;
            for (const char* c : names) {
                if (v == c) return (out = i), true;
                ++i;
            }
            err = n + ": " + v + "?";
            return false;
        };
        int k = 0;
        if (n == "--bin") o_.bin_file = v;
        else if (n == "--data") o_.data_file = v;
        else if (n == "--plugin") o_.plugin_files.emplace_back(v);
        else if (n == "--plugin-path") o_.plugin_path = v;
        else if (n == "--bank-path") o_.bank_paths.emplace_back(v);
        else if (n == "--model") {
            if (!choose({"auto", "syxg50", "2006le"}, k)) return false;
            o_.model = k == 1 ? hvyxg::Model::Syxg50 : k == 2 ? hvyxg::Model::Syxg2006le : hvyxg::Model::Auto;
        } else if (n == "--polyphony") {
            const long p = std::strtol(v.c_str(), nullptr, 10);
            if (p < 0 || p > hvyxg::Synth::kMaxPolyphony) return (err = "--polyphony: 0 … 1024"), false;
            o_.max_polyphony = static_cast<std::uint16_t>(p);
        } else if (n == "--steal-fade") {
            o_.steal_fade_ms = std::strtof(v.c_str(), nullptr);
        } else if (n == "--interp") {
            if (!choose({"linear", "hermite", "lanczos"}, k)) return false;
            o_.interpolation = k == 1 ? hvyxg::Interpolation::Hermite
                             : k == 2 ? hvyxg::Interpolation::Lanczos3
                                      : hvyxg::Interpolation::Linear;
        } else if (n == "--enable-mu") o_.enable_mu = true;
        else if (n == "--mu-layout") {
            if (!choose({"auto", "classic", "big"}, k)) return false;
            o_.mu_layout = k == 1 ? hvyxg::MuLayout::Classic : k == 2 ? hvyxg::MuLayout::Big : hvyxg::MuLayout::Auto;
        } else if (n == "--mu-voice-map") {
            if (!choose({"default", "native", "basic"}, k)) return false;
            o_.mu_voice_map = k == 1 ? hvyxg::MuVoiceMap::Native : k == 2 ? hvyxg::MuVoiceMap::Basic : hvyxg::MuVoiceMap::Default;
        } else if (n == "--mu-model") {
            if (!choose({"auto", "mu1000", "mu2000", "mu128"}, k)) return false;
            o_.mu_rom_model = k == 1 ? hvyxg::MuRomModel::Mu1000
                            : k == 2 ? hvyxg::MuRomModel::Mu2000
                            : k == 3 ? hvyxg::MuRomModel::Mu128
                                     : hvyxg::MuRomModel::Auto;
        } else if (n == "--mu-cache") o_.mu_cache = v;
        else if (n == "--dll-exact") o_.dll_exact = true;
        else if (n == "--mu-part-eq") o_.mu_part_eq = true;
        else if (n == "--2006full") o_.full_xg_2006le = true;
        return true;
    }

    std::unique_ptr<render::Instrument> make(const render::Setup& s, std::string& err) override {
        o_.sample_rate = static_cast<float>(s.format.rate);
        o_.ports = std::min<std::uint32_t>(s.groups, 4);  // the synth's 16-part ports, at most 4
        try {
            return std::make_unique<YxgInstrument>(o_);
        } catch (const std::exception& e) {
            err = e.what();
            return nullptr;
        }
    }

private:
    hvyxg::Synth::Options o_;
};

} // namespace

int main(int argc, char** argv) {
    Yxg tool;
    return render::main(tool, argc, argv);
}
