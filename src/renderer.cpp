// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "renderer.hpp"

#include <algorithm>
#include <type_traits>
#include <variant>

namespace hvyxg {

Renderer::Renderer(std::shared_ptr<const Bank> bank, float sample_rate,
                   std::uint16_t max_polyphony, Interpolation interpolation, float steal_fade_ms,
                   bool dll_exact, bool second_engine, bool mu_part_eq, bool full_xg)
    : bank_(impl(std::move(bank))), sample_rate_(sample_rate), max_polyphony_(max_polyphony),
      interpolation_(interpolation), steal_fade_ms_(steal_fade_ms), dll_exact_(dll_exact),
      full_xg_(full_xg && !dll_exact),
      second_engine_(second_engine), mu_part_eq_(mu_part_eq && !dll_exact), multi_eq_(sample_rate) {}

// The polyphony patch (SXG-Create-NF utility/dev/poly/poly.asm, `route`): free voices of A
// and, with extra voices, of B are read at the start of each host block; a
// Note On (velocity > 0) goes to A while A's count is ≥ 2 (then −2), else to B while B's is
// ≥ 2 (−2), else to A. Note-offs (also velocity 0) and every other event go to both.
void Renderer::begin_message(bool note_on) {
    route_ = !b_ ? Route::PerAction : !note_on ? Route::Both : note_to_b() ? Route::B : Route::A;
}

bool Renderer::note_to_b() {
    if (budget_a_ >= 2) {
        budget_a_ -= 2;
        return false;
    }
    if (budget_b_ >= 2) {
        budget_b_ -= 2;
        wake_b();
        return true;
    }
    return false;
}

void Renderer::wake_b() {
    // A note wakes a sleeping B: 4 s awake, and it takes A's control-rate clock so its envelopes and LFOs tick in step with A.
    if (b_->awake <= 0) {
        b_->tones->follow_clock(*tones_);
        b_->effects->follow_clock(*effects_);
        b_->awake = 4 * static_cast<std::int64_t>(sample_rate_);
    }
}

void Renderer::sysex_received() {
    // keep_awake: a SysEx that reaches a sleeping B (after the block's routing, so a note in
    // the same block still wakes it with A's clock) keeps it awake for sample rate / 4 samples.
    if (b_) b_->sysex = true;
}

void Renderer::handle_action(const Action& action) {
    std::visit(
        [&](const auto& a) {
            using T = std::decay_t<decltype(a)>;
            if constexpr (std::is_same_v<T, action::Init>) {
                const ModuleContext ctx{a.state,  sample_rate_, max_polyphony_, interpolation_, steal_fade_ms_,
                                        dll_exact_, mu_part_eq_,  full_xg_,      bank_->tables()};
                tones_ = bank_->make_tone_module(ctx);
                effects_ = bank_->make_system_effects(ctx);
                b_.reset();
                if (second_engine_) {
                    b_ = std::make_unique<SecondEngine>();
                    b_->tones = bank_->make_tone_module(ctx);
                    b_->effects = bank_->make_system_effects(ctx);
                }
                budget_a_ = static_cast<std::int64_t>(tones_->free_voices());
                budget_b_ = b_ ? static_cast<std::int64_t>(b_->tones->free_voices()) : 0;
                block_ = std::min(tones_->block_size(), Buses::kMaxBlock);
                // DLL-exact: no multi EQ (neither DLL renders it).
                multi_eq_ram_ = dll_exact_ ? nullptr : a.state.multi_eq;
                multi_eq_ = eq::MultiEqStage(sample_rate_);
            } else if (!tones_) {
                return;
            } else if constexpr (std::is_same_v<T, action::NoteOn>) {
                quiet_frames_ = 0;
                const Route r = route_ == Route::PerAction && b_ ? (note_to_b() ? Route::B : Route::A) : route_;
                (r == Route::B ? *b_->tones : *tones_).note_on(*a.part, a.note, a.velocity);
            } else {
                const auto each = [&](auto&& f) {
                    if (route_ != Route::B) f(*tones_);
                    if (b_ && route_ != Route::A) f(*b_->tones);
                };
                if constexpr (std::is_same_v<T, action::NoteOff>) {
                    each([&](ToneModule& t) { t.note_off(*a.part, a.note); });
                } else if constexpr (std::is_same_v<T, action::SustainChange>) {
                    each([&](ToneModule& t) { t.sustain_changed(*a.part); });
                } else if constexpr (std::is_same_v<T, action::SostenutoChange>) {
                    each([&](ToneModule& t) { t.sostenuto_changed(*a.part); });
                } else if constexpr (std::is_same_v<T, action::PortamentoControl>) {
                    each([&](ToneModule& t) { t.portamento_control(*a.part, a.key); });
                } else if constexpr (std::is_same_v<T, action::ReleaseAll>) {
                    each([&](ToneModule& t) { t.release_all(*a.part); });
                } else if constexpr (std::is_same_v<T, action::KillAll>) {
                    each([&](ToneModule& t) {
                        if (a.sound_off) t.sound_off(*a.part);
                        else t.kill_all(*a.part);
                    });
                } else if constexpr (std::is_same_v<T, action::Reset>) {
                    each([&](ToneModule& t) { t.reset(); });
                } else if constexpr (std::is_same_v<T, action::EgTimeChange>) {
                    each([&](ToneModule& t) { t.eg_time_changed(*a.part, a.attack); });
                }
            }
        },
        action);
}

void Renderer::render_block() {
    buses_.clear();
    if (tones_) {
        effects_->prepare(buses_);
        tones_->render(buses_);
        effects_->process(buses_, block_);
        // Engine B renders after A into the same output while it is awake; then it stays
        // awake 4 s while it has voices, else counts down (poly.asm `poly_main`).
        if (b_ && b_->sysex) {
            b_->sysex = false;
            if (b_->awake <= 0) b_->awake = static_cast<std::int64_t>(sample_rate_) >> 2;
        }
        if (b_ && b_->awake > 0) {
            Buses& bb = b_->buses;
            bb.clear();
            b_->effects->prepare(bb);
            b_->tones->render(bb);
            b_->effects->process(bb, block_);
            for (std::size_t i = 0; i < block_; ++i) {
                buses_.dry_l[i] += bb.dry_l[i];
                buses_.dry_r[i] += bb.dry_r[i];
            }
            if (b_->tones->free_voices() < b_->tones->polyphony())
                b_->awake = 4 * static_cast<std::int64_t>(sample_rate_);
            else
                b_->awake = std::max<std::int64_t>(b_->awake - static_cast<std::int64_t>(block_), 0);
        }
        if (b_) {
            budget_a_ = static_cast<std::int64_t>(tones_->free_voices());
            budget_b_ = static_cast<std::int64_t>(b_->tones->free_voices());
        }
        // The multi EQ sits on the final output, after the effect returns.
        if (multi_eq_ram_) {
            multi_eq_.update(*multi_eq_ram_);
            if (multi_eq_.active()) multi_eq_.process(buses_.dry_l.data(), buses_.dry_r.data(), block_);
        }
    }
    for (std::size_t i = 0; i < block_; ++i) {
        out_[2 * i] = buses_.dry_l[i];
        out_[2 * i + 1] = buses_.dry_r[i];
    }
    pos_ = 0;
    len_ = block_;
    const bool audible = tones_ && (tones_->active() || effects_->active() ||
                                    (b_ && b_->awake > 0 && (b_->tones->active() || b_->effects->active())));
    quiet_frames_ = audible ? 0 : quiet_frames_ + block_;
}

void Renderer::render(std::span<float> interleaved_stereo) {
    std::size_t frames = interleaved_stereo.size() / 2;
    float* dst = interleaved_stereo.data();
    while (frames > 0) {
        if (pos_ == len_) render_block();
        const std::size_t n = std::min(frames, len_ - pos_);
        std::copy_n(out_.data() + 2 * pos_, 2 * n, dst);
        pos_ += n;
        dst += 2 * n;
        frames -= n;
        frames_ += n;
    }
}

bool Renderer::needs_render() const {
    return sleep_delay_frames_ == 0 || quiet_frames_ < sleep_delay_frames_;
}

} // namespace hvyxg
