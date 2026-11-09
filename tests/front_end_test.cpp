// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// The MIDI front end (midi/front_end.hpp) and the public Synth: SysEx parsing, RPN / NRPN,
// resets, the drum rules, event timing, the byte-stream parser and bank loading errors. Runs
// on a fake bank (no bank files needed).
#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <variant>
#include <vector>

#include "hvyxg/bank.hpp"
#include "midi/front_end.hpp"
#include "null_module.hpp"
#include "hvyxg/synth.hpp"
#include "synth_access.hpp"
#include "test_support.hpp"

using namespace hvyxg;

namespace {

/// Records every note-on with the block it lands in.
struct Log {
    std::vector<std::pair<std::uint64_t, int>> notes;  // (block, note)
};

class RecordingTones final : public ToneModule {
public:
    explicit RecordingTones(Log* log) : log_(log) {}
    std::size_t block_size() const override { return 64; }
    void note_on(const midi::Part&, std::uint8_t note, std::uint8_t) override {
        log_->notes.emplace_back(block_, note);
    }
    void note_off(const midi::Part&, std::uint8_t) override {}
    void sustain_changed(const midi::Part&) override {}
    void sostenuto_changed(const midi::Part&) override {}
    void release_all(const midi::Part&) override {}
    void kill_all(const midi::Part&) override {}
    void render(Buses&) override { ++block_; }
    bool active() const override { return false; }
    std::size_t voice_count() const override { return 0; }

private:
    Log* log_;
    std::uint64_t block_ = 0;
};

/// A bank whose kit images name their program in the level byte of note 36.
class FakeBank final : public BankImpl {
public:
    FakeBank() : BankImpl(test::tables(), nullptr) {}
    Log* log = nullptr;
    Model model = Model::Syxg2006le;
    Model type() const override { return model; }
    std::optional<GsBanks> gsb;
    std::optional<GsBanks> gs_banks() const override { return gsb; }
    bool kit118 = false;
    bool is_kit_bank(std::uint8_t msb) const override { return BankImpl::is_kit_bank(msb) || (kit118 && msb == 118); }
    std::uint8_t drum_part_key_assign(ResetMode) const override { return 1; }
    std::uint16_t default_polyphony() const override { return 32; }
    bool xg_in_gs = true;
    bool xg_parameters_in_gs_mode() const override { return xg_in_gs; }
    bool table = false;
    bool xg_parameter_table() const override { return table; }
    std::optional<DrumKitSetup> drum_setup(std::uint8_t msb, std::uint8_t program) const override {
        if (!is_kit_bank(msb)) return std::nullopt;
        DrumKitSetup s{};
        s[36 - 13].level = program;
        s[36 - 13].pitch_coarse = msb;
        return s;
    }
    std::unique_ptr<ToneModule> make_tone_module(const ModuleContext&) const override {
        return std::make_unique<RecordingTones>(log);
    }
    std::unique_ptr<SystemEffects> make_system_effects(const ModuleContext&) const override {
        return std::make_unique<DryEffects>();
    }
};

struct Sink final : ActionSink {
    std::vector<Action> actions;
    void act(const Action& a) override { actions.push_back(a); }
    template <typename T> int count() const {
        int n = 0;
        for (const auto& a : actions) n += std::holds_alternative<T>(a) ? 1 : 0;
        return n;
    }
};

struct Fe {
    std::shared_ptr<FakeBank> bank = std::make_shared<FakeBank>();
    Sink sink;
    std::unique_ptr<midi::FrontEnd> fe;
    explicit Fe(midi::FrontEndOptions o = {}) {
        fe = std::make_unique<midi::FrontEnd>(bank, sink, o);
        fe->power_on();
        sink.actions.clear();
    }
    void send(std::vector<std::uint8_t> m, std::size_t port = 0) { fe->message(m, port); }
    void xg(std::vector<std::uint8_t> addr_data) {
        std::vector<std::uint8_t> m{0xF0, 0x43, 0x10, 0x4C};
        m.insert(m.end(), addr_data.begin(), addr_data.end());
        m.push_back(0xF7);
        send(m);
    }
    void cc(std::uint8_t ch, std::uint8_t c, std::uint8_t v) { send({static_cast<std::uint8_t>(0xB0 | ch), c, v}); }
    const midi::xg::MultiPart& ram(std::size_t p) const { return fe->part_ram(p); }
    const midi::Part& part(std::size_t p) const { return fe->part(p); }
};

std::vector<std::uint8_t> gs(std::vector<std::uint8_t> addr_data) {
    unsigned sum = 0;
    for (auto b : addr_data) sum += b;
    std::vector<std::uint8_t> m{0xF0, 0x41, 0x10, 0x42, 0x12};
    m.insert(m.end(), addr_data.begin(), addr_data.end());
    m.push_back(static_cast<std::uint8_t>((128 - (sum & 0x7F)) & 0x7F));
    m.push_back(0xF7);
    return m;
}

} // namespace

TEST_CASE("front_end_power_on_defaults") {
    Fe f;
    CHECK(f.fe->reset_mode() == ResetMode::XG);
    CHECK(f.fe->part_count() == 16);
    const auto& r = f.ram(0);
    CHECK(r.volume == 100);
    CHECK(r.pan == 64);
    CHECK(r.reverb_send == 40);
    CHECK(r.ac[0].controller_number == 16);  // AC1 = CC16
    CHECK(r.ac[1].controller_number == 17);
    CHECK(r.bend.pitch_control == 0x42);
    CHECK(r.bend.lfo_pmod_depth == 0);       // measured: bend LFO depths 0
    CHECK(r.mw.lfo_pmod_depth == 10);
    CHECK(r.get_detune() == 0x80);
    // Part 10: drum setup 1, the engine's drum note assign, the XG kit bank.
    const auto& d = f.ram(9);
    CHECK(d.part_mode == 2);
    CHECK(d.key_assign == 1);
    CHECK(f.part(9).program == midi::Program{true, 127, 0, 0});
    CHECK(f.part(0).program == midi::Program{true, 0, 0, 0});
    // Drum setup 1 holds the part's kit image.
    CHECK(f.fe->drum_setup(0)[36 - 13].pitch_coarse == 127);
}

TEST_CASE("front_end_xg_parameter_change_and_clamping") {
    Fe f;
    f.xg({0x08, 0x00, 0x08, 0x10});  // note shift below the range → 0x28
    CHECK(f.ram(0).note_shift == 0x28);
    f.xg({0x08, 0x00, 0x09, 0x09, 0x04});  // detune, two nibbles in one message
    CHECK(f.ram(0).get_detune() == 0x94);
    f.xg({0x08, 0x01, 0x0B, 0x50, 0x11, 0x22});  // volume, velocity sense depth, offset
    CHECK(f.ram(1).volume == 0x50);
    CHECK(f.ram(1).velocity_sense_depth == 0x11);
    CHECK(f.ram(1).velocity_sense_offset == 0x22);
    f.xg({0x00, 0x00, 0x06, 0x7F});  // transpose clamps to +24
    CHECK(f.fe->system().transpose == 0x58);
    f.xg({0x00, 0x00, 0x00, 0x00, 0x04, 0x03, 0x02});  // master tune, four nibbles
    CHECK(f.fe->system().get_master_tune() == 0x0432);
    // Device number: any accepted by default; a configured one filters.
    Fe g(midi::FrontEndOptions{1, 3});
    g.send({0xF0, 0x43, 0x12, 0x4C, 0x08, 0x00, 0x0B, 0x10, 0xF7});
    CHECK(g.ram(0).volume == 100);
    g.send({0xF0, 0x43, 0x13, 0x4C, 0x08, 0x00, 0x0B, 0x10, 0xF7});
    CHECK(g.ram(0).volume == 0x10);
}

