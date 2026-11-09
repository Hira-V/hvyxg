// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "midi/front_end.hpp"

#include <algorithm>
#include <optional>

namespace hvyxg::midi {

namespace {

std::uint8_t clamp7(int v) { return static_cast<std::uint8_t>(std::clamp(v, 0, 127)); }

/// Roland GS part block (40 1x / 40 2x): x = 0 is part 10, 1..9 parts 1..9, A..F parts 11..16.
std::size_t gs_block_part(std::uint8_t x) {
    x &= 0x0F;
    if (x == 0) return 9;
    return x <= 9 ? x - 1u : x;
}

/// One parameter of the S-YXG50's XG parameter table (the DLL's per-byte descriptors, one
/// table per address block).
struct TableParam {
    std::uint8_t size;  ///< bytes of the parameter starting here; 0: a continuation byte
    std::uint8_t last;  ///< last address of the table block
};

/// The S-YXG50 table entry at (hi, mid, lo), or nullopt for a block the DLL has no table for
/// (it ignores those; hvyxg keeps its superset handling there).
std::optional<TableParam> syxg50_param(std::uint8_t hi, std::uint8_t mid, std::uint8_t lo) {
    const auto in = [&](std::uint8_t a, std::uint8_t b) { return lo >= a && lo <= b; };
    const auto one = [](std::uint8_t last) { return TableParam{1, last}; };
    if (hi == 0x00 && mid == 0x00) {
        // System: master tune 00-03 is one 4-byte parameter.
        if (lo == 0x00) return TableParam{4, 0x06};
        if (lo <= 0x03) return TableParam{0, 0x06};
        if (lo <= 0x06) return one(0x06);
        return std::nullopt;
    }
    if (hi == 0x02 && mid == 0x01) {
        // Effect 1: reverb / chorus / variation type MSB+LSB (00, 20, 40) and the variation
        // parameters 1-10 (42-55, MSB+LSB) are 2-byte parameters.
        static constexpr std::uint8_t blocks[][2] = {{0x00, 0x0D}, {0x10, 0x15}, {0x20, 0x2E},
                                                     {0x30, 0x35}, {0x40, 0x60}, {0x70, 0x75}};
        for (const auto& b : blocks) {
            if (!in(b[0], b[1])) continue;
            if (lo == 0x00 || lo == 0x20 || lo == 0x40) return TableParam{2, b[1]};
            if (lo == 0x01 || lo == 0x21 || lo == 0x41) return TableParam{0, b[1]};
            if (in(0x42, 0x55)) return TableParam{static_cast<std::uint8_t>(lo % 2 == 0 ? 2 : 0), b[1]};
            return one(b[1]);
        }
        return std::nullopt;
    }
    if (hi == 0x08) {
        // Multi part: detune 09-0A is one 2-byte parameter.
        if (in(0x00, 0x28)) return lo == 0x09 ? TableParam{2, 0x28} : (lo == 0x0A ? TableParam{0, 0x28} : one(0x28));
        if (in(0x30, 0x6E)) return one(0x6E);
        return std::nullopt;
    }
    if ((hi & 0xF0) == 0x30 && (hi & 0x0F) < 2 && mid >= 0x0D && mid <= 0x5B && lo <= 0x0F) return one(0x0F);
    return std::nullopt;
}

/// Parameters whose effect the S-YXG50 applies in the end-of-message handler (master tune,
/// master volume / attenuator, element reserve, program, receive channel, mono / poly,
/// key assign, part mode, volume). A parameter change carrying bytes past its parameter
/// is flagged as an error, which skips that handler: the RAM is written but the change
/// is not applied until a later event reads it (`deferred_lost_`) [code, meas]. The others
/// are read from the RAM and act anyway.
bool syxg50_deferred(std::uint8_t hi, std::uint8_t mid, std::uint8_t lo) {
    if (hi == 0x00 && mid == 0x00) return lo == 0x00 || lo == 0x04 || lo == 0x05;
    if (hi == 0x08) {
        switch (lo) {
        case 0x00: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x0B: return true;
        default: return false;
        }
    }
    return false;
}

} // namespace

FrontEnd::FrontEnd(std::shared_ptr<const Bank> bank, ActionSink& sink, FrontEndOptions options)
    : bank_(impl(std::move(bank))), sink_(sink), options_(options),
      part_count_(kPartsPerPort * std::clamp<std::size_t>(options.ports, 1, kMaxPorts)) {
    for (std::size_t i = 0; i < parts_.size(); ++i) {
        ram_[i].reset(i);
        parts_[i].id = i;
        parts_[i].ram = &ram_[i];
        parts_[i].ram_ext = &ram_ext_[i];
    }
    // The effect RAM's type defaults come from the bank's engine tables.
    effect_ = xg::EffectData(bank_->tables());
    for (auto& e : insertion_) e = xg::EffectInsertion(bank_->tables());
    effect_.mu_types = bank_->mu();
}

EngineState FrontEnd::state() const {
    EngineState s;
    s.system = &system_;
    s.reset_mode = &mode_;
    s.drum_setup = &drum_;
    s.effect1 = &effect_;
    s.effect_insertion = &insertion_;
    s.multi_eq = &multi_eq_;
    s.parts = parts_.data();
    s.part_count = part_count_;
    return s;
}

void FrontEnd::touch_all() {
    for (std::size_t i = 0; i < part_count_; ++i) ++parts_[i].touch;
}

void FrontEnd::refresh_gain(Part& p) {
    p.gain_volume = ram_[p.id].volume;
    p.gain_master = system_.master_volume;
}

void FrontEnd::refresh_gain_all() {
    for (std::size_t i = 0; i < parts_.size(); ++i) refresh_gain(parts_[i]);
}

void FrontEnd::rebuild_routing() {
    for (std::size_t i = 0; i < parts_.size(); ++i) ps_[i].routed_channel = ram_[i].rcv_channel;
}

void FrontEnd::power_on() {
    emit(action::Init{state()});
    reset(ResetMode::XG);
}

// ── Resets ──────────────────────────────────────────────────────────────────────────────

void FrontEnd::reset(ResetMode mode) {
    for (std::size_t i = 0; i < part_count_; ++i) emit(action::KillAll{&parts_[i]});
    emit(action::Reset{});
    mode_ = mode;
    system_.reset();
    gm_master_volume_ = 0x3FFF;
    // Effects: the XG reset image in every mode (both DLLs keep their XG effect state after a
    // GM reset; the counters restart the units); a GS reset then sets the GS reverb / chorus
    // the DLL's way (gs_effects.cpp).
    // S-YXG50 GM2 On does not reload the effect RAM (no, which
    // the XG and GM resets call); it sets the GM2 default reverb (type 4, Large Hall = XG
    // 01 07) and chorus (type 2, Chorus 3 = XG 41 05, chorus → reverb 0) through the GM2
    // global-parameter handlers, each a type write that loads the type's defaults [code, meas].
    const bool syxg50_gm2 = mode == ResetMode::GM2 && bank_->type() != Model::Syxg2006le;
    if (!syxg50_gm2) effect_.reset();
    else effect_.restart();  // the units are set up again from the RAM
    gs_effect_ = gs::EffectState{};
    if (mode == ResetMode::GS) gs::reset(gs_effect_, effect_, gs_dialect());
    if (syxg50_gm2) {
        effect_.write(0x00, 0x01);
        effect_.write(0x01, 0x07);
        effect_.write(0x20, 0x41);
        effect_.write(0x21, 0x05);
        effect_.write(0x2E, 0x00);
    }
    for (auto& e : insertion_) e.reset();
    multi_eq_.reset();
    for (auto& e : ram_ext_) e.reset();
    drum_kit_.fill(Program{});
    setup_bank_.fill({0x7F, 0, 0, 0, 0, 0, 0});
    for (std::size_t i = 0; i < parts_.size(); ++i) {
        Part& p = parts_[i];
        xg::MultiPart& r = ram_[i];
        r.reset(i);
        const bool drum = r.part_mode != 0;
        if (drum) r.key_assign = bank_->drum_part_key_assign(mode);
        // XG (and GM2) select the drum part's kit with MSB 127; GM / GS drum parts have bank 0
        // and play the GS kit row. A GS-translating engine (2006LE) sets its GS banks: MSB
        // 119 / 118, LSB 0, program 0 on every part [code].
        if (drum) r.bank_msb = (mode == ResetMode::XG || mode == ResetMode::GM2) ? 127 : 0;
        // S-YXG50 GM2 On: every part to the GM2 banks, MSB 121 (drum parts 120), LSB 0.
        if (syxg50_gm2) r.bank_msb = drum ? kDrumBankMsbGm2 : 121;
        if (mode == ResetMode::GS)
            if (const auto gb = bank_->gs_banks()) r.bank_msb = drum ? gb->drum_msb : gb->melodic_msb;
        p.controller = Controller{};
        p.pitchbend = 0x2000;
        p.cat_value = 0;
        p.pat_values.fill(0);
        p.rpn = Rpn{};
        p.controller.cc_values[7] = r.volume;
        p.controller.cc_values[10] = r.pan;
        p.controller.cc_values[11] = 127;
        p.controller.cc_values[91] = r.reverb_send;
        ps_[i] = PartState{};
        ps_[i].bank_msb = r.bank_msb;
        ps_[i].routed_channel = r.rcv_channel;
        ps_[i].applied_mode = r.part_mode;
        p.drum_setup_program = Program{};
        update_program(p, i < part_count_);
    }
    refresh_gain_all();
    system_.applied_master_tune = system_.get_master_tune();
    // Setups not loaded by a part (drum setup 2..) start as the Standard Kit.
    for (std::size_t s = 0; s < drum_.size(); ++s)
        if (!drum_kit_[s].valid) load_drum_slot(s, image_msb(kDrumBankMsbXg), 0);
}

void FrontEnd::all_notes_off() {
    for (std::size_t i = 0; i < part_count_; ++i) emit(action::ReleaseAll{&parts_[i]});
}

// ── Programs and drums ──────────────────────────────────────────────────────────────────

bool FrontEnd::plays_kit(const Part& p) const {
    if (p.ram->part_mode != 0) return true;
    // A normal part plays a kit for a kit bank MSB in XG / GM2 (measured on both DLLs:
    // MSB 127 on channel 1 plays the Standard Kit); GM ignores bank select.
    return (mode_ == ResetMode::XG || mode_ == ResetMode::GM2) && bank_->is_kit_bank(p.ram->bank_msb);
}

std::uint8_t FrontEnd::image_msb(std::uint8_t msb) const {
    // The S-YXG50 engine plays the XG kit tables in XG / GM2 mode (MSB 127 / 126 / 120) and
    // the GS kit table in GS / GM mode; hvyxg's GS marker MSB names the latter. The 2006LE
    // plays its GS kits (MSB 118) in GS mode.
    if (mode_ == ResetMode::XG || mode_ == ResetMode::GM2)
        return bank_->is_kit_bank(msb) && msb != kDrumBankMsbGs ? msb : kDrumBankMsbXg;
    if (gs_translated())
        return bank_->is_kit_bank(msb) && msb != kDrumBankMsbGs ? msb : bank_->gs_banks()->drum_msb;
    return kDrumBankMsbGs;
}

void FrontEnd::load_drum_slot(std::size_t slot, std::uint8_t msb, std::uint8_t program) {
    if (slot >= drum_.size()) return;
    if (auto img = bank_->drum_setup(msb, program)) drum_[slot] = *img;
    else drum_[slot].fill(xg::DrumSetup{});
    drum_kit_[slot] = Program{true, msb, 0, program};
}

void FrontEnd::update_program(Part& p, bool reload_drums) {
    const xg::MultiPart& r = *p.ram;
    std::uint8_t msb = r.bank_msb;
    // A drum-mode part always plays a kit: a melodic MSB selects the GS / GM kit row
    // (measured on both DLLs, e.g. GS "use for rhythm part" + CC#0 0).
    if (r.part_mode != 0 && !bank_->is_kit_bank(msb))
        msb = gs_translated() ? bank_->gs_banks()->drum_msb : kDrumBankMsbGs;
    p.program = Program{true, msb, r.bank_lsb, r.program_number};
    if (!reload_drums || !plays_kit(p)) return;
    const std::uint8_t img = image_msb(msb);
    if (r.part_mode >= 2) {
        load_drum_slot(r.part_mode - 2u, img, r.program_number);
    } else if (r.part_mode == 0 && bank_->xg_parameter_table() && mode_ != ResetMode::GS) {
        // The S-YXG50: a normal part playing a kit uses drum setup 2 (its drum-setup pointer
        // is that slot's): the kit change loads it [meas: register log, drum NRPN probes].
        load_drum_slot(1, img, r.program_number);
    } else if (auto set = bank_->drum_setup(img, r.program_number)) {
        p.drum_setup = *set;
        p.drum_setup_program = p.program;
    }
}

void FrontEnd::save_mode_bank(std::size_t part) {
    if (!bank_->xg_parameter_table()) return;
    const xg::MultiPart& r = ram_[part];
    PartState& s = ps_[part];
    const std::uint8_t msb = r.bank_msb, lsb = r.bank_lsb, prog = r.program_number;
    const auto drum = [&](std::array<std::uint8_t, 7>& b, bool by_msb) {
        b[0] = msb;
        const std::size_t at = !by_msb || msb == 0x7F ? 1 : (msb == kDrumBankMsbGm2 ? 5 : 3);
        b[at] = lsb;
        b[at + 1] = prog;
    };
    if (r.part_mode == 0) {
        // The DLL keeps it only when the bank holds the program (not the fallback voice of an
        // unknown bank); hvyxg keeps every selection.
        s.normal_bank[0] = msb;
        s.normal_bank[msb == 121 ? 3 : 1] = lsb;
        s.normal_bank[msb == 121 ? 4 : 2] = prog;
    } else if (r.part_mode == 1) {
        drum(s.drum_bank, true);
    } else if (r.part_mode - 2u < setup_bank_.size()) {
        drum(setup_bank_[r.part_mode - 2u], mode_ != ResetMode::GS);
    }
}

void FrontEnd::restore_mode_bank(std::size_t part) {
    if (!bank_->xg_parameter_table()) return;
    xg::MultiPart& r = ram_[part];
    const PartState& s = ps_[part];
    const bool xg = mode_ != ResetMode::GS;
    std::array<std::uint8_t, 3> b{0, 0, 0};
    const auto drum = [&](const std::array<std::uint8_t, 7>& d, bool by_msb) {
        const std::size_t at = !by_msb || d[0] == 0x7F ? 1 : (d[0] == kDrumBankMsbGm2 ? 5 : 3);
        b = {d[0], d[at], d[at + 1]};
    };
    if (r.part_mode == 0) {
        // GS mode: bank 0, program 0.
        if (xg) {
            const bool gm2 = s.normal_bank[0] == 121;
            b = {s.normal_bank[0], s.normal_bank[gm2 ? 3 : 1], s.normal_bank[gm2 ? 4 : 2]};
        }
    } else if (r.part_mode == 1) {
        drum(s.drum_bank, true);
    } else if (r.part_mode - 2u < setup_bank_.size()) {
        drum(setup_bank_[r.part_mode - 2u], xg);
    } else {
        return;
    }
    r.bank_msb = b[0];
    r.bank_lsb = b[1];
    r.program_number = b[2];
    // A parameter change leaves the pending bank select at the new mode's bank; a bulk dump
    // keeps the pending one (saves and restores for it).
    if (!bulk_) {
        ps_[part].bank_msb = b[0];
        ps_[part].bank_lsb = b[1];
    }
}

void FrontEnd::program_change(Part& p, std::uint8_t program) {
    xg::MultiPart& r = ram_[p.id];
    const PartState& s = ps_[p.id];
    if (gs_translated()) {
        // The 2006LE's GS layer: MSB 119 with the GS variation number as LSB, or MSB 118
        // LSB 0 on a rhythm part [code].
        const auto gb = *bank_->gs_banks();
        const bool drum = r.part_mode != 0;
        r.bank_msb = drum ? gb.drum_msb : gb.melodic_msb;
        r.bank_lsb = drum ? 0 : s.gs_variation;
    } else {
        r.bank_msb = s.bank_msb;
        r.bank_lsb = s.bank_lsb;
    }
    r.program_number = program & 0x7F;
    // A part mode written without its end-of-message step (S-YXG50): the program change plays
    // the part in its applied mode again [meas: a normal part given mode 2 that way is silent,
    // then plays its melodic program after a program change].
    r.part_mode = ps_[p.id].applied_mode;
    update_program(p, true);
    save_mode_bank(p.id);
}

xg::DrumSetup* FrontEnd::drum_note(Part& p, std::uint8_t note) {
    if (note < xg::kDrumSetupFirstNote || note >= xg::kDrumSetupFirstNote + xg::kDrumSetupNotes) return nullptr;
    const std::size_t i = note - xg::kDrumSetupFirstNote;
    const int mode = p.ram->part_mode;
    if (mode >= 2) return &drum_[static_cast<std::size_t>(mode - 2)][i];
    // The S-YXG50: a normal part playing a kit edits drum setup 2 (see update_program).
    if (mode == 0 && bank_->xg_parameter_table() && mode_ != ResetMode::GS && plays_kit(p))
        return &drum_[1][i];
    // Parts in mode 0 / 1 that play a kit edit their own copy (measured on the 2006LE).
    if (p.drum_setup_program.valid && p.drum_setup_program == p.program) return &p.drum_setup[i];
    return nullptr;
}

// ── Channel messages ────────────────────────────────────────────────────────────────────

void FrontEnd::message(std::span<const std::uint8_t> msg, std::size_t port) {
    if (msg.empty()) return;
    const std::uint8_t status = msg[0];
    if (status == 0xF0) {
        sysex(msg);
        return;
    }
    if (status < 0x80 || status >= 0xF0) return;  // running status bytes, system messages
    const std::uint8_t type = status & 0xF0;
    const std::size_t need = (type == 0xC0 || type == 0xD0) ? 2 : 3;
    if (msg.size() < need) return;
    const std::uint8_t d1 = msg[1] & 0x7F;
    const std::uint8_t d2 = need == 3 ? msg[2] & 0x7F : 0;
    const std::size_t channel = std::min(port, kMaxPorts - 1) * kPartsPerPort + (status & 0x0F);
    // Every part whose receive channel is this one (XG lets several parts share a channel).
    for (std::size_t i = 0; i < part_count_; ++i)
        if (ps_[i].routed_channel == channel) channel_message(parts_[i], status, d1, d2);
}

void FrontEnd::channel_message(Part& p, std::uint8_t status, std::uint8_t d1, std::uint8_t d2) {
    const xg::MultiPart& r = *p.ram;
    switch (status & 0xF0) {
    case 0x80:
        if (r.rcv_note_message) emit(action::NoteOff{&p, d1});
        break;
    case 0x90:
        if (!r.rcv_note_message) break;
        if (d2 == 0) emit(action::NoteOff{&p, d1});
        else emit(action::NoteOn{&p, d1, d2});
        break;
    case 0xA0:
        if (r.rcv_poly_after_touch) p.pat_values[d1] = d2, ++p.touch;
        break;
    case 0xB0:
        if (r.rcv_control_change) control_change(p, d1, d2), ++p.touch;
        break;
    case 0xC0:
        if (r.rcv_program_change) program_change(p, d1);
        break;
    case 0xD0:
        if (r.rcv_channel_after_touch) p.cat_value = d1, ++p.touch;
        break;
    case 0xE0:
        if (r.rcv_pitch_bend) p.pitchbend = static_cast<std::uint16_t>(d2 << 7 | d1), ++p.touch;
        break;
    default: break;
    }
}

void FrontEnd::control_change(Part& p, std::uint8_t cc, std::uint8_t v) {
    xg::MultiPart& r = ram_[p.id];
    PartState& s = ps_[p.id];
    Controller& c = p.controller;
    c.cc_values[cc] = v;
    // AC1 / AC2 take the CC on their current number, as well as its own function (every
    // number 0..95 in both DLLs, measured).
    for (std::size_t k = 0; k < c.ac.size(); ++k)
        if (r.ac[k].controller_number == cc) c.ac[k] = v;
    switch (cc) {
    case 0:
        // A GS-translating engine keeps CC#0 as the GS variation number (2006LE).
        if (r.rcv_bank_select) (gs_translated() ? s.gs_variation : s.bank_msb) = v;
        break;
    case 32:
        // … and ignores CC#32 in GS mode [code].
        if (r.rcv_bank_select && !gs_translated()) s.bank_lsb = v;
        break;
    case 1:
        if (r.rcv_modulation) c.modulation = v;
        break;
    case 5: r.portamento_time = v; break;
    case 6: data_entry(p, v, -1, 0); break;
    case 38: data_entry(p, -1, v, 0); break;
    case 7:
        if (r.rcv_volume) r.volume = v, refresh_gain(p);
        break;
    case 10:
        if (r.rcv_pan) r.pan = v;
        break;
    case 11:
        if (r.rcv_expression) c.expression = v, refresh_gain(p);
        break;
    case 64:
        if (!r.rcv_hold1) break;
        c.sustain = v >= 64;
        emit(action::SustainChange{&p});
        break;
    case 65:
        if (r.rcv_portamento) r.portamento_switch = v >= 64 ? 1 : 0;
        break;
    case 66: {
        if (!r.rcv_sostenuto) break;
        const bool on = v >= 64;
        if (on != c.sostenuto) {
            c.sostenuto = on;
            emit(action::SostenutoChange{&p});
        }
        break;
    }
    case 67:
        if (r.rcv_soft_pedal) c.soft = v >= 64;
        break;
    // Sound controllers: the XG part parameters they edit (as the equivalent NRPNs).
    case 71: r.filter_resonance = v; break;
    case 72:
        r.eg_release_time = v;
        emit(action::EgTimeChange{&p, false});
        break;
    case 73:
        r.eg_attack_time = v;
        emit(action::EgTimeChange{&p, true});
        break;
    case 74: r.filter_cutoff_freq = v; break;
    case 75: r.eg_decay_time = v; break;
    case 76: r.vibrato_rate = v; break;
    case 77: r.vibrato_depth = v; break;
    case 78: r.vibrato_delay = v; break;
    case 84:
        c.portamento_control = v;
        emit(action::PortamentoControl{&p, v});
        break;
    case 91: r.reverb_send = v; break;
    case 93: r.chorus_send = v; break;
    case 94:
        // The S-YXG50 takes CC94 only while the variation is in system connection (syxg50
        // tests engine[0x69dd] == 1); in insertion connection the message is lost,
        // and a later switch to system plays the send stored before [meas]. The SysEx part
        // parameter 08 nn 14 is always stored.
        if (bank_->type() != Model::Syxg2006le && effect_.variation_connection() != 1) break;
        r.variation_send = v;
        break;
    case 96: data_entry(p, -1, -1, +1); break;
    case 97: data_entry(p, -1, -1, -1); break;
    case 98:
        s.nrpn_lsb = v;
        s.selected = Selected::Nrpn;
        break;
    case 99:
        s.nrpn_msb = v;
        s.selected = Selected::Nrpn;
        break;
    case 100:
        s.rpn_lsb = v;
        s.selected = Selected::Rpn;
        break;
    case 101:
        s.rpn_msb = v;
        s.selected = Selected::Rpn;
        break;
    case 120: emit(action::KillAll{&p, true}); break;
    case 121: reset_controllers(p); break;
    case 123:
    case 124:
    case 125: emit(action::ReleaseAll{&p}); break;
    // Mono / poly (both DLLs, measured): CC126 counts only with a value up to 16, CC127 only
    // with 0. An accepted message sets the mode and silences the part's notes as all sound off
    // does, also when the mode does not change (as an 08 nn 05 write).
    case 126:
        if (v > 16) break;
        r.mode = 0;
        emit(action::KillAll{&p});
        break;
    case 127:
        if (v != 0) break;
        r.mode = 1;
        emit(action::KillAll{&p});
        break;
    default: break;
    }
}

void FrontEnd::reset_controllers(Part& p) {
    // XG specification, "Reset All Controllers": pitch bend, aftertouch, modulation,
    // expression, hold 1, portamento, sostenuto, soft pedal, portamento control, RPN / NRPN
    // numbers. Volume, pan, sends, bank select and the RPN values are kept.
    Controller& c = p.controller;
    p.pitchbend = 0x2000;
    p.cat_value = 0;
    p.pat_values.fill(0);
    c.modulation = 0;
    c.expression = 127;
    refresh_gain(p);
    c.soft = false;
    c.portamento_control = -1;
    emit(action::PortamentoControl{&p, 0x80});
    for (const std::uint8_t cc : {1, 64, 65, 66, 67, 84}) c.cc_values[cc] = 0;
    c.cc_values[11] = 127;
    // The 2006LE also clears the AC1 / AC2 values; the S-YXG50 keeps them [meas].
    if (bank_->type() == Model::Syxg2006le) c.ac = {};
    ram_[p.id].portamento_switch = 0;
    PartState& s = ps_[p.id];
    s.selected = Selected::None;
    s.rpn_msb = s.rpn_lsb = s.nrpn_msb = s.nrpn_lsb = 0x7F;
    if (c.sustain) {
        c.sustain = false;
        emit(action::SustainChange{&p});
    }
    if (c.sostenuto) {
        c.sostenuto = false;
        emit(action::SostenutoChange{&p});
    }
}

void FrontEnd::data_entry(Part& p, int msb, int lsb, int delta) {
    const xg::MultiPart& r = *p.ram;
    const PartState& s = ps_[p.id];
    if (s.selected == Selected::Rpn && r.rcv_rpn) set_rpn(p, s.rpn_msb, s.rpn_lsb, msb, lsb, delta);
    else if (s.selected == Selected::Nrpn && r.rcv_nrpn && lsb < 0) set_nrpn(p, s.nrpn_msb, s.nrpn_lsb, msb, delta);
}

void FrontEnd::set_rpn(Part& p, std::uint8_t pm, std::uint8_t pl, int vm, int vl, int delta) {
    if (pm != 0) return;
    xg::MultiPart& r = ram_[p.id];
    Rpn& rpn = p.rpn;
    const auto step = [&](int cur) { return vm >= 0 ? vm : cur + delta; };
    switch (pl) {
    case 0:  // pitch bend sensitivity: semitones (the cents LSB is ignored, as XG)
        if (vm >= 0 || delta != 0) r.write(0x23, clamp7(64 + step(r.bend.pitch_control - 64)));
        break;
    case 1:  // fine tune
        if (vm >= 0 || delta != 0) rpn.fine_msb = clamp7(step(rpn.fine_msb));
        if (vl >= 0) rpn.fine_lsb = static_cast<std::uint8_t>(vl);
        break;
    case 2:  // coarse tune (XG range ±24)
        if (vm >= 0 || delta != 0) rpn.coarse = static_cast<std::uint8_t>(std::clamp(step(rpn.coarse), 0x28, 0x58));
        break;
    case 5: {  // GM2 modulation depth range → the MW LFO pitch depth (superset mapping)
        if (vm >= 0 || delta != 0) rpn.mod_depth_msb = clamp7(step(rpn.mod_depth_msb));
        if (vl >= 0) rpn.mod_depth_lsb = static_cast<std::uint8_t>(vl);
        // XG MW PMod depth d gives ≈ ±5·d cents at full wheel in both engines' default law
        // (d = 10 ≈ ±50 cents = the GM2 default of 0.5 semitone).
        const int cents = rpn.mod_depth_msb * 100 + rpn.mod_depth_lsb * 100 / 128;
        r.mw.lfo_pmod_depth = clamp7((cents + 2) / 5);
        break;
    }
    default: break;
    }
}

void FrontEnd::set_nrpn(Part& p, std::uint8_t nm, std::uint8_t nl, int value, int delta) {
    xg::MultiPart& r = ram_[p.id];
    // Part NRPNs (XG / GS): 01 08..0A vibrato, 01 20/21 filter, 01 63/64/66 EG.
    if (nm == 0x01) {
        std::uint8_t addr = 0;
        switch (nl) {
        case 0x08: addr = 0x15; break;
        case 0x09: addr = 0x16; break;
        case 0x0A: addr = 0x17; break;
        case 0x20: addr = 0x18; break;
        case 0x21: addr = 0x19; break;
        case 0x63: addr = 0x1A; break;
        case 0x64: addr = 0x1B; break;
        case 0x66: addr = 0x1C; break;
        case 0x24:
            // XG2.0 part HPF cutoff (0A nn 20). Superset: neither DLL renders it.
            if (options_.dll_exact) return;
            ram_ext_[p.id].write(0x20, clamp7(value >= 0 ? value : ram_ext_[p.id].hpf_cutoff + delta));
            return;
        // XG2.0 part EQ: bass / treble gain, bass / treble frequency (08 nn 72 / 73 / 76 / 77).
        case 0x30: addr = 0x72; break;
        case 0x31: addr = 0x73; break;
        case 0x34: addr = 0x76; break;
        case 0x35: addr = 0x77; break;
        default: return;
        }
        if (options_.dll_exact && addr >= 0x72) return;
        r.write(addr, clamp7(value >= 0 ? value : r[addr] + delta));
        // The attack / release NRPNs run the CC73 / CC72 handlers, which update the sounding voices.
        if (addr == 0x1A || addr == 0x1C) emit(action::EgTimeChange{&p, addr == 0x1A});
        return;
    }
    // Drum NRPNs 14..1F rr: the drum setup of the note rr.
    static constexpr std::array<int, 12> kDrumParam = {
        0x0B, 0x0C, 0x0D, 0x0E,  // 14 cutoff, 15 resonance, 16 attack, 17 decay
        0x00, 0x01, 0x02, -1,    // 18 pitch coarse, 19 fine, 1A level
        0x04, 0x05, 0x06, 0x07,  // 1C pan, 1D reverb, 1E chorus, 1F variation send
    };
    // XG2.0 drum NRPNs (MU2000; superset, neither DLL renders them): 24 rr HPF cutoff, 30 / 31
    // rr EQ bass / treble gain, 34 / 35 rr EQ bass / treble frequency.
    int addr = -1;
    if (nm >= 0x14 && nm <= 0x1F) addr = kDrumParam[nm - 0x14u];
    else if (!options_.dll_exact) {
        switch (nm) {
        case 0x24: addr = 0x50; break;
        case 0x30: addr = 0x20; break;
        case 0x31: addr = 0x21; break;
        case 0x34: addr = 0x24; break;
        case 0x35: addr = 0x25; break;
        default: break;
        }
    }
    if (addr < 0) return;
    xg::DrumSetup* d = drum_note(p, nl);
    if (!d) return;
    const int idx = xg::DrumSetup::index_of(static_cast<std::uint8_t>(addr));
    const std::uint8_t v = clamp7(value >= 0 ? value : (*d)[static_cast<std::size_t>(idx)] + delta);
    d->write(static_cast<std::uint8_t>(addr), v);
    // XG "EG decay rate": both decay segments.
    if (addr == 0x0E) d->write(0x0F, v);
}

// ── SysEx ───────────────────────────────────────────────────────────────────────────────

bool FrontEnd::device_matches(std::uint8_t dev) const {
    return options_.device_id < 0 || dev == 0x7F || (dev & 0x0F) == options_.device_id;
}

void FrontEnd::sysex(std::span<const std::uint8_t> msg) {
    // F0 id … F7 (a missing F7 is tolerated).
    std::span<const std::uint8_t> body = msg.subspan(1);
    if (!body.empty() && body.back() == 0xF7) body = body.first(body.size() - 1);
    if (body.empty()) return;
    for (const std::uint8_t b : body)
        if (b & 0x80) return;  // truncated / interleaved: ignore
    switch (body[0]) {
    case 0x43: yamaha(body.subspan(1)); break;
    case 0x41: roland(body.subspan(1)); break;
    case 0x7E: universal_non_realtime(body.subspan(1)); break;
    case 0x7F: universal_realtime(body.subspan(1)); break;
    default: break;
    }
}

void FrontEnd::universal_non_realtime(std::span<const std::uint8_t> b) {
    // dev 09 01 GM1 On, 09 03 GM2 On, 09 02 GM Off.
    if (b.size() < 3 || !device_matches(b[0]) || b[1] != 0x09) return;
    switch (b[2]) {
    case 0x01: reset(ResetMode::GM); break;
    case 0x03: reset(ResetMode::GM2); break;
    case 0x02:
        // GM Off: the S-YXG50 runs its XG System On (the handler of 00 00 7E) [code, meas]; the 2006LE only leaves GM mode.
        if (bank_->type() != Model::Syxg2006le) reset(ResetMode::XG);
        else mode_ = ResetMode::XG;
        break;
    default: break;
    }
}

void FrontEnd::universal_realtime(std::span<const std::uint8_t> b) {
    // dev 04 01 ll mm master volume; 04 03 / 04 04 master fine / coarse tuning (GM2).
    if (b.size() < 5 || !device_matches(b[0]) || b[1] != 0x04) return;
    const int v = b[4] << 7 | b[3];
    if (b[2] == 0x01 || b[2] == 0x03 || b[2] == 0x04) touch_all();
    switch (b[2]) {
    case 0x01:
        // Both DLLs store the MSB as the XG master volume (00 00 04): the LSB is ignored and
        // the last of the two messages wins [meas].
        gm_master_volume_ = static_cast<std::uint16_t>(v);
        system_.write(0x04, b[4]);
        refresh_gain_all();
        break;
    case 0x03: {
        if (bank_->xg_parameter_table()) {
            // The S-YXG50 keeps it apart (rounded toward 0), live on
            // sounding notes, while the XG master tune is taken at the note-on.
            system_.master_fine_gm2 = static_cast<std::int16_t>((v - 0x2000) * 100 / 0x2000);
            break;
        }
        // ±100 cents onto the XG master tune (0.1 cent steps, 0x400 = 0), applied at once.
        const int t = std::clamp(0x400 + (v - 0x2000) * 1000 / 0x2000, 0, 0x7FF);
        system_.write(0x00, static_cast<std::uint8_t>(t >> 12 & 15));
        system_.write(0x01, static_cast<std::uint8_t>(t >> 8 & 15));
        system_.write(0x02, static_cast<std::uint8_t>(t >> 4 & 15));
        system_.write(0x03, static_cast<std::uint8_t>(t & 15));
        system_.applied_master_tune = system_.get_master_tune();
        break;
    }
    case 0x04: system_.master_coarse = static_cast<std::uint8_t>(std::clamp<int>(b[4], 0x28, 0x58)); break;
    default: break;
    }
}

void FrontEnd::yamaha(std::span<const std::uint8_t> b) {
    if (b.size() < 2) return;
    const std::uint8_t kind = b[0] & 0xF0;
    if (!device_matches(b[0] & 0x0F)) return;
    if (b[1] != 0x4C) return;  // XG model id
    if (kind == 0x10) {
        // Parameter change: 1n 4C hh mm ll dd…
        if (b.size() < 6) return;
        xg_parameter(b[2], b[3], b[4], b.subspan(5), false);
    } else if (kind == 0x00) {
        // Bulk dump: 0n 4C bh bl hh mm ll dd… cs (checksum over bh..cs = 0 mod 128). The
        // S-YXG50 does not check the checksum [meas].
        if (b.size() < 8) return;
        const std::size_t count = static_cast<std::size_t>(b[2]) << 7 | b[3];
        if (b.size() < 7 + count + 1) return;
        unsigned sum = 0;
        for (std::size_t i = 2; i < 7 + count + 1; ++i) sum += b[i];
        if ((sum & 0x7F) != 0 && !bank_->xg_parameter_table()) return;
        // The S-YXG2006LE ignores every XG bulk dump [meas]; hvyxg applies them unless DLL-exact.
        if (options_.dll_exact && bank_->type() == Model::Syxg2006le) return;
        xg_parameter(b[4], b[5], b[6], b.subspan(7, count), true);
    }
}

void FrontEnd::xg_parameter(std::uint8_t hi, std::uint8_t mid, std::uint8_t lo,
                            std::span<const std::uint8_t> data, bool bulk) {
    // The 2006LE ignores XG parameters in GS mode, all but XG System On [meas]; full XG
    // (`FrontEndOptions::full_xg`) obeys them, as the S-YXG50 does.
    const bool command = hi == 0x00 && mid == 0x00 && lo >= 0x7E;
    if (mode_ == ResetMode::GS && !bank_->xg_parameters_in_gs_mode() && !options_.full_xg && !command) return;
    // System 00 00 7D..7F are commands, not memory.
    if (hi == 0x00 && mid == 0x00 && lo >= 0x7D) {
        if (lo == 0x7E || lo == 0x7F) {
            reset(ResetMode::XG);  // XG System On, All Parameter Reset
        } else if (!data.empty() && data[0] < drum_.size()) {
            const std::size_t s = data[0];  // drum setup reset: the setup's current kit
            const Program k = drum_kit_[s];
            load_drum_slot(s, k.bank_msb, k.program);
        }
        return;
    }
    if (bank_->xg_parameter_table()) {
        if (const auto p = syxg50_param(hi, mid, lo)) {
            // The S-YXG50: the address must start
            // a parameter, and a parameter is written only once all its bytes have arrived.
            if (p->size == 0) return;
            if (!bulk) {
                // A parameter change carries one parameter; bytes beyond it are an error.
                if (data.size() < p->size) return;
                deferred_lost_ = data.size() > p->size && syxg50_deferred(hi, mid, lo);
                for (std::size_t i = 0; i < p->size; ++i)
                    xg_write(hi, mid, static_cast<std::uint8_t>(lo + i), data[i]);
                deferred_lost_ = false;
                return;
            }
            // A bulk dump walks the block's parameters up to its last address; a parameter cut
            // short by the end of the data is dropped.
            bulk_ = true;
            std::size_t i = 0;
            for (unsigned a = lo; i < data.size() && a <= p->last;) {
                const auto q = syxg50_param(hi, mid, static_cast<std::uint8_t>(a));
                const std::size_t n = q ? q->size : 1;
                if (n == 0 || i + n > data.size()) break;
                for (std::size_t j = 0; j < n; ++j)
                    xg_write(hi, mid, static_cast<std::uint8_t>(a + j), data[i + j]);
                i += n;
                a += static_cast<unsigned>(n);
            }
            bulk_ = false;
            return;
        }
    }
    for (std::size_t i = 0; i < data.size() && lo + i < 0x80; ++i)
        xg_write(hi, mid, static_cast<std::uint8_t>(lo + i), data[i]);
}

void FrontEnd::xg_write(std::uint8_t hi, std::uint8_t mid, std::uint8_t lo, std::uint8_t v) {
    if (hi == 0x00 && mid == 0x00) {
        system_.write(lo, v);
        if (!deferred_lost_) {
            if (lo == 0x04) refresh_gain_all(), touch_all();  // not the master tune
            if (lo <= 0x03) system_.applied_master_tune = system_.get_master_tune();
        }
    } else if (hi == 0x02 && mid == 0x01) {
        // S-YXG50 (measured): a write of the
        // variation connection silences the insertion part's sounding voices, a write of the
        // insertion part those of the old and of the new part, as all sound off does, also
        // when the value does not change. The 2006LE does neither (measured).
        const bool kill = bank_->type() != Model::Syxg2006le && (lo == 0x5A || lo == 0x5B);
        if (kill) kill_insertion_part();
        effect_.write(lo, v);
        if (kill && lo == 0x5B) kill_insertion_part();
    } else if (options_.dll_exact &&
               (hi == 0x03 || (hi == 0x02 && mid == 0x40) ||
                (hi == 0x0A && (lo == 0x20 || bank_->type() != Model::Syxg2006le)))) {
        // DLL-exact: neither DLL renders the multi EQ, the XG2.0 insertion blocks or the part
        // HPF (bit-identical output with any of them [meas]); the S-YXG50 has no 0A block at all,
        // while the 2006LE acts on its 0A nn 01 (mono priority) and 40..45 (level controls).
        return;
    } else if (hi == 0x02 && mid == 0x40) {
        multi_eq_.write(lo, v);
    } else if (hi == 0x03 && mid < insertion_.size()) {
        insertion_[mid].write(lo, v);
    } else if (hi == 0x08 && mid < part_count_) {
        part_write(mid, lo, v);
    } else if (hi == 0x0A && mid < part_count_) {
        ram_ext_[mid].write(lo, v);
    } else if ((hi & 0xF0) == 0x30 && (hi & 0x0F) < drum_.size()) {
        if (mid >= xg::kDrumSetupFirstNote && mid < xg::kDrumSetupFirstNote + xg::kDrumSetupNotes)
            drum_[hi & 0x0Fu][mid - xg::kDrumSetupFirstNote].write(lo, v);
    }
    // The other XG2.0 part extensions 0A nn and display data are not modelled.
}

void FrontEnd::kill_insertion_part() {
    // only an insertion part below 0x10 (0x7F = off) has voices to silence.
    const std::uint8_t p = effect_.variation_part();
    if (p < 16 && p < part_count_) emit(action::KillAll{&parts_[p]});
}

void FrontEnd::part_write(std::size_t part, std::uint8_t addr, std::uint8_t v) {
    xg::MultiPart& r = ram_[part];
    Part& p = parts_[part];
    const std::uint8_t old_mode = ps_[part].applied_mode;
    if (!r.write(addr, v)) return;
    ++p.touch;
    if (deferred_lost_) {
        // The S-YXG50 skipped the end-of-message step (bytes past the parameter): the RAM holds
        // the value, nothing is applied [meas]. A later event that reads it picks it up: CC7 /
        // CC11 / reset controllers the volume, a receive-channel change of any part the channel
        // map, the next note-on the mono / poly mode; a part mode left this way silences the
        // part (the DLL takes the drum path from the RAM without a kit) until a program change.
        if (addr == 0x07 && r.part_mode != old_mode) p.program.valid = false;
        return;
    }
    switch (addr) {
    case 0x1A:
    case 0x1C:
        // (deferred handlers): the sounding voices too.
        emit(action::EgTimeChange{&p, addr == 0x1A});
        break;
    case 0x01: ps_[part].bank_msb = r.bank_msb; break;
    case 0x02: ps_[part].bank_lsb = r.bank_lsb; break;
    case 0x03:
        // A program number write is a program change with the RAM's bank.
        ps_[part].bank_msb = r.bank_msb;
        ps_[part].bank_lsb = r.bank_lsb;
        update_program(p, true);
        save_mode_bank(part);
        break;
    case 0x07:
        ps_[part].applied_mode = r.part_mode;
        if (r.part_mode != old_mode) {
            // The S-YXG50 selects the bank and program last used in the new mode (a normal part after a reset: 0 / 0 / 0); the pending CC0 / CC32 stay [code, meas].
            restore_mode_bank(part);
            emit(action::KillAll{&p});
            update_program(p, true);
        }
        break;
    case 0x04: rebuild_routing(); break;  // the whole channel map from the RAM
    case 0x05:
        // Mono / poly: every write, also of the same value, silences the part's notes as all
        // sound off does (S-YXG50: damped; 2006LE: removed at rate
        // 0x70) [meas], as CC126 / CC127.
        emit(action::KillAll{&p});
        break;
    case 0x0B:
        p.controller.cc_values[7] = r.volume;
        refresh_gain(p);
        break;
    case 0x0E: p.controller.cc_values[10] = r.pan; break;
    default: break;
    }
}

// ── Roland GS ───────────────────────────────────────────────────────────────────────────

void FrontEnd::roland(std::span<const std::uint8_t> b) {
    // dev 42 12 a1 a2 a3 dd… cs (DT1, GS model id 0x42); checksum over a1..cs = 0 mod 128.
    if (b.size() < 7 || !device_matches(b[0]) || b[1] != 0x42 || b[2] != 0x12) return;
    unsigned sum = 0;
    for (std::size_t i = 3; i < b.size(); ++i) sum += b[i];
    if ((sum & 0x7F) != 0) return;
    const std::uint8_t a1 = b[3], a2 = b[4], a3 = b[5];
    const std::span<const std::uint8_t> data = b.subspan(6, b.size() - 7);
    // GS reset (40 00 7F) and "system mode set" (00 00 7F) reset into GS mode.
    if ((a1 == 0x40 && a2 == 0x00 && a3 == 0x7F) || (a1 == 0x00 && a2 == 0x00 && a3 == 0x7F)) {
        reset(ResetMode::GS);
        return;
    }
    if (a1 == 0x40 && (a2 & 0xF0) == 0x10 && gs_dialect() == gs::Dialect::Syxg2006le && !options_.full_xg) {
        le_gs_part(gs_block_part(a2), a3, data);
        return;
    }
    // Full XG on the 2006LE: every byte of a GS message counts, as on the S-YXG50, through the
    // 2006LE's own GS translation.
    const bool single = data.size() == 1 || options_.full_xg;
    for (std::size_t i = 0; i < data.size() && a3 + i < 0x80; ++i)
        gs_write(a1, a2, static_cast<std::uint8_t>(a3 + i), data[i], single);
}

void FrontEnd::gs_write(std::uint8_t a1, std::uint8_t a2, std::uint8_t a3, std::uint8_t v, bool single) {
    if (a1 == 0x40 && a2 == 0x00) {
        // System: master tune 00..03 (same nibble format as XG), volume 04, key shift 05.
        if (a3 <= 0x04) {
            system_.write(a3, v);
            system_.applied_master_tune = system_.get_master_tune();
            refresh_gain_all();
        } else if (a3 == 0x05) {
            system_.write(0x06, v);
        }
    } else if (a1 == 0x40 && a2 == 0x01) {
        // Patch common: reverb / chorus macros and parameters onto the XG effect RAM.
        if (a3 >= 0x30 && a3 <= 0x40) gs::write(gs_effect_, effect_, gs_dialect(), a3, v, single);
    } else if (a1 == 0x40 && (a2 & 0xF0) == 0x10) {
        gs_part_write(gs_block_part(a2), a3, v);
    } else if (a1 == 0x40 && (a2 & 0xF0) == 0x20) {
        // Controller block: MOD pitch / LFO1 pitch depth, BEND pitch range.
        const std::size_t part = gs_block_part(a2);
        if (part >= part_count_) return;
        if (a3 == 0x00) part_write(part, 0x1D, v);
        else if (a3 == 0x04) part_write(part, 0x20, v);
        else if (a3 == 0x10) part_write(part, 0x23, v);
    } else if (a1 == 0x41) {
        // Drum map m (a2 high nibble) parameter (low nibble) of note a3.
        const std::size_t slot = (a2 >> 4) & 1;
        if (a3 < xg::kDrumSetupFirstNote || a3 >= xg::kDrumSetupFirstNote + xg::kDrumSetupNotes) return;
        xg::DrumSetup& d = drum_[slot][a3 - xg::kDrumSetupFirstNote];
        switch (a2 & 0x0F) {
        case 1: d.write(0x00, clamp7(64 + v - a3)); break;  // play note number
        case 2: d.write(0x02, v); break;
        case 3: d.write(0x03, v); break;
        case 4: d.write(0x04, v); break;
        case 5: d.write(0x05, v); break;
        case 6: d.write(0x06, v); break;
        case 7: d.write(0x09, v); break;
        case 8: d.write(0x0A, v); break;
        case 9: d.write(0x07, v); break;
        default: break;
        }
    }
}

void FrontEnd::le_gs_part(std::size_t part, std::uint8_t a, std::span<const std::uint8_t> data) {
    // The 2006LE's GS layer turns a GS part parameter into XG part parameter changes only when
    // the message carries exactly that parameter: one byte, two for the tone number (00) and
    // the pitch offset fine (17), twelve for the scale tuning (40) [code].
    if (part >= part_count_) return;
    if (a == 0x00) {
        if (data.size() != 2) return;
        xg::MultiPart& r = ram_[part];
        if (gs_translated()) {
            const auto gb = *bank_->gs_banks();
            const bool drum = r.part_mode != 0;
            r.bank_msb = drum ? gb.drum_msb : gb.melodic_msb;
            r.bank_lsb = drum ? 0 : data[0];
        } else {
            r.bank_msb = data[0];
            r.bank_lsb = 0;
        }
        ps_[part].bank_msb = r.bank_msb;
        ps_[part].bank_lsb = r.bank_lsb;
        r.program_number = data[1] & 0x7F;
        update_program(parts_[part], true);
    } else if (a == 0x17) {
        if (data.size() != 2) return;
        part_write(part, 0x09, data[0]);
        part_write(part, 0x0A, data[1]);
    } else if (a == 0x40) {
        if (data.size() != 12) return;
        for (std::size_t i = 0; i < 12; ++i) part_write(part, static_cast<std::uint8_t>(0x41 + i), data[i]);
    } else if (data.size() == 1 && a != 0x01 && a != 0x18 && a < 0x40) {
        gs_part_write(part, a, data[0]);
    }
}

void FrontEnd::gs_part_write(std::size_t part, std::uint8_t a, std::uint8_t v) {
    if (part >= part_count_) return;
    Part& p = parts_[part];
    switch (a) {
    case 0x00: (gs_translated() ? ps_[part].gs_variation : ps_[part].bank_msb) = v; break;  // tone number: CC#0 value …
    case 0x01: program_change(p, v); break;    // … and program
    case 0x02: part_write(part, 0x04, v >= 16 ? 0x7F : v); break;
    case 0x13: part_write(part, 0x05, v); break;
    case 0x14: part_write(part, 0x06, v == 0 ? 0 : 1); break;
    case 0x15:
        part_write(part, 0x07, v == 0 ? 0 : static_cast<std::uint8_t>(1 + std::min<int>(v, 2)));
        // The 2006LE's GS layer also makes a part that leaves the rhythm mode play its GS
        // bank's program 0 [code].
        if (v == 0 && gs_translated()) {
            part_write(part, 0x01, bank_->gs_banks()->melodic_msb);
            part_write(part, 0x02, 0);
            part_write(part, 0x03, 0);
        }
        break;
    case 0x16: part_write(part, 0x08, v); break;
    case 0x17: part_write(part, 0x09, v); break;
    case 0x18: part_write(part, 0x0A, v); break;
    case 0x19: part_write(part, 0x0B, v); break;
    case 0x1A: part_write(part, 0x0C, v); break;
    case 0x1B: part_write(part, 0x0D, v); break;
    case 0x1C: part_write(part, 0x0E, v); break;
    case 0x1D: part_write(part, 0x0F, v); break;
    case 0x1E: part_write(part, 0x10, v); break;
    case 0x1F: part_write(part, 0x59, v); break;
    case 0x20: part_write(part, 0x60, v); break;
    case 0x21: part_write(part, 0x12, v); break;
    case 0x22: part_write(part, 0x13, v); break;
    case 0x30: part_write(part, 0x15, v); break;
    case 0x31: part_write(part, 0x16, v); break;
    case 0x32: part_write(part, 0x18, v); break;
    case 0x33: part_write(part, 0x19, v); break;
    case 0x34: part_write(part, 0x1A, v); break;
    case 0x35: part_write(part, 0x1B, v); break;
    case 0x36: part_write(part, 0x1C, v); break;
    case 0x37: part_write(part, 0x17, v); break;
    case 0x23: part_write(part, 0x40, v); break;  // Rx bank select
    default:
        // Rx switches 03..12 (pitch bend … soft pedal) in the XG order 30..3F.
        if (a >= 0x03 && a <= 0x12) part_write(part, static_cast<std::uint8_t>(0x30 + (a - 0x03)), v);
        if (a >= 0x40 && a <= 0x4B) part_write(part, static_cast<std::uint8_t>(0x41 + (a - 0x40)), v);
        break;
    }
}

} // namespace hvyxg::midi
