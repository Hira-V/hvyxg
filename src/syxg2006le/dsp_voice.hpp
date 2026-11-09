// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// The S-YXG2006LE DSP voice (renderer): one sample, one filter, an amp EG,
// a pitch EG, a filter EG and an LFO, ticked once per 64-sample block.
//
// Per tick, in the DLL's order: the 30 ms start gate, LFO, pitch EG + phase increment,
// filter EG → filter, amp EG, output gains, sample render + interpolation, filter, mix.

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

#include "accel/control_le.hpp"
#include "steal_fade.hpp"
#include "eq.hpp"
#include "hvyxg/types.hpp"
#include "syxg2006le/dsp.hpp"
#include "syxg2006le/voice_setup.hpp"

namespace hvyxg::syxg2006le {

/// Engine-wide constants derived from the output rate.
struct RateTables {
    RateTables(const tbl::dll::Tables& tables, float fs);
    /// The DLL's tables these were made from.
    const tbl::dll::Tables& tb;
    float fs;
    std::array<std::int32_t, 128> inc_attack{};
    std::array<std::int32_t, 128> inc_decay{};
    std::array<std::int32_t, 128> inc_release{};
    /// Phase-increment mantissas T[i] = (44100/fs)·I[i]·2^-12 (I =).
    std::array<float, 1024> pitch{};
    /// Log-cutoff offset B = trunc(65536·log2(44100/fs)).
    std::int32_t cutoff_base = 0;
    /// 30 ms in samples (start gate).
    std::int32_t gate_samples = 0;
    /// Glide steps of the output stage per tick: pan and sends ([9]).
    std::int32_t inc_pan = 0, inc_send = 0;
    /// Portamento steps per tick: max(1, trunc(2^22 / (fs · S[r]))).
    std::array<std::int32_t, 128> inc_portamento{};
    Lanczos3 lanczos;
    /// The shared random generator of S&H LFOs and random pan.
    std::uint64_t lfsr = 1;
    double random();
};

/// The parts of a voice's output that the tone module recomputes every block.
struct VoiceControls {
    /// Total pitch in cents (note + controllers), clamped ±9600.
    int pitch_cents = 0;
    /// Cutoff in cents after the controller filter sum, and resonance.
    int cutoff_cents = 19200;
    int resonance = 16;
    /// LFO controller depths: PMod (cents p-p), FMod (cents), AMod (units).
    int pmod = 0, fmod = 0, amod = 0;
    /// Voice level 0..255 (the static level + controller amplitude sum).
    int level = 0;
    /// Pan attenuation of the left / right output.
    int pan_l = 24, pan_r = 24;
    /// Send attenuations: dry, reverb, chorus, variation; ≥ 255 = off.
    std::array<int, 4> send{8, 255, 255, 255};
};

class DspVoice {
public:
    enum class Output : std::size_t { DryL, DryR, Reverb, ChorusL, ChorusR, VariationL, VariationR };
    static constexpr std::size_t kOutputs = 7;