TEST_CASE("front_end_bulk_dump_checksum") {
    Fe f;
    // 08 00 0B: volume 0x33, velocity sense depth 0x44.
    std::vector<std::uint8_t> body{0x00, 0x02, 0x08, 0x00, 0x0B, 0x33, 0x44};
    unsigned sum = 0;
    for (auto b : body) sum += b;
    std::vector<std::uint8_t> m{0xF0, 0x43, 0x00, 0x4C};
    m.insert(m.end(), body.begin(), body.end());
    m.push_back(static_cast<std::uint8_t>((128 - (sum & 0x7F)) & 0x7F));
    m.push_back(0xF7);
    auto bad = m;
    bad[bad.size() - 2] ^= 1;
    f.send(bad);
    CHECK(f.ram(0).volume == 100);
    f.send(m);
    CHECK(f.ram(0).volume == 0x33);
    CHECK(f.ram(0).velocity_sense_depth == 0x44);
    // Device byte 0x7F (F0 43 7F 4C …) is not a bulk dump (both DLLs ignore it [meas]).
    for (const Model model : {Model::Syxg50, Model::Syxg2006le}) {
        Fe g;
        g.bank->model = model;
        g.bank->table = model == Model::Syxg50;
        auto all = m;
        all[2] = 0x7F;
        g.send(all);
        CHECK(g.ram(0).volume == 100);
    }
}

namespace {
std::vector<std::uint8_t> xg_bulk(std::vector<std::uint8_t> addr, std::vector<std::uint8_t> data, bool good = true) {
    std::vector<std::uint8_t> body{static_cast<std::uint8_t>(data.size() >> 7), static_cast<std::uint8_t>(data.size() & 0x7F)};
    body.insert(body.end(), addr.begin(), addr.end());
    body.insert(body.end(), data.begin(), data.end());
    unsigned sum = 0;
    for (auto b : body) sum += b;
    std::vector<std::uint8_t> m{0xF0, 0x43, 0x00, 0x4C};
    m.insert(m.end(), body.begin(), body.end());
    m.push_back(static_cast<std::uint8_t>(((128 - (sum & 0x7F)) & 0x7F) ^ (good ? 0 : 0x11)));
    m.push_back(0xF7);
    return m;
}
} // namespace

TEST_CASE("front_end_syxg50_xg_parameter_table") {
    // The S-YXG50 parses XG messages through its parameter table [code; meas: syxg50.dll, piano /
    // organ pitch and level per message].
    Fe f;
    f.bank->table = true;
    // A lone byte of the 2-byte part detune does nothing; both bytes in one message act.
    f.xg({0x08, 0x00, 0x09, 0x0C});
    CHECK(f.ram(0).get_detune() == 0x80);
    f.xg({0x08, 0x00, 0x0A, 0x0F});
    CHECK(f.ram(0).get_detune() == 0x80);
    f.xg({0x08, 0x00, 0x09, 0x0C, 0x0F});
    CHECK(f.ram(0).get_detune() == 0xCF);
    // A message starting before the detune carries only its first parameter: note shift is
    // written (read from the RAM at the note-on), the detune bytes are not.
    f.xg({0x08, 0x00, 0x08, 0x41, 0x04, 0x00});
    CHECK(f.ram(0).note_shift == 0x41);
    CHECK(f.ram(0).get_detune() == 0xCF);
    // Bytes past the parameter: the volume is written to the RAM but not applied (the
    // end-of-message handler is skipped), pan (read from the RAM) acts, the detune is written
    // and the trailing byte is not.
    f.xg({0x08, 0x01, 0x0B, 0x40, 0x7F});
    CHECK(f.ram(1).volume == 0x40);
    CHECK(f.part(1).gain_volume == 100);
    CHECK(f.ram(1).velocity_sense_depth == 64);
    f.xg({0x08, 0x01, 0x0E, 0x7F, 0x40});
    CHECK(f.ram(1).pan == 0x7F);
    CHECK(f.ram(1).note_limit_low == 0);
    f.xg({0x08, 0x01, 0x09, 0x0C, 0x0F, 0x40});
    CHECK(f.ram(1).get_detune() == 0xCF);
    CHECK(f.ram(1).volume == 0x40);
    CHECK(f.part(1).gain_volume == 100);
    // Master tune: four nibbles or nothing; not from 01; with a fifth byte written but not
    // applied.
    f.xg({0x00, 0x00, 0x00, 0x00, 0x05});
    f.xg({0x00, 0x00, 0x01, 0x05, 0x0F, 0x04});
    CHECK(f.fe->system().get_master_tune() == 0x0400);
    f.xg({0x00, 0x00, 0x00, 0x00, 0x05, 0x0F, 0x04, 0x40});
    CHECK(f.fe->system().get_master_tune() == 0x05F4);
    CHECK(f.fe->system().applied_master_tune == 0x0400);
    f.xg({0x00, 0x00, 0x00, 0x00, 0x05, 0x0F, 0x04});
    CHECK(f.fe->system().applied_master_tune == 0x05F4);
    // Effect types and variation parameters are MSB + LSB pairs.
    const auto& e = f.fe->effects();
    const auto r0 = e.reverb_type_writes;
    f.xg({0x02, 0x01, 0x00, 0x02});
    f.xg({0x02, 0x01, 0x01, 0x01});
    CHECK(e.reverb_type_writes == r0);
    f.xg({0x02, 0x01, 0x00, 0x02, 0x00});
    CHECK(e.reverb_type_writes == r0 + 2);
    CHECK(e[0x00] == 0x02);
    const std::uint8_t v43 = e[0x43];
    f.xg({0x02, 0x01, 0x43, static_cast<std::uint8_t>(v43 ^ 1)});
    CHECK(e[0x43] == v43);
    f.xg({0x02, 0x01, 0x42, e[0x42], static_cast<std::uint8_t>(v43 ^ 1)});
    CHECK(e[0x43] == (v43 ^ 1));
    // Bulk dumps: whole parameters up to the table block's end, a cut-off parameter dropped,
    // a dump starting inside a parameter ignored, no checksum check.
    f.send(xg_bulk({0x08, 0x02, 0x07}, {0x00, 0x42, 0x0C}));
    CHECK(f.ram(2).note_shift == 0x42);
    CHECK(f.ram(2).get_detune() == 0x80);
    f.send(xg_bulk({0x08, 0x02, 0x0A}, {0x0F, 0x40}));
    CHECK(f.ram(2).volume == 100);
    f.send(xg_bulk({0x08, 0x02, 0x09}, {0x0C, 0x0F, 0x40}, false));
    CHECK(f.ram(2).get_detune() == 0xCF);
    CHECK(f.ram(2).volume == 0x40);
    f.send(xg_bulk({0x08, 0x02, 0x27}, {0x41, 0x41, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0}));
    CHECK(f.ram(2).rcv_note_message == 1);
    // The 2006LE writes each byte at its own address.
    Fe g;
    g.xg({0x08, 0x00, 0x09, 0x0C});
    CHECK(g.ram(0).get_detune() == 0xC0);
    g.xg({0x08, 0x00, 0x0B, 0x40, 0x7F});
    CHECK(g.ram(0).volume == 0x40);
    CHECK(g.ram(0).velocity_sense_depth == 0x7F);
}

