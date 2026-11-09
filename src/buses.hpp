// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// The mix buses one render block of voices writes into, and the system effects read.
//
// Contract between the tone module and the system effects: every bus holds exactly what the
// DLL's voice section writes into the corresponding DLL bus, times the factor that takes the
// DLL's dry bus to its output (S-YXG50 `2^-15 · master`, S-YXG2006LE `−1 · 2^-13`). So a
// dry-bus value of 1.0 becomes an output sample of 1.0, and a voice whose send attenuation
// equals its dry attenuation puts the same value on both buses. Everything the DLL does after
// its voice buses (effect input gains such as the 2006LE reverb input × −2, the effect
// kernels, returns, pans, effect-to-effect sends) belongs to the system effects.

#include <array>
#include <cstddef>
#include <cstdint>

namespace hvyxg {

struct Buses {
    /// Largest render block of any engine (S-YXG50: 128 samples, S-YXG2006LE: 64).
    static constexpr std::size_t kMaxBlock = 128;
    using Channel = std::array<float, kMaxBlock>;

    Channel dry_l{};
    Channel dry_r{};
    /// Reverb send (mono in both DLLs: no pan on the reverb send).
    Channel reverb{};
    Channel chorus_l{};
    Channel chorus_r{};
    Channel variation_l{};
    Channel variation_r{};

    /// XG2.0 insertion effects (fx_insertion.hpp): the parts with insertion blocks render
    /// their dry pair into a chain pre-bus instead of the dry bus (unity part gain, no part
    /// pan, no sends); the insertion stage runs the chain and mixes it back. `part_chain[p]`
    /// is the chain of part p (−1 = none), `chains` the number of active chains (only those
    /// are cleared, so the cost is nil without insertion effects). Set by
    /// `SystemEffects::prepare` before the voices render.
    static constexpr std::size_t kMaxInsertionChains = 8;
    static constexpr std::size_t kMaxParts = 64;
    std::array<Channel, kMaxInsertionChains> chain_l{};
    std::array<Channel, kMaxInsertionChains> chain_r{};
    std::array<std::int8_t, kMaxParts> part_chain = no_chains();
    std::size_t chains = 0;

    /// The chain of part `id`, or −1.
    int chain_of(std::size_t id) const {
        return chains == 0 || id >= kMaxParts ? -1 : part_chain[id];
    }

    void clear() {
        for (Channel* c : {&dry_l, &dry_r, &reverb, &chorus_l, &chorus_r, &variation_l,
                           &variation_r})
            c->fill(0.0f);
        for (std::size_t i = 0; i < chains; ++i) {
            chain_l[i].fill(0.0f);
            chain_r[i].fill(0.0f);
        }
    }

private:
    static constexpr std::array<std::int8_t, kMaxParts> no_chains() {
        std::array<std::int8_t, kMaxParts> a{};
        a.fill(-1);
        return a;
    }
};

} // namespace hvyxg
