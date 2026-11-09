// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#include "hvyxg/synth.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include "bank_impl.hpp"
#include "midi/front_end.hpp"
#include "renderer.hpp"
#include "synth_access.hpp"

namespace hvyxg {


namespace {

/// `Options::full_xg_2006le` applies to the S-YXG2006LE bank only, and never in DLL-exact mode.
bool full_xg(const Bank& bank, const Synth::Settings& o) {
    return o.full_xg_2006le && !o.dll_exact && bank.type() == Model::Syxg2006le;
}

} // namespace

struct Synth::Impl final : ActionSink {
    /// A message waiting for its frame.
    struct Pending {
        std::uint64_t frame;
        std::uint32_t at;   // offset into `bytes`
        std::uint32_t len;
        std::uint8_t port;
    };

    std::shared_ptr<const Bank> bank;
    float rate;
    Renderer renderer;
    midi::FrontEnd front;
    std::uint64_t frame = 0;
    // Messages waiting for their frame (time order) and their bytes. Reserved up front so that
    // `midi` does not allocate while at most kQueueMessages / kQueueBytes are pending.
    static constexpr std::size_t kQueueMessages = 1024;
    static constexpr std::size_t kQueueBytes = 16384;
    std::vector<Pending> queue;
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> spare;  // compaction target, swapped with `bytes`
    std::size_t head = 0;
    // midi_stream parser (SysEx longer than kStreamBytes grows it).
    static constexpr std::size_t kStreamBytes = 4096;
    std::vector<std::uint8_t> stream;
    std::uint8_t running = 0;
    std::size_t stream_need = 0;
    // UMP SysEx gathered per group (groups 0..3; a message longer than kStreamBytes is dropped).
    static constexpr std::size_t kUmpGroups = 4;
    std::array<std::vector<std::uint8_t>, kUmpGroups> ump_sysex;

    Impl(std::shared_ptr<const Bank> b, const Settings& o)
        : bank(std::move(b)), rate(o.sample_rate),
          // DLL-exact: the DLL's polyphony, linear interpolation, its steal and one port.
          renderer(bank, o.sample_rate,
                   o.max_polyphony && !o.dll_exact ? o.max_polyphony : bank->default_polyphony(),
                   o.dll_exact ? Interpolation::Linear : o.interpolation, o.dll_exact ? 0.0f : o.steal_fade_ms,
                   o.dll_exact,
                   // The patched DLL's second engine: with an MU bank, unless a polyphony
                   // of the superset's own is asked for.
                   bank->mu() && (o.dll_exact || o.max_polyphony == 0), o.mu_part_eq, full_xg(*bank, o)),
          front(bank, *this,
                midi::FrontEndOptions{o.dll_exact ? 1 : o.ports, o.device_id, o.dll_exact, full_xg(*bank, o)}) {
        queue.reserve(kQueueMessages);
        bytes.reserve(kQueueBytes);
        spare.reserve(kQueueBytes);
        stream.reserve(kStreamBytes);
        for (auto& g : ump_sysex) g.reserve(kStreamBytes);
    }

    void act(const Action& a) override { renderer.handle_action(a); }

    /// One complete message to the front end; the renderer learns which MIDI message the
    /// actions belong to (the patched DLL's second engine routes whole messages).
    void deliver(std::span<const std::uint8_t> msg, std::size_t port) {
        const std::uint8_t st = msg[0];
        if (st == 0xF0) renderer.sysex_received();
        renderer.begin_message((st & 0xF0) == 0x90 && msg.size() >= 3 && (msg[2] & 0x7F) != 0);
        front.message(msg, port);
        renderer.end_message();
    }

    /// At the start of a block (`frame`): apply every message whose frame lies before the end
    /// of that block. The DLLs act on an event from the start of the engine block that
    /// contains it [meas: note onsets at offsets 0..255 against both DLLs]; a message whose
    /// block has already started (it arrived too late) acts from the next block.
    void apply_due() {
        const std::uint64_t end = frame + renderer.block_size();
        while (head < queue.size() && queue[head].frame < end) {
            const Pending& p = queue[head++];
            deliver(std::span(bytes).subspan(p.at, p.len), p.port);
        }
        if (head == queue.size() && head != 0) {
            queue.clear();
            bytes.clear();
            head = 0;
        } else if (head != 0 && (2 * head >= queue.size() || 2 * bytes.size() >= kQueueBytes)) {
            compact();
        }
    }