TEST_CASE("front_end_syxg50_trailing_bytes") {
    // An S-YXG50 parameter change with bytes past its parameter writes the RAM and skips the
    // end-of-message handlers; a later event that reads the RAM picks the value up [meas:
    // syxg50.dll, organ level / routing / mode probes].
    Fe f;
    f.bank->table = true;
    // Volume: applied by the next CC11 (or CC7, reset controllers) of the part.
    f.xg({0x08, 0x01, 0x0B, 0x20, 0x7F});
    CHECK(f.part(1).gain_volume == 100);
    f.cc(1, 11, 127);
    CHECK(f.part(1).gain_volume == 0x20);
    // Master volume: each part takes it at its next gain update, the others keep the old one.
    f.xg({0x00, 0x00, 0x04, 0x20, 0x7F});
    CHECK(f.fe->system().master_volume == 0x20);
    CHECK(f.part(1).gain_master == 127);
    f.cc(1, 11, 127);
    CHECK(f.part(1).gain_master == 0x20);
    CHECK(f.part(2).gain_master == 127);
    f.xg({0x00, 0x00, 0x04, 0x30});
    CHECK(f.part(2).gain_master == 0x30);
    // Program: written, not changed; a later program change message sets its own.
    const auto prog = f.part(1).program;
    f.xg({0x08, 0x01, 0x03, 0x50, 0x40});
    CHECK(f.ram(1).program_number == 0x50);
    CHECK(f.part(1).program == prog);
    // Receive channel: the channel map keeps the part until a receive-channel change of any
    // part rebuilds it from the RAM.
    f.xg({0x08, 0x01, 0x04, 0x02, 0x40});
    f.sink.actions.clear();
    f.send({0x91, 60, 100});
    CHECK(f.sink.actions.size() == 1);
    f.xg({0x08, 0x05, 0x04, 0x05});
    f.sink.actions.clear();
    f.send({0x91, 60, 100});
    CHECK(f.sink.actions.empty());
    f.send({0x92, 60, 100});
    CHECK(f.sink.actions.size() == 2);
    // Mono / poly: no damp now (the clean write damps the part); the next note uses the RAM.
    f.sink.actions.clear();
    f.xg({0x08, 0x03, 0x05, 0x00, 0x40});
    CHECK(f.sink.actions.empty());
    CHECK(f.ram(3).mode == 0);
    f.xg({0x08, 0x03, 0x05, 0x01});
    CHECK(f.sink.actions.size() == 1);
    // Part mode: written but not applied; the part is silent (the DLL takes the drum path from
    // the RAM without a kit) until a program change, which plays it in its applied mode.
    f.xg({0x08, 0x04, 0x07, 0x02, 0x40});
    CHECK(f.ram(4).part_mode == 2);
    CHECK(!f.part(4).program.valid);
    f.send({0xC4, 20});
    CHECK(f.ram(4).part_mode == 0);
    CHECK(f.part(4).program.valid);
    CHECK(f.part(4).program.program == 20);
}

TEST_CASE("front_end_resets") {
    Fe f;
    f.cc(0, 7, 20);
    f.xg({0x02, 0x01, 0x0C, 0x10});
    // GM On: GM mode, everything back to defaults, every part silenced.
    f.send({0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7});
    CHECK(f.fe->reset_mode() == ResetMode::GM);
    CHECK(f.ram(0).volume == 100);
    CHECK(f.fe->effects()[0x0C] == 64);
    CHECK(f.sink.count<action::KillAll>() == 16);
    // GM / GS drum parts play the GS kit row.
    CHECK(f.part(9).program.bank_msb == kDrumBankMsbGs);
    f.send({0xF0, 0x7E, 0x7F, 0x09, 0x03, 0xF7});
    CHECK(f.fe->reset_mode() == ResetMode::GM2);
    f.send(gs({0x40, 0x00, 0x7F, 0x00}));
    CHECK(f.fe->reset_mode() == ResetMode::GS);
    f.xg({0x00, 0x00, 0x7E, 0x00});
    CHECK(f.fe->reset_mode() == ResetMode::XG);
    CHECK(f.part(9).program.bank_msb == 127);
    // A GS message with a bad checksum is ignored.
    auto bad = gs({0x40, 0x00, 0x7F, 0x00});
    bad[bad.size() - 2] ^= 1;
    f.send(bad);
    CHECK(f.fe->reset_mode() == ResetMode::XG);
}

TEST_CASE("front_end_2006le_ignores_xg_parameters_in_gs_mode") {
    Fe f;
    f.bank->xg_in_gs = false;
    f.send(gs({0x40, 0x00, 0x7F, 0x00}));
    f.xg({0x08, 0x00, 0x0B, 0x20});
    CHECK(f.ram(0).volume == 100);       // ignored
    f.send(gs({0x40, 0x11, 0x19, 0x20}));
    CHECK(f.ram(0).volume == 0x20);      // GS part level still works
    f.xg({0x00, 0x00, 0x7E, 0x00});      // XG System On is obeyed
    CHECK(f.fe->reset_mode() == ResetMode::XG);
    f.xg({0x08, 0x00, 0x0B, 0x20});
    CHECK(f.ram(0).volume == 0x20);
}

TEST_CASE("front_end_effect_type_writes_count") {
    HVYXG_NEED_TABLES();
    Fe f;
    const auto& e = f.fe->effects();
    const auto r0 = e.reverb_type_writes, c0 = e.chorus_type_writes, v0 = e.variation_type_writes;
    f.xg({0x02, 0x01, 0x00, 0x01, 0x00});  // Hall 1 again (MSB + LSB): two writes
    CHECK(e.reverb_type_writes == r0 + 2);
    f.xg({0x02, 0x01, 0x20, 0x41});
    CHECK(e.chorus_type_writes == c0 + 1);
    f.xg({0x02, 0x01, 0x40, 0x05});
    CHECK(e.variation_type_writes == v0 + 1);
    // A parameter write is not a type write; a reset counts for all.
    f.xg({0x02, 0x01, 0x02, 0x20});
    CHECK(e.reverb_type_writes == r0 + 2);
    CHECK(e[0x02] == 0x20);
    f.xg({0x00, 0x00, 0x7E, 0x00});
    CHECK(e.reverb_type_writes == r0 + 3);
    CHECK(e.chorus_type_writes == c0 + 2);
    CHECK(e.variation_type_writes == v0 + 2);
    CHECK(e[0x02] == 18);  // Hall 1 reverb time back
    // Insertion blocks count too.
    const auto i0 = f.fe->insertions()[2].type_writes;
    f.xg({0x03, 0x02, 0x00, 0x49, 0x00});
    CHECK(f.fe->insertions()[2].type_writes == i0 + 2);
}

TEST_CASE("front_end_rpn") {
    Fe f;
    // Bend range 12 via RPN 0,0, then the null RPN blocks data entry.
    f.cc(0, 101, 0);
    f.cc(0, 100, 0);
    f.cc(0, 6, 12);
    CHECK(f.ram(0).bend.pitch_control == 64 + 12);
    f.cc(0, 96, 0);  // increment
    CHECK(f.ram(0).bend.pitch_control == 64 + 13);
    f.cc(0, 101, 0x7F);
    f.cc(0, 100, 0x7F);
    f.cc(0, 6, 2);
    CHECK(f.ram(0).bend.pitch_control == 64 + 13);
    // Fine (14 bit) and coarse tune.
    f.cc(0, 101, 0);
    f.cc(0, 100, 1);
    f.cc(0, 6, 0x50);
    f.cc(0, 38, 0x11);
    CHECK(f.part(0).rpn.fine_msb == 0x50);
    CHECK(f.part(0).rpn.fine_lsb == 0x11);
    f.cc(0, 100, 2);
    f.cc(0, 6, 0x7F);
    CHECK(f.part(0).rpn.coarse == 0x58);
    // GM2 modulation depth range: 1 semitone → MW LFO PMod 20.
    f.cc(0, 100, 5);
    f.cc(0, 6, 1);
    f.cc(0, 38, 0);
    CHECK(f.ram(0).mw.lfo_pmod_depth == 20);
}

