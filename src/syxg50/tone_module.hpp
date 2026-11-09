// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// The S-YXG50 voice engine: the DLL's control layer (program / element / zone selection,
// pitch, level, filter, envelopes, LFO, controllers, drums, voice allocation, the 10 ms
// control tick) driving `ChipVoice`s. Addresses are DLL VAs.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "accel/kernels.hpp"
#include "bank_impl.hpp"
#include "module.hpp"
#include "syxg50/bank_data.hpp"
#include "syxg50/dll_tables.hpp"
#include "syxg50/chip.hpp"
#include "midi/part.hpp"

namespace hvyxg::midi {
struct Part;
}

namespace hvyxg::syxg50 {

/// The drum-setup RAM image of kit (bank MSB, program) that the tone module expects
/// the XG template with bytes 2-10 (level, alternate group, pan,
/// sends, key assign, receive note off / on) from the kit's entries. Pitch, cutoff, resonance
/// and EG bytes stay at 64: they are offsets onto the kit entry (in GS mode too).
std::optional<DrumKitSetup> drum_setup(const dll::Tables& tb, const BankData& bank, std::uint8_t bank_msb,
                                       std::uint8_t program);

class ToneModule50 final : public ToneModule {
public:
    ToneModule50(std::shared_ptr<const BankData> bank, const ModuleContext& ctx);
    ~ToneModule50() override;

    std::size_t block_size() const override { return kBlock; }
    void note_on(const midi::Part& part, std::uint8_t note, std::uint8_t velocity) override;
    void note_off(const midi::Part& part, std::uint8_t note) override;
    void sustain_changed(const midi::Part& part) override;
    void sostenuto_changed(const midi::Part& part) override;
    void release_all(const midi::Part& part) override;
    void kill_all(const midi::Part& part) override;
    void sound_off(const midi::Part& part) override;
    void reset() override;
    void eg_time_changed(const midi::Part& part, bool attack) override;
    void render(Buses& buses) override;
    bool active() const override;
    std::size_t free_voices() const override { return free_.size(); }
    std::size_t polyphony() const override { return voices_.size(); }
    void follow_clock(const ToneModule& leader) override;
    std::size_t voice_count() const override;

    struct Voice;
    struct Ctl;

private:
    struct PartState {
        /// Key of the last note-on (portamento source, `part[0xc9]`): 60 after a reset or a
        /// part kill.
        int last_key = 60;
        /// Mono mode: held keys (`part[0xcb..]`), the sounding key and its velocity.
        std::array<std::uint8_t, 8> held{};
        int held_count = 0;
        int mono_key = -1;
        int mono_vel = 0;
        /// Serial of the last mono note (its register sets are `part[0xd4..0xd5]`), 0 = none.
        std::uint32_t mono_serial = 0;
        /// Per-block controller sums (valid when `ctl_block == block_`).
        std::uint64_t ctl_block = ~std::uint64_t{0};
    };

    void play(const midi::Part& part, std::uint8_t note, std::uint8_t raw_vel, int vel,
              std::uint32_t serial);
    void start_drum(const midi::Part& part, std::uint8_t note, int vel, std::uint32_t serial);
    bool start_element(const midi::Part& part, std::uint8_t note, int vel, const std::uint8_t* pv,
                       const std::uint8_t* elem, const std::uint8_t* drum_entry,
                       const std::uint8_t* drum_d, const std::uint8_t* drum_src,
                       std::uint32_t serial, int porta_src);
    bool make_room(const midi::Part& part, int n);
    void free_voice(Voice& v);
    void retire(Voice& v);
    void enqueue(Voice& v);
    void detach_tail(Voice& v);
    void monitor(Voice& v);
    void apply_touch();
    void bind_phys(Voice& v);
    int aeg_rate(const Voice& v, int drum_byte, int elem_byte) const;
    int release_rate(const Voice& v) const;
    void key_on_flush();
    int phys_status(const Voice& v) const;
    void release(Voice& v, int rr = -1);
    void release_note(Voice& first, int rr);
    /// Damp; `steal_fade` > 0: the superset linear steal fade instead of the damp
    /// rate.
    void damp(Voice& v, std::uint32_t steal_fade = 0);
    class VoiceList;
    VoiceList part_voices(const midi::Part& part) const;
    VoiceList group_of(Voice& first) const;
    void key_off(Voice& v);
    void peg_set(Voice& v, int n);
    void peg_advance(Voice& v);
    void feg_set(Voice& v, int n);
    void feg_advance(Voice& v);
    void tick();
    void lfo_tick(Voice& v);
    void set_lfo(Voice& v, const Ctl& c);
    void update(Voice& v, bool tick);
    void commit_cutoff(Voice& v, int ctrl);
    const Ctl& ctl(const midi::Part& part);
    std::uint8_t rand();
    int drum_mode(const midi::Part& part) const;