    /// Drop the applied messages and their bytes while others still wait (a host that always
    /// has a later message queued would otherwise grow the queue without bound). Within the
    /// reserved capacity this does not allocate.
    void compact() {
        spare.clear();
        for (std::size_t i = head; i < queue.size(); ++i) {
            Pending& p = queue[i];
            const auto from = bytes.begin() + static_cast<std::ptrdiff_t>(p.at);
            p.at = static_cast<std::uint32_t>(spare.size());
            spare.insert(spare.end(), from, from + static_cast<std::ptrdiff_t>(p.len));
        }
        bytes.swap(spare);
        queue.erase(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(head));
        head = 0;
    }
};

bool Synth::check(const Settings& o, std::string& error) {
    if (!(o.sample_rate >= 8000.0f && o.sample_rate <= 192000.0f)) {
        error = "hvyxg::Synth: sample rate out of range (8 kHz … 192 kHz)";
        return false;
    }
    if (o.max_polyphony > kMaxPolyphony) {
        error = "hvyxg::Synth: max_polyphony above " + std::to_string(kMaxPolyphony);
        return false;
    }
    return true;
}

Synth::Synth(std::shared_ptr<const Bank> bank, const Settings& settings) {
    Settings s = settings;
    s.sample_rate = std::clamp(s.sample_rate, 8000.0f, 192000.0f);
    s.max_polyphony = std::min(s.max_polyphony, kMaxPolyphony);
    impl_ = std::make_unique<Impl>(std::move(bank), s);
    impl_->front.power_on();
}

Synth::~Synth() = default;

void Synth::midi(std::span<const std::uint8_t> msg, std::uint32_t frame_offset, std::size_t port) {
    if (msg.empty()) return;
    Impl& m = *impl_;
    if (frame_offset == 0 && m.head == m.queue.size()) {
        m.deliver(msg, port);
        return;
    }
    // Keep the queue in time order; equal times keep their arrival order.
    const std::uint64_t at = m.frame + frame_offset;
    const Impl::Pending p{at, static_cast<std::uint32_t>(m.bytes.size()), static_cast<std::uint32_t>(msg.size()),
                          static_cast<std::uint8_t>(std::min<std::size_t>(port, 255))};  // the front end clamps to 3
    m.bytes.insert(m.bytes.end(), msg.begin(), msg.end());
    const auto pos = std::upper_bound(m.queue.begin() + static_cast<std::ptrdiff_t>(m.head), m.queue.end(), at,
                                      [](std::uint64_t f, const Impl::Pending& q) { return f < q.frame; });
    m.queue.insert(pos, p);
}

void Synth::midi_stream(std::span<const std::uint8_t> in, std::uint32_t frame_offset, std::size_t port) {
    Impl& m = *impl_;
    for (const std::uint8_t b : in) {
        if (b >= 0xF8) continue;  // real-time bytes (clock, active sensing, …)
        if (b == 0xF0) {
            m.stream.assign(1, b);
            m.running = 0;
            m.stream_need = 0;
            continue;
        }
        if (b == 0xF7) {
            if (!m.stream.empty() && m.stream[0] == 0xF0) {
                m.stream.push_back(b);
                midi(m.stream, frame_offset, port);
            }
            m.stream.clear();
            continue;
        }
        if (b & 0x80) {
            m.stream.clear();
            if (b >= 0xF0) {  // system common: cancels running status, ignored
                m.running = 0;
                continue;
            }
            m.running = b;
            m.stream_need = ((b & 0xF0) == 0xC0 || (b & 0xF0) == 0xD0) ? 1 : 2;
            m.stream.push_back(b);
            continue;
        }
        if (!m.stream.empty() && m.stream[0] == 0xF0) {
            m.stream.push_back(b);
            continue;
        }
        if (m.running == 0) continue;
        if (m.stream.empty()) m.stream.push_back(m.running);
        m.stream.push_back(b);
        if (m.stream.size() == m.stream_need + 1) {
            midi(m.stream, frame_offset, port);
            m.stream.clear();
        }
    }
}

void Synth::render(float* out, std::size_t frames) {
    Impl& m = *impl_;
    std::size_t done = 0;
    while (done < frames) {
        std::size_t n = m.renderer.frames_to_boundary();
        if (n == 0) {
            m.apply_due();
            n = m.renderer.block_size();
        }
        n = std::min(n, frames - done);
        m.renderer.render(std::span(out + 2 * done, 2 * n));
        done += n;
        m.frame += n;
    }
}

void Synth::ump(std::span<const std::uint32_t> w, std::uint32_t frame_offset) {
    if (w.empty()) return;
    Impl& m = *impl_;
    const std::uint32_t type = w[0] >> 28, group = (w[0] >> 24) & 15, status = (w[0] >> 16) & 0xFF;
    switch (type) {
    case 0x1:  // system: only System Reset, as an XG System On (power-on)
        if (status == 0xFF) {
            static constexpr std::uint8_t kXgOn[] = {0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7};
            midi(kXgOn, frame_offset, group);
        }
        break;
    case 0x2: {  // MIDI 1.0 channel voice
        if (status < 0x80 || status >= 0xF0) break;
        const std::uint8_t msg[3] = {static_cast<std::uint8_t>(status), static_cast<std::uint8_t>((w[0] >> 8) & 0x7F),
                                     static_cast<std::uint8_t>(w[0] & 0x7F)};
        const std::size_t n = (status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0 ? 2 : 3;
        midi(std::span(msg, n), frame_offset, group);
        break;
    }
    case 0x3: {  // 7-bit SysEx: status 0 complete, 1 start, 2 continue, 3 end
        if (w.size() < 2 || group >= Impl::kUmpGroups) break;
        std::vector<std::uint8_t>& b = m.ump_sysex[group];
        const std::uint32_t st = (w[0] >> 20) & 15, n = std::min<std::uint32_t>((w[0] >> 16) & 15, 6);
        const std::uint8_t data[6] = {static_cast<std::uint8_t>(w[0] >> 8), static_cast<std::uint8_t>(w[0]),
                                      static_cast<std::uint8_t>(w[1] >> 24), static_cast<std::uint8_t>(w[1] >> 16),
                                      static_cast<std::uint8_t>(w[1] >> 8), static_cast<std::uint8_t>(w[1])};
        if (st == 0 || st == 1) b.assign(1, 0xF0);
        else if (b.empty()) break;  // a continuation with no start
        if (st > 3) break;
        if (b.size() + n + 1 > Impl::kStreamBytes) {  // too long: dropped
            b.clear();
            break;
        }
        for (std::uint32_t i = 0; i < n; ++i) b.push_back(data[i] & 0x7F);
        if (st == 0 || st == 3) {
            b.push_back(0xF7);
            midi(b, frame_offset, group);
            b.clear();
        }
        break;
    }
    default: break;
    }
}

void Synth::render(std::int32_t* out, std::size_t frames) {
    constexpr std::size_t kChunk = 256;
    float buf[2 * kChunk];
    while (frames > 0) {
        const std::size_t n = std::min(frames, kChunk);
        render(buf, n);
        for (std::size_t i = 0; i < 2 * n; ++i) {
            const float v = buf[i] * 32768.0f;
            out[i] = v != v ? 0 : static_cast<std::int32_t>(std::lrint(std::clamp(v, -2147483520.0f, 2147483520.0f)));
        }
        out += 2 * n;
        frames -= n;
    }
}

void Synth::reset() {
    Impl& m = *impl_;
    m.queue.clear();
    m.bytes.clear();
    m.head = 0;
    m.stream.clear();
    m.running = 0;
    for (auto& b : m.ump_sysex) b.clear();
    m.front.reset(ResetMode::XG);
}

Model Synth::model() const { return impl_->bank->type(); }
float Synth::sample_rate() const { return impl_->rate; }
std::size_t Synth::block_size() const { return impl_->renderer.block_size(); }
ResetMode Synth::reset_mode() const { return impl_->front.reset_mode(); }
std::size_t Synth::part_count() const { return impl_->front.part_count(); }
const midi::Part& SynthAccess::part(const Synth& s, std::size_t i) { return s.impl_->front.part(i); }
const Bank& Synth::bank() const { return *impl_->bank; }

Synth::PartProgram Synth::part_program(std::size_t i) const {
    const midi::Part& p = impl_->front.part(i);
    return {p.program.bank_msb, p.program.bank_lsb, p.program.program, p.ram->part_mode != 0};
}

std::size_t Synth::voice_count() const {
    const Renderer& r = impl_->renderer;
    const ToneModule* t = r.tone_module();
    const ToneModule* tb = r.second_tone_module();
    return (t ? t->voice_count() : 0) + (tb && r.second_engine_awake() ? tb->voice_count() : 0);
}

bool Synth::active() const {
    const Renderer& r = impl_->renderer;
    const ToneModule* t = r.tone_module();
    const SystemEffects* e = r.system_effects();
    const ToneModule* tb = r.second_tone_module();
    const SystemEffects* eb = r.second_system_effects();
    // A sleeping engine B renders nothing until a note wakes it.
    const bool b_on = r.second_engine_awake();
    return (t && t->active()) || (e && e->active()) || (b_on && ((tb && tb->active()) || (eb && eb->active())));
}

} // namespace hvyxg