TEST_CASE("front_end_nrpn_part_and_drums") {
    Fe f;
    // XG part NRPN 01 20 (cutoff) and 01 63 (attack).
    f.cc(0, 99, 0x01);
    f.cc(0, 98, 0x20);
    f.cc(0, 6, 90);
    CHECK(f.ram(0).filter_cutoff_freq == 90);
    f.cc(0, 98, 0x63);
    f.cc(0, 6, 30);
    CHECK(f.ram(0).eg_attack_time == 30);
    // Drum NRPN on part 10 (drum setup 1): 1A rr level, 1C rr pan, 17 rr decay (both).
    f.cc(9, 99, 0x1A);
    f.cc(9, 98, 38);
    f.cc(9, 6, 77);
    CHECK(f.fe->drum_setup(0)[38 - 13].level == 77);
    f.cc(9, 99, 0x17);
    f.cc(9, 6, 20);
    CHECK(f.fe->drum_setup(0)[38 - 13].eg_decay1_rate == 20);
    CHECK(f.fe->drum_setup(0)[38 - 13].eg_decay2_rate == 20);
    // Data entry applies to the last selected of RPN / NRPN.
    f.cc(9, 101, 0);
    f.cc(9, 100, 0);
    f.cc(9, 6, 5);
    CHECK(f.ram(9).bend.pitch_control == 64 + 5);
    CHECK(f.fe->drum_setup(0)[38 - 13].eg_decay1_rate == 20);
    // A normal part playing an XG kit (MSB 127) edits its own copy, not the setup RAM.
    f.cc(1, 0, 127);
    f.send({0xC1, 25});
    CHECK(f.part(1).drum_setup_program == f.part(1).program);
    CHECK(f.part(1).drum_setup[36 - 13].level == 25);
    f.cc(1, 99, 0x1A);
    f.cc(1, 98, 36);
    f.cc(1, 6, 99);
    CHECK(f.part(1).drum_setup[36 - 13].level == 99);
    CHECK(f.fe->drum_setup(0)[36 - 13].level == 0);
    // XG drum setup SysEx 3n rr pp.
    f.xg({0x30, 40, 0x04, 0x11});
    CHECK(f.fe->drum_setup(0)[40 - 13].pan == 0x11);
    f.xg({0x31, 40, 0x50, 0x22});  // HPF cutoff of setup 2
    CHECK(f.fe->drum_setup(1)[40 - 13].hpf_cutoff_freq == 0x22);
}

TEST_CASE("front_end_drum_rules") {
    Fe f;
    // A program change on the drum part loads drum setup 1 from the kit.
    f.send({0xC9, 8});
    CHECK(f.part(9).program == midi::Program{true, 127, 0, 8});
    CHECK(f.fe->drum_setup(0)[36 - 13].level == 8);
    // A melodic bank MSB on a drum-mode part plays the GS kit row.
    f.cc(9, 0, 0);
    f.send({0xC9, 16});
    CHECK(f.part(9).program.bank_msb == kDrumBankMsbGs);
    CHECK(f.part(9).program.program == 16);
    // GS "use for rhythm part" (40 1x 15) on part 2 with bank 0 → a kit, drum setup 1.
    f.send(gs({0x40, 0x12, 0x15, 0x01}));
    CHECK(f.ram(1).part_mode == 2);
    CHECK(f.part(1).program.bank_msb == kDrumBankMsbGs);
    // Back to a normal part: the melodic program again.
    f.send(gs({0x40, 0x12, 0x15, 0x00}));
    CHECK(f.ram(1).part_mode == 0);
    CHECK(f.part(1).program.bank_msb == 0);
    // Bank select alone does not change the program.
    f.cc(0, 0, 8);
    CHECK(f.part(0).program.bank_msb == 0);
    f.send({0xC0, 4});
    CHECK(f.part(0).program == midi::Program{true, 8, 0, 4});
}

TEST_CASE("front_end_channel_messages") {
    Fe f;
    f.send({0x90, 60, 100});
    f.send({0x90, 60, 0});  // velocity 0 = note off
    f.send({0x80, 61, 10});
    CHECK(f.sink.count<action::NoteOn>() == 1);
    CHECK(f.sink.count<action::NoteOff>() == 2);
    // Sustain reports every change; sostenuto only its transitions.
    f.cc(0, 64, 127);
    f.cc(0, 64, 100);
    CHECK(f.sink.count<action::SustainChange>() == 2);
    CHECK(f.part(0).controller.sustain);
    f.cc(0, 66, 127);
    f.cc(0, 66, 120);
    CHECK(f.sink.count<action::SostenutoChange>() == 1);
    // Reset all controllers: bend, modulation, expression, pedals.
    f.send({0xE0, 0x7F, 0x7F});
    f.cc(0, 1, 50);
    f.cc(0, 11, 30);
    f.cc(0, 121, 0);
    CHECK(f.part(0).pitchbend == 0x2000);
    CHECK(f.part(0).controller.modulation == 0);
    CHECK(f.part(0).controller.expression == 127);
    CHECK_FALSE(f.part(0).controller.sustain);
    CHECK_FALSE(f.part(0).controller.sostenuto);
    // All notes off / mono / poly (front_end_mono_poly_messages).
    f.cc(0, 123, 0);
    f.cc(0, 126, 1);
    CHECK(f.ram(0).mode == 0);
    f.cc(0, 127, 0);
    CHECK(f.ram(0).mode == 1);
    CHECK(f.sink.count<action::ReleaseAll>() == 1);
    CHECK(f.sink.count<action::KillAll>() == 2);
    // Aftertouch, CC → RAM.
    f.send({0xD0, 33});
    f.send({0xA0, 60, 44});
    CHECK(f.part(0).cat_value == 33);
    CHECK(f.part(0).pat_values[60] == 44);
    f.cc(0, 7, 90);
    f.cc(0, 10, 0);
    f.cc(0, 74, 20);
    f.cc(0, 91, 1);
    f.cc(0, 94, 2);
    CHECK(f.ram(0).volume == 90);
    CHECK(f.ram(0).pan == 0);
    CHECK(f.ram(0).filter_cutoff_freq == 20);
    CHECK(f.ram(0).reverb_send == 1);
    CHECK(f.ram(0).variation_send == 2);
    // Two parts on one receive channel both get the notes.
    f.xg({0x08, 0x03, 0x04, 0x00});
    f.sink.actions.clear();
    f.send({0x90, 64, 64});
    CHECK(f.sink.count<action::NoteOn>() == 2);
    // A part off (7F) receives nothing.
    f.xg({0x08, 0x00, 0x04, 0x7F});
    f.sink.actions.clear();
    f.send({0x90, 64, 64});
    CHECK(f.sink.count<action::NoteOn>() == 1);
    // Universal real-time master volume: both DLLs write its MSB to the XG master volume
    // (LSB ignored) and the last of the two messages wins [meas].
    CHECK(f.fe->system().master_volume == 127);
    f.send({0xF0, 0x7F, 0x7F, 0x04, 0x01, 0x00, 0x40, 0xF7});
    CHECK(f.fe->gm_master_volume() == 0x2000);
    CHECK(f.fe->system().master_volume == 64);
    f.send({0xF0, 0x7F, 0x7F, 0x04, 0x01, 0x7F, 0x3F, 0xF7});
    CHECK(f.fe->system().master_volume == 63);
    f.send({0xF0, 0x7F, 0x7F, 0x04, 0x01, 0x7F, 0x7E, 0xF7});
    CHECK(f.fe->system().master_volume == 126);
    f.send({0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x04, 100, 0xF7});
    CHECK(f.fe->system().master_volume == 100);
    f.send({0xF0, 0x7F, 0x7F, 0x04, 0x01, 0x7F, 0x7F, 0xF7});
    CHECK(f.fe->system().master_volume == 127);
    f.send({0xF0, 0x7F, 0x7F, 0x04, 0x01, 0x00, 0x20, 0xF7});
    CHECK(f.fe->system().master_volume == 32);
    // Every reset restores 127 (measured: GM On, GM2 On, XG On, XG All Parameter Reset).
    f.send({0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7});
    CHECK(f.fe->system().master_volume == 127);
}

TEST_CASE("front_end_ports") {
    Fe f(midi::FrontEndOptions{2, -1});
    CHECK(f.fe->part_count() == 32);
    f.send({0xB0, 7, 33}, 1);
    CHECK(f.ram(16).volume == 33);
    CHECK(f.ram(0).volume == 100);
    CHECK(f.ram(25).part_mode == 2);  // part 10 of port B is a drum part too
}

