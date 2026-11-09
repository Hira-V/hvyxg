// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "steal_fade.hpp"
#include "syxg50/tone_module.hpp"
#if defined(HVYXG_CHECK_CONTROL)
#include <cstdio>
#endif

#include "fx/fx_system.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "fx/fx_system.hpp"
#include "engine_tables.hpp"
#include "syxg50/dll_tables.hpp"
#include "syxg50/laws.hpp"
#include "midi/part.hpp"

namespace hvyxg::syxg50 {

namespace {

constexpr int kInstant = -0x8000;
/// Release rate of a cut note (same-key retrigger, alternate group, mono).
constexpr int kCutRelease = 0x5a;
/// Damp (stolen voice, all sound off): release-table rate 0x7e, linear.
constexpr int kDamp = 0xfe;
constexpr int kOwnWaveRelease = 0x50;

int clamp7(int v) { return std::clamp(v, 0, 127); }

using Rec = std::array<std::uint8_t, 16>;

} // namespace

/// Controller sums of one part for one block.
struct ToneModule50::Ctl {
    int gain = 0;
    int pitch = 0;   // cents: fine tune + MW/CAT/AC pitch
    int bend = 0;    // cents
    int amp = 0;     // level units
    int cutoff = 0;  // cutoff units (part cutoff, bend, MW/CAT/AC)
    int pmd = 0;     // controller vibrato depth sum (0..127)
    int amd = 0;     // 0..15
    int fmd = 0;     // FMD sum
    int detune = 0;
    int coarse = 0;  // RPN coarse tune (part[0xb7])
};

/// One element voice: the control layer's per-voice state plus its chip voice.
struct ToneModule50::Voice {
    ChipVoice chip;
    bool in_use = false;
    const midi::Part* part = nullptr;
    std::uint8_t note = 0;
    int vel = 0;
    std::uint32_t serial = 0;
    std::size_t index = 0;
    bool keyed = false;
    bool sus_pending = false; // note-off arrived under sustain
    int sostenuto = 0;        // 1 captured, 3 note-off arrived while captured
    bool released = false;
    bool detached = false;    // drum auto-release expired
    /// The index is in the free FIFO (a drum key whose auto-release expired stays busy for
    /// the AEG monitor until it frees it).
    bool queued = false;
    /// v[0x6c]: the AEG monitor does not free the voice in D2 (D2 level above D1 level, or a
    /// delayed key-on), cleared by the note-off.
    bool hold = false;
    std::uint64_t alloc_seq = 0; // allocation order (the part's voice list)
    bool phys_due = false;       // keyed, not yet bound to a physical chip voice
    // v[0xc] bit 0 (level / LFO update due, cleared at the level round-robin turn) and bits 1-2
    // (pitch / cutoff commit due, cleared by the tick's commit); set by the part's controller
    // messages (`Part::touch`), the LFO fade (bit 0) and sample-and-hold (bit 2).
    bool flag0 = false, flag12 = false;
    std::uint32_t touch_seen = 0;
    int stage = 3;            // v[1]: 3 attack/D1, 5 the same after the auto-release, 4 D2,
                              // 1 released, 6 key-off before a delayed key-on
    int score = 0xff;
    int phys = -1; // the physical chip voice the register set points at

    const std::uint8_t* elem = nullptr;
    std::uint8_t wave_format = 0;
    int kind = 0; // 0 melodic, 1 own-wave drum, 2 prevoice drum
    Rec d{};      // drum setup record
    eq::VoiceEqSettings eq_drum{};  // drum keys: the note EQ / HPF latched at note-on
    Rec src{};    // drum fields record
    const std::uint8_t* entry = nullptr;
    bool rcv_note_off = true;
    std::uint8_t alt_group = 0;
    int auto_release = 0; // 40 ms units, 0 = never
    int key_delay = 0;    // element byte 68 key-on delay: > 0 = chip not keyed yet

