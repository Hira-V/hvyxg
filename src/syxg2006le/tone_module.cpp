// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "steal_fade.hpp"
#include "syxg2006le/tone_module.hpp"
#if defined(HVYXG_CHECK_CONTROL)
#include <cstdio>
#endif
#include "engine_tables.hpp"

#include <algorithm>

#include "syxg2006le/controllers.hpp"
#include "syxg2006le/programs.hpp"
#include "midi/part.hpp"
#include "syxg2006le/tbl/dll_tables.hpp"
#include "syxg2006le/tbl/param_blocks.hpp"

namespace hvyxg::syxg2006le {

using namespace tbl;

namespace {

/// DSP objects beyond one per handle (the renderer's pool,: 64 objects for 32
/// handles), where removed voices fade out; with a superset steal fade (`steal_fade_ms`), whose
/// longer fades overlap more, at least 64.
std::size_t spare_objects(const ModuleContext& ctx, std::size_t voices) {
    return ctx.steal_fade_ms > 0.0f ? std::max<std::size_t>(voices, 64) : voices;
}

/// Send level → attenuation ([[v]]).
int send_units(const dll::Tables& tb, int v) {
    v &= 0x7F;
    return tb.kSendAtt[static_cast<std::size_t>(v >= 127 ? 255 : 2 * v)];
}

int t128(int v) { return v >= 127 ? 128 : v; }

/// Detune table index: (XG 08 nn 09 << 4) | XG 08 nn 0A. The RAM holds either
/// the two SysEx nibbles or, at its default, the combined byte 0x80.
std::size_t detune_index(const midi::xg::MultiPart& ram) {
    if (ram.detune_msb <= 0x0F) return static_cast<std::size_t>((ram.detune_msb << 4) | (ram.detune_lsb & 0x0F));
    return ram.detune_msb;
}

/// Part detune in cents; voice type 6 scales it by the pitch key ev[1]
/// ((index − 0x80)·T[key] / 4096, towards zero) [meas: MSB 118 piano, detune
/// 0x00 / 0xA0 / 0xFF at six keys, −111 … −123 dB].
int detune_cents(const dll::Tables& tb, const midi::xg::MultiPart& ram, bool type6, std::uint8_t note) {
    const std::size_t idx = detune_index(ram);
    if (!type6) return tb.kDetune[idx];
    const int d = static_cast<int>(idx) - 0x80;
    const int t = d >= 0 ? tb.kDetuneKeyUp[note & 0x7F] : tb.kDetuneKeyDown[note & 0x7F];
    return t * d / 4096;
}

} // namespace

ToneModule2006::ToneModule2006(std::shared_ptr<const BankData> bank, const ModuleContext& ctx)
    : bank_(std::move(bank)), ctx_(ctx), tb_(ctx.tables->le), tables_(tb_, ctx.sample_rate),
      slots_(std::max<std::size_t>(ctx.max_polyphony, 1)),
      live_limit_(std::max<std::size_t>(ctx.max_polyphony, 1)) {
    free_.resize(live_limit_);
    // Note-on scratch and note lists, sized up front so that playing does not allocate.
    prev_voices_.reserve(slots_.size());
    live_serials_.reserve(slots_.size());
    for (PartNotes& pn : notes_) pn.list.reserve(kNoteListReserve);
    for (std::size_t i = 0; i < live_limit_; ++i) push_free(i);
    // Handle i starts on object i; the free-object cursor starts after them.
    objs_.resize(live_limit_ + spare_objects(ctx, live_limit_));
    for (std::size_t i = 0; i < objs_.size(); ++i) objs_[i].dsp.init_object(static_cast<int>(i));
    for (std::size_t i = 0; i < live_limit_; ++i) slots_[i].obj = i;
    cursor_ = live_limit_;
    // seg8 kits as drum-setup objects.
    std::size_t kits = 0;
    for (std::size_t v = 0; v < bank_->voice_count(); ++v) {
        const auto& voice = bank_->voice(v);
        if (voice.is_drum() && voice.drum_kit != 0xFFFF)
            kits = std::max<std::size_t>(kits, voice.drum_kit + 1u);
    }
    kits_.resize(kits);
    for (std::size_t k = 0; k < kits; ++k) {
        const DrumKitSetup img = kit_setup(*bank_, static_cast<std::uint16_t>(k));
        kits_[k] = img;
    }
}

const midi::xg::DrumSetup* ToneModule2006::drum_setup(const midi::Part& part, std::uint8_t note,
                                                      const syxg2006le::tbl::Voice& voice) const {
    if (note < 13 || note > 91 || voice.drum_kit >= kits_.size()) return nullptr;
    const std::size_t i = note - 13u;
    // The all-zero seg8 entries mark notes the kit does not have.
    if (kits_[voice.drum_kit][i].is_empty()) return nullptr;
    const int mode = part.ram->part_mode;
    const auto& ram = *ctx_.shared.drum_setup;
    // Drum-setup RAM: part mode n ≥ 2 = drum setup n − 1. Other parts playing a kit use their
    // own copy (measured: a drum NRPN on a normal-mode part edits only that part).
    if (mode >= 2 && static_cast<std::size_t>(mode - 2) < ram.size())
        return &ram[static_cast<std::size_t>(mode - 2)][i];
    if (part.drum_setup_program.valid && part.drum_setup_program == part.program)
        return &part.drum_setup[i];
    return &kits_[voice.drum_kit][i];
}

int ToneModule2006::pedal(const midi::Part& part) {
    const int v = part.controller.cc_values[64];
    return part.controller.sustain ? std::max(v, 64) : std::min(v, 63);
}

void ToneModule2006::push_free(std::size_t index) {
    free_[(free_head_ + free_count_) % free_.size()] = static_cast<std::uint32_t>(index);
    ++free_count_;
    slots_[index].queued = true;
    slots_[index].allocated = false;
}

ToneModule2006::Slot& ToneModule2006::allocate() {
    if (free_count_ > 0) {
        Slot& s = slots_[free_[free_head_]];
        free_head_ = (free_head_ + 1) % free_.size();
        --free_count_;
        s.queued = false;
        s.allocated = true;
        s.score = 1;
        s.alloc_seq = ++alloc_seq_;
        kill_handle(s, 0x7E, true); // mode 1 (nothing while the object is idle)
        return s;
    }
    // Steal (strategies by the attempt count):
    // first the released voice (record flags & 0x88 = 0x80) with the highest non-zero score,
    // ties to the lowest index; once that has failed in this note-on, any voice
    // with the highest score, ties to the highest index. Scores are those of
    // the last refresh (1 for voices allocated since).
    Slot* victim = nullptr;
    if (!steal_any_) {
        int best = 0;
        for (std::size_t i = 0; i < live_limit_; ++i) {
            Slot& s = slots_[i];
            if (!s.allocated || (s.vflags & 0x88) != 0x80) continue;
            if (s.score > best) {
                best = s.score;
                victim = &s;
            }
        }
        if (!victim) steal_any_ = true;
    }
    if (!victim) {
        int best = -1;
        for (std::size_t i = 0; i < live_limit_; ++i) {
            Slot& s = slots_[i];
            if (!s.allocated) continue;
            if (s.score >= best) {
                best = s.score;
                victim = &s;
            }
        }
    }
    if (!victim) victim = &slots_.front();
    // The entry goes to the new note; the stolen sound fades out at release rate 0x7e
    // (mode 1).
    kill_handle(*victim, 0x7E, true);
    victim->allocated = true;
    victim->queued = false;
    victim->score = 1;
    victim->alloc_seq = ++alloc_seq_;
    return *victim;
}

void ToneModule2006::kill_handle(Slot& s, int rate, bool steal) {
    Obj& o = objs_[s.obj];
    if (o.dsp.score_state() == 3) return; // idle or already killed
    if (o.dsp.waiting() || o.dsp.ended()) {
        o.dsp.stop();
        o.busy = false;
    } else {
        o.dying = true;
        o.info = s;
        const auto fade = steal ? steal_fade_samples(ctx_.steal_fade_ms, ctx_.sample_rate) : std::uint32_t{0};
        if (fade) o.dsp.start_steal_fade(fade);
        else o.dsp.kill(tables_, rate);
    }
    for (std::size_t k = 0; k < objs_.size(); ++k) {
        const std::size_t i = cursor_;
        cursor_ = (cursor_ + 1) % objs_.size();
        if (!objs_[i].busy) {
            s.obj = i;
            return;
        }
    }
    o.dsp.stop();
    o.busy = o.dying = false;
}

void ToneModule2006::start_voice(Slot& s, const ElementData& el) {
    Obj& o = objs_[s.obj];
    o.busy = true;
    o.dying = false;
    o.owner = static_cast<std::size_t>(&s - slots_.data());
    o.dsp.start(tables_, s.params, el, bank_->wave(), controls(s));
}

void ToneModule2006::remove_note(Slot& s, int rate) {
    kill_handle(s, rate, false);
    s.key_released = true;
    s.sustained = s.sostenuto = s.off_pending = false;
    if (s.allocated) push_free(static_cast<std::size_t>(&s - slots_.data()));
}

template <class Pred> void ToneModule2006::remove_voices(Pred pred, int rate) {
    std::array<Slot*, 256> v{};
    std::size_t n = 0;
    for (std::size_t i = 0; i < live_limit_ && n < v.size(); ++i)
        if (slots_[i].allocated && pred(slots_[i])) v[n++] = &slots_[i];
    std::sort(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(n), [](const Slot* a, const Slot* b) {
        return a->serial != b->serial ? a->serial < b->serial : a->alloc_seq < b->alloc_seq;
    });
    for (std::size_t i = 0; i < n; ++i) remove_note(*v[i], rate);
}

void ToneModule2006::refresh_scores() {
    // (every 10th controller flush): per entry in index order, the score is the
    // level component's velocity term (≥ 0) plus, outside the key-on delay and the
    // attack, the amp-EG term (the signal peak in level units, 255 once ended). Above the
    // threshold (240, or 224 with every entry in use) the voice is
    // removed (reason 4: rate 0x70) — unless the EG may still rise ("may die"
    // false), which caps the score at the threshold. Otherwise the stored score adds the part
    // volume's attenuation, and a drum voice above 0x60 is marked as a
    // released voice (flags | 0x84).
    for (std::size_t i = 0; i < live_limit_; ++i) {
        Slot& s = slots_[i];
        if (!s.allocated) continue;
        const std::size_t used = live_limit_ - free_count_;
        const int threshold = used >= live_limit_ ? 224 : 240;
        int score = std::max(s.params.velocity_level, 0);
        bool may_die = true;
        const DspVoice& d = dsp(s);
        if (d.score_state() != 0 && d.score_stage() != 0) {
            score += d.score_units();
            may_die = d.may_die(ctx_.sample_rate);
        }
        if (!may_die) score = std::min(score, threshold);
        if (score > threshold) {
            remove_note(s, 0x70);
            s.score = 0xFFFF;
            continue;
        }
        const int vol = s.part->ram->volume;
        const int idx = vol == 0 ? 2 : std::min(2 * vol, 255);
        s.score = score + tb_.kVolumeAtt[static_cast<std::size_t>(idx)];
        if (score > 0x60 && s.drum) s.vflags |= 0x84;
    }
}

void ToneModule2006::track(const midi::Part& part) {
    const std::size_t id = part.id % parts_.size();
    if (parts_[id] == &part) return;
    parts_[id] = &part;
    sources_[id] = controller_sources(part);
    eg_times_[id] = {part.ram->eg_attack_time, part.ram->eg_decay_time, part.ram->eg_release_time};
}

void ToneModule2006::xg_eg_changes() {
    // An XG attack / decay / release change (CC73 / 75 / 72, 08 nn 1A / 1B / 1C) is re-applied
    // to the part's sounding voices: the attack and the decays from their pre-XG rates; the
    // release only on voices already released, from the current pedal.
    for (std::size_t id = 0; id < parts_.size(); ++id) {
        const midi::Part* part = parts_[id];
        if (!part) continue;
        const std::array<int, 3> now{part->ram->eg_attack_time, part->ram->eg_decay_time, part->ram->eg_release_time};
        const auto was = eg_times_[id];
        if (now == was) continue;
        eg_times_[id] = now;
        for (Slot& s : slots_) {
            if (!s.allocated || s.part != part || !dsp(s).active()) continue;
            DspVoice& d = dsp(s);
            if (now[0] != was[0]) d.set_xg_attack(tables_, xg_attack_rate(tb_, s.params.amp, now[0]));
            if (now[1] != was[1]) {
                AmpEgParams a = s.params.amp;
                apply_xg_decay(a, now[0], now[1]);
                d.set_xg_decay(tables_, a.decay1, a.decay2, a.decay3);
            }
            if (now[2] != was[2] && d.released()) d.set_release_rate(tables_, amp_release_rate(tb_, s.params.amp, *part, s.drum, pedal(*part)));
        }
    }
}

void ToneModule2006::controller_timer() {
    // Once per block, after the block's events and before its voices render (the sample count includes the block about to be rendered): 2 ms ticks by rendered time;
    // every sixth tick flushes the controller sources of every part, so a flush acts on the block that triggers it.
    rendered_ += kBlock;
    const std::uint64_t ms = rendered_ * 1000 / static_cast<std::uint64_t>(ctx_.sample_rate);
    while (ms >= ms_done_ + 2) {
        ms_done_ += 2;
        if (timer_count_ >= 5) {
            timer_count_ = 0;
            // The scheduler list: the score refresh (period 10, registered
            // first) and the parts' controller flushes (period 1).
            if (++flush_count_ >= 10) {
                flush_count_ = 0;
                refresh_scores();
            }
            for (std::size_t i = 0; i < parts_.size(); ++i)
                if (parts_[i]) sources_[i] = controller_sources(*parts_[i]);
        } else {
            ++timer_count_;
        }
    }
}

std::optional<std::size_t> ToneModule2006::current_voice(const midi::Part& part) const {
    if (!part.program.valid) return std::nullopt;
    std::uint8_t msb = part.program.bank_msb;
    if (part.ram->part_mode != 0 && !is_drum_bank(msb)) msb = kDrumBankMsbXg;
    const auto vi = resolve_voice(*bank_, msb, part.program.bank_lsb, part.program.program,
                                  *ctx_.shared.reset_mode);
    if (!vi) return std::nullopt;
    return static_cast<std::size_t>(*vi);
}

bool ToneModule2006::live(std::uint64_t serial) const {
    return std::any_of(slots_.begin(), slots_.end(), [&](const Slot& s) {
        return s.allocated && s.serial == serial;
    });
}

void ToneModule2006::prune(PartNotes& pn) {
    // A note leaves its handler's list when its last voice ends (reason 5),
    // unless its voices were taken from it (clears the count).
    // live() for every note, from one pass over the slots (prune runs at every note-on).
    if (pn.list.empty()) return;
    live_serials_.clear();
    for (const Slot& s : slots_)
        if (s.allocated) live_serials_.push_back(s.serial);
    std::sort(live_serials_.begin(), live_serials_.end());
    std::erase_if(pn.list, [&](const Note& n) {
        return !n.detached && !std::binary_search(live_serials_.begin(), live_serials_.end(), n.serial);
    });
}

void ToneModule2006::detach(Note& n, int rate) {
    // (note method 7, reason 1 →: rate 0x70).
    remove_voices([&](const Slot& s) { return s.serial == n.serial; }, rate);
    n.detached = true;
}

void ToneModule2006::check_mode(const midi::Part& part) {
    // Setting the mono/poly mode (CC126 / CC127 / 08 nn 05) removes every
    // note of the part (reason 0: rate 0x70).
    PartNotes& pn = notes_[part.id % notes_.size()];
    const int mode = part.ram->mode;
    if (pn.mode >= 0 && pn.mode != mode) {
        remove_voices([&](const Slot& s) { return s.part == &part; }, 0x70);
        pn.list.clear();
    }
    pn.mode = mode;
}

void ToneModule2006::portamento_control(const midi::Part& part, std::uint8_t key) {
    // CC84 (note handler method 4): a key joins the pending set; 0x80 clears it.
    track(part);
    PartNotes& pn = notes_[part.id % notes_.size()];
    if (key >= 0x80) {
        pn.cc84.reset();
        pn.cc84_count = 0;
        return;
    }
    pn.cc84.set(key);
    ++pn.cc84_count;
}

void ToneModule2006::note_on(const midi::Part& part, std::uint8_t note, std::uint8_t velocity) {
    track(part);
    check_mode(part);
    const auto& ram = *part.ram;
    if (!part.program.valid || velocity == 0) return;
    // raw key and velocity against the part limits.
    if (note < ram.note_limit_low || note > ram.note_limit_high) return;
    if (velocity < ram.velocity_limit_low || velocity > ram.velocity_limit_high) return;
    const auto vi = current_voice(part);
    if (!vi) return;
    const Voice& voice = bank_->voice(*vi);
    if (!voice.is_drum()) {
        melodic_note_on(part, note, velocity, false);
        return;
    }

    // Drum handler.
    const midi::xg::DrumSetup* drum = drum_setup(part, note, voice);
    if (!drum || !drum->rcv_note_on) return;
    // Retrigger rules: the notes they remove fade out at release
    // rate 0x70.
    const std::uint64_t serial = ++serial_;
    const bool single = ram.key_assign == 0 || (ram.key_assign == 2 && drum->key_assign == 0);
    if (single) {
        remove_voices([&](const Slot& s) { return s.part == &part && s.note == note; }, 0x70);
    } else {
        // Multi: at most four instances of a key; the oldest goes.
        std::array<std::uint64_t, 8> seen{};
        std::size_t n = 0;
        for (const Slot& s : slots_) {
            if (!s.allocated || s.part != &part || s.note != note) continue;
            if (std::find(seen.begin(), seen.begin() + static_cast<std::ptrdiff_t>(n), s.serial) ==
                    seen.begin() + static_cast<std::ptrdiff_t>(n) &&
                n < seen.size())
                seen[n++] = s.serial;
        }
        if (n > 3) {
            const std::uint64_t oldest = *std::min_element(seen.begin(), seen.begin() + static_cast<std::ptrdiff_t>(n));
            remove_voices([&](const Slot& s) { return s.part == &part && s.serial == oldest; }, 0x70);
        }
    }
    if (drum->alternate_group != 0)
        remove_voices([&](const Slot& s) { return s.part == &part && s.drum && s.alt_group == drum->alternate_group; },
                      0x70);

    const std::uint8_t vel1 = sensed_velocity(*bank_, voice, part, velocity);
    const int zone_vel = vel1 - 1;
    bool first = true;
    for (const Group& g : voice.groups) {
        const std::uint8_t zone_key = group_key(tb_, g, note).first;
        for (const Zone& z : g.zones) {
            if (z.condition != 0) continue;
            if (zone_key < z.key_low || zone_key > z.key_high) continue;
            if (zone_vel < z.vel_low || zone_vel > z.vel_high) continue;
            const ElementData* el = bank_->element(z.element);
            if (!el || !el->playable) continue;
            SetupContext c{.tb = &tb_, .bank = bank_.get(), .part = &part, .voice = &voice, .group = &g,
                           .zone = &z, .element = el, .drum = drum, .ev = {}};
            c.ev.note = note;
            c.ev.pitch_key = 60;
            c.ev.cents = 0;
            c.ev.scale_key = static_cast<std::uint8_t>(std::clamp(note + g.transpose, 0, 127));
            c.ev.vel1 = vel1;
            if (first) steal_any_ = false;
            first = false;
            Slot& s = allocate();
            s.params = setup_voice(c);
            s.part = &part;
            s.note = note;
            s.key = note;
            s.pitch_key = 60;
            s.group = &g;
            s.zone = &z;
            s.element = el;
            s.ev = c.ev;
            s.drum = true;
            s.rcv_note_off = drum->rcv_note_off;
            s.alt_group = drum->alternate_group;
            s.random_pan = static_cast<int>(tables_.random() * 128.0) & 0x7F;
            s.sostenuto = false;
            s.off_pending = s.sustained = s.key_released = false;
            s.vflags = 0x10;
            s.serial = serial;
            s.eq_drum = eq::voice_settings(ctx_.dll_exact, ctx_.mu_part_eq, part, drum, true);
            dsp(s).eq.start(s.eq_drum, ctx_.sample_rate);
            start_voice(s, *el);
        }
    }
}

void ToneModule2006::melodic_note_on(const midi::Part& part, std::uint8_t note, std::uint8_t velocity,
                                     bool retrigger) {
    // The melodic note handler: (note-on), (note-on mode),
    // (after the start). `retrigger`: a mono re-trigger from a note-off
    // (sets bit 0, which bypasses the high-note-priority hold).
    const auto& ram = *part.ram;
    if (note < ram.note_limit_low || note > ram.note_limit_high) return;
    if (velocity < ram.velocity_limit_low || velocity > ram.velocity_limit_high) return;
    const auto vi = current_voice(part);
    if (!vi) return;
    const Voice& voice = bank_->voice(*vi);
    if (voice.is_drum()) return;
    PartNotes& pn = notes_[part.id % notes_.size()];
    prune(pn);

    // system transpose + note shift, folded by octaves into 0..127.
    int key = note + (ram.note_shift - 64) + (ctx_.shared.system->transpose - 64);
    while (key > 127) key -= 12;
    while (key < 0) key += 12;
    const std::uint8_t vel1 = sensed_velocity(*bank_, voice, part, velocity);

    // Single key assign: every note of the key goes (handler method 8, reason 2: rate 0x70).
    if (ram.key_assign == 0) {
        for (Note& n : pn.list)
            if (n.note == note) detach(n, 0x70);
        std::erase_if(pn.list, [&](const Note& n) { return n.note == note; });
    }
    // with the sostenuto pedal down, re-striking a latched key latches the new
    // note too.
    const bool sost_latch = part.controller.sostenuto &&
                            std::any_of(pn.list.begin(), pn.list.end(),
                                        [&](const Note& n) { return n.note == note && n.sostenuto; });

    // --- the note-on mode, the previous note and the glide source.
    const bool mono = ram.mode == 0;
    const bool high = part.ram_ext && part.ram_ext->mono_priority != 0;
    const auto valid = [&](const Note& n) { return (!n.released || n.sustained) && n.voice == *vi; };
    const auto latest_valid = [&]() -> int {
        for (int i = static_cast<int>(pn.list.size()) - 1; i >= 0; --i)
            if (valid(pn.list[static_cast<std::size_t>(i)])) return i;
        return -1;
    };
    const auto highest_valid = [&](int exclude) -> int {
        int best = -1, best_key = 0;
        for (int i = static_cast<int>(pn.list.size()) - 1; i >= 0; --i) {
            const Note& n = pn.list[static_cast<std::size_t>(i)];
            if (valid(n) && best_key < n.key && i != exclude) {
                best = i;
                best_key = n.key;
            }
        }
        return best;
    };
    Mode mode = Mode::Normal;
    int prev = -1;
    int src_key = key;
    int time = 0;
    bool cc84 = false, silent = false;
    std::uint8_t ev_velocity = velocity, ev_vel1 = vel1;
    if (pn.cc84_count > 0) {
        // the lowest pending CC84 key that still sounds (first unreleased note
        // of that key, same program); keys that do not are dropped.
        int found = -1;
        for (;;) {
            int b = -1;
            for (int k = 0; k < 128; ++k)
                if (pn.cc84.test(static_cast<std::size_t>(k))) {
                    b = k;
                    break;
                }
            if (b < 0) {
                pn.cc84_count = 0;
                break;
            }
            int cand = -1;
            for (std::size_t i = 0; i < pn.list.size(); ++i)
                if (!pn.list[i].released && pn.list[i].note == b) {
                    cand = static_cast<int>(i);
                    break;
                }
            if (cand >= 0 && pn.list[static_cast<std::size_t>(cand)].voice == *vi) {
                found = cand;
                break;
            }
            pn.cc84.reset(static_cast<std::size_t>(b));
            if (--pn.cc84_count == 0) break;
        }
        if (found >= 0) {
            // The glide time is forced to 0; within an octave the note takes the source note's
            // voices over (mode 7), else it starts legato from them.
            cc84 = true;
            prev = found;
            const Note& p = pn.list[static_cast<std::size_t>(found)];
            src_key = p.key;
            ev_velocity = p.velocity;
            ev_vel1 = p.vel1;
            mode = key < p.key ? Mode::LegatoDown : Mode::Legato;
            if (std::abs(key - p.key) < 12) mode = Mode::Takeover;
        } else if (mono) {
            prev = latest_valid();
        } else {
            pn.cc84.reset();
            pn.cc84_count = 0;
        }
    } else if (mono) {
        // Priority (XG 0A nn 01): the last note, or the highest held note; a lower note under
        // high-note priority only joins the held keys.
        prev = high ? highest_valid(-1) : latest_valid();
        if (high && prev >= 0 && key < pn.list[static_cast<std::size_t>(prev)].key && !retrigger) {
            silent = true;
        } else if (ram.portamento_switch) {
            time = ram.portamento_time;
            if (prev >= 0) {
                const Note& p = pn.list[static_cast<std::size_t>(prev)];
                src_key = p.key;
                ev_velocity = p.velocity;
                ev_vel1 = p.vel1;
                mode = key < src_key ? Mode::LegatoDown : Mode::Legato;
            } else {
                src_key = pn.last_key;
                mode = key < src_key ? Mode::FromLastDown : Mode::FromLast;
            }
        }
    } else if (ram.portamento_switch) {
        time = ram.portamento_time;
        src_key = pn.last_key;
        mode = key < src_key ? Mode::FromLastDown : Mode::FromLast;
        if (latest_valid() >= 0 && !pn.list.empty()) {
            const int tail = static_cast<int>(pn.list.size()) - 1;
            if (pn.list.back().key == pn.last_key && valid(pn.list.back())) {
                prev = tail;
                mode = key < src_key ? Mode::FromTailDown : Mode::FromTail;
            }
        }
    }

    const std::uint64_t serial = ++serial_;
    const bool damper = voice.damper_mode == 3;
    if (silent) {
        // Note method 2: registered without voices.
        Note n{.serial = serial, .note = note, .key = key, .velocity = velocity, .vel1 = vel1,
               .voice = *vi, .damper = damper};
        n.detached = true;
        n.sostenuto = sost_latch;
        pn.list.push_back(n);
        pn.last_key = key;
        return;
    }

    const std::uint64_t prev_serial = prev >= 0 ? pn.list[static_cast<std::size_t>(prev)].serial : 0;
    bool started = false;
    if (mode == Mode::Takeover) {
        // the new note takes the source note's voices; each is re-pitched to the
        // new key (the pitch component; the glide jumps to 0 at time 0).
        for (Slot& s : slots_) {
            if (!s.allocated || s.serial != prev_serial) continue;
            const auto [pk, pc] = group_key(tb_, *s.group, static_cast<std::uint8_t>(key));
            s.ev.note = note;
            s.ev.pitch_key = pk;
            s.ev.cents = pc;
            SetupContext c{.tb = &tb_, .bank = bank_.get(), .part = &part, .voice = &voice, .group = s.group,
                           .zone = s.zone, .element = s.element, .drum = nullptr, .ev = {}};
            c.ev = s.ev;
            s.params.pitch_cents = setup_voice(c).pitch_cents;
            s.pitch_key = pk;
            s.note = note;
            s.key = key;
            s.serial = serial;
            dsp(s).retarget(s.params.pitch_cents);
            started = true;
        }
    } else {
        // The previous note's voices as they are now (matches them by the group's
        // layer byte; read before the new voices are allocated).
        auto& prev_voices = prev_voices_;  // preallocated: note-on must not allocate
        prev_voices.clear();
        if (prev >= 0 && static_cast<int>(mode) >= 3)
            for (const Slot& s : slots_)
                if (s.allocated && s.serial == prev_serial) prev_voices.emplace_back(s, dsp(s));
        // Modes 2 / 4 / 6 pick their zones with the source key.
        const int match_key =
            (mode == Mode::FromLastDown || mode == Mode::FromTailDown || mode == Mode::LegatoDown) ? src_key : key;
        const int zone_vel = ev_vel1 - 1;
        steal_any_ = false;
        for (const Group& g : voice.groups) {
            const auto [zone_key, zone_cents] = group_key(tb_, g, static_cast<std::uint8_t>(match_key));
            const auto [pitch_key, pitch_cents] = group_key(tb_, g, static_cast<std::uint8_t>(key));
            (void)zone_cents;
            const Slot* pv = nullptr;
            const DspVoice* pd = nullptr;
            for (const auto& [ps, pdsp] : prev_voices)
                if (ps.group && ps.group->layer == g.layer) {
                    pv = &ps;
                    pd = &pdsp;
                    break;
                }
            for (const Zone& z : g.zones) {
                if (z.condition != 0) continue;
                if (zone_key < z.key_low || zone_key > z.key_high) continue;
                if (zone_vel < z.vel_low || zone_vel > z.vel_high) continue;
                const ElementData* el = bank_->element(z.element);
                if (!el || !el->playable) continue;
                const int m = static_cast<int>(mode);
                // Portamento: from the source's
                // group-tracked key, or from the matching voice's pitch key plus its current
                // glide offset; nothing at time 0. A glide down from the matching voice also
                // takes over its context records (the 0x11 words: group, zone,
                // element, sample), so the voice plays the previous voice's element.
                int glide = 0;
                const Group* cg = &g;
                const Zone* cz = &z;
                if (m >= 1 && time > 0) {
                    if (pv && m >= 3)
                        glide = pv->pitch_key * 100 + pd->portamento_cents() - pitch_key * 100;
                    else
                        glide = (group_key(tb_, g, static_cast<std::uint8_t>(src_key)).first - pitch_key) * 100;
                    glide = std::clamp(glide, -12700, 12700);
                    if (pv && m >= 3 && glide > 0 && pv->element) {
                        cg = pv->group;
                        cz = pv->zone;
                        el = pv->element;
                    }
                }
                SetupContext c{.tb = &tb_, .bank = bank_.get(), .part = &part, .voice = &voice, .group = cg,
                               .zone = cz, .element = el, .drum = nullptr, .ev = {}};
                c.ev.note = note;
                c.ev.pitch_key = pitch_key;
                c.ev.cents = pitch_cents;
                c.ev.scale_key = static_cast<std::uint8_t>(std::clamp(key + g.transpose, 0, 127));
                c.ev.vel1 = ev_vel1;

                Slot& s = allocate();
                s.params = setup_voice(c);
                s.part = &part;
                s.note = note;
                s.key = key;
                s.pitch_key = pitch_key;
                s.group = cg;
                s.zone = cz;
                s.element = el;
                s.ev = c.ev;
                s.drum = false;
                s.rcv_note_off = true;
                s.alt_group = 0;
                s.random_pan = static_cast<int>(tables_.random() * 128.0) & 0x7F;
                s.sostenuto = sost_latch;
                s.off_pending = s.sustained = s.key_released = false;
                s.vflags = 0;
                s.serial = serial;
                dsp(s).eq.start(eq::voice_settings(ctx_.dll_exact, ctx_.mu_part_eq, part, nullptr, false), ctx_.sample_rate);
                start_voice(s, *el);
                // Envelope takeover from the matching voice (modes 3–6: pitch / filter EG,
                // 5 / 6: amp EG too).
                if (pv && m >= 3) {
                    if (m >= 5) {
                        dsp(s).take_over_amp(tables_, *pd);
                        s.params.amp = dsp(s).amp_params();
                    }
                    dsp(s).take_over_mod_egs(tables_, *pd);
                }
                if (m >= 1 && time > 0) dsp(s).set_portamento(tables_, glide, 127 - time);
                started = true;
            }
        }
    }
    // A note that started no voice is dropped (fails: no).
    if (!started) return;

    if (cc84 && prev >= 0) {
        Note& p = pn.list[static_cast<std::size_t>(prev)];
        if (pn.cc84.test(p.note)) {
            pn.cc84.reset(p.note);
            --pn.cc84_count;
        }
        detach(p, 0x70);
    }
    if (mono) {
        if (prev >= 0) detach(pn.list[static_cast<std::size_t>(prev)], 0x70);
        // every released note goes (sustained or sostenuto-held ones too).
        for (Note& n : pn.list)
            if (n.released) detach(n, 0x70);
        std::erase_if(pn.list, [](const Note& n) { return n.released; });
    }
    Note n{.serial = serial, .note = note, .key = key, .velocity = ev_velocity, .vel1 = ev_vel1,
           .voice = *vi, .damper = damper};
    if (mode == Mode::Takeover) {
        const auto it = std::find_if(pn.list.begin(), pn.list.end(),
                                     [&](const Note& x) { return x.serial == prev_serial; });
        if (it != pn.list.end()) n.sostenuto = it->sostenuto;
    }
    n.sostenuto = n.sostenuto || sost_latch;
    pn.list.push_back(n);
    pn.last_key = key;
}

void ToneModule2006::release(Slot& s, int pedal_value) {
    // the voice record is marked keyed off (steal candidate) first.
    s.vflags |= 0x80;
    const int rate = amp_release_rate(tb_, s.params.amp, *s.part, s.drum, pedal_value);
    dsp(s).note_off(tables_, rate, pedal_value >= 64);
}

void ToneModule2006::note_off_note(const midi::Part& part, PartNotes& pn, std::size_t i) {
    // Note method 3.
    Note& n = pn.list[i];
    const int p = pedal(part);
    n.released = true;
    const auto voices = [&](auto&& f) {
        for (Slot& s : slots_)
            if (s.allocated && s.serial == n.serial && !s.key_released) f(s);
    };
    if (p >= 64 && n.damper) { // held by the sustain pedal
        n.sustained = true;
        voices([](Slot& s) {
            s.key_released = true;
            s.sustained = true;
        });
        return;
    }
    if (n.sostenuto) {
        voices([](Slot& s) {
            s.key_released = true;
            s.off_pending = true;
        });
        return;
    }
    if (!live(n.serial)) {
        pn.list.erase(pn.list.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
    voices([&](Slot& s) {
        s.key_released = true;
        release(s, p);
    });
}

void ToneModule2006::note_off(const midi::Part& part, std::uint8_t note) {
    PartNotes& pn = notes_[part.id % notes_.size()];
    prune(pn);
    // the first unreleased note of the key.
    const auto first = [&]() -> int {
        for (std::size_t i = 0; i < pn.list.size(); ++i)
            if (!pn.list[i].released && pn.list[i].note == note) return static_cast<int>(i);
        return -1;
    };
    const int i = first();
    if (i >= 0) {
        const std::uint64_t serial = pn.list[static_cast<std::size_t>(i)].serial;
        if (part.ram->mode == 0) {
            // releasing the sounding key of a mono part re-triggers the held key
            // that takes over (last-note priority: the nearest earlier unreleased key; high-note
            // priority: the next highest held key), with the released note's velocity
            const bool high = part.ram_ext && part.ram_ext->mono_priority != 0;
            int target = -1;
            if (!high) {
                if (i == static_cast<int>(pn.list.size()) - 1)
                    for (int j = i - 1; j >= 0; --j)
                        if (!pn.list[static_cast<std::size_t>(j)].released) {
                            target = j;
                            break;
                        }
            } else if (const auto vi = current_voice(part)) {
                const auto highest = [&](int exclude) {
                    int best = -1, best_key = 0;
                    for (int j = static_cast<int>(pn.list.size()) - 1; j >= 0; --j) {
                        const Note& n = pn.list[static_cast<std::size_t>(j)];
                        if ((!n.released || n.sustained) && n.voice == *vi && best_key < n.key && j != exclude) {
                            best = j;
                            best_key = n.key;
                        }
                    }
                    return best;
                };
                if (highest(-1) == i) target = highest(i);
            }
            if (target >= 0) {
                const std::uint8_t key = pn.list[static_cast<std::size_t>(target)].note;
                const std::uint8_t velocity = pn.list[static_cast<std::size_t>(i)].velocity;
                note_off_note(part, pn, static_cast<std::size_t>(target));
                melodic_note_on(part, key, velocity, true);
            }
        }
        for (std::size_t j = 0; j < pn.list.size(); ++j)
            if (pn.list[j].serial == serial) {
                if (!pn.list[j].released) note_off_note(part, pn, j);
                break;
            }
    }

    // Drum notes: the note-off goes to the oldest unreleased note
    // of the key only (a re-struck key keeps its newer note keyed).
    const int p = pedal(part);
    std::uint64_t oldest = 0;
    for (const Slot& s : slots_)
        if (s.allocated && s.part == &part && s.drum && s.note == note && !s.key_released &&
            (oldest == 0 || s.serial < oldest))
            oldest = s.serial;
    for (Slot& s : slots_) {
        if (!s.allocated || s.part != &part || !s.drum || s.note != note || s.key_released ||
            s.serial != oldest)
            continue;
        s.key_released = true;
        // Drum notes without "receive note off" play to their end; their voice
        // records are marked released without a key-off (0x88).
        if (!s.rcv_note_off) {
            s.vflags |= 0x88;
            continue;
        }
        // Damper-mode-3 voices are held by the sustain pedal.
        if (p >= 64 && s.params.amp.damper_mode == 3) {
            s.sustained = true;
            continue;
        }
        if (s.sostenuto) {
            s.off_pending = true;
            continue;
        }
        release(s, p);
    }
}

void ToneModule2006::sustain_changed(const midi::Part& part) {
    // A pedal change re-derives the release rate of the part's released voices from the new
    // pedal value (as for an XG release change).
    const int p = pedal(part);
    for (Slot& s : slots_)
        if (s.allocated && s.part == &part && dsp(s).active() && dsp(s).released())
            dsp(s).set_release_rate(tables_, amp_release_rate(tb_, s.params.amp, part, s.drum, p));
    // Pedal up re-runs the note-off of held notes.
    if (p >= 64) return;
    for (Slot& s : slots_) {
        if (!s.allocated || s.part != &part || !s.sustained) continue;
        s.sustained = false;
        if (s.sostenuto) s.off_pending = true;
        else release(s, p);
    }
    PartNotes& pn = notes_[part.id % notes_.size()];
    for (Note& n : pn.list)
        if (n.sustained) n.sustained = false;
    std::erase_if(pn.list, [&](const Note& n) { return n.released && !n.sostenuto && !live(n.serial); });
}

void ToneModule2006::sostenuto_changed(const midi::Part& part) {
    // note method 6: latch every unreleased note; on release, run the
    // note-off of the released ones again.
    const bool on = part.controller.sostenuto;
    const int p = pedal(part);
    for (Slot& s : slots_) {
        if (!s.allocated || s.part != &part) continue;
        if (on) {
            if (!s.key_released) s.sostenuto = true;
        } else if (s.sostenuto) {
            s.sostenuto = false;
            if (s.off_pending && !s.sustained) {
                if (p >= 64 && s.params.amp.damper_mode == 3) s.sustained = true;
                else release(s, p);
            }
            s.off_pending = false;
        }
    }
    PartNotes& pn = notes_[part.id % notes_.size()];
    for (Note& n : pn.list) {
        if (on) {
            if (!n.released) n.sostenuto = true;
        } else if (n.sostenuto) {
            n.sostenuto = false;
            if (n.released && p >= 64 && n.damper) n.sustained = true;
        }
    }
    if (!on)
        std::erase_if(pn.list, [&](const Note& n) {
            return n.released && !n.sustained && !n.sostenuto && !live(n.serial);
        });
}

void ToneModule2006::release_all(const midi::Part& part) {
    check_mode(part);
    for (Slot& s : slots_) {
        if (!s.allocated || s.part != &part || dsp(s).released()) continue;
        if (s.drum && !s.rcv_note_off) continue;
        s.key_released = true;
        s.sustained = s.sostenuto = s.off_pending = false;
        release(s, 0);
    }
    PartNotes& pn = notes_[part.id % notes_.size()];
    for (Note& n : pn.list) {
        n.released = true;
        n.sustained = n.sostenuto = false;
    }
    std::erase_if(pn.list, [&](const Note& n) { return !live(n.serial); });
}

void ToneModule2006::kill_all(const midi::Part& part) {
    // CC120: every note fades out at release rate 0x70.
    remove_voices([&](const Slot& s) { return s.part == &part; }, 0x70);
    notes_[part.id % notes_.size()].list.clear();
}

ControllerSums ToneModule2006::sums_for(const midi::Part& part, std::uint8_t note, bool drum, bool insertion) const {
    const auto& src = sources_[part.id % sources_.size()];
    if (!in_block_) return controller_sums(tb_, part, src, *ctx_.shared.system, note, drum, insertion, master256(ctx_.shared));
    const int key = (controller_pat(part, note) + 1) << 2 | (drum ? 2 : 0) | (insertion ? 1 : 0);
    auto& ways = sums_memo_[part.id % sums_memo_.size()];
    std::size_t spare = 0;
    for (std::size_t i = 0; i < kSumsWays; ++i) {
        if (ways[i].block == block_ && ways[i].key == key) return ways[i].sums;
        if (ways[i].block != block_) spare = i;
    }
    SumsMemo& m = ways[spare];       // a stale way, else the last (a fifth key in a block recomputes)
    m.block = block_;
    m.key = key;
    m.sums = controller_sums(tb_, part, src, *ctx_.shared.system, note, drum, insertion, master256_);
    return m.sums;
}

VoiceControls ToneModule2006::controls(const Slot& s) const {
    const midi::Part& part = *s.part;
    const auto& ram = *part.ram;
    const VoiceParams& p = s.params;
    // Variation insertion (superset routing, /
    // the DLL leaves its variation inaudible): the insertion part's voices go
    // to the variation only, its volume / expression / master and part pan apply after the
    // effect (fx::SystemFx), drum keys with variation send 0 bypass the effect.
    const midi::xg::EffectData& fx = *ctx_.shared.effect1;
    const bool system_var = fx.variation_connection() == 1;
    // XG2.0 insertion effects (superset, fx_insertion.hpp): a part with insertion blocks
    // renders like the variation insertion part, its dry pair into the chain pre-bus.
    const bool chained = buses_ && buses_->chain_of(part.id) >= 0;
    // DLL-exact: the variation is inert, the insertion part plays like the others.
    const bool insertion_part =
        chained || (!ctx_.dll_exact && !system_var && part.id == fx.variation_part());
    const ControllerSums sums = sums_for(part, s.note, s.drum, insertion_part);
    VoiceControls c;
    // Pitch.
    const int pitch = p.pitch_cents + sums.pitch + detune_cents(tb_, ram, p.amp.type6, static_cast<std::uint8_t>(s.pitch_key));
    c.pitch_cents = std::clamp(pitch, -9600, 9600);
    // Filter.
    c.cutoff_cents = std::clamp(p.filter.cutoff + (p.filter.type == 0 ? sums.filter : 0), 4800, 19200);
    int r = p.filter.resonance;
    const int o = (t128(ram.filter_resonance) - 64) * 124 >> 6;
    r = o < 0 ? r + o : std::max(r, 16 + o);
    c.resonance = std::clamp(r, 16, 140);
    c.pmod = sums.pmod;
    c.fmod = sums.fmod;
    c.amod = sums.amod;
    // Level.
    c.level = std::clamp(p.level + sums.amp, 0, 255);
    // Pan: output modes 0 / 1 / 5. Random-pan voices (part pan 0 or drum pan
    // 0) take P[r] + 16 / P[128 − r] + 16 with r = random & 0x7f and ignore
    // the part pan.
    if (p.random_pan) {
        c.pan_l = std::min(255, pan_attenuation(tb_, s.random_pan) + 16);
        c.pan_r = std::min(255, pan_attenuation(tb_, 128 - s.random_pan) + 16);
    } else if (insertion_part) {
        c.pan_l = std::min(255, pan_attenuation(tb_, p.pan) + 8);
        c.pan_r = std::min(255, 8 + pan_attenuation(tb_, 128 - p.pan));
    } else {
        const int part_pan = std::min<int>(ram.pan, 127);
        c.pan_l = std::min(255, pan_attenuation(tb_, p.pan) + 8 + pan_attenuation(tb_, part_pan));
        c.pan_r = std::min(255, pan_attenuation(tb_, 128 - part_pan) + 8 + pan_attenuation(tb_, 128 - p.pan));
    }
    // Sends: routing mode 0 in system connection, mode 1 (dry fixed at 8, no
    // variation send) for the other parts in insertion connection.
    const int drum_rev = s.drum ? send_units(tb_, p.drum_reverb) : 0;
    const int drum_cho = s.drum ? send_units(tb_, p.drum_chorus) : 0;
    const int drum_var = s.drum ? send_units(tb_, p.drum_variation) : 0;
    c.send[0] = system_var ? 8 + send_units(tb_, ram.dry_level) : 8;
    c.send[1] = std::min(255, drum_rev + send_units(tb_, ram.reverb_send));
    c.send[2] = std::min(255, drum_cho + send_units(tb_, ram.chorus_send));
    c.send[3] = system_var ? std::min(255, drum_var + send_units(tb_, ram.variation_send)) : 255;
    if (chained) {
        c.send = {8, 255, 255, 255};
    } else if (insertion_part) {
        if (!s.drum || p.drum_variation != 0) {
            // The variation output at the dry output's attenuation (8), so that a Thru
            // insertion keeps the part's level (the return applies the part's pan law).
            c.send = {255, 255, 255, 8};
        } else {
            c.send[0] = 8 + send_units(tb_, ram.dry_level);
        }
    }
    return c;
}

void ToneModule2006::render(Buses& buses) {
    buses_ = &buses;
    xg_eg_changes();
    controller_timer();
    for (const midi::Part* part : parts_)
        if (part) check_mode(*part);
    // The block's shared values (sums_for, the EQ memo): from here to the end of the voices nothing a
    // voice reads changes.
    if (++block_ == 0) {
        block_ = 1;
        for (auto& ways : sums_memo_) ways = {};
        eq_memo_ = {};
    }
    master256_ = master256(ctx_.shared);
    in_block_ = true;
    std::array<float*, DspVoice::kOutputs> out = {
        buses.dry_l.data(),    buses.dry_r.data(),    buses.reverb.data(),
        buses.chorus_l.data(), buses.chorus_r.data(), buses.variation_l.data(),
        buses.variation_r.data()};
    // The voices in batches (docs/ACCELERATION.md, "Control across voices"): each voice's begin in
    // order -- the shared random generator's draws in their order, as ever -- then the batch's control
    // (no shared state), each voice's samples, the filter across the batch, then each voice's EQ, fade
    // and outputs in voice order (render_end). Two batches in flight: a full batch's control is issued
    // (issue: a target's kernel, on its vector unit), then the batch before it finished (finish), so the
    // scalar work of one overlaps the vector work of the other without reading what is still in flight.
    // The voices still mix into the buses in voice order.
    const auto issue = [&](Batch& bt) {
        if constexpr (accel::kControlLE) {
            if (!bt.n) return;
            accel::LanesLE& L = bt.lanes;
            L.base = reinterpret_cast<std::uint8_t*>(&objs_.front().dsp);
            for (std::size_t i = 0; i < bt.n; ++i) bt.obj[i]->dsp.to_lanes(L, i, tables_, bt.controls[i]);
            L.n = bt.n;
#if defined(HVYXG_CHECK_CONTROL)
            for (std::size_t i = 0; i < bt.n; ++i) bt.check[i] = bt.obj[i]->dsp;   // before the kernel writes them
#endif
            accel::control_le(L, DspVoice::fields_le(), DspVoice::control_tables(tables_));
        }
    };
    const auto finish = [&](Batch& bt) {
        const std::size_t nb = bt.n;
        if (!nb) return;
        if constexpr (accel::kControlLE) {
            accel::LanesLE& L = bt.lanes;
            // the lanes the kernel kept back, and those it does not take: the reference
#if defined(HVYXG_CHECK_CONTROL)
            {
                static long lanes = 0, kept = 0;
                for (std::size_t i = 0; i < nb; ++i) kept += L.ok[i] != 0;
                if (((lanes += static_cast<long>(nb)) & ~0xFFFFL) != ((lanes - static_cast<long>(nb)) & ~0xFFFFL))
                    std::fprintf(stderr, "control_le: %ld of %ld lanes by the kernel\n", kept, lanes);
            }
#endif
            for (std::size_t i = 0; i < nb; ++i) {
#if defined(HVYXG_CHECK_CONTROL)
                if (L.ok[i]) {
                    static int reported = 0;
                    static long checked = 0;
                    if ((++checked & 0xFFFF) == 1) std::fprintf(stderr, "control_le: %ld lanes checked\n", checked);
                    DspVoice& ref = bt.check[i];
                    ref.control(tables_, bt.controls[i]);
                    bt.obj[i]->dsp.from_lanes(L, i, bt.controls[i]);
                    if (reported < 20 && bt.obj[i]->dsp.differs_from(ref, "control_le")) ++reported;
                    continue;
                }
#endif
                if (L.ok[i]) bt.obj[i]->dsp.from_lanes(L, i, bt.controls[i]);
                else bt.obj[i]->dsp.control(tables_, bt.controls[i]);
            }
        } else {
            for (std::size_t i = 0; i < nb; ++i) bt.obj[i]->dsp.control(tables_, bt.controls[i]);
        }
        for (std::size_t i = 0; i < nb; ++i) bt.obj[i]->dsp.signal(ctx_.interpolation, tables_, bt.rows[i].data(), bt.svf[i]);
        std::array<float*, accel::kBatch> lp{}, bp{};
        std::array<float, accel::kBatch> f{}, df{}, q{};
        for (std::size_t i = 0; i < nb; ++i) {
            lp[i] = bt.svf[i].lp, bp[i] = bt.svf[i].bp;
            f[i] = bt.svf[i].f, df[i] = bt.svf[i].df, q[i] = bt.svf[i].q;
        }
        accel::svf_le_lanes(bt.rows[0].data(), kBlock, lp.data(), bp.data(), f.data(), df.data(), q.data(), nb, kBlock);
        for (std::size_t i = 0; i < nb; ++i) {
            Obj& o = *bt.obj[i];
            o.dsp.render_end(bt.out[i], bt.rows[i].data());
            if (o.dying && !o.dsp.active()) o.busy = o.dying = false;
        }
        bt.n = 0;
    };
    Batch* cur = batch_[0].get();
    Batch* prev = batch_[1].get();
    cur->n = prev->n = 0;
    const auto flush = [&] {
        issue(*cur);
        finish(*prev);
        std::swap(cur, prev);
    };
    for (Obj& o : objs_) {
        if (!o.dsp.active()) {
            // A finished fade frees its object; an ended voice keeps its handle's object busy
            // until the handle lets it go.
            if (o.dying) o.busy = o.dying = false;
            continue;
        }
        const Slot& s = o.dying ? o.info : slots_[o.owner];
        tables_.random(); // the engine advances its LFSR before each voice
        const int chain = buses.chain_of(s.part->id);
        out[0] = chain < 0 ? buses.dry_l.data() : buses.chain_l[static_cast<std::size_t>(chain)].data();
        out[1] = chain < 0 ? buses.dry_r.data() : buses.chain_r[static_cast<std::size_t>(chain)].data();
        // Superset part EQ / HPF: live on melodic voices, latched at note-on on drum notes.
        if (!s.drum) {
            EqMemo& em = eq_memo_[s.part->id % eq_memo_.size()];
            if (em.block != block_) {
                em.block = block_;
                em.settings = eq::voice_settings(ctx_.dll_exact, ctx_.mu_part_eq, *s.part, nullptr, false);
            }
            o.dsp.eq.update(em.settings, ctx_.sample_rate);
        }
        // A portamento time change re-reads the glide rate of the part's voices (event 0x17,
        o.dsp.set_portamento_rate(tables_, 127 - s.part->ram->portamento_time);
        Batch& bt = *cur;
        bt.controls[bt.n] = controls(s);
        if (!o.dsp.begin(tables_)) {
            if (o.dying && !o.dsp.active()) o.busy = o.dying = false;
            continue;
        }
        bt.out[bt.n] = out;
        bt.obj[bt.n++] = &o;
        if (bt.n == accel::kBatch) flush();
    }
    flush();                          // the last batch's control, and the one before it
    finish(*prev);                    // the last
    in_block_ = false;
}

bool ToneModule2006::active() const {
    return std::any_of(objs_.begin(), objs_.end(), [](const Obj& o) { return o.dsp.active(); });
}

std::size_t ToneModule2006::voice_count() const {
    return static_cast<std::size_t>(
        std::count_if(objs_.begin(), objs_.end(), [](const Obj& o) { return o.dsp.active(); }));
}

} // namespace hvyxg::syxg2006le