TEST_CASE("synth_event_timing_and_stream") {
    HVYXG_NEED_TABLES();
    Log log;
    auto bank = std::make_shared<FakeBank>();
    bank->log = &log;
    Synth::Options o;
    o.tables = test::tables();
    o.bank = bank;
    Synth s(o);
    CHECK(s.block_size() == 64);
    std::vector<float> buf(2 * 1000);
    // An event acts from the start of the block that contains it, as in the DLLs:
    // offsets 0, 63 → block 0; 64 → block 1.
    const std::uint8_t n0[] = {0x90, 60, 100}, n1[] = {0x90, 61, 100}, n2[] = {0x90, 62, 100};
    s.midi(n2, 64);
    s.midi(n1, 63);
    s.midi(n0, 0);
    s.render(buf.data(), 10);  // a partial block: the rest render later
    s.render(buf.data(), 300);
    REQUIRE(log.notes.size() == 3);
    CHECK(log.notes[0] == std::pair<std::uint64_t, int>{0, 60});
    CHECK(log.notes[1] == std::pair<std::uint64_t, int>{0, 61});
    CHECK(log.notes[2] == std::pair<std::uint64_t, int>{1, 62});
    // Offsets are relative to the next render call. 310 frames in (block 4, 256..319, is
    // rendering): offset 30 → frame 340 → block 5; offset 5 → frame 315, whose block has
    // started → also from block 5.
    s.midi(n0, 30);
    s.midi(n1, 5);
    s.render(buf.data(), 200);
    REQUIRE(log.notes.size() == 5);
    CHECK(log.notes[3] == std::pair<std::uint64_t, int>{5, 61});
    CHECK(log.notes[4] == std::pair<std::uint64_t, int>{5, 60});
    // Running status, a real-time byte inside a message, SysEx split over two calls.
    log.notes.clear();
    const std::uint8_t a[] = {0x91, 40, 1, 0xF8, 41, 2, 0xF0, 0x43, 0x10, 0x4C, 0x08, 0x01};
    const std::uint8_t b[] = {0x0B, 0x22, 0xF7, 42, 3};
    s.midi_stream(a);
    s.midi_stream(b);
    s.render(buf.data(), 64);
    REQUIRE(log.notes.size() == 2);
    CHECK(log.notes[0].second == 40);
    CHECK(log.notes[1].second == 41);
    CHECK(SynthAccess::part(s, 1).ram->volume == 0x22);
    // The note after the SysEx has no status (running status was cancelled): dropped.
    CHECK(s.reset_mode() == ResetMode::XG);
}

TEST_CASE("bank_loading_errors") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "hvyxg_bank_test";
    fs::create_directories(dir);
    const auto write = [&](const char* name, const std::vector<std::uint8_t>& bytes) {
        std::ofstream(dir / name, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return dir / name;
    };
    const auto kind = [](auto&& f) {
        try {
            f();
        } catch (const BankError& e) {
            return static_cast<int>(e.kind());
        }
        return -1;
    };
    const auto wave = write("wave.tbl", std::vector<std::uint8_t>(64, 0));
    // Missing file.
    CHECK(kind([&] { load_bank(dir / "nope.tbl", wave, hvyxg::Model::Auto, test::bank_options()); }) == static_cast<int>(BankError::Kind::Io));
    // Short file, unknown header.
    const auto short_bin = write("short.tbl", {'M', 'U', '5', '0'});
    CHECK(kind([&] { load_bank(short_bin, wave, hvyxg::Model::Auto, test::bank_options()); }) == static_cast<int>(BankError::Kind::Header));
    // S-YXG50 header whose segments do not fit the file.
    std::vector<std::uint8_t> bin(0x64 + 16, 0);
    bin[0] = 'M';
    bin[0x1F] = 1;
    bin[0x20] = 0xFF;  // segment 0: 255 bytes
    CHECK_FALSE(sniff_model(bin));
    const auto big = write("big.tbl", bin);
    CHECK(kind([&] { load_bank(big, wave, Model::Syxg50, test::bank_options()); }) == static_cast<int>(BankError::Kind::Header));
    // A header that fits but lacks the segments the engine needs.
    bin[0x20] = 16;
    CHECK(sniff_model(bin) == Model::Syxg50);
    const auto empty = write("empty.tbl", bin);
    CHECK(kind([&] { load_bank(empty, wave, hvyxg::Model::Auto, test::bank_options()); }) == static_cast<int>(BankError::Kind::Table));
    // S-YXG2006LE: the right magic over a broken table.
    std::vector<std::uint8_t> le(0x100, 0xFF);
    const char magic[] = "UTG VPRM";
    std::copy(magic, magic + 8, le.begin());
    CHECK(sniff_model(le) == Model::Syxg2006le);
    const auto le_bin = write("le.tbl", le);
    CHECK(kind([&] { load_bank(le_bin, wave, hvyxg::Model::Auto, test::bank_options()); }) == static_cast<int>(BankError::Kind::Table));
    // The wrong header for a forced model.
    CHECK(kind([&] { load_bank(big, wave, Model::Syxg2006le, test::bank_options()); }) == static_cast<int>(BankError::Kind::Header));
    // Synth reports the same.
    Synth::Options o;
    o.tables = test::tables();
    o.bin_file = dir / "nope.tbl";
    o.data_file = wave;
    CHECK_THROWS_AS(Synth{o}, BankError);
    fs::remove_all(dir);
}

TEST_CASE("front_end_gs_effects_syxg50") {
    HVYXG_NEED_TABLES();
    Fe f;
    f.bank->model = Model::Syxg50;
    const auto& e = f.fe->effects();
    f.send(gs({0x40, 0x00, 0x7F, 0x00}));
    // GS reset: reverb type Hall 2 over the XG Hall 1 parameters; chorus keeps the XG image.
    CHECK(e[0x00] == 0x01);
    CHECK(e[0x01] == 0x01);
    CHECK(e[0x02] == 18);
    CHECK(e[0x05] == 13);
    CHECK(e[0x06] == 49);
    CHECK(e[0x0C] == 64);
    CHECK(e[0x20] == 0x41);
    CHECK(e[0x22] == 6);
    CHECK(e[0x25] == 0x6A);
    // Macro Room 1: character 0, pre-LPF 3, time 80.
    f.send(gs({0x40, 0x01, 0x30, 0x00}));
    CHECK(e[0x00] == 0x02);
    CHECK(e[0x01] == 0x00);
    CHECK(e[0x02] == 16);      // time 80 through the Room 1 / Plate table
    CHECK(e[0x06] == 0x30);    // pre-LPF 3
    CHECK(e[0x12] == 0x35);    // ER / reverb balance of the S-YXG50 Room 1 row
    // Time, then character Hall 1 (time 80 through the other table), level.
    f.send(gs({0x40, 0x01, 0x31, 0x03}));
    CHECK(e[0x00] == 0x01);
    CHECK(e[0x02] == 23);
    f.send(gs({0x40, 0x01, 0x34, 127}));
    CHECK(e[0x02] == 57);
    f.send(gs({0x40, 0x01, 0x33, 100}));
    CHECK(e[0x0C] == 100);
    // Macro Delay: no reverb.
    f.send(gs({0x40, 0x01, 0x30, 0x06}));
    CHECK(e[0x00] == 0x00);
    // An XG reverb type outside Room / Hall / Stage / Plate stops the character / time.
    f.xg({0x02, 0x01, 0x00, 0x11, 0x00});
    const std::uint8_t time = e[0x02];
    f.send(gs({0x40, 0x01, 0x34, 0}));
    CHECK(e[0x02] == time);
    // Chorus macro Feedback Chorus: rate 2, depth 24, feedback 64, delay 127.
    f.send(gs({0x40, 0x01, 0x38, 0x04}));
    CHECK(e[0x20] == 0x41);
    CHECK(e[0x22] == 5);
    CHECK(e[0x23] == 32);
    CHECK(e[0x24] == 96);
    CHECK(e[0x25] == 127);
    CHECK(e[0x2C] == 64);
    // Multi-byte messages set every parameter: level, feedback, delay, rate, depth, send.
    f.send(gs({0x40, 0x01, 0x3A, 11, 12, 13, 60, 15, 16}));
    CHECK(e[0x2C] == 11);
    CHECK(e[0x24] == 70);
    CHECK(e[0x25] == 13);
    CHECK(e[0x22] == 127);  // rate ≥ 48
    CHECK(e[0x23] == 20);
    CHECK(e[0x2E] == 16);
}