    /// Construction as pool object `index`: the LFO phase starts at 0 and a
    /// non-key-synced note-on adds `index·65°` to wherever the phase is.
    void init_object(int index);
    std::uint32_t lfo_offset() const { return lfo_offset_; }
    /// Note-on (and the per-object note-on methods).
    void start(RateTables& t, const VoiceParams& p, const ElementData& el,
               std::span<const std::int16_t> wave, const VoiceControls& c);
    /// Portamento: glide from `start_cents` to 0 at `rate` (127 − XG
    /// portamento time), linear in pitch.
    void set_portamento(const RateTables& t, int start_cents, int rate);
    /// Change the glide rate (a running glide continues at the new rate).
    void set_portamento_rate(const RateTables& t, int rate) {
        porta_step_ = t.inc_portamento[static_cast<std::size_t>(std::clamp(rate, 0, 127))];
    }
    /// Current portamento offset in cents (the start of a following glide): `pos·75 >> 12`
    /// (floor).
    int portamento_cents() const { return (porta_pos_ * 75) >> 12; }
    /// Note-on modes 5 / 6 (mono legato): continue the amp EG of `prev` (the
    /// matching voice of the previous note) from where it is, with its rates and levels.
    void take_over_amp(const RateTables& t, const DspVoice& prev);
    /// Note-on modes 3–7: continue the pitch and filter EGs of
    /// `prev`.
    void take_over_mod_egs(const RateTables& t, const DspVoice& prev);
    /// Mode 7 (CC84 voice takeover, with time 0): jump to the new pitch.
    void retarget(int pitch_cents);
    /// The part's XG attack / decay changed (CC73 / CC75): new rates for the running EG,
    /// except the stages a legato takeover fixed.
    void set_xg_attack(const RateTables& t, int rate);
    void set_xg_decay(const RateTables& t, int d1, int d2, int d3);
    /// The amp EG parameters the voice runs with (copied by a legato takeover).
    const AmpEgParams& amp_params() const { return amp_; }
    /// Note-off: `release_rate` = amp-EG release rate, `damper_down` selects the filter-EG
    /// release.
    void note_off(const RateTables& t, int release_rate, bool damper_down);
    /// Change the release rate of a released voice (pedal moved).
    void set_release_rate(const RateTables& t, int release_rate);
    /// Fast linear release at `rate` ≥ 0x60 (voice state 9): 0x70 for all
    /// sound off / key assign / alternate groups / mono, 0x7e for stolen voices.
    void kill(const RateTables& t, int rate = 0x70);
    /// Superset: fade the sound out linearly over `samples` (a stolen voice), then end.
    void start_steal_fade(std::uint32_t samples) { steal_fade_.start(samples); }
    /// One 64-sample tick, mixed into `out` (one buffer per output).
    void render(RateTables& t, const VoiceControls& c,
                std::array<float*, kOutputs> out, Interpolation interp);
    /// One block's filter, for a caller that runs it across voices (accel::svf_le_lanes).
    struct SvfStep {
        float* lp = nullptr;
        float* bp = nullptr;
        float f = 0.0f, df = 0.0f, q = 1.0f;
    };
    /// A tick in steps, as render() does them (docs/ACCELERATION.md, "Control across voices"):
    /// begin -- the pending note-off, the start gate and key-on delay, an S&H LFO's draw from the
    /// shared generator (false: nothing to render this block); control -- LFO, EGs, pitch, filter,
    /// the output glides and gain ramps (no shared state: a caller may run a batch's together);
    /// signal -- the samples into x and the peak, and the filter's step; the filter over x, by the
    /// caller (svf); then EQ, fade and the outputs.
    bool begin(RateTables& t);
    void control(RateTables& t, const VoiceControls& c);
    void signal(Interpolation interp, const RateTables& t, float* x, SvfStep& svf);
    void render_end(std::array<float*, kOutputs> out, float* x);
    /// control() across a batch (accel/control_le.hpp): this voice's state and the block's controls
    /// into lane i (ok[i] = 0 where the voice is outside the states a kernel takes), and back from it.
    void to_lanes(accel::LanesLE& L, std::size_t i, const RateTables& t, const VoiceControls& c) const;
    void from_lanes(const accel::LanesLE& L, std::size_t i, const VoiceControls& c);
    static accel::ControlTablesLE control_tables(const RateTables& t);
    static const accel::FieldsLE& fields_le();
#if defined(HVYXG_CHECK_CONTROL)
    /// Debugging a control kernel: the fields control() writes that differ from `ref`'s, printed; true
    /// if any did.
    bool differs_from(const DspVoice& ref, const char* where) const;
#endif
    /// The LFO waves that sample and hold a random value.
    static bool sh_wave(int w) { return w == 3 || w == 8 || w == 12; }

    bool active() const { return state_ != State::Idle; }
    /// Silence the voice at once, keeping its object state (LFO phase) for the next note.
    void stop() {
        state_ = State::Idle;
        ended_ = false;
    }
    /// Ended by itself (sample or EG end) and not stopped since.
    bool ended() const { return ended_; }
    /// In its key-on delay or not yet ticked (a kill stops it at once).
    bool waiting() const { return state_ == State::Delay || (state_ == State::Running && !started_); }
    bool released() const { return released_; }

    /// Superset: the part / drum-note EQ and HPF after the filter, before the outputs
    /// (eq.hpp; set by the tone module, nothing while flat).
    eq::VoiceEq eq;

    /// Steal-score inputs (the renderer getters read):
    /// the voice state class: 0 key-on delay, 1 keyed, 2 released, 3 idle.
    int score_state() const;
    /// The amp-EG stage: 0 attack, 1 decay 1, 2 decay 2, 3 decay 3 (or ended
    /// keyed), 4 sustain, 5 release (or ended released).
    int score_stage() const;
    /// The amp-EG term: the peak of |EG gain × sample| since its last reset, in
    /// level units (16 per octave); 255 once the voice has ended.
    int score_units() const;
    /// "May die": the amp EG never gets louder again from its current stage
    /// and has been in it for more than 90 ms, or its levels never rise after the attack.
    bool may_die(float fs) const;

private:
    StealFade steal_fade_;
    enum class State : std::uint8_t { Idle, Delay, Running };