    std::shared_ptr<const BankData> bank_;
    ModuleContext ctx_;
    const dll::Tables& tb_;
    RateTables rt_;
    std::vector<std::unique_ptr<Voice>> voices_;
    /// render's batch (accel::kBatch voices): their blocks between the samples and the mix, so the
    /// filter runs across them (accel::svf50_lanes). 16 KB, allocated once.
    struct Batch {
        std::array<std::array<float, kBlock>, accel::kBatch> rows{};
        std::array<Voice*, accel::kBatch> voice{};
        std::array<ChipVoice::SvfStep, accel::kBatch> svf{};
    };
    std::unique_ptr<Batch> batch_ = std::make_unique<Batch>();
    /// Free voice indices, a FIFO: 0, 1, … at
    /// power-on; a note takes the head, a voice that ends or is
    /// stolen goes to the tail.
    class Fifo {
    public:
        void init(std::size_t capacity) {
            buf_.assign(capacity, 0);
            head_ = size_ = 0;
        }
        bool empty() const { return size_ == 0; }
        std::size_t size() const { return size_; }
        void clear() { head_ = size_ = 0; }
        std::size_t front() const { return buf_[head_]; }
        void pop_front() {
            head_ = (head_ + 1) % buf_.size();
            --size_;
        }
        // Each voice index is in the FIFO at most once, so it never holds more than the
        // voice count (its capacity).
        void push_back(std::size_t i) {
            if (size_ == buf_.size()) return;
            buf_[(head_ + size_++) % buf_.size()] = i;
        }
        void push_front(std::size_t i) {
            if (size_ == buf_.size()) return;
            head_ = (head_ + buf_.size() - 1) % buf_.size();
            buf_[head_] = i;
            ++size_;
        }

    private:
        std::vector<std::size_t> buf_;
        std::size_t head_ = 0, size_ = 0;
    };
    Fifo free_;
    /// Preallocated scratch storage, so note-on / steal / release paths do not allocate:
    /// voice lists leased by `VoiceList` (LIFO; nested at most a few deep), the steal search's
    /// note scores and `bind_phys`'s busy map.
    mutable std::array<std::vector<Voice*>, 8> scratch_;
    mutable std::size_t scratch_depth_ = 0;
    /// make_room's notes of the victim part: serial, highest element score, met yet.
    struct NoteScore {
        std::uint32_t serial;
        int score;
        bool seen;
    };
    std::vector<NoteScore> note_score_;
    std::vector<std::uint8_t> phys_busy_;
    /// Chip voices whose register set was freed while they still sound (stolen / cut
    /// voices fading out, binds a fresh chip voice to each key-on).
    std::vector<ChipVoice> tails_;
    /// The EQ / HPF of the drum key being started (start_drum → start_element).
    eq::VoiceEqSettings drum_eq_{};
    std::array<PartState, midi::kMaxPartSize> parts_{};
    std::unique_ptr<std::array<Ctl, midi::kMaxPartSize>> ctls_;
    std::uint64_t block_ = 0;
    std::uint64_t samples_ = 0;
    std::uint64_t ms_done_ = 0;
    std::uint32_t ticks_ = 0;
    /// Level round-robin cursor, 0..31.
    std::uint32_t level_cursor_ = 0;
    std::uint32_t serial_ = 0;
    std::uint64_t alloc_seq_ = 0;
    /// Physical chip voices (256 for 128 register sets) and their cursor.
    std::size_t phys_count_ = 256;
    std::size_t phys_cursor_ = 0;
    std::uint8_t rand_ = 0;
    /// The buses of the block being rendered (insertion chains of the parts).
    const Buses* buses_ = nullptr;
};

/// A list of voices in one of the tone module's preallocated scratch vectors (leased for the
/// list's lifetime, LIFO), so building it does not allocate. Deeper nesting than the scratch
/// count falls back to a vector of its own.
class ToneModule50::VoiceList {
public:
    template <class Fill>
    VoiceList(const ToneModule50& t, Fill&& fill) : t_(t) {
        if (t.scratch_depth_ < t.scratch_.size()) {
            v_ = &t.scratch_[t.scratch_depth_++];
            leased_ = true;
        }
        v_->clear();
        fill(*v_);
    }
    ~VoiceList() {
        if (leased_) --t_.scratch_depth_;
    }
    VoiceList(const VoiceList&) = delete;
    VoiceList& operator=(const VoiceList&) = delete;
    auto begin() const { return v_->begin(); }
    auto end() const { return v_->end(); }

private:
    const ToneModule50& t_;
    std::vector<Voice*> own_;
    std::vector<Voice*>* v_ = &own_;
    bool leased_ = false;
};

} // namespace hvyxg::syxg50