TEST_CASE("front_end_gs_effects_2006le") {
    HVYXG_NEED_TABLES();
    Fe f;
    const auto& e = f.fe->effects();
    f.send(gs({0x40, 0x00, 0x7F, 0x00}));
    // GS reset: the Hall 2 macro and the Chorus 3 macro with delay 5.
    CHECK(e[0x00] == 0x01);
    CHECK(e[0x01] == 0x01);
    CHECK(e[0x02] == 18);
    CHECK(e[0x05] == 0x12);
    CHECK(e[0x06] == 0x36);
    CHECK(e[0x12] == 0x32);
    CHECK(e[0x22] == 8);
    CHECK(e[0x23] == 25);
    CHECK(e[0x24] == 0x44);
    CHECK(e[0x25] == 5);
    CHECK(e[0x30] == 0x28);
    // Macro Delay: Room 1 with an early-reflection delay line; time 32 → 9.
    f.send(gs({0x40, 0x01, 0x30, 0x06}));
    CHECK(e[0x00] == 0x02);
    CHECK(e[0x01] == 0x00);
    CHECK(e[0x02] == 9);
    CHECK(e[0x04] == 0x3F);
    CHECK(e[0x10] == 0x10);
    CHECK(e[0x14] == 0x5A);
    // The time table follows the macro (6: the "other" table).
    f.send(gs({0x40, 0x01, 0x34, 127}));
    CHECK(e[0x02] == 57);
    // Only one-parameter messages count.
    f.send(gs({0x40, 0x01, 0x33, 0x20, 0x30}));
    CHECK(e[0x0C] == 64);
    f.send(gs({0x40, 0x01, 0x33, 0x20}));
    CHECK(e[0x0C] == 0x20);
    f.send(gs({0x40, 0x01, 0x3F, 64}));
    CHECK(e[0x2E] == 64);
    // A chorus macro leaves the chorus → reverb send alone.
    f.send(gs({0x40, 0x01, 0x38, 0x00}));
    CHECK(e[0x2E] == 64);
    CHECK(e[0x25] == 0x70);
}

TEST_CASE("front_end_2006le_gs_banks") {
    Fe f;
    f.bank->gsb = BankImpl::GsBanks{119, 118};
    f.bank->kit118 = true;
    f.send(gs({0x40, 0x00, 0x7F, 0x00}));
    CHECK(f.part(0).program == midi::Program{true, 119, 0, 0});
    CHECK(f.part(9).program == midi::Program{true, 118, 0, 0});
    // CC#0 is the GS variation number, CC#32 is ignored.
    f.cc(0, 0, 8);
    f.cc(0, 32, 3);
    f.send({0xC0, 25});
    CHECK(f.part(0).program == midi::Program{true, 119, 8, 25});
    // The rhythm part plays MSB 118 whatever CC#0 says; its drum setup comes from that kit.
    f.cc(9, 0, 0);
    f.send({0xC9, 16});
    CHECK(f.part(9).program == midi::Program{true, 118, 0, 16});
    CHECK(f.fe->drum_setup(0)[36 - 13].pitch_coarse == 118);
    CHECK(f.fe->drum_setup(0)[36 - 13].level == 16);
    // Tone number (40 1x 00) needs both bytes; a program-only message is ignored.
    f.send(gs({0x40, 0x12, 0x00, 0x01, 0x30}));
    CHECK(f.part(1).program == midi::Program{true, 119, 1, 0x30});
    f.send(gs({0x40, 0x12, 0x01, 0x05}));
    CHECK(f.part(1).program.program == 0x30);
    // GS receive switches map onto XG 30..3F.
    f.send(gs({0x40, 0x12, 0x03, 0x00}));
    CHECK(f.ram(1).rcv_pitch_bend == 0);
    // XG System On leaves the GS banks.
    f.xg({0x00, 0x00, 0x7E, 0x00});
    f.send({0xC0, 25});
    CHECK(f.part(0).program == midi::Program{true, 0, 0, 25});
}

TEST_CASE("front_end_variation_insertion_writes_silence_parts") {
    // syxg50.dll (renders: organ chords on the insertion part and on another part, each
    // write mid-note): a write of 02 01 5B silences the old and the new insertion part as all
    // sound off does, also when the value does not change; a write of 5A silences the
    // insertion part. Other parts keep sounding. The 2006LE silences nothing.
    const auto kills = [](const Sink& s) {
        std::vector<std::size_t> ids;
        for (const auto& a : s.actions)
            if (const auto* k = std::get_if<action::KillAll>(&a)) ids.push_back(k->part->id);
        return ids;
    };
    Fe f;
    f.bank->model = Model::Syxg50;
    f.xg({0x02, 0x01, 0x5B, 0x00});  // from off (7F): the new part only
    CHECK(kills(f.sink) == std::vector<std::size_t>{0});
    f.sink.actions.clear();
    f.xg({0x02, 0x01, 0x5B, 0x01});
    CHECK(kills(f.sink) == std::vector<std::size_t>{0, 1});
    f.sink.actions.clear();
    f.xg({0x02, 0x01, 0x5B, 0x01});
    CHECK(kills(f.sink) == std::vector<std::size_t>{1, 1});
    f.sink.actions.clear();
    f.xg({0x02, 0x01, 0x5A, 0x01});
    f.xg({0x02, 0x01, 0x5A, 0x00});
    CHECK(kills(f.sink) == std::vector<std::size_t>{1, 1});
    CHECK(f.fe->effects().variation_connection_writes > 0);
    f.sink.actions.clear();
    f.xg({0x02, 0x01, 0x5B, 0x7F});
    CHECK(kills(f.sink) == std::vector<std::size_t>{1});
    f.sink.actions.clear();
    f.xg({0x02, 0x01, 0x5A, 0x00});
    f.xg({0x02, 0x01, 0x56, 0x30});
    CHECK(kills(f.sink).empty());

    Fe g;
    g.bank->model = Model::Syxg2006le;
    g.xg({0x02, 0x01, 0x5B, 0x00});
    g.xg({0x02, 0x01, 0x5A, 0x00});
    g.xg({0x02, 0x01, 0x5B, 0x01});
    CHECK(kills(g.sink).empty());
}

TEST_CASE("front_end_syxg50_cc94_only_in_system_connection") {
    // (measured: CC94 before a later 02 01 5A 01 leaves the variation
    // silent; the SysEx part parameter 08 nn 14 works in either connection).
    Fe f;
    f.bank->model = Model::Syxg50;
    f.cc(0, 94, 100);
    CHECK(f.ram(0).variation_send == 0);
    f.xg({0x08, 0x00, 0x14, 0x30});
    CHECK(f.ram(0).variation_send == 0x30);
    f.xg({0x02, 0x01, 0x5A, 0x01});
    f.cc(0, 94, 100);
    CHECK(f.ram(0).variation_send == 100);
    f.xg({0x02, 0x01, 0x5A, 0x00});
    f.cc(0, 94, 5);
    CHECK(f.ram(0).variation_send == 100);

    Fe g;  // the 2006LE stores it in both connections
    g.bank->model = Model::Syxg2006le;
    g.cc(0, 94, 100);
    CHECK(g.ram(0).variation_send == 100);
}