    // The EGs' and LFO's fields in the order the control kernel reads them, in runs of up to 8
    // 32-bit words (accel/control_le.hpp: one segment load a run; fields_le checks).
    struct AmpEg {
        int state = 0;
        std::int32_t x = 0;
        std::int32_t hold = 0;
        std::int32_t xstep = 0;
        float gain_prev = 0.0f, gain = 0.0f;
        float slope = 0.0f, intercept = 0.0f;
        std::array<std::int32_t, 4> inc{}; // attack, decay1..3
        std::array<std::int32_t, 4> level{};
        std::int32_t inc_release = 0;
        int decay1_rate = 0, decay2_rate = 0, attack_rate = 0, decay3_rate = 0;
        bool recompute = false;
        bool linear_release = false;
        bool finished = false;
    };
    struct ModEg {
        int state = 7;
        std::int32_t pos = 0;
        std::int32_t delay = 0;
        std::array<std::int32_t, 3> inc{};
        std::int32_t inc_sustain = 0, inc_release = 0;
        std::array<std::int32_t, 4> level{};
        std::int32_t release_level = 0;
        std::int32_t delay_target = 0;
        ModEgParams params;
    };
    struct Lfo {
        int wave = 6;
        int invert = 0;
        std::uint32_t phase = 0;
        std::int32_t inc = 0;
        float pitch_depth = 0, amp_depth = 0, filter_depth = 0;
        float amod = 0.0f;
        float sh = 0.0f;
        // Outputs (1/65536 octave, attenuation × 4096): after sh, in the kernel's second run.
        std::int32_t out_pitch = 0, out_amp = 0, out_filter = 0, out_bias = 0;
        int state = 2;
        std::int32_t counter = 0, delay = 0, fade = 0;
        float fade_factor = 1.0f;
        int fade_flags = 0;
        bool sh_flag = true;
    };

    void amp_init(const RateTables& t, const AmpEgParams& a);
    /// SVF coefficient for a log cutoff (cutoff + filter EG), with the LFO terms and the
    /// resonance ceiling.
    float filter_coefficient(const RateTables& t, std::int32_t cutoff, int resonance) const;
    static void mod_take_over(const RateTables& t, ModEg& eg, const ModEg& prev, bool prev_started);
    void amp_tick(const RateTables& t);
    void amp_advance();
    void amp_to_release();
    static bool mod_tick(ModEg& eg, bool gated);
    static void mod_advance(ModEg& eg);
    void lfo_tick(RateTables& t, const VoiceControls& c, bool gated);
    void render_samples(std::span<float, kBlock> dst, Interpolation interp,
                        const RateTables& t);


    State state_ = State::Idle;
    /// Steal-score state: the signal peak and its reset timer (period
    /// 4096 samples), EG ticks in the current stage, L1 <= L2 <= L3 at note-on.
    float peak_ = 0.0f;
    bool amp_monotonic_ = true;
    bool ended_released_ = false;
    /// A note-off waiting for the next render (rate, damper).
    struct PendingOff {
        bool pending = false;
        int rate = 0;
        bool damper = false;
    } off_;
    void apply_note_off(const RateTables& t);
    std::uint8_t xg_freeze_ = 0;
    /// The voice ended by itself (DSP states 4 / 8): still "busy" for the pool.
    bool ended_ = false;
    std::uint32_t lfo_offset_ = 0;
    bool released_ = false;
    bool started_ = false;
    bool sample_ended_ = false;
    std::int32_t gate_ = 0;
    std::int32_t delay_counter_ = 0, delay_target_ = 0;

    // Sample.
    const std::int16_t* base_ = nullptr; // sample index 0 (loop start)
    std::int64_t pre_loop_ = 0, loop_len_ = 0;
    bool one_shot_ = false;
    std::int64_t pos_ = 0;
    std::int32_t frac_ = 0;
    std::int32_t loop_tune_ = 0;

    // Filter.
    Svf svf_;
    float q_ = 1.0f;
    int filter_type_ = 0;

    // The control kernel's runs (accel/control_le.hpp; fields_le checks they are contiguous):
    // pitch and filter ...
    std::int32_t pitch_base_ = 0;
    std::int32_t porta_pos_ = 0, porta_step_ = 0;
    std::int32_t inc_ = 0;
    std::int32_t cutoff_cur_ = 0, cutoff_target_ = 0;
    float f_prev_ = 0.0f;
    float f_new_ = 0.0f;           // the filter coefficient at the block's end
    // ... the steal score's tick counts (EG ticks in the current stage; the peak's reset timer,
    // period 4096 samples) ...
    std::uint32_t stage_ticks_ = 0;
    std::int32_t peak_timer_ = 0;
    // ... the output stage: glided level, pan and send attenuations ...
    std::int32_t level_x_ = 0;
    std::array<std::int32_t, 2> pan_x_{};
    std::array<std::int32_t, 4> send_x_{};
    // ... and the per-output gains, the previous block's and the mix ramps' ends.
    std::array<float, kOutputs> gain_prev_{};
    std::array<float, kOutputs> mix_from_{}, mix_to_{};

    AmpEg eg_;
    AmpEgParams amp_;
    ModEg peg_, feg_;
    Lfo lfo_;
    bool mixed_ = false;
    /// The block's start gate (begin).
    bool gated_ = false;
};

} // namespace hvyxg::syxg2006le