    // pitch
    int base_cents = 0;
    int porta = 0, porta_tgt = 0, porta_frac = 0;
    bool porta_up = false;
    int sh_cents = 0;
    // PEG (¼ cent)
    int peg = 0, peg_tgt = 0, peg_step = 0, peg_stage = 0;
    int peg_vs = 0;
    // FEG (¼ cutoff unit)
    int feg = 0, feg_tgt = 0, feg_step = 0, feg_stage = 0;
    int feg_vs = 0;
    // AEG
    int d2r = 0, d2l = 0, rr = 0;
    int rr_normal = 0; // the element's release rate register
    int rr66 = 0;      // v[0x66]: the release rate the level round robin rewrites
    bool damper = false;
    // filter
    int cutoff0 = 0;
    int resonance = 0;
    std::uint8_t cg_counter = 0xf0;
    std::uint8_t cg_rate = 0x7f;
    int cg_ctrl = 0;
    int cg_last = 0;
    int fmd_centre = 0;
    // level
    int level0 = 0;
    int zone_atten = 0;
    int tvel = 0;
    int pan = 64;     // combined element / drum pan (v76)
    int pan_send = 64; // chorus pan attenuation index (v77)
    bool random_pan = false;
    int drum_level = 127;
    std::array<int, 3> drum_sends{127, 127, 127};
    int level_reg = 0;
    /// Level, pan and sends are recomputed at the next update (note-on, level round robin).
    bool level_due = true;
    // LFO
    bool lfo_on = false;
    int lfo_rate_base = 14; // index base into for the part vibrato rate (GS: by program)
    bool lfo_tri = true;
    bool lfo_sh = false;
    int lfo_delay = 0;
    int vib_target = 0;
    int vib_step = 0;
    int vib = 0;
    int lfo_state = 0; // v[0xb]: 0 delay, 1 fading, 2 done
    int sh_count = 8;
};

std::optional<DrumKitSetup> drum_setup(const dll::Tables& tb, const BankData& bank, std::uint8_t bank_msb,
                                       std::uint8_t program) {
    const BankMode mode =
        (bank_msb == kDrumBankMsbXg || bank_msb == kSfxBankMsbXg || bank_msb == kDrumBankMsbGm2)
            ? BankMode::Xg
            : BankMode::Gs;
    const std::size_t kit = bank.drum_kit(mode, bank_msb, 0, program);
    DrumKitSetup setup{};
    for (std::size_t i = 0; i < setup.size(); ++i) {
        const std::uint8_t* e = bank.drum_entry(kit, static_cast<std::uint8_t>(13 + i));
        Rec r{};
        std::copy(tb.kDrumTemplate.begin(), tb.kDrumTemplate.end(), r.begin());
        if (e != nullptr) std::copy(e + 2, e + 11, r.begin() + 2);
        else r[10] = 0;
        // Big layout: +0x0A holds the root key; the "Full" patch sets receive note on to 1.
        if (e != nullptr && bank.mu().big) r[10] = 1;
        setup[i] = midi::xg::DrumSetup::from_bytes(r);
    }
    return setup;
}

ToneModule50::ToneModule50(std::shared_ptr<const BankData> bank, const ModuleContext& ctx)
    : bank_(std::move(bank)), ctx_(ctx), tb_(ctx.tables->syxg50), rt_(tb_, ctx.sample_rate),
      ctls_(std::make_unique<std::array<Ctl, midi::kMaxPartSize>>()) {
    const std::size_t n = std::max<std::size_t>(ctx.max_polyphony, 1);
    voices_.reserve(n);
    free_.init(n);
    for (std::size_t i = 0; i < n; ++i) {
        voices_.push_back(std::make_unique<Voice>());
        voices_.back()->index = i;
        free_.push_back(i);
    }
    tails_.resize(n);
    phys_count_ = 2 * n;
    for (auto& s : scratch_) s.reserve(n);
    note_score_.reserve(n);
    phys_busy_.resize(phys_count_);
}

ToneModule50::~ToneModule50() = default;

int ToneModule50::drum_mode(const midi::Part& part) const {
    // A normal part given an XG drum / SFX kit bank (MSB 127 / 126) plays the kit [meas: MSB
    // 127 on channel 1 plays the Standard Kit] with drum setup 2, like part mode 3 [meas: a drum NRPN or 31 rr XG write reaches it,
    // and its NRPNs reach part mode 3 parts].
    const int m = part.ram->part_mode;
    if (m != 0) return m;
    if (bank_mode(*ctx_.shared.reset_mode) == BankMode::Xg &&
        (part.program.bank_msb == kDrumBankMsbXg || part.program.bank_msb == kSfxBankMsbXg))
        return 3;
    return 0;
}

std::uint8_t ToneModule50::rand() {
    // r = 17 − 77·r (8 bit), shared by S&H and random pan.
    rand_ = static_cast<std::uint8_t>(17 - 77 * rand_);
    return rand_;
}

void ToneModule50::follow_clock(const ToneModule& leader) {
    // The patched DLL copies the plugin's sample / 10-ms counters (read
    // by its process loop) from engine A to engine B when a note wakes B.
    // `leader` is engine A's tone module, made by the same bank: a ToneModule50 too.
    const auto& a = static_cast<const ToneModule50&>(leader);
    samples_ = a.samples_;
    ms_done_ = a.ms_done_;
}

bool ToneModule50::active() const {
    return std::any_of(voices_.begin(), voices_.end(),
                       [](const auto& v) { return v->chip.active() || v->key_delay > 0; }) ||
           std::any_of(tails_.begin(), tails_.end(), [](const auto& c) { return c.active(); });
}

std::size_t ToneModule50::voice_count() const {
    const auto n = std::count_if(voices_.begin(), voices_.end(), [](const auto& v) { return v->chip.active(); }) +
                   std::count_if(tails_.begin(), tails_.end(), [](const auto& c) { return c.active(); });
    return static_cast<std::size_t>(n);
}

const ToneModule50::Ctl& ToneModule50::ctl(const midi::Part& part) {
    const std::size_t id = part.id % parts_.size();
    Ctl& c = (*ctls_)[id];
    if (parts_[id].ctl_block == block_) return c;
    parts_[id].ctl_block = block_;
    const midi::xg::MultiPart& r = *part.ram;
    const midi::Controller& cc = part.controller;
    const int mw = cc.modulation, cat = part.cat_value;
    const int ac1 = cc.ac[0];
    const int ac2 = cc.ac[1];
    const int bend = static_cast<int>(part.pitchbend) - 8192;
    // The part gain from the volume and master volume it was last computed from (`Part::gain_volume`, `gain_master`).
    c.gain = law::part_gain(part.gain_volume, cc.expression, part.gain_master);
    // fine tune (RPN 1) + MW / CAT / AC1 / AC2 pitch.
    const int fine = (part.rpn.fine_msb << 7 | part.rpn.fine_lsb) - 8192;
    c.pitch = (fine == -8192 ? -100 : fine / 82) + law::ctrl_pitch(r.mw.pitch_control, mw) +
              law::ctrl_pitch(r.cat.pitch_control, cat) +
              law::ctrl_pitch(r.ac[0].pitch_control, ac1) +
              law::ctrl_pitch(r.ac[1].pitch_control, ac2);
    c.bend = law::bend_cents(r.bend.pitch_control, part.pitchbend);
    // amplitude.
    const int bx = std::abs(bend >> 6);
    c.amp = law::amp_term(r.mw.amplitude_control, mw) + law::amp_term(r.cat.amplitude_control, cat) +
            law::amp_term(r.ac[0].amplitude_control, ac1) +
            law::amp_term(r.ac[1].amplitude_control, ac2) +
            (bend < 0 ? -1 : 1) * law::amp_term(r.bend.amplitude_control, bx);
    // cutoff; the part cutoff offset goes through its own table in GS mode.
    const int pc_off = r.filter_cutoff_freq - 64;
    const int part_cutoff = bank_mode(*ctx_.shared.reset_mode) != BankMode::Gs
                                ? pc_off * 16
                                : (pc_off < 0 ? -1 : 1) * tb_.kGsPartCutoff[static_cast<std::size_t>(std::abs(pc_off))];
    c.cutoff = part_cutoff + (((r.bend.filter_control - 64) * 256 * bend) >> 16) +
               (((r.mw.filter_control - 64) * mw) >> 2) + (((r.cat.filter_control - 64) * cat) >> 2) +
               (((r.ac[0].filter_control - 64) * ac1) >> 2) +
               (((r.ac[1].filter_control - 64) * ac2) >> 2);
    // LFO depths.
    const int bp = std::abs(part.pitchbend / 64 - 128);
    c.pmd = std::min(127, bp * r.bend.lfo_pmod_depth / 128 + mw * r.mw.lfo_pmod_depth / 128 +
                              cat * r.cat.lfo_pmod_depth / 128 + ac1 * r.ac[0].lfo_pmod_depth / 128 +
                              ac2 * r.ac[1].lfo_pmod_depth / 128);
    const int ba = std::abs(bend) >> 8;
    c.amd = std::min(15, std::min(127, ((ba * r.bend.lfo_amod_depth) >> 8) +
                                           ((mw * r.mw.lfo_amod_depth) >> 10) +
                                           ((cat * r.cat.lfo_amod_depth) >> 10) +
                                           ((ac1 * r.ac[0].lfo_amod_depth) >> 10) +
                                           ((ac2 * r.ac[1].lfo_amod_depth) >> 10)));
    c.fmd = ((ba * r.bend.lfo_fmod_depth) >> 8) + ((mw * r.mw.lfo_fmod_depth) >> 10) +
            ((cat * r.cat.lfo_fmod_depth) >> 10) + ((ac1 * r.ac[0].lfo_fmod_depth) >> 10) +
            ((ac2 * r.ac[1].lfo_fmod_depth) >> 10);
    c.detune = law::detune_cents(r.get_detune());
    c.coarse = std::clamp<int>(part.rpn.coarse, 40, 88) - 64;
    return c;
}

// ── note-on ─────────────────────────────────────────────────────────────────────────

void ToneModule50::note_on(const midi::Part& part, std::uint8_t note, std::uint8_t velocity) {
    apply_touch();
    const midi::xg::MultiPart& ram = *part.ram;
    if (!part.program.valid) return;
    // Note and velocity limits: tested on the raw velocity.
    if (note < ram.note_limit_low || note > ram.note_limit_high) return;
    if (velocity < ram.velocity_limit_low || velocity > ram.velocity_limit_high) return;
    const int vel =
        law::velocity_sense(velocity, ram.velocity_sense_depth, ram.velocity_sense_offset);
    const std::uint32_t serial = ++serial_;
    if (drum_mode(part) != 0) {
        start_drum(part, note, vel, serial);
        key_on_flush();
        return;
    }
    PartState& ps = parts_[part.id % parts_.size()];
    if (ram.mode == 0) {
        // Mono: only the previous mono note's register sets (`part[0xd4..]`)
        // are cut at rate 0x5a: while keys are held or the sustain pedal is down (`part[0xd6]`
        // bit 0) its keyed voices on the last key, and when no key is held its
        // released voices still sounding; voices of older notes play on
        // [code, meas].
        const bool held = ps.held_count > 0;
        if (ps.mono_serial != 0)
            for (auto& vp : voices_) {
                Voice& v = *vp;
                if (!v.in_use || v.part != &part || v.serial != ps.mono_serial) continue;
                if (v.keyed ? (held || part.controller.sustain) && v.note == ps.last_key : !held)
                    release(v, kCutRelease);
            }
        if (ps.held_count < 8) {
            ps.held[static_cast<std::size_t>(ps.held_count++)] = note;
        } else {
            auto it = std::min_element(ps.held.begin(), ps.held.end());
            *it = note;
        }
        ps.mono_key = note;
        ps.mono_vel = vel;
    } else if (ram.key_assign == 0) {
        // Single: cut the first note of the same key in the part's
        // voice list (allocation order).
        for (Voice* vp : part_voices(part)) {
            if (vp->note == note && !vp->detached) {
                release_note(*vp, kCutRelease);
                break;
            }
        }
    }
    play(part, note, velocity, vel, serial);
    key_on_flush();
}

void ToneModule50::play(const midi::Part& part, std::uint8_t note, std::uint8_t, int vel,
                        std::uint32_t serial) {
    const midi::xg::MultiPart& ram = *part.ram;
    PartState& ps = parts_[part.id % parts_.size()];
    const BankMode mode = bank_mode(*ctx_.shared.reset_mode);
    const std::uint8_t* pv = bank_->program(mode, part.program.bank_msb, part.program.bank_lsb,
                                            part.program.program);
    const int porta_src = ram.portamento_switch ? ps.last_key : -1;
    ps.last_key = note;
    if (ram.mode == 0) ps.mono_serial = serial;
    if (pv == nullptr) return;
    std::array<const std::uint8_t*, 2> sel{};
    int n = 0;
    for (int i = 0; i < BankData::element_count(pv); ++i) {
        const std::uint8_t* e = BankData::element(pv, i);
        if (note >= e[1] && note <= e[2] && vel >= e[3] && vel <= e[4]) sel[static_cast<std::size_t>(n++)] = e;
    }
    if (n == 0 || !make_room(part, n)) return;
    for (int i = 0; i < n; ++i)
        start_element(part, note, vel, pv, sel[static_cast<std::size_t>(i)], nullptr, nullptr,
                      nullptr, serial, porta_src);
}

void ToneModule50::start_drum(const midi::Part& part, std::uint8_t note, int vel,
                              std::uint32_t serial) {
    // key setup.
    const midi::xg::MultiPart& ram = *part.ram;
    const BankMode mode = bank_mode(*ctx_.shared.reset_mode);
    const std::size_t kit = bank_->drum_kit(mode, part.program.bank_msb, part.program.bank_lsb,
                                            part.program.program);
    const std::uint8_t* e = bank_->drum_entry(kit, note);
    if (e == nullptr) return;
    Rec d{}, src{};
    const bool xg = mode != BankMode::Gs;
    const midi::xg::DrumSetup* ramrec = nullptr;
    const int pmode = drum_mode(part);
    if (pmode >= 2 && note >= 13 && note <= 91 &&
        static_cast<std::size_t>(pmode - 2) < ctx_.shared.drum_setup->size())
        ramrec = &(*ctx_.shared.drum_setup)[static_cast<std::size_t>(pmode - 2)][note - 13u];
    if (xg && (pmode == 1 || ramrec == nullptr)) {
        std::copy(tb_.kDrumDefault.begin(), tb_.kDrumDefault.end(), d.begin());
        if (pmode == 1) std::copy(e, e + 16, src.begin());
        else src = d;
    } else if (ramrec != nullptr) {
        for (std::size_t i = 0; i < 16; ++i) d[i] = (*ramrec)[i];
        if (!xg) {
            // GS: the RAM's pitch / filter / EG bytes are offsets onto the kit entry.
            for (std::size_t i : {0u, 1u, 11u, 12u, 13u, 14u, 15u})
                d[i] = static_cast<std::uint8_t>(clamp7(e[i] + d[i] - 64));
        }
        src = d;
    } else {
        std::copy(e, e + 16, d.begin());
        src = d;
    }
    if (src[10] == 0) return; // receive note on
    {
        // Superset EQ / HPF of the note (eq.hpp): the drum-setup RAM, else the part's own copy.
        const midi::xg::DrumSetup* rec = ramrec;
        if (rec == nullptr && note >= 13 && note <= 91 && part.drum_setup_program.valid &&
            part.drum_setup_program == part.program)
            rec = &part.drum_setup[note - 13u];
        drum_eq_ = eq::voice_settings(ctx_.dll_exact, ctx_.mu_part_eq, part, rec, true);
    }

    PartState& ps = parts_[part.id % parts_.size()];
    ps.last_key = note;
    // Alternate group, else single key assign: cut at rate 0x5a.
    const int assign = ram.key_assign == 2 ? src[8] : ram.key_assign;
    for (Voice* vp : part_voices(part)) {
        Voice& v = *vp;
        if (v.detached || v.kind == 0) continue;
        if (src[3] != 0 ? v.alt_group == src[3] : (assign == 0 && v.note == note)) {
            release_note(v, kCutRelease);
            break;
        }
    }

    if (bank_->drum_own_wave(e)) {
        if (!make_room(part, 1)) return;
        start_element(part, note, vel, nullptr, nullptr, e, d.data(), src.data(), serial, -1);
        return;
    }
    const std::uint8_t* pv = bank_->drum_prevoice(e);
    if (pv == nullptr) return;
    // Prevoice keys play key d[0] (64 in XG), not the played key.
    const int key = d[0];
    std::array<const std::uint8_t*, 2> sel{};
    int n = 0;
    for (int i = 0; i < BankData::element_count(pv); ++i) {
        const std::uint8_t* el = BankData::element(pv, i);
        if (key >= el[1] && key <= el[2] && vel >= el[3] && vel <= el[4]) sel[static_cast<std::size_t>(n++)] = el;
    }
    if (n == 0 || !make_room(part, n)) return;
    for (int i = 0; i < n; ++i)
        start_element(part, note, vel, pv, sel[static_cast<std::size_t>(i)], e, d.data(),
                      src.data(), serial, -1);
}

bool ToneModule50::start_element(const midi::Part& part, std::uint8_t note, int vel,
                                 const std::uint8_t* pv, const std::uint8_t* e,
                                 const std::uint8_t* entry, const std::uint8_t* dd,
                                 const std::uint8_t* ss, std::uint32_t serial, int porta_src) {
    // The voice comes from the head of the free FIFO, so the
    // index — and with it the voice's turns in the tick's round robins — cycles through the
    // polyphony instead of reusing the lowest free voice [code, meas].
    if (free_.empty()) return false;
    Voice& v = *voices_[free_.front()];
    free_.pop_front();
    const std::uint64_t seq = ++alloc_seq_;
    const std::size_t index = v.index;
    // A freed voice's chip voice plays on until its index is taken again (binds
    // a fresh chip voice to the key-on); keep it as a tail.
    if (v.chip.active()) detach_tail(v);
    // The update flags (v[0xc]) are not cleared when a register set is taken: a flag left
    // pending by its previous note carries over.
    const bool f0 = v.flag0, f12 = v.flag12;
    v = Voice{};
    v.flag0 = f0;
    v.flag12 = f12;
    v.index = index;
    v.alloc_seq = seq;
    v.in_use = true;
    v.part = &part;
    v.touch_seen = part.touch;
    v.note = note;
    v.vel = vel;
    v.serial = serial;
    v.keyed = true;
    v.elem = e;
    v.entry = entry;
    v.kind = entry == nullptr ? 0 : (e == nullptr ? 1 : 2);
    if (dd != nullptr) std::copy(dd, dd + 16, v.d.begin());
    if (ss != nullptr) std::copy(ss, ss + 16, v.src.begin());
    if (v.kind != 0) v.eq_drum = drum_eq_;
    v.chip.eq.start(v.kind == 0 ? eq::voice_settings(ctx_.dll_exact, ctx_.mu_part_eq, part, nullptr, false) : v.eq_drum,
                    ctx_.sample_rate);

    const midi::xg::MultiPart& ram = *part.ram;
    const midi::xg::System& sys = *ctx_.shared.system;
    const BankMode mode = bank_mode(*ctx_.shared.reset_mode);
    const bool xg = mode != BankMode::Gs;
    const Ctl& c = ctl(part);
    const int pa = ram.eg_attack_time - 64, pd = ram.eg_decay_time - 64,
              pr = ram.eg_release_time - 64;
    // GS mode, melodic voice of a bank below 64: the vibrato rate base, the
    // amp release offset and the vibrato delay table come from the program (XG: 14, 1, 0).
    int rate_base = 14, release_offset = 1, delay_table = 0;
    if (!xg && v.kind == 0 && ram.bank_msb < 64) {
        const std::size_t prog = ram.program_number & 0x7Fu;
        rate_base = tb_.kGsVibratoRateBase[prog];
        release_offset = tb_.kGsReleaseOffset[prog];
        delay_table = 2 * tb_.kGsVibratoDelayTable[prog];
    }

    if (v.kind != 0) {
        v.alt_group = v.src[3];
        v.rcv_note_off = v.src[9] != 0;
        v.drum_level = v.d[2] == 0 ? 0 : v.src[2];
        v.drum_sends = {v.src[5], v.src[6], v.src[7]};
        if (!v.rcv_note_off && v.kind == 1) {
            const std::size_t kit = bank_->drum_kit(mode, part.program.bank_msb,
                                                    part.program.bank_lsb, part.program.program);
            const auto& t = kit > 9 ? tb_.kDrumAutoReleaseXg
                                    : (kit == 7 ? tb_.kDrumAutoReleaseBrush : tb_.kDrumAutoReleaseGs);
            v.auto_release = t[note & 0x7f];
        }
    }

    WaveRef wave{};
    std::uint32_t skip = 0;
    if (v.kind == 1) {
        // Own-wave drum key: pitch independent of the note.
        wave = bank_->drum_wave(entry);
        const int semis = v.d[0] + static_cast<std::int8_t>(xg ? entry[29] : entry[28]) - bank_->drum_root_key(entry) +
                          (ram.note_shift - 64) + c.coarse;
        v.base_cents = semis * 100 + (v.d[1] - 64) + (xg ? entry[1] - 64 : 0);
    } else {
        // Portamento source / key tracking.
        const int key = v.kind == 2 ? v.d[0] : note;
        bool glide = porta_src >= 0 && porta_src != key && v.kind == 0;
        const int base = glide ? std::max(porta_src, static_cast<int>(key)) : key;
        const law::Tracked t = law::key_track(tb_, base, e[15], e[16]);
        int key6 = t.key + (ram.note_shift - 64) + (e[13] - 64) + c.coarse;
        if (v.kind == 0) key6 += static_cast<int>(sys.transpose) - 64;
        key6 = clamp7(key6);
        const std::uint8_t* z = bank_->zone(e, key6);
        if (z == nullptr) {
            // No zone: the DLL selects the zones before it takes a voice; give it back.
            v.in_use = false;
            v.queued = true;
            free_.push_front(v.index);
            return false;
        }
        wave = bank_->zone_wave(z);
        skip = static_cast<std::uint32_t>(e[75]) * 128 + e[76];
        v.zone_atten = z[0];
        int cents = (key6 - z[1]) * 100 + static_cast<std::int8_t>(z[2]) + (e[14] - 64) + t.cents;
        // The XG master tune is taken at the note-on [meas: a
        // master tune change never reaches a sounding note]; the GM2 master tuning is live:
        // update().
        if (v.kind == 0)
            cents += ram.scale_tuning[note % 12] - 64 + (static_cast<int>(sys.applied_master_tune) - 0x400) / 10;
        else
            cents += v.d[1] - 64;
        v.base_cents = cents;
        if (glide) {
            const int dd2 = (static_cast<int>(key) - porta_src) *
                            tb_.kKeyTrack[std::min<std::size_t>(e[15], 7)];
            v.porta_up = key > porta_src;
            if (ram.portamento_time == 0) {
                v.porta = v.porta_tgt = v.porta_up ? 0 : dd2;
            } else if (v.porta_up) {
                v.porta = -dd2;
                v.porta_tgt = 0;
            } else {
                v.porta = 0;
                v.porta_tgt = dd2;
            }
        }
    }
    v.chip.set_wave(bank_->wave(), wave, skip);
    v.wave_format = wave.format;

    // Level.
    if (v.kind == 1) {
        v.level0 = 128;
        v.tvel = law::velocity_atten(tb_, 1, vel);
    } else {
        v.level0 = e[55] * pv[0] / 99;
        if (v.level0 != 0) {
            const int ks = static_cast<std::int8_t>(law::key_scaling(e + 56, e + 60, note) << 1);
            v.level0 = std::clamp(v.level0 + ks, 0, 128);
        }
        v.tvel = law::velocity_atten(tb_, e[64], law::velocity_offset(vel, e[32]));
    }

    // Pan.
    const int epan = v.kind == 1 ? 64
                                 : (e[65] == 15 ? tb_.kKeyPan[note] : tb_.kElementPanPos[e[65] & 15]);
    const int base_pan = v.kind == 0 ? 64 : (v.kind == 1 ? v.src[4] : v.d[4]);
    // Random pan only on drum keys: key pan 0 or part pan 0.
    if (v.kind != 0 && (base_pan == 0 || ram.pan == 0)) {
        v.random_pan = true;
        v.pan = v.pan_send = rand() >> 1;
    } else {
        v.pan = clamp7(base_pan + epan - 64);
        v.pan_send = v.kind == 1 ? v.pan : epan;
    }

    // Filter (own wave).
    if (v.kind == 1) {
        v.cutoff0 = tb_.kCutoff[static_cast<std::size_t>(xg ? clamp7(v.d[11] + entry[11] - 64) : v.d[11])];
        v.resonance = (xg ? clamp7(v.d[12] + entry[12] - 64) : v.d[12]) >> 1;
    } else {
        const int idx = v.kind == 2 && xg ? clamp7(v.d[11] + e[33] - 64) : e[33];
        v.cutoff0 = tb_.kCutoff[static_cast<std::size_t>(idx)] +
                    32 * law::key_scaling(e + 34, e + 38, note);
        int r = e[31];
        const int s = e[77] - 64;
        if (s > 0) r -= ((128 - vel) * s * 18) >> 8;
        else if (s < 0) r -= (vel * -s * 18) >> 8;
        r = std::max(r, 0);
        if (v.kind == 2 && xg) r = clamp7(v.d[12] + 2 * r - 64) >> 1;
        v.resonance = r;
    }

    // AEG.
    if (v.kind == 1) {
        // XG: a sum of 94 or more gives the kit's own rate, at least 94 (the
        // drum setup offset cannot raise the attack past max(94, kit rate)); GS (absolute
        // copy): above 94 only when the kit's rate is [code, meas].
        int a;
        if (xg) {
            a = v.d[13] + entry[13] - 64;
            if (a >= 94) a = std::max<int>(94, entry[13]);
            a = clamp7(a);
        } else {
            a = v.d[13];
            if (a > 94 && entry[13] < 94) a = 94;
        }
        const int x = a >> 1;
        int ar;
        if (pa <= 0) {
            int t = 2 * x;
            if (t < 94) t = std::min(t - pa, 94);
            ar = t + 33;
        } else {
            ar = 2 * std::min<int>(tb_.kPartAttackRate[static_cast<std::size_t>(pa)], x + 33);
        }
        ar = std::max(ar, 0x20);
        const int d1 = xg ? clamp7(entry[14] + v.d[14] - 64) : v.d[14];
        const int d2 = xg ? clamp7(entry[15] + v.d[15] - 64) : v.d[15];
        const int k = pd <= 0 ? law::floor_div(pd, 4) : (pd * 0x70) >> 8;
        v.d2r = 2 * std::clamp(d2 / 2 - k, 0, 63);
        v.d2l = 0xff;
        v.rr = kOwnWaveRelease;
        if (ar >= 0x80) {
            // Hold path: hold rate 0x7e − AR, then D2 straight to silence.
            v.chip.set_attack((0x7e - ar) & 0x7f, 0);
            v.chip.set_decay(d2, 0xff);
            v.stage = 4;
        } else if (ar == 0x7f) {
            // 0x7f → 0xff: commit as instant attack + hold (0x4c0 = 0x7f, 0x4c1 = 0).
            v.chip.set_attack(0x7f, 0);
            v.chip.set_decay(d1, 4);
        } else {
            v.chip.set_attack(ar | 0x80, 0xff);
            v.chip.set_decay(d1, 4);
        }
    } else {
        // (melodic), (drum prevoice).
        const int ks = law::aeg_key_scaling(note, e[67], e[66]);
        int e69 = e[69], e70 = e[70], e71 = e[71];
        if (v.kind == 2 && xg) {
            if (bank_->mu().on) {
                // SXG-Create-NF drum EG patch: the element rate's unit is two drum units, so
                // the drum setup offset counts half: (offset + 2 · rate − 64) >> 1.
                e69 = clamp7((v.d[13] + 2 * e69 - 64) >> 1);
                e70 = clamp7((v.d[14] + 2 * e70 - 64) >> 1);
                e71 = clamp7((v.d[15] + 2 * e71 - 64) >> 1);
            } else {
                e69 = clamp7(e69 + v.d[13] - 64);
                e70 = clamp7(e70 + v.d[14] - 64);
                e71 = clamp7(e71 + v.d[15] - 64);
            }
        }
        int r = pa <= 0 ? std::min(63, e69 - law::floor_div(pa, 2))
                        : std::min<int>(e69, tb_.kPartAttackRate[static_cast<std::size_t>(pa)]);
        if (r == 0) r = 4;
        int arr = 2 * r;
        if (arr == 0x7e) arr = 0xff;
        int d1 = e70;
        if (pa > 0) d1 = std::min(d1, ((63 - pa) >> 2) + 2);
        d1 += ks;
        d1 = d1 <= 0 ? 1 : std::min(d1, 63);
        const int o = pd <= 0 ? law::floor_div(pd, 4) : (pd * 0x70) >> 8;
        const int d2 = std::clamp(e71 + ks - o, 0, 63);
        int rr;
        if (pr <= 0) rr = e[72] <= 55 ? std::min(55, e[72] - law::floor_div(pr, 2)) : e[72];
        else rr = std::min<int>(e[72], tb_.kPartReleaseRate[static_cast<std::size_t>(pr + release_offset)]);
        rr += ks;
        rr = rr <= 0 ? 1 : std::min(rr, 63);
        // Commit: AR ≥ 0x80 → instant attack and a hold (0x4c1 = 0).
        if (arr >= 0x80) v.chip.set_attack(arr & 0x7f, 0);
        else v.chip.set_attack(arr | 0x80, 0xff);
        v.chip.set_decay(2 * d1, static_cast<std::uint8_t>(-2 * (e[73] + 1)));
        v.d2r = 2 * d2;
        v.d2l = static_cast<std::uint8_t>(-2 * (e[74] + 1));
        // a D2 level above the D1 level keeps the held voice from the monitor.
        if (v.d2l < static_cast<std::uint8_t>(-2 * (e[73] + 1))) v.hold = true;
        v.rr = 2 * rr;
        v.rr_normal = v.rr;
        // Poly melodic voices (v[0x7e] = 0x80) take the sustain pedal's damper rule.
        v.damper = v.kind == 0 && ram.mode != 0;
    }

    if (v.kind != 1) {
        // PEG.
        v.peg_vs = law::eg_level_vs(e[18], vel);
        v.peg = law::peg_level(e[26] - 64, v.peg_vs, e[17]) +
                law::peg_part_level(ram.pitch_eg_init_level - 64, v.peg_vs);
        v.peg_stage = 0;
        // FEG.
        const auto& curve = (e[5] & 0x80) ? tb_.kFegVelCurve1 : tb_.kFegVelCurve0;
        const int s = e[42] - 64;
        const int cv = curve[static_cast<std::size_t>(clamp7(vel))];
        v.feg_vs = s < 0 ? (cv * -s * 36) >> 7 : (s > 0 ? ((128 - cv) * s * 36) >> 7 : 0);
        v.feg = law::feg_level(e[50], v.feg_vs);
        v.feg_stage = 0;
        // LFO.
        v.lfo_on = true;
        v.lfo_tri = (e[5] & 7) != 0;
        v.lfo_sh = (e[5] & 7) >= 2;
        v.lfo_rate_base = rate_base;
        int dl = e[8] ? (3 * e[8] >> 2) + 3 : 0;
        const int pdl = ram.vibrato_delay;
        const int td = tb_.kVibratoDelay[static_cast<std::size_t>((pdl & 0x7f) + delay_table)];
        if (pdl < 64) dl = std::min(dl, td);
        else if (pdl > 64) dl = std::max(dl, td);
        v.lfo_delay = dl;
        // GS mode: its own depth table, and the element depth alone when that is larger.
        const int t = (xg ? tb_.kVibratoDepth : tb_.kVibratoDepthGs)[ram.vibrato_depth & 0x7f];
        if (t < e[10] && !v.lfo_sh) {
            v.vib_target = xg ? std::max(0, t + e[10]) : e[10];
            v.vib_step = std::clamp(v.vib_target / (e[9] + 1), 1, 20);
        } else {
            v.vib_target = std::max(0, 2 * t);
            v.vib_step = std::clamp(v.vib_target / (e[9] + 1), 1, 5);
        }
        // fade state 0 (delay, then fade), or 2 (done) without delay and fade.
        v.lfo_state = (dl == 0 && e[9] == 0) ? 2 : 0;
        v.vib = v.lfo_state == 2 ? v.vib_target : 0;
    }

    if (v.kind != 1) {
        // Stage 0: L0 → L1 at R1; an instant R1 starts at L1.
        peg_set(v, 1);
        if (v.peg_step == kInstant) v.peg = v.peg_tgt;
        feg_set(v, 1);
        if (v.feg_step == kInstant) v.feg = v.feg_tgt;
    }
    // Key-on delay: element byte 68 → the delay table; the voice is
    // set up now but keyed by the 10-ms tick, see tick().
    if (v.kind != 1) v.key_delay = tb_.kKeyOnDelay[e[68] & 15];
    if (v.key_delay != 0) v.hold = true;
    v.level_due = true;
    // The note-on reads the part's current controller state (the DLL recomputes the part
    // gain when volume, expression or master volume change), not this block's cache.
    parts_[part.id % parts_.size()].ctl_block = ~std::uint64_t{0};
    // the note-on commits the cutoff with the glide counter at 0xf0 (instant),
    // then sets it to 0xc1, so the next commit keeps that rate once and the glide then follows
    // the cutoff steps [code, meas].
    v.cg_counter = 0xf0;
    update(v, true);
    v.cg_counter = 0xc1;
    if (v.key_delay == 0) {
        v.chip.start(rt_);
        v.phys_due = true; // bound to a physical voice at the note's key-on flush (key_on_flush)
    }
    // the initial stealing score from the velocity term v[0x71].
    v.score = 0xff - std::min(2 * v.tvel, 0xff);
    return true;
}
// ── voice pool ──────────────────────────────────────────────────────────────────────

void ToneModule50::detach_tail(Voice& v) {
    auto it = std::find_if(tails_.begin(), tails_.end(), [](const auto& c) { return !c.active(); });
    if (it != tails_.end()) *it = v.chip;
    v.chip.stop();
}

void ToneModule50::free_voice(Voice& v) {
    if (v.chip.active()) detach_tail(v);
    retire(v);
}

void ToneModule50::retire(Voice& v) {
    // The voice goes to the tail of the free FIFO.
    if (!v.in_use) return;
    v.in_use = false;
    enqueue(v);
}

void ToneModule50::enqueue(Voice& v) {
    if (v.queued) return;
    v.queued = true;
    free_.push_back(v.index);
}

bool ToneModule50::make_room(const midi::Part& part, int n) {
    const auto free_count = [&] {
        return static_cast<int>(free_.size());
    };
    while (free_count() < n) {
        // Voices per part (and the part itself: part ids are unique, so the voices of one id
        // all belong to one part), in one pass.
        std::array<int, midi::kMaxPartSize> active{}, keyed{};
        std::array<const midi::Part*, midi::kMaxPartSize> owner{};
        for (auto& vp : voices_) {
            if (!vp->in_use || vp->queued) continue;
            const std::size_t id = vp->part->id % parts_.size();
            ++active[id];
            if (vp->keyed) ++keyed[id];
            owner[id] = vp->part;
        }
        const midi::Part* victim_part = nullptr;
        bool released_only = false;
        for (int p = static_cast<int>(parts_.size()) - 1; p >= 0 && victim_part == nullptr; --p) {
            const auto id = static_cast<std::size_t>(p);
            if (active[id] == 0) continue;
            const int reserve = owner[id]->ram->element_reserve;
            if (active[id] > reserve && active[id] > keyed[id]) {
                victim_part = owner[id];
                released_only = true;
            }
        }
        for (int p = static_cast<int>(parts_.size()) - 1; p >= 0 && victim_part == nullptr; --p) {
            const auto id = static_cast<std::size_t>(p);
            if (active[id] != 0 && active[id] > owner[id]->ram->element_reserve) victim_part = owner[id];
        }
        if (victim_part == nullptr && active[part.id % parts_.size()] > 0) victim_part = &part;
        if (victim_part == nullptr) return false;
        // each note (element group, first met in the part's voice
        // list, i.e. allocation order) scores the highest of its elements' v[0x6d]; the lowest
        // score wins, ties to the earlier note; the part's lowest key is protected unless it is
        // the only candidate (not in the released-only search).
        int low_key = 128;
        for (Voice* vp : part_voices(*victim_part)) low_key = std::min<int>(low_key, vp->note);
        Voice* best = nullptr;
        Voice* best_other = nullptr;
        int best_score = 0x100, other_score = 0x100;
        // A note's score is the highest of its elements' (the part-list voices with its serial,
        // its group_of ring), found for all notes in one sorted pass instead of a voice scan
        // per note; each note is met at its first voice in the list.
        const VoiceList list = part_voices(*victim_part);
        std::vector<NoteScore>& notes = note_score_;
        notes.clear();
        for (Voice* vp : list) notes.push_back({vp->serial, vp->score, false});
        std::sort(notes.begin(), notes.end(),
                  [](const NoteScore& a, const NoteScore& b) { return a.serial != b.serial ? a.serial < b.serial : a.score < b.score; });
        for (std::size_t i = notes.size(); i-- > 1;)  // every entry of a serial gets its highest score
            if (notes[i - 1].serial == notes[i].serial) notes[i - 1].score = notes[i].score;
        for (Voice* vp : list) {
            Voice& v = *vp;
            NoteScore& ns = *std::lower_bound(notes.begin(), notes.end(), v.serial,
                                              [](const NoteScore& a, std::uint32_t s) { return a.serial < s; });
            if (ns.seen) continue;
            ns.seen = true;
            const int score = std::max(0, ns.score);
            if (released_only && v.keyed) continue;
            if (score < best_score) {
                best = &v;
                best_score = score;
            }
            if (v.note != low_key && score < other_score) {
                best_other = &v;
                other_score = score;
            }
        }
        if (!released_only && best != nullptr && best->note == low_key && best_other != nullptr)
            best = best_other;
        if (best == nullptr) return false;
        // the group from the chosen voice round its ring is damped
        // and freed to the FIFO; each voice stays busy, released, until the AEG
        // monitor ends it.
        // Superset steal fade (`steal_fade_ms`): the stolen sound fades out linearly instead of
        // at the damp rate; it then ends on its chip voice and the monitor frees it as usual.
        const auto fade = steal_fade_samples(ctx_.steal_fade_ms, ctx_.sample_rate);
        for (Voice* w : group_of(*best)) {
            damp(*w, fade);
            enqueue(*w);
        }
    }
    return true;
}

// ── note-off, pedals ────────────────────────────────────────────────────────────────

void ToneModule50::release(Voice& v, int rr) {
    // Key-off: 0x500 = RR | 0x80, 0x501 = 0xff; PEG / FEG release.
    if (v.key_delay > 0) {
        // a note-off before the delayed key-on cancels it (state 6, never keyed).
        // The AEG monitor frees it at its next visit (state 6).
        v.key_delay = 0;
        v.keyed = false;
        v.hold = false;
        v.stage = 6;
        return;
    }
    if (rr >= 0) v.rr = rr;
    v.rr66 = v.rr;
    v.keyed = false;
    v.hold = false;
    v.released = true;
    v.sus_pending = false;
    v.stage = 1;
    v.chip.set_decay(v.rr | 0x80, 0xff);
    if (v.kind == 1) return;
    const std::uint8_t* e = v.elem;
    const midi::xg::MultiPart& ram = *v.part->ram;
    // PEG release: skipped (stage 5) only when the PEG holds at
    // L3 (stage 3) and L3 = L4 with no part release level.
    if (e[29] == e[30] && v.peg_stage == 3 && ram.pitch_eg_release_level == 64) {
        v.peg_stage = 5;
    } else {
        int r = e[25];
        const int q = ram.pitch_eg_release_time - 64;
        r = q <= 0 ? std::min(63, r - law::floor_div(q, 2))
                   : std::min<int>(r, tb_.kPartReleaseRate[static_cast<std::size_t>(q)]);
        v.peg_tgt = law::peg_level(e[30] - 64, v.peg_vs, e[17]) +
                    law::peg_part_level(ram.pitch_eg_release_level - 64, v.peg_vs);
        r = std::clamp(r + law::floor_div(64 - ram.eg_release_time, 4), 0, 63);
        r += law::eg_key_scaling(v.note, e[21], e[20]);
        if (r > 62) v.peg_step = kInstant;
        else {
            r += law::eg_rate_vs(e[19], v.vel);
            v.peg_step = tb_.kPegStep[static_cast<std::size_t>(std::clamp(r, 0, 62))];
        }
        if (v.peg_tgt < v.peg && v.peg_step != kInstant) v.peg_step = -v.peg_step;
        v.peg_stage = 4;
    }
    // FEG release: R4 → L4 (byte 54); skipped (stage 5) only when the FEG holds
    // at L3 (stage 3) and L3 = L4.
    if (e[53] == e[54] && v.feg_stage == 3) {
        v.feg_stage = 5;
    } else {
        v.feg_stage = 4;
        feg_set(v, 4);
    }
}

void ToneModule50::peg_set(Voice& v, int n) {
    // rate R<n> and target L<n> (n = 1..3); the step is negative
    // when the target is below the current level.
    const std::uint8_t* e = v.elem;
    const midi::xg::MultiPart& ram = *v.part->ram;
    int r = e[21 + n];
    if (n == 1) {
        const int q = ram.pitch_eg_attack_time - 64;
        if (q < 0) r = std::min(63, r - law::floor_div(q, 2));
        else if (q > 0) r = std::min<int>(r, tb_.kPartAttackRate[static_cast<std::size_t>(q)]);
    }
    // Part EG attack (R1) / decay (R3) offset, key scaling, velocity.
    const int p = n == 1 ? ram.eg_attack_time : (n == 3 ? ram.eg_decay_time : 64);
    r = std::clamp(r + law::floor_div(64 - p, 4), 0, 63);
    r += law::eg_key_scaling(v.note, e[21], e[20]);
    if (r > 62) {
        v.peg_step = kInstant;
    } else {
        r += law::eg_rate_vs(e[19], v.vel);
        v.peg_step = tb_.kPegStep[static_cast<std::size_t>(std::clamp(r, 0, 62))];
    }
    v.peg_tgt = law::peg_level(e[26 + n] - 64, v.peg_vs, e[17]);
    if (v.peg_tgt < v.peg && v.peg_step != kInstant) v.peg_step = -v.peg_step;
}

void ToneModule50::peg_advance(Voice& v) {
    // the level jumps to the target; stage 0 → 1 (L1 → L2 at R2), 1 → 2 (L2 → L3
    // at R3); a stage whose levels are equal is skipped at once; 2 → 3 holds, 4 → 5 ends.
    const std::uint8_t* e = v.elem;
    v.peg = v.peg_tgt;
    const int st = ++v.peg_stage;
    if (st == 1) {
        if (e[27] != e[28]) {
            peg_set(v, 2);
            return;
        }
        v.peg_stage = 2;
    } else if (st != 2) {
        return;
    }
    if (e[28] == e[29]) {
        v.peg_stage = 3;
        return;
    }
    peg_set(v, 3);
}

void ToneModule50::feg_set(Voice& v, int n) {
    // rate R<n> and target L<n> (n = 1..4); the part EG time
    // clamps the rate (not the attack when L1 < L0); the step is negative when
    // the target is below the current level.
    const std::uint8_t* e = v.elem;
    const midi::xg::MultiPart& ram = *v.part->ram;
    int r = e[45 + n];
    int p = n == 1 ? ram.eg_attack_time
                   : (n == 3 ? ram.eg_decay_time : (n == 4 ? ram.eg_release_time : 64));
    if (n == 1 && p < 64 && e[51] < e[50]) p = 64;
    const int t = tb_.kFegPartRateClamp[static_cast<std::size_t>(p & 0x7f)];
    if (p < 64) r = std::max(r, t);
    else if (p > 64) r = std::min(r, t);
    if (r > 62) {
        v.feg_step = kInstant;
    } else {
        int c = std::max(0, r + law::eg_key_scaling(v.note, e[45], e[44]));
        if (c > 62) {
            v.feg_step = kInstant; // [63]
        } else {
            c += law::eg_rate_vs(e[43], v.vel);
            v.feg_step = tb_.kFegStep[static_cast<std::size_t>(std::clamp(c, 0, 62))];
        }
    }
    v.feg_tgt = law::feg_level(e[50 + n], v.feg_vs);
    if (v.feg_tgt < v.feg && v.feg_step != kInstant) v.feg_step = -v.feg_step;
}

void ToneModule50::feg_advance(Voice& v) {
    // as the PEG's; every stage change also restarts the cutoff glide counter
    // at 0xc1 (the next commit keeps the faster of its rate and the previous one).
    const std::uint8_t* e = v.elem;
    v.feg = v.feg_tgt;
    const int st = ++v.feg_stage;
    v.cg_counter = 0xc1;
    if (st == 1) {
        if (e[51] != e[52]) {
            feg_set(v, 2);
            return;
        }
        v.feg_stage = 2;
    } else if (st != 2) {
        return;
    }
    if (e[52] == e[53]) {
        v.feg_stage = 3;
        return;
    }
    feg_set(v, 3);
}

void ToneModule50::damp(Voice& v, std::uint32_t steal_fade) {
    // Damp: a delayed key-on is cancelled; else key-off with the damp
    // rate (0x500 = 0xfe), released state, v[0x66] = 0x3e.
    v.hold = false;
    v.keyed = false;
    if (v.key_delay > 0) {
        v.key_delay = 0;
        return;
    }
    if (steal_fade != 0)
        v.chip.start_steal_fade(steal_fade);
    else
        v.chip.set_decay(kDamp, 0xff);
    v.sus_pending = false;
    v.released = true;
    if (v.stage != 0) v.stage = 1;
    v.rr66 = 0x3e;
}

void ToneModule50::release_note(Voice& first, int rr) {
    // Release and free round the element ring from `first`: release the note's elements at
    // `rr` and free their register sets.
    // The voices stay busy, released, until the AEG monitor ends them (their pitch, cutoff and
    // level still follow the part meanwhile).
    for (Voice* v : group_of(first)) {
        release(*v, rr);
        enqueue(*v);
    }
}

void ToneModule50::bind_phys(Voice& v) {
    // a key-on takes the first physical chip voice not sounding at or after
    // the cursor (256 of them, the cursor left after it) and points the register set at it.
    v.phys_due = false;
    std::vector<std::uint8_t>& busy = phys_busy_;
    std::fill(busy.begin(), busy.end(), std::uint8_t{0});
    for (const auto& w : voices_)
        if (w.get() != &v && w->chip.active() && w->chip.phys >= 0) busy[static_cast<std::size_t>(w->chip.phys)] = true;
    for (const auto& t : tails_)
        if (t.active() && t.phys >= 0) busy[static_cast<std::size_t>(t.phys)] = true;
    std::size_t p = phys_cursor_;
    for (std::size_t n = 0; n < phys_count_; ++n) {
        p = (phys_cursor_ + n) % phys_count_;
        if (!busy[p]) break;
    }
    phys_cursor_ = (p + 1) % phys_count_;
    v.chip.phys = static_cast<int>(p);
    v.phys = static_cast<int>(p);
}

void ToneModule50::key_on_flush() {
    // at the end of a note-on: the key-on bitmaps, by register-set
    // index.
    for (auto& vp : voices_)
        if (vp->phys_due) bind_phys(*vp);
}

int ToneModule50::phys_status(const Voice& v) const {
    // The AEG monitor reads the status of the physical voice the register set points at. Once that voice has ended (0xff) a later key-on of
    // another register set may take it, and the status is then that note's [code; meas:
    // DLL status log of TrekMedG voice 97].
    if (v.phys < 0) return 0xff;
    for (const auto& w : voices_)
        if (w->chip.active() && w->chip.phys == v.phys) return w->chip.status();
    for (const auto& t : tails_)
        if (t.active() && t.phys == v.phys) return t.status();
    return 0xff;
}

ToneModule50::VoiceList ToneModule50::part_voices(const midi::Part& part) const {
    // The part's voice list: voices join it when taken and leave it
    // when freed, so it runs in allocation order.
    return VoiceList(*this, [&](std::vector<Voice*>& out) {
        for (const auto& vp : voices_)
            if (vp->in_use && !vp->queued && vp->part == &part) out.push_back(vp.get());
        std::sort(out.begin(), out.end(), [](const Voice* a, const Voice* b) { return a->alloc_seq < b->alloc_seq; });
    });
}

ToneModule50::VoiceList ToneModule50::group_of(Voice& first) const {
    // The note's element ring (v[0x7d],: in allocation order), from `first`.
    return VoiceList(*this, [&](std::vector<Voice*>& g) {
        for (const auto& vp : voices_)
            if (vp->in_use && !vp->queued && vp->part == first.part && vp->serial == first.serial)
                g.push_back(vp.get());
        std::sort(g.begin(), g.end(), [](const Voice* a, const Voice* b) { return a->alloc_seq < b->alloc_seq; });
        const auto it = std::find(g.begin(), g.end(), &first);
        if (it != g.end()) std::rotate(g.begin(), it, g.end());
    });
}

void ToneModule50::key_off(Voice& v) {
    // sostenuto-captured notes wait for the pedal.
    if (v.sostenuto != 0) {
        v.sostenuto = 3;
        return;
    }
    release(v);
}

void ToneModule50::note_off(const midi::Part& part, std::uint8_t note) {
    apply_touch();
    const midi::xg::MultiPart& ram = *part.ram;
    PartState& ps = parts_[part.id % parts_.size()];
    if (drum_mode(part) == 0 && ram.mode == 0) {
        // Mono note-off.
        auto* end = ps.held.data() + ps.held_count;
        auto* it = std::find(ps.held.data(), end, note);
        if (it != end) {
            std::copy(it + 1, end, it);
            --ps.held_count;
        }
        if (ps.held_count > 0 && note == ps.mono_key) {
            const std::uint8_t top = *std::max_element(ps.held.data(), ps.held.data() + ps.held_count);
            bool keyed = false;
            for (auto& vp : voices_)
                if (vp->in_use && vp->part == &part && vp->keyed) keyed = true;
            if (ram.portamento_switch && !keyed) return;
            for (auto& vp : voices_)
                if (vp->in_use && vp->part == &part && vp->keyed) release(*vp, kCutRelease);
            ps.mono_key = top;
            play(part, top, top, ps.mono_vel, ++serial_);
            key_on_flush();
            return;
        }
        if (ps.held_count > 0) return;
    }
    // Key-off: the first keyed note of the key in the
    // part's voice list (allocation order), passing over notes already waiting for the
    // sostenuto pedal (or, with the sustain pedal down, for the sustain pedal).
    const bool sus = part.controller.sustain;
    for (Voice* vp : part_voices(part)) {
        Voice& v = *vp;
        if (v.note != note || !v.keyed || v.detached) continue;
        if (v.kind != 0 && !v.rcv_note_off) continue;
        if (!sus ? v.sostenuto == 3 : !((v.sostenuto != 3 && !v.sus_pending) || v.sostenuto == 1))
            continue;
        const std::uint32_t s = v.serial;
        for (auto& wp : voices_) {
            Voice& w = *wp;
            if (!w.in_use || w.serial != s || !w.keyed) continue;
            if (part.controller.sustain) {
                w.sus_pending = true;
                if (w.sostenuto == 1) w.sostenuto = 3;
            } else {
                key_off(w);
            }
        }
        return;
    }
}

void ToneModule50::sustain_changed(const midi::Part& part) {
    apply_touch();
    // the pedal catches the released poly voices of the part —
    // down: their release rate v[0x66] becomes at most the D2 rate; up: the element's
    // release rate again. The level round robin writes it to the chip (update()).
    for (Voice* vp : part_voices(part)) {
        Voice& v = *vp;
        if (!v.damper || v.keyed) continue;
        if (part.controller.sustain) v.rr66 = std::min(v.rr66, v.d2r);
        else v.rr66 = v.rr = v.rr_normal;
    }
    if (part.controller.sustain) return;
    for (auto& vp : voices_) {
        Voice& v = *vp;
        if (!v.in_use || v.part != &part || !v.keyed || !v.sus_pending) continue;
        v.sus_pending = false;
        if (v.sostenuto == 0) release(v);
    }
    if (part.ram->mode != 0) parts_[part.id % parts_.size()].held_count = 0;
}

void ToneModule50::sostenuto_changed(const midi::Part& part) {
    apply_touch();
    const bool on = part.controller.sostenuto;
    for (auto& vp : voices_) {
        Voice& v = *vp;
        if (!v.in_use || v.part != &part) continue;
        if (on) {
            if (v.keyed && !v.sus_pending) v.sostenuto |= 1;
            continue;
        }
        const bool waiting = v.sostenuto == 3 && v.keyed;
        v.sostenuto = 0;
        if (!waiting) continue;
        if (part.controller.sustain) v.sus_pending = true;
        else release(v);
    }
}

void ToneModule50::release_all(const midi::Part& part) {
    apply_touch();
    for (auto& vp : voices_) {
        Voice& v = *vp;
        if (!v.in_use || v.part != &part || !v.keyed) continue;
        v.sostenuto = 0;
        if (v.kind == 0 || v.rcv_note_off) release(v);
    }
    parts_[part.id % parts_.size()].held_count = 0;
}

void ToneModule50::sound_off(const midi::Part& part) {
    apply_touch();
    // All sound off (CC120): every voice in the part's list is damped; the AEG monitor then frees it. The mono key list and the portamento source stay.
    for (Voice* vp : part_voices(part)) damp(*vp);
}

void ToneModule50::kill_all(const midi::Part& part) {
    // (mono / poly, part mode, variation connection / insertion-part writes):
    // the voices as all sound off, then the part's held-key count and mono flags
    // cleared and the portamento source set to 60 [code, meas].
    sound_off(part);
    PartState& ps = parts_[part.id % parts_.size()];
    ps.held_count = 0;
    ps.last_key = 60;
}

void ToneModule50::eg_time_changed(const midi::Part& part, bool attack) {
    // CC73 / 08 nn 1A and CC72 / 08 nn 1C
    // walk the part's voice list [code, meas]:
    // - attack: the voice's attack register is taken again (`v[0x67]`, at most 0x7c; the chip
    //   reads it only at the key-on, so this reaches voices whose delayed key-on is pending),
    //   and a filter EG in its attack stage takes the new rate;
    // - release: the release rate is computed again with the new part release time (also for
    //   released, cut or damped voices: the level round robin writes it at the voice's next
    //   turn), and a filter EG in its release stage takes the new rate.
    apply_touch();
    const midi::xg::MultiPart& ram = *part.ram;
    for (Voice* vp : part_voices(part)) {
        Voice& v = *vp;
        if (v.kind == 1) continue;
        if (attack) {
            if (v.key_delay > 0) {
                const int pa = ram.eg_attack_time - 64;
                int r = pa <= 0 ? std::min(63, aeg_rate(v, 13, 69) - law::floor_div(pa, 2))
                                : std::min<int>(aeg_rate(v, 13, 69), tb_.kPartAttackRate[static_cast<std::size_t>(pa)]);
                if (r == 0) r = 4;
                const int arr = std::min(2 * r, 0x7c);
                v.chip.set_attack(arr | 0x80, 0xff);
            }
            if (v.feg_stage == 0) feg_set(v, 1);
        } else {
            v.rr = v.rr_normal = v.rr66 = 2 * release_rate(v);
            if (v.feg_stage == 4) feg_set(v, 4);
        }
    }
}

int ToneModule50::aeg_rate(const Voice& v, int drum_byte, int elem_byte) const {
    // The element's AEG rate byte, with the drum setup offset on a drum key (XG mode).
    int r = v.elem[elem_byte];
    if (v.kind == 2 && bank_mode(*ctx_.shared.reset_mode) != BankMode::Gs) {
        if (bank_->mu().on) r = clamp7((v.d[static_cast<std::size_t>(drum_byte)] + 2 * r - 64) >> 1);
        else r = clamp7(r + v.d[static_cast<std::size_t>(drum_byte)] - 64);
    }
    return r;
}

int ToneModule50::release_rate(const Voice& v) const {
    // the element release rate with the part release time, key scaling and
    // (GS, melodic voice of a bank below 64) the program's release offset.
    const std::uint8_t* e = v.elem;
    const midi::Part& part = *v.part;
    const midi::xg::MultiPart& ram = *part.ram;
    const int pr = ram.eg_release_time - 64;
    int release_offset = 1;
    if (bank_mode(*ctx_.shared.reset_mode) == BankMode::Gs && v.kind == 0 && ram.bank_msb < 64)
        release_offset = tb_.kGsReleaseOffset[ram.program_number & 0x7Fu];
    const int ks = law::aeg_key_scaling(v.note, e[67], e[66]);
    int rr;
    if (pr <= 0) rr = e[72] <= 55 ? std::min(55, e[72] - law::floor_div(pr, 2)) : e[72];
    else rr = std::min<int>(e[72], tb_.kPartReleaseRate[static_cast<std::size_t>(pr + release_offset)]);
    rr += ks;
    return rr <= 0 ? 1 : std::min(rr, 63);
}

void ToneModule50::reset() {
    // A reset: every register set is damped and freed (its chip
    // voice fades on), the free-voice FIFO restarts at 0, 1, …
    // and every part's note state is cleared [code, meas].
    for (auto& vp : voices_) {
        Voice& v = *vp;
        if (v.in_use && v.chip.active()) detach_tail(v);
        v.in_use = false;
        v.queued = false;
        v.keyed = false;
        v.phys_due = false;
    }
    free_.clear();
    for (std::size_t i = 0; i < voices_.size(); ++i) free_.push_back(i);
    for (PartState& ps : parts_) {
        ps.held_count = 0;
        ps.last_key = 60;
        ps.mono_key = -1;
        ps.mono_vel = 0;
        ps.mono_serial = 0;
    }
}

// ── control ─────────────────────────────────────────────────────────────────────────

void ToneModule50::commit_cutoff(Voice& v, int ctrl) {
    // (own wave:, glide rate 0x41).
    const midi::Part& part = *v.part;
    const midi::xg::MultiPart& ram = *part.ram;
    // The controller term the glide compares includes the poly-aftertouch filter term.
    if (v.kind == 0 && ram.pat.filter_control != 64 && v.note >= 36 && v.note < 98) {
        const int pat = part.pat_values[v.note];
        if (pat) ctrl += ((ram.pat.filter_control - 64) * pat) / 4;
    }
    // The element cutoff with the FEG is capped at 0xfff before the controller and FMD terms
    // are added.
    int c = std::min(v.cutoff0 + (v.feg >> 2), 0xfff) + ctrl + 2 * v.fmd_centre;
    const int si = c <= 0x800 ? 1 : std::min(c, 0xfff);
    int rate;
    if (v.kind == 1 && v.cg_counter != 0xf0) {
        // Own-wave drum keys: the note-on commits through (0xf0: instant), later
        // ones through at the level round robin, always at rate 0x41.
        rate = 0x41;
    } else if (v.cg_counter == 0xf0) {
        // The note-on's commit (sets 0xf0 around it, then 0xc1).
        rate = 0x7f;
        v.cg_ctrl = ctrl;
    } else if (ctrl != v.cg_ctrl) {
        v.cg_ctrl = ctrl;
        v.cg_counter = 0x81;
        rate = 0x41;
    } else if (v.cg_counter < 0xc0) {
        ++v.cg_counter;
        rate = 0x41;
    } else {
        const int d = std::abs(si - v.cg_last) >> 1;
        rate = d < 0x80 ? tb_.kCutoffGlideRate[static_cast<std::size_t>(d)] : 0x60;
        if (v.cg_counter >= 0xc1) {
            rate = std::max<int>(rate, v.cg_rate);
            --v.cg_counter;
        }
    }
    v.cg_rate = static_cast<std::uint8_t>(rate);
    v.cg_last = si;
    v.chip.set_cutoff_glide(rate);
    v.chip.set_filter(si & 0x7ff, law::part_resonance(v.resonance, ram.filter_resonance) >> 1);
}

void ToneModule50::update(Voice& v, bool tick) {
    const midi::Part& part = *v.part;
    const midi::xg::MultiPart& ram = *part.ram;
    const Ctl& c = ctl(part);
    // Superset part EQ / HPF: live on melodic voices, latched at note-on on drum keys.
    if (v.kind == 0) v.chip.eq.update(eq::voice_settings(ctx_.dll_exact, ctx_.mu_part_eq, part, nullptr, false), ctx_.sample_rate);
    // Pitch: the control layer writes the pitch register only at the note-on and in
    // the 10-ms tick (every voice every tick), so bend, the pitch
    // controllers, fine / coarse tune and master tune reach a sounding voice at the next tick,
    // 0-3 blocks after the event's block [code, meas].
    // Own-wave drum keys have no tick commit; the level round robin commits
    // their pitch and cutoff instead.
    const bool commit = tick || (v.level_due && v.kind == 1);
    if (commit) {
        // GS mode scales the part detune by the key (key = the note [inferred]).
        const int detune = bank_mode(*ctx_.shared.reset_mode) == BankMode::Gs
                               ? law::detune_cents_gs(tb_, ram.get_detune(), v.note)
                               : c.detune;
        int cents = v.base_cents + detune + c.pitch + c.bend + v.porta + (v.peg >> 2) + v.sh_cents;
        if (v.kind == 0) {
            // GM2 master fine / coarse tuning: live on sounding
            // melodic notes at their next commit [meas], not on drum keys.
            const midi::xg::System& sys = *ctx_.shared.system;
            cents += sys.master_fine_gm2 + (sys.master_coarse - 64) * 100;
        }
        if (v.kind == 0 && ram.pat.pitch_control != 64 && v.note >= 36 && v.note < 98)
            cents += law::ctrl_pitch(ram.pat.pitch_control, part.pat_values[v.note]);
        v.chip.set_pitch(cents, (v.wave_format & 0xc0) == 0xc0);
    }

    // Level, pan and sends: set at the note-on, then only when the
    // voice's turn comes in the level round robin (see tick()), so volume,
    // expression and master volume changes reach a sounding voice up to 80 ms late [meas].
    if (v.level_due) {
        v.level_due = false;
        // Variation connection: in insertion connection the insertion part's
        // voices go to the variation only; its volume, expression, master and pan apply after
        // the effect (sets its part gain to 0x80, drops its part pan).
        const midi::xg::EffectData& fx = *ctx_.shared.effect1;
        const bool system = fx.variation_connection() == 1;
        // XG2.0 insertion effects (superset, fx_insertion.hpp): a part with insertion blocks
        // renders like the variation insertion part, its dry pair into the chain pre-bus.
        const bool chained = buses_ && buses_->chain_of(part.id) >= 0;
        const bool insertion_part = chained || (!system && part.id == fx.variation_part());

        // Level.
        // A drum level or part gain of 0 gives L = 0 without the controller terms (master
        // volume 0 and expression 0 are silent [meas]).
        int lev = v.level0;
        if (v.kind != 0) lev = v.drum_level == 0 ? 0 : std::max(1, ((v.drum_level + 1) * lev) >> 7);
        const int gain = insertion_part ? 0x80 : c.gain;
        if (lev != 0 && gain != 0) {
            if (!insertion_part) lev = std::max(1, (gain * lev) >> 7);
            if (v.kind == 0) lev += law::amp_term(ram.pat.amplitude_control, part.pat_values[v.note]);
            lev = std::clamp(lev + c.amp, 0, 128);
        } else {
            lev = 0;
        }
        v.level_reg = law::level_register(tb_, lev, v.zone_atten, v.tvel);
        const law::PanAtten pa = v.random_pan       ? law::random_pan(tb_, v.pan)
                                 : insertion_part ? law::element_pan_atten(tb_, v.pan)
                                                  : law::pan_atten(tb_, ram.pan, v.pan);
        // Sends: system connection = the part's; insertion = dry 127 and no variation for the
        // other parts; the insertion part 100 % variation (drum keys with variation send 0
        // bypass the effect and use the part's dry / reverb / chorus).
        int dry = ram.dry_level, rev = ram.reverb_send, cho = ram.chorus_send, var = ram.variation_send;
        if (chained) {
            dry = 127;
            rev = cho = var = 0;
        } else if (!system) {
            if (!insertion_part) {
                dry = 127;
                var = 0;
            } else if (v.kind == 0 || v.drum_sends[2] != 0) {
                dry = rev = cho = 0;
                var = 127;
            } else {
                var = 0;
            }
        }
        if (v.kind != 0 && !chained) {
            rev = (rev * v.drum_sends[0]) >> 7;
            cho = (cho * v.drum_sends[1]) >> 7;
            if (!insertion_part) var = (var * v.drum_sends[2]) >> 7;
        }
        // Centre-pan attenuation of the mono-input chorus / variation (engine[0x66dd] /
        // engine[0x66dc]; the variation term only in system connection).
        const int pan_term = tb_.kChorusPanAtten[static_cast<std::size_t>(v.pan_send)];
        const int cho_reg = std::min(0xff, law::send_atten(tb_, cho) + (fx::chorus_stereo_input(fx) ? 0 : pan_term));
        const int var_reg = std::min(
            0xff, law::send_atten(tb_, var) + (system && !fx::variation_stereo_input(ctx_.tables->fx, fx) ? pan_term : 0));
        v.chip.set_levels(v.level_reg, pa.left, pa.right, law::send_atten(tb_, dry), law::send_atten(tb_, rev),
                          cho_reg, var_reg);

        // LFO (rate, depths). The control layer writes the LFO registers (0x480 speed / wave,
        // 0x481 PMD, 0x441 AMD / FMD) only at the note-on and here, in the level round robin, so
        // the element fade-in steps, controller depth changes (MW, …) and the part vibrato rate
        // reach a sounding voice at its next round-robin turn, up to 80 ms late [code, meas].
        if (v.lfo_on) set_lfo(v, c);
        // A released voice gets its release rate v[0x66] again, from the decay table
        // (0x500 without bit 7).
        if (v.stage == 1) v.chip.set_decay(v.rr66, 0xff);
    }

    // Cutoff: like the pitch, written only at the note-on and in the 10-ms tick (FEG step, controller term), so CC74, the MW / AC filter controls etc. reach a sounding
    // voice at the next tick [code, meas].
    if (commit) commit_cutoff(v, c.cutoff);
}

void ToneModule50::set_lfo(Voice& v, const Ctl& c) {
    const midi::Part& part = *v.part;
    const midi::xg::MultiPart& ram = *part.ram;
    const std::uint8_t* e = v.elem;
    // Rate: the element speed (byte 7), clamped by the part vibrato rate
    // (from the voice's rate base, 14 in XG) when that is not 64; live.
    int rate = e[7];
    const int pvr = ram.vibrato_rate & 0x7f;
    if (pvr != 64) {
        const int t = tb_.kVibratoRate[static_cast<std::size_t>(v.lfo_rate_base + pvr)];
        rate = pvr > 64 ? std::max(rate, t) : std::min(rate, t);
    }
    const int speed = tb_.kLfoSpeed[static_cast<std::size_t>(rate & 63)];
    int pc = c.pmd;
    if (v.kind == 0 && v.note >= 36 && v.note < 98)
        pc = std::min(127, pc + part.pat_values[v.note] * ram.pat.lfo_pmod_depth / 128);
    // GS mode has its own controller PMD table (engine[0x63ae] ≠ 1).
    const auto& ctrl_pmd = bank_mode(*ctx_.shared.reset_mode) == BankMode::Gs ? tb_.kCtrlPmdGs : tb_.kCtrlPmdXg;
    const int pmd = std::max<int>(ctrl_pmd[static_cast<std::size_t>(pc)],
                                      tb_.kVibratoPmd[static_cast<std::size_t>(std::min(v.vib, 127))]);
    const int amd = std::max(c.amd, static_cast<int>(v.elem[12]));
    const int s = c.fmd <= 0 ? 0 : (c.fmd == 1 ? 4 : ((std::min(c.fmd, 22) + 1) >> 1) + 4);
    int fmd;
    if (s < v.elem[11]) {
        fmd = ((tb_.kFmdTable[v.elem[11] & 15] << 1) & 0xe0) >> 5;
        v.fmd_centre = 0;
    } else {
        const int reg = std::min(tb_.kFmdTable[static_cast<std::size_t>(std::min(s, 15))] << 3, 0xe0);
        fmd = reg >> 5;
        v.fmd_centre = reg;
    }
    v.chip.set_lfo(std::min(amd, 31), fmd, speed, v.lfo_tri, pmd);
}

void ToneModule50::apply_touch() {
    // The part messages since the last call mark the voices in the part's list
    // (flags |= 7). Called before every voice event and tick, so a message that
    // came before a cut still marks the cut voice, as in the DLL.
    for (auto& vp : voices_) {
        Voice& v = *vp;
        if (!v.in_use || v.queued || v.part->touch == v.touch_seen) continue;
        v.touch_seen = v.part->touch;
        v.flag0 = v.flag12 = true;
    }
}

void ToneModule50::monitor(Voice& v) {
    // the chip's AEG status (0xff once the chip
    // voice has ended) gives a score `0xfc − 4·att` that, in decay / release, also counts the
    // velocity attenuation v[0x71] (at most 0xc0) and becomes the stealing score v[0x6d].
    const int st = phys_status(v);
    const int hi = st & 0xc0;
    int score = ~(st << 2) & 0xfc;
    if (st & 0x80) {
        const int t = std::min(2 * v.tvel, 0xc0);
        score = score < t ? 0 : score - t;
        v.score = score;
    }
    // Own-wave drum keys that ignore note-offs (v[0x87] = the drum record's receive note off) have their own thresholds.
    const bool own = v.kind == 1 && !v.rcv_note_off;
    const auto end = [&] {
        // The voice is freed: register set to the FIFO's tail, its chip voice plays on.
        v.score = 0;
        v.auto_release = 0;
        free_voice(v);
    };
    switch (v.stage) {
    case 1: // released: below 0x20, 0x80 once the release target is reached (or ended)
        if (score < (own ? 0x70 : (hi == 0xc0 ? 0x80 : 0x20)) && !v.hold) end();
        break;
    case 6: // key-off before the delayed key-on
        end();
        break;
    case 3:
    case 5: // D1 target reached: D2
        if (hi == 0xc0) {
            v.chip.set_decay(v.d2r, v.d2l);
            v.stage = 4;
        }
        break;
    case 4: // held in D2
        if (score < (own ? (v.auto_release != 0 ? 0x70 : 0x80) : 0x20) && !v.hold) end();
        break;
    default: break;
    }
}

void ToneModule50::lfo_tick(Voice& v) {
    // (every 20 ms).
    if (!v.lfo_on) return;
    // The fade: after the delay, a step per LFO tick to the target, each marking the voice's
    // LFO registers for its next level round-robin turn (flag bit 0); the first step comes
    // even when the target is already reached.
    if (v.lfo_state == 0 && v.lfo_delay > 0) {
        --v.lfo_delay;
    } else if (v.lfo_state != 2) {
        v.vib += v.vib_step;
        v.lfo_state = 1;
        if (v.vib >= v.vib_target) {
            v.vib = v.vib_target;
            v.lfo_state = 2;
        }
        v.flag0 = true;
    }
    if (v.lfo_sh && --v.sh_count <= 0) {
        const std::uint8_t* e = v.elem;
        v.sh_cents = static_cast<std::int16_t>(e[10] * (static_cast<std::int8_t>(rand()) >> 1)) >> 2;
        v.sh_count = 64 - (e[7] & 63);
        v.flag12 = true;
    }
}

void ToneModule50::tick() {
    // one 10 ms control tick.
    ++ticks_;
    const auto step_to = [](int cur, int tgt, int s) {
        if (s == kInstant) return tgt;
        return cur < tgt ? std::min(tgt, cur + s) : std::max(tgt, cur - s);
    };
    // The key-on delay step, before the voice walk: delayed key-ons.
    for (auto& vp : voices_) {
        Voice& v = *vp;
        if (!v.in_use || v.key_delay <= 0) continue;
        v.key_delay -= 20;
        if (v.key_delay <= 0) {
            v.key_delay = 0;
            v.chip.start(rt_);
            bind_phys(v);
        }
    }
    apply_touch();
    // The level round robin. Four steps per tick, each
    // advancing a cursor mod 32 and refreshing level, pan and sends of the voices with
    // index ≡ cursor (mod 32), so each voice every 8 ticks (80 ms) at a phase set by its
    // index. The cursor runs from power-on [code, meas].
    for (int step = 0; step < 4; ++step) {
        level_cursor_ = (level_cursor_ + 1) & 31;
        for (auto& vp : voices_)
            if (vp->in_use && (vp->index & 31) == level_cursor_ && vp->flag0) {
                // Only for voices with flag bit 0 set, which it clears; before
                // the AEG monitor and the voice walk of the same tick.
                vp->level_due = true;
                vp->flag0 = false;
                update(*vp, false);
            }
    }
    // The AEG monitor: eight steps per tick of
    // its own cursor (zeroed at power-on, advanced before
    // use), so voice c (mod 32) on the ticks n with (n − 1) mod 4 = ((c − 1) mod 32) / 8: the
    // D1 → D2 switch, the voice's stealing score and the end of the voice [code].
    // The order matters: the voices it frees join the FIFO in cursor order.
    for (std::uint32_t step = 0; step < 8; ++step) {
        const std::uint32_t cur = (8 * (ticks_ - 1) + step + 1) & 31;
        for (std::size_t i = cur; i < voices_.size(); i += 32) {
            Voice& v = *voices_[i];
            if (v.in_use && v.key_delay == 0) monitor(v);
        }
    }
    for (auto& vp : voices_) {
        Voice& v = *vp;
        if (!v.in_use) continue;
        // The voice walk steps its cursors from power-on (31,
        // so the first tick starts at 0): per iteration (16 per tick) the LFO step one cursor
        // step, then the pitch / filter step two, each covering the four voices ≡ cursor (mod 32); the
        // auto-release countdown eight per tick [code].
        const std::uint32_t cur = static_cast<std::uint32_t>(v.index & 31);
        // The LFO step (every 2nd tick): LFO delay, fade, sample and hold — voices 0-15 (mod 32)
        // on the odd ticks, 16-31 on the even ones, at iteration cur mod 16; the pitch / filter step reaches
        // the voice at iteration cur / 2, so the LFO step comes first for cursors 0 and 16-31.
        const bool lfo_now = ((ticks_ - 1) & 1) == (cur >> 4);
        const bool lfo_first = (cur & 15) <= cur / 2;
        if (lfo_now && lfo_first) lfo_tick(v);
        // The pitch / filter step (every tick, not on own-wave drum keys): portamento, PEG,
        // FEG; pitch and cutoff are committed only while one of them moves or the voice's
        // update flags are set (bits 1-2 cleared here, bit 0 by its level round-robin turn).
        bool moving = false;
        if (v.kind != 1) {
            moving = v.porta != v.porta_tgt || (v.peg_stage != 3 && v.peg_stage != 5) ||
                     (v.feg_stage != 3 && v.feg_stage != 5);
            if (v.porta != v.porta_tgt) {
                const int t = v.part->ram->portamento_time & 0x7f;
                const int tbl = tb_.kPortaRate[static_cast<std::size_t>(t)];
                if (t == 0) {
                    v.porta = v.porta_tgt;
                } else if (t < 24) {
                    v.porta = step_to(v.porta, v.porta_tgt, 2 * tbl);
                } else {
                    // 8.8 fixed step.
                    const int sign = v.porta < v.porta_tgt ? 1 : -1;
                    v.porta_frac += sign * 2 * (tbl & 0xff);
                    v.porta += sign * 2 * (tbl >> 8);
                    const int cc = static_cast<std::int16_t>(v.porta_frac) / 256;
                    if (cc != 0) {
                        v.porta += cc;
                        v.porta_frac &= 0xff;
                    }
                    if (sign > 0 ? v.porta >= v.porta_tgt : v.porta <= v.porta_tgt)
                        v.porta = v.porta_tgt;
                }
            }
            // step; on reaching (or passing) the target, or at
            // once for an instant step, advance. Stages 3 (hold) and 5 (released, done) rest.
            const auto reached = [](int& at, int tgt, int step) {
                if (step == kInstant) return true;
                at += step;
                return step < 0 ? !(tgt < at) : !(at < tgt);
            };
            if (v.peg_stage != 3 && v.peg_stage != 5 && reached(v.peg, v.peg_tgt, v.peg_step))
                peg_advance(v);
            if (v.feg_stage != 3 && v.feg_stage != 5 && reached(v.feg, v.feg_tgt, v.feg_step))
                feg_advance(v);
        }
        const bool commit = v.kind != 1 && (moving || v.flag0 || v.flag12);
        if (commit) v.flag12 = false;
        update(v, commit);
        if (lfo_now && !lfo_first) lfo_tick(v);
    }
    // The auto-release step (every 4th tick: voices 8k…8k+7 on the ticks n ≡ k + 1
    // (mod 4), in cursor order): the drum auto-release countdown. At 0 the register set is
    // freed (to the FIFO's tail) while the voice stays busy (state 4, or 5 before
    // its D2) until the AEG monitor ends it.
    for (std::uint32_t step = 0; step < 8; ++step) {
        const std::uint32_t cur = 8 * ((ticks_ - 1) & 3) + step;
        for (std::size_t i = cur; i < voices_.size(); i += 32) {
            Voice& v = *voices_[i];
            if (!v.in_use || v.key_delay != 0 || v.auto_release <= 0 || --v.auto_release != 0)
                continue;
            v.detached = true;
            v.stage = v.stage == 3 ? 5 : 4;
            v.score = 0;
            enqueue(v);
        }
    }
}

void ToneModule50::render(Buses& buses) {
    ++block_;
    buses_ = &buses;
    // per 128-sample block the DLL runs the block's MIDI events, then the 10 ms
    // ticks due by the block's end, then renders it. The block's events have already been
    // applied (the synth applies them at the block's start, as the DLL), so run those ticks
    // here [meas: element key-on delay, whole-song correlation at lag 0].
    const auto ms = static_cast<std::uint64_t>(static_cast<double>(samples_ + kBlock) * 1000.0 /
                                               static_cast<double>(ctx_.sample_rate));
    while (ms >= ms_done_ + 10) {
        ms_done_ += 10;
        tick();
    }
    // The voices in batches (docs/ACCELERATION.md, "Control across voices"): the batch's control
    // (accel::control_50 where a target has it, else voice by voice), each voice's samples, the filter
    // across the batch, then each voice's EQ, fade and mix in voice order (render_end) -- the buses'
    // sums in the same order as one voice at a time. Two batches in flight: a full batch's control is
    // issued, then the batch before it finished, so the scalar work of one overlaps the vector work of
    // the other.
    const auto issue = [&](Batch& bt) {
        if constexpr (accel::kControl50) {
            if (!bt.n) return;
            accel::Lanes50& L = bt.lanes;
            L.base = reinterpret_cast<std::uint8_t*>(&voices_.front()->chip);
            for (std::size_t i = 0; i < bt.n; ++i) bt.voice[i]->chip.to_lanes(L, i);
            L.n = bt.n;
#if defined(HVYXG_CHECK_CONTROL)
            for (std::size_t i = 0; i < bt.n; ++i) bt.check[i] = bt.voice[i]->chip;   // before the kernel writes them
#endif
            accel::control_50(L, ChipVoice::fields_50(), ChipVoice::control_tables(rt_));
        }
    };
    const auto finish = [&](Batch& bt) {
        const std::size_t nb = bt.n;
        if (!nb) return;
        if constexpr (accel::kControl50) {
            for (std::size_t i = 0; i < nb; ++i) {
                bt.voice[i]->chip.from_lanes(bt.lanes, i);
#if defined(HVYXG_CHECK_CONTROL)
                static int reported = 0;
                static long checked = 0;
                if ((++checked & 0xFFFF) == 1) std::fprintf(stderr, "control_50: %ld lanes checked\n", checked);
                bt.check[i].control(rt_);
                if (reported < 20 && bt.voice[i]->chip.differs_from(bt.check[i], "control_50")) ++reported;
#endif
            }
        } else {
            for (std::size_t i = 0; i < nb; ++i) bt.voice[i]->chip.control(rt_);
        }
        for (std::size_t i = 0; i < nb; ++i) bt.voice[i]->chip.signal(rt_, ctx_.interpolation, bt.rows[i].data(), bt.svf[i]);
        std::array<float*, accel::kBatch> band{}, low{};
        std::array<float, accel::kBatch> f{}, df{}, q{};
        for (std::size_t i = 0; i < nb; ++i) {
            band[i] = bt.svf[i].band, low[i] = bt.svf[i].low;
            f[i] = bt.svf[i].f, df[i] = bt.svf[i].df, q[i] = bt.svf[i].q;
        }
        accel::svf50_lanes(bt.rows[0].data(), kBlock, band.data(), low.data(), f.data(), df.data(), q.data(), nb, kBlock);
        for (std::size_t i = 0; i < nb; ++i) bt.voice[i]->chip.render_end(buses, bt.rows[i].data());
        bt.n = 0;
    };
    Batch* cur = batch_[0].get();
    Batch* prev = batch_[1].get();
    cur->n = prev->n = 0;
    for (auto& vp : voices_) {
        Voice& v = *vp;
        // A voice whose chip voice has ended stays busy until the AEG monitor frees it.
        if (!v.in_use || !v.chip.active()) continue;
        update(v, false);
        v.chip.set_chain(buses.chain_of(v.part->id));
        if (!v.chip.begin()) continue;
        cur->voice[cur->n++] = &v;
        if (cur->n == accel::kBatch) {
            issue(*cur);
            finish(*prev);
            std::swap(cur, prev);
        }
    }
    issue(*cur);
    finish(*prev);
    finish(*cur);
    for (auto& t : tails_)
        if (t.active()) t.render(buses, rt_, ctx_.interpolation);
    samples_ += kBlock;
}

} // namespace hvyxg::syxg50