TEST_CASE("front_end_syxg50_gm2_on_and_gm_off") {
    HVYXG_NEED_TABLES();
    // GM2 On keeps the effect RAM, sets the GM2
    // reverb (Large Hall = 01 07) and chorus (Chorus 3 = 41 05, to reverb 0) as type writes,
    // and every part to MSB 121 (drum parts 120), LSB 0. GM Off is the XG System On. The 2006LE keeps its old rules.
    Fe f;
    f.bank->model = Model::Syxg50;
    f.xg({0x02, 0x01, 0x0C, 0x20});
    f.xg({0x02, 0x01, 0x5A, 0x01});
    const auto restarts = f.fe->effects().restarts;
    f.send({0xF0, 0x7E, 0x7F, 0x09, 0x03, 0xF7});
    const auto& e = f.fe->effects();
    CHECK(e.restarts == restarts + 1);
    CHECK(e[0x00] == 0x01);
    CHECK(e[0x01] == 0x07);
    CHECK(e[0x02] == 25);  // the type's defaults
    CHECK(e[0x0C] == 0x20);  // kept
    CHECK(e[0x20] == 0x41);
    CHECK(e[0x21] == 0x05);
    CHECK(e[0x22] == 9);
    CHECK(e[0x2E] == 0);
    CHECK(e[0x5A] == 1);  // kept
    CHECK(f.ram(0).bank_msb == 121);
    CHECK(f.ram(9).bank_msb == kDrumBankMsbGm2);
    CHECK(f.ram(0).bank_lsb == 0);
    // GM Off: an XG reset.
    f.cc(0, 7, 30);
    f.send({0xF0, 0x7E, 0x7F, 0x09, 0x02, 0xF7});
    CHECK(f.ram(0).volume == 100);
    CHECK(f.ram(0).bank_msb == 0);
    CHECK(e[0x00] == 0x01);
    CHECK(e[0x01] == 0x00);
    CHECK(e[0x5A] == 0);

    Fe g;
    g.bank->model = Model::Syxg2006le;
    g.xg({0x02, 0x01, 0x0C, 0x20});
    g.send({0xF0, 0x7E, 0x7F, 0x09, 0x03, 0xF7});
    CHECK(g.fe->effects()[0x0C] == 0x40);
    CHECK(g.ram(0).bank_msb == 0);
    g.cc(0, 7, 30);
    g.send({0xF0, 0x7E, 0x7F, 0x09, 0x02, 0xF7});
    CHECK(g.ram(0).volume == 30);
}

TEST_CASE("front_end_kill_kinds_and_reset") {
    // CC120 silences the voices only; a reset silences every part, then resets the engine.
    Fe f;
    f.cc(0, 120, 0);
    REQUIRE(f.sink.actions.size() == 1);
    CHECK(std::get<action::KillAll>(f.sink.actions[0]).sound_off);
    f.sink.actions.clear();
    f.cc(0, 126, 1);
    CHECK_FALSE(std::get<action::KillAll>(f.sink.actions.at(0)).sound_off);
    f.sink.actions.clear();
    f.xg({0x00, 0x00, 0x7E, 0x00});
    CHECK(f.sink.count<action::KillAll>() == 16);
    CHECK(f.sink.count<action::Reset>() == 1);
    CHECK(std::holds_alternative<action::Reset>(f.sink.actions.at(16)));
}

TEST_CASE("front_end_syxg50_part_mode_change_selects_the_mode_bank") {
    // a clean part-mode change selects the bank and
    // program last used in the new mode (normal mode after a reset: 0 / 0 / 0), and a parameter
    // change leaves the pending bank select there [meas: the drum part switched to mode 0 plays
    // the piano, then program 16 the organ, −110 dB].
    Fe f;
    f.bank->model = Model::Syxg50;
    f.bank->table = true;
    f.xg({0x08, 0x09, 0x07, 0x00});
    CHECK(f.ram(9).bank_msb == 0);
    CHECK(f.ram(9).program_number == 0);
    f.send({0xC9, 16});
    CHECK(f.ram(9).bank_msb == 0);
    CHECK(f.ram(9).program_number == 16);
    // Back to drum setup 1: its kit (MSB 127, program 0); then normal again: program 16.
    f.xg({0x08, 0x09, 0x07, 0x02});
    CHECK(f.ram(9).bank_msb == 127);
    CHECK(f.ram(9).program_number == 0);
    f.xg({0x08, 0x09, 0x07, 0x00});
    CHECK(f.ram(9).program_number == 16);
    // The 2006LE keeps the RAM's bank and program.
    Fe g;
    g.xg({0x08, 0x09, 0x07, 0x00});
    CHECK(g.ram(9).bank_msb == 127);
}

TEST_CASE("front_end_ac1_ac2_values") {
    // Measured on both DLLs (AC1 / AC2 → amplitude, pitch, filter, LFO depths and the variation
    // control depths): AC1 / AC2 take a CC only on their current number; a number change keeps
    // the value; every reset clears it; reset all controllers clears it on the 2006LE only.
    for (const Model model : {Model::Syxg50, Model::Syxg2006le}) {
        CAPTURE(static_cast<int>(model));
        Fe f;
        f.bank->model = model;
        f.bank->table = model == Model::Syxg50;
        f.fe->reset(ResetMode::XG);
        const auto& c = f.part(0).controller;
        CHECK(c.ac[0] == 0);
        CHECK(c.ac[1] == 0);
        f.cc(0, 16, 127);
        f.cc(0, 17, 90);
        CHECK(c.ac[0] == 127);
        CHECK(c.ac[1] == 90);
        // Reassigned to CC20 / CC21 (never received): the values stay.
        f.xg({0x08, 0x00, 0x59, 20});
        f.xg({0x08, 0x00, 0x60, 21});
        CHECK(c.ac[0] == 127);
        CHECK(c.ac[1] == 90);
        // The old numbers no longer move them; the new ones do.
        f.cc(0, 16, 0);
        f.cc(0, 17, 0);
        CHECK(c.ac[0] == 127);
        CHECK(c.ac[1] == 90);
        f.cc(0, 20, 64);
        f.cc(0, 21, 30);
        CHECK(c.ac[0] == 64);
        CHECK(c.ac[1] == 30);
        // Reset all controllers.
        f.cc(0, 121, 0);
        if (model == Model::Syxg2006le) {
            CHECK(c.ac[0] == 0);
            CHECK(c.ac[1] == 0);
        } else {
            CHECK(c.ac[0] == 64);
            CHECK(c.ac[1] == 30);
        }
        // Back to CC16 (last received 0): the value stays again.
        f.cc(0, 20, 100);
        f.xg({0x08, 0x00, 0x59, 16});
        CHECK(c.ac[0] == 100);
        // Every number 0..95 counts, also one with its own function, and as well as it.
        f.xg({0x08, 0x00, 0x59, 7});
        CHECK(c.ac[0] == 100);  // not the part volume
        f.cc(0, 7, 50);
        CHECK(c.ac[0] == 50);
        CHECK(f.ram(0).volume == 50);
        f.xg({0x08, 0x00, 0x59, 0});
        f.cc(0, 0, 3);
        CHECK(c.ac[0] == 3);
        // Resets clear the values (XG On, GM On, XG All Parameter Reset; measured).
        f.send({0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7});
        CHECK(c.ac[0] == 0);
        CHECK(c.ac[1] == 0);
    }
}

