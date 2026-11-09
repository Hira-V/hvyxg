// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// The voices' signal kernels (docs/ACCELERATION.md): what a voice block does to samples, once the
// engine's control code has worked out the block's parameters. Each kernel has a portable reference
// (namespace ref, kernels_scalar.cpp: the engines' own arithmetic, unchanged, always built) and may have
// a target's version (rvv: RVV 1.0 / XTheadVector, dsp: Andes DSP), chosen at compile time; the names in
// namespace accel call the best one built. A target's version keeps within its form's error bound
// against ref (exact where that is cheap), and the tests hold it to that.

#include <cstddef>
#include <cstdint>

namespace hvyxg::accel {

/// Buses a voice mixes into at most (dry L/R, reverb, chorus L/R, variation L/R).
inline constexpr std::size_t kMaxBuses = 7;

/// The S-YXG50's sample walk (syxg50::ChipVoice): positions in samples from the loop start (negative
/// in the pre-roll), with a 32-bit fraction; the wave unsigned (offset 0x80 / 0x8000), 8 or 16 bit.
struct Walk50 {
    const std::uint8_t* wave = nullptr;
    std::size_t wave_size = 0;
    std::size_t loop_start = 0;     // byte offset of sample 0 (the loop start)
    bool pcm8 = false;
    std::int32_t pre = 0;           // samples before the loop start
    std::int32_t loop_len = 0;      // 0: one-shot (ends at position 0)
    std::uint32_t loop_frac = 0;    // the loop's 32-bit fraction
    std::int32_t pos = 0;
    std::uint32_t frac = 0;
    bool ended = false;
};

namespace ref {

/// n samples of the walk at a step of inc_int + inc_frac / 2^32, linear (the DLL's) or 4-point Hermite,
/// into out; 0 once the walk has ended. Advances w.
void resample50(Walk50& w, std::int32_t inc_int, std::uint32_t inc_frac, bool hermite, float* out, std::size_t n);

/// The S-YXG50's Chamberlin low-pass, in place: band += f (x - q band - low); low += f band; x = low,
/// f stepping by df after each sample.
void svf50(float& band, float& low, float f, float df, float q, float* x, std::size_t n);

/// dst[b][i] += x[i] · g[b], g[b] += dg[b] after each sample, for the `buses` buses, n samples.
void mix_ramps(const float* x, float* const* dst, const float* g, const float* dg, std::size_t buses, std::size_t n);

/// svf50 for `lanes` voices side by side: voice l's block at x + l·stride, its state *band[l], *low[l],
/// its f, df, q at [l]. The filter is recursive in time, so a vector unit runs it across voices.
void svf50_lanes(float* x, std::size_t stride, float* const* band, float* const* low, const float* f, const float* df,
                 const float* q, std::size_t lanes, std::size_t n);

} // namespace ref

#if defined(HVYXG_ACCEL_RVV)
namespace rvv {
void resample50(Walk50& w, std::int32_t inc_int, std::uint32_t inc_frac, bool hermite, float* out, std::size_t n);
void mix_ramps(const float* x, float* const* dst, const float* g, const float* dg, std::size_t buses, std::size_t n);
void svf50_lanes(float* x, std::size_t stride, float* const* band, float* const* low, const float* f, const float* df,
                 const float* q, std::size_t lanes, std::size_t n);
} // namespace rvv
#endif

// The best version built.
inline void resample50(Walk50& w, std::int32_t inc_int, std::uint32_t inc_frac, bool hermite, float* out, std::size_t n) {
#if defined(HVYXG_ACCEL_RVV)
    rvv::resample50(w, inc_int, inc_frac, hermite, out, n);
#else
    ref::resample50(w, inc_int, inc_frac, hermite, out, n);
#endif
}
inline void svf50(float& band, float& low, float f, float df, float q, float* x, std::size_t n) {
    ref::svf50(band, low, f, df, q, x, n);
}
inline void mix_ramps(const float* x, float* const* dst, const float* g, const float* dg, std::size_t buses, std::size_t n) {
#if defined(HVYXG_ACCEL_RVV)
    rvv::mix_ramps(x, dst, g, dg, buses, n);
#else
    ref::mix_ramps(x, dst, g, dg, buses, n);
#endif
}
inline void svf50_lanes(float* x, std::size_t stride, float* const* band, float* const* low, const float* f, const float* df,
                        const float* q, std::size_t lanes, std::size_t n) {
#if defined(HVYXG_ACCEL_RVV)
    rvv::svf50_lanes(x, stride, band, low, f, df, q, lanes, n);
#else
    ref::svf50_lanes(x, stride, band, low, f, df, q, lanes, n);
#endif
}

/// The most voices a batch takes (svf50_lanes' lanes).
inline constexpr std::size_t kBatch = 32;

} // namespace hvyxg::accel
