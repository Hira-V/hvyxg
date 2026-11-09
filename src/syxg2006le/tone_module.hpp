// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// The S-YXG2006LE tone module: note handling (voice selection, the per-note components,
// pedals, voice allocation) around a pool of DSP voices rendered in 64-sample blocks.

#include <array>
#include <bitset>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "module.hpp"
#include "syxg2006le/bank_data.hpp"
#include "syxg2006le/controllers.hpp"
#include "syxg2006le/dsp_voice.hpp"
#include "syxg2006le/voice_setup.hpp"
#include "midi/xg_ram.hpp"

namespace hvyxg::syxg2006le {

class ToneModule2006 final : public ToneModule {
public:
    ToneModule2006(std::shared_ptr<const BankData> bank, const ModuleContext& ctx);

    std::size_t block_size() const override { return kBlock; }
    void note_on(const midi::Part& part, std::uint8_t note, std::uint8_t velocity) override;
    void note_off(const midi::Part& part, std::uint8_t note) override;
    void sustain_changed(const midi::Part& part) override;
    void sostenuto_changed(const midi::Part& part) override;
    void portamento_control(const midi::Part& part, std::uint8_t key) override;
    void release_all(const midi::Part& part) override;
    void kill_all(const midi::Part& part) override;
    void render(Buses& buses) override;
    bool active() const override;
    std::size_t voice_count() const override;


private:
    /// A voice handle (the voice-manager entry and its voice record; 32 entries)
    /// and the DSP object it currently drives.
    struct Slot {
        /// The pool object the handle drives.
        std::size_t obj = 0;
        VoiceParams params;
        const midi::Part* part = nullptr;
        /// The MIDI note that started the voice (note-off / key assign) and ev[0].
        std::uint8_t note = 0;
        bool drum = false;
        /// Drum notes: receive note off, alternate group.
        bool rcv_note_off = true;
        int alt_group = 0;
        /// Random pan chosen at note-on (random-pan voices).
        int random_pan = 64;
        /// Held by the sostenuto pedal; note-off arrived while held.
        bool sostenuto = false;
        bool off_pending = false;
        /// Note-off arrived with the sustain pedal down (damper-mode-3 voices are held).
        bool sustained = false;
        /// The key has been released (note-off, or a drum note without note-off).
        bool key_released = false;
        /// In the free list.
        bool queued = true;
        /// Holds a voice-manager entry: from allocation until a steal, a note
        /// removal or the score refresh frees it. A voice whose sound has ended keeps its
        /// entry until the next refresh.
        bool allocated = false;
        /// Voice record flags read by the steal: 0x80 keyed off, 0x08 key released
        /// without a key-off (drum keys without note off), 0x04 a quiet drum voice
        /// 0x10 drum.
        std::uint8_t vflags = 0;
        /// Allocation order (a note's voices are removed in this order).
        std::uint64_t alloc_seq = 0;
        /// Note-on serial (all layers of one note share it).
        std::uint64_t serial = 0;
        /// Transposed key (zone match / portamento) and the voice's group-tracked pitch key
        /// (ev[1], the portamento component's key).
        int key = 60;
        int pitch_key = 60;
        /// The records the voice was set up from (re-evaluated by a CC84 takeover).
        const syxg2006le::tbl::Group* group = nullptr;
        const syxg2006le::tbl::Zone* zone = nullptr;
        const ElementData* element = nullptr;
        NoteEvent ev{};
        /// Steal score latched by the score refresh (1 right after allocation).
        int score = 1;
        /// Drum notes: the superset note EQ / HPF latched at note-on (eq.hpp).
        eq::VoiceEqSettings eq_drum{};
    };

    /// A DSP object of the renderer's pool (64 for 32 handles: objects, status).
    /// An object keeps its state between notes (the LFO phase runs on from note to note); a
    /// removed voice's object fades out unmapped ("dying") while its handle takes the next
    /// free object.
    struct Obj {
        DspVoice dsp;
        /// Status ≠ 0: started and neither stopped nor finished fading (an ended voice still
        /// mapped to its handle stays busy).
        bool busy = false;
        bool dying = false;
        /// The handle that started it, and a copy of its record while dying.
        std::size_t owner = 0;
        Slot info;
    };
    DspVoice& dsp(Slot& s) { return objs_[s.obj].dsp; }
    const DspVoice& dsp(const Slot& s) const { return objs_[s.obj].dsp; }
    /// A melodic note object of a part's note handler (the list,
    /// oldest first). It outlives its voices while its key is held (mono: the held-key stack).
    struct Note {
        std::uint64_t serial = 0;
        std::uint8_t note = 0;     ///< raw key
        int key = 60;              ///< transposed key
        std::uint8_t velocity = 0; ///< raw velocity
        std::uint8_t vel1 = 1;     ///< sensed velocity + 1
        std::size_t voice = 0;     ///< program voice
        bool damper = false;       ///< damper-mode-3 voice (bit 1)
        bool released = false;     ///< bit 7
        bool sustained = false;    ///< bit 6
        bool sostenuto = false;    ///< bit 5
        /// Its voices were killed (mono) or it never had any (high-note priority): it leaves
        /// the list only at its note-off.
        bool detached = false;
    };
    struct PartNotes {
        std::vector<Note> list;
        /// CC84 keys waiting for the next note-on (count, bitset).
        std::bitset<128> cc84;
        int cc84_count = 0;
        /// Last note-on key (starts at 60).
        int last_key = 60;
        /// The mono/poly mode seen last (a change kills the part's notes).
        int mode = -1;
    };
    enum class Mode : int { Normal = 0, FromLast = 1, FromLastDown = 2, FromTail = 3, FromTailDown = 4,
                            Legato = 5, LegatoDown = 6, Takeover = 7 };