TEST_CASE("front_end_mono_poly_messages") {
    // Measured on both DLLs: CC126 counts with a value up to 16, CC127 only with 0; an accepted
    // message or any 08 nn 05 write silences the part's notes (all sound off), also when the mode
    // does not change.
    for (const Model model : {Model::Syxg50, Model::Syxg2006le}) {
        CAPTURE(static_cast<int>(model));
        Fe f;
        f.bank->model = model;
        f.bank->table = model == Model::Syxg50;
        f.fe->reset(ResetMode::XG);
        f.sink.actions.clear();
        // Ignored: no mode change, no action.
        for (const std::uint8_t v : {17, 64, 127}) f.cc(0, 126, v);
        for (const std::uint8_t v : {1, 64, 127}) f.cc(0, 127, v);
        CHECK(f.ram(0).mode == 1);
        CHECK(f.sink.actions.empty());
        // Accepted.
        f.cc(0, 127, 0);  // poly again: silences the part
        CHECK(f.ram(0).mode == 1);
        CHECK(f.sink.count<action::KillAll>() == 1);
        f.cc(0, 126, 16);
        CHECK(f.ram(0).mode == 0);
        f.cc(0, 126, 0);
        CHECK(f.ram(0).mode == 0);
        CHECK(f.sink.count<action::KillAll>() == 3);
        f.cc(0, 127, 1);
        CHECK(f.ram(0).mode == 0);
        f.xg({0x08, 0x00, 0x05, 0x00});  // unchanged
        f.xg({0x08, 0x00, 0x05, 0x01});
        CHECK(f.ram(0).mode == 1);
        CHECK(f.sink.count<action::KillAll>() == 5);
        CHECK(f.sink.count<action::ReleaseAll>() == 0);
        for (const auto& a : f.sink.actions)
            if (const auto* k = std::get_if<action::KillAll>(&a)) CHECK(k->part->id == 0);
    }
}

TEST_CASE("front_end_xg2_eq_hpf_nrpns") {
    // XG2.0 (MU2000): part NRPNs 01 30 / 31 / 34 / 35 (part EQ bass / treble gain, bass /
    // treble frequency) and 01 24 (part HPF), drum NRPNs 30 / 31 / 34 / 35 / 24 rr (the note
    // EQ and HPF). Measured on S-MU2000: each NRPN changes the spectrum exactly as its SysEx.
    for (const bool exact : {false, true}) {
        CAPTURE(exact);
        midi::FrontEndOptions o;
        o.dll_exact = exact;
        Fe f(o);
        const auto nrpn = [&](std::uint8_t ch, std::uint8_t msb, std::uint8_t lsb, std::uint8_t v) {
            f.cc(ch, 99, msb);
            f.cc(ch, 98, lsb);
            f.cc(ch, 6, v);
        };
        nrpn(0, 0x01, 0x30, 0x4C);
        nrpn(0, 0x01, 0x31, 0x34);
        nrpn(0, 0x01, 0x34, 0x20);
        nrpn(0, 0x01, 0x35, 0x30);
        nrpn(0, 0x01, 0x24, 0x70);
        nrpn(9, 0x30, 38, 0x4A);
        nrpn(9, 0x31, 38, 0x36);
        nrpn(9, 0x34, 38, 0x10);
        nrpn(9, 0x35, 38, 0x2A);
        nrpn(9, 0x24, 38, 0x60);
        const auto& d = f.fe->drum_setup(0)[38 - 13];
        if (!exact) {
            CHECK(f.ram(0).eq_bass_gain == 0x4C);
            CHECK(f.ram(0).eq_treble_gain == 0x34);
            CHECK(f.ram(0).eq_bass_freq == 0x20);
            CHECK(f.ram(0).eq_treble_freq == 0x30);
            CHECK(f.fe->part_ram_ext(0).hpf_cutoff == 0x70);
            CHECK(d.eq_bass_gain == 0x4A);
            CHECK(d.eq_treble_gain == 0x36);
            CHECK(d.eq_bass_freq == 0x10);
            CHECK(d.eq_treble_freq == 0x2A);
            CHECK(d.hpf_cutoff_freq == 0x60);
            // Data increment / decrement work on them like on the other NRPNs.
            f.cc(9, 96, 0);
            CHECK(d.hpf_cutoff_freq == 0x61);
        } else {
            // DLL-exact: neither DLL renders them.
            CHECK(f.ram(0).eq_bass_gain == 64);
            CHECK(f.fe->part_ram_ext(0).hpf_cutoff == 64);
            CHECK(d == midi::xg::DrumSetup{});  // the fake kit image, untouched
        }
    }
}

TEST_CASE("front_end_0a_level_controls") {
    // 0A nn 40..45: the S-YXG2006LE's level control per controller (MW, PB, CAT, PAT, AC1, AC2);
    // DLL-exact keeps them on the 2006LE (its DLL acts on them) and drops the whole 0A block on
    // the S-YXG50 (its DLL has none), and the part HPF 0A nn 20 on both.
    for (const Model m : {Model::Syxg2006le, Model::Syxg50}) {
        for (const bool exact : {false, true}) {
            CAPTURE(exact);
            midi::FrontEndOptions o;
            o.dll_exact = exact;
            Fe f(o);
            f.bank->model = m;
            f.xg({0x0A, 0x00, 0x40, 0x7F, 0x00, 0x20, 0x60, 0x10, 0x50});
            f.xg({0x0A, 0x00, 0x20, 0x70});
            f.xg({0x0A, 0x00, 0x01, 0x01});
            const auto& e = f.fe->part_ram_ext(0);
            const bool kept = !exact || m == Model::Syxg2006le;
            CHECK(e.level_control == (kept ? std::array<std::uint8_t, 6>{0x7F, 0x00, 0x20, 0x60, 0x10, 0x50}
                                           : std::array<std::uint8_t, 6>{64, 64, 64, 64, 64, 64}));
            CHECK(e.mono_priority == (kept ? 1 : 0));
            CHECK(e.hpf_cutoff == (exact ? 64 : 0x70));
            f.fe->reset(ResetMode::XG);
            CHECK(f.fe->part_ram_ext(0).level_control == std::array<std::uint8_t, 6>{64, 64, 64, 64, 64, 64});
        }
    }
}

TEST_CASE("front_end_full_xg_on_the_2006le") {
    // `FrontEndOptions::full_xg` lifts the 2006LE's XGlite front-end limits: XG parameter
    // changes after a GS reset, and GS messages carrying several parameters (every byte to its
    // own address, as on the S-YXG50, through the 2006LE's GS translation).
    for (const bool full : {false, true}) {
        CAPTURE(full);
        midi::FrontEndOptions o;
        o.full_xg = full;
        Fe f(o);
        f.bank->model = Model::Syxg2006le;
        f.bank->xg_in_gs = false;
        f.bank->gsb = BankImpl::GsBanks{119, 118};
        f.send(gs({0x40, 0x00, 0x7F, 0x00}));
        REQUIRE(f.fe->reset_mode() == ResetMode::GS);
        f.xg({0x08, 0x00, 0x0B, 0x22});
        CHECK(f.ram(0).volume == (full ? 0x22 : 100));
        // GS part 1: volume (19) and pan (1C) in one message, then the volume alone.
        f.send(gs({0x40, 0x11, 0x19, 0x30, 0x40, 0x50, 0x10}));
        CHECK(f.ram(0).volume == (full ? 0x30 : 100));
        CHECK(f.ram(0).pan == (full ? 0x10 : 64));
        f.send(gs({0x40, 0x11, 0x19, 0x44}));
        CHECK(f.ram(0).volume == 0x44);
        // The program number alone (01) is a program change only with full XG; the tone number
        // (00, two bytes) works on both and keeps the 2006LE's GS banks.
        f.send(gs({0x40, 0x11, 0x00, 0x08, 0x05}));
        CHECK(f.part(0).program.program == 5);
        CHECK(f.part(0).program.bank_msb == 119);
        CHECK(f.part(0).program.bank_lsb == 8);
        f.send(gs({0x40, 0x11, 0x01, 0x06}));
        CHECK(f.part(0).program.program == (full ? 6 : 5));
        // GS reverb level and time in one message (40 01 33 / 34).
        const std::uint8_t level_before = f.fe->effects()[0x0C];
        f.send(gs({0x40, 0x01, 0x33, 0x20, 0x40}));
        CHECK((f.fe->effects()[0x0C] != level_before) == full);
    }
}