    const midi::xg::DrumSetup* drum_setup(const midi::Part& part, std::uint8_t note,
                                          const syxg2006le::tbl::Voice& voice) const;
    Slot& allocate();
    /// unless the handle's object is idle, kill it (a voice in its
    /// key-on delay or already ended stops at once, any other fades out at `rate`, or with the
    /// superset steal fade when `steal`) and give the handle the next free pool object from
    /// the round-robin cursor (none free: the killed object stops and stays).
    void kill_handle(Slot& s, int rate, bool steal);
    /// Start the handle's object on a note.
    void start_voice(Slot& s, const ElementData& el);
    /// Remove a voice from its note: its sound fades out in the dying pool at
    /// `rate` and its entry returns to the free list at once.
    void remove_note(Slot& s, int rate);
    /// Remove every voice matching `pred` in note order (oldest note first, each note's
    /// voices in allocation order).
    template <class Pred> void remove_voices(Pred pred, int rate);
    void refresh_scores();
    VoiceControls controls(const Slot& s) const;
    void release(Slot& s, int pedal);
    static int pedal(const midi::Part& part);
    void melodic_note_on(const midi::Part& part, std::uint8_t note, std::uint8_t velocity, bool retrigger);
    bool live(std::uint64_t serial) const;
    void prune(PartNotes& pn);
    void detach(Note& n, int rate);
    void note_off_note(const midi::Part& part, PartNotes& pn, std::size_t i);
    void check_mode(const midi::Part& part);
    std::optional<std::size_t> current_voice(const midi::Part& part) const;

    std::shared_ptr<const BankData> bank_;
    ModuleContext ctx_;
    const tbl::dll::Tables& tb_;
    RateTables tables_;
    /// Voice handles [0, live_limit_).
    std::vector<Slot> slots_;
    /// DSP object pool and its free-object cursor.
    std::vector<Obj> objs_;
    std::size_t cursor_ = 0;
    /// FIFO free list of live voices (takes the head; finished voices are
    /// appended at the tail): which DSP voice a note gets sets its LFO phase.
    std::vector<std::uint32_t> free_;
    /// melodic_note_on's snapshot of the previous note's voices (reserved for every slot).
    std::vector<std::pair<Slot, DspVoice>> prev_voices_;
    /// prune's sorted serials of the live voices (reserved for every slot).
    std::vector<std::uint64_t> live_serials_;
    /// Note objects reserved per part: a part holds one per sounding or held key (the list
    /// keeps its capacity, so it allocates only beyond this many at once).
    static constexpr std::size_t kNoteListReserve = 128;
    std::size_t free_head_ = 0, free_count_ = 0;
    void push_free(std::size_t index);
    std::uint64_t serial_ = 0;
    std::size_t live_limit_ = 32;
    std::array<PartNotes, 64> notes_{};
    /// Controller sources latched by the 12 ms controller timer, per part id.
    std::array<ControllerSources, 64> sources_{};
    std::array<const midi::Part*, 64> parts_{};
    /// Controller timer: samples rendered, 2 ms ticks done, and
    /// the tick counter (every 6th tick flushes). Run before the block's voices render.
    std::uint64_t rendered_ = 0, ms_done_ = 0;
    int timer_count_ = 0;
    /// Steal-score refresh: a period-10 entry on the same scheduler list as the controller
    /// flush, i.e. every 10th flush (120 ms).
    int flush_count_ = 0;
    /// The steal tries released voices first until that fails once within a note-on
    /// (attempt counter, strategy).
    bool steal_any_ = false;
    std::uint64_t alloc_seq_ = 0;
    /// The XG EG times (attack, decay, release) each part's voices last saw.
    std::array<std::array<int, 3>, 64> eg_times_{};
    void xg_eg_changes();
    void track(const midi::Part& part);
    void controller_timer();
    /// Drum-setup images of every seg8 kit (the setup of parts not tied to a setup RAM).
    std::vector<midi::xg::DrumKitSetup> kits_;
    /// The buses of the block being rendered (insertion chains of the parts).
    const Buses* buses_ = nullptr;
};

} // namespace hvyxg::syxg2006le
