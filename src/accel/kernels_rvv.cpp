// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// The kernels in RVV (kernels.hpp, docs/ACCELERATION.md): RVV 1.0 (the X280) and RVV 0.7.1 (the C920,
// through GCC's XTheadVector intrinsics) from one source -- so no fractional LMUL, no vsext / vzext,
// no tail or mask policies; VLEN-agnostic (vsetvl everywhere); 32-bit elements at most (ELEN 32 is
// enough). Built only with HVYXG_ACCEL_RVV, and the vector unit in -march.
//
// resample50 is exact against the reference: the walk is integer, the samples' conversions exact (a
// power-of-two scale), and the interpolation the reference's operations in its order, unfused
// (-ffp-contract=off). mix_ramps is not: its ramps are g0 + i·dg (a fused multiply-add) where the
// reference adds dg sample by sample -- a difference of rounding, for its speed.

#if defined(HVYXG_ACCEL_RVV) && (defined(__riscv_vector) || defined(__riscv_xtheadvector))

#include "accel/kernels.hpp"

#include <riscv_vector.h>

#include <cstddef>
#include <cstdint>

namespace hvyxg::accel::rvv {

namespace {

// How many samples from (pos, frac), stepping inc (32.32), keep the position below `limit`: the k ≥ 0
// with pos + ⌊(frac + k·inc) / 2³²⌋ < limit, at most n.
std::size_t run_below(std::int32_t pos, std::uint32_t frac, std::uint64_t inc, std::int64_t limit, std::size_t n) {
    if (pos >= limit) return 0;
    if (!inc) return n;
    // frac + k·inc < (limit - pos)·2³²  ⇔  k < ((limit - pos)·2³² - frac) / inc
    const std::uint64_t room = (static_cast<std::uint64_t>(limit - pos) << 32) - frac;
    const std::uint64_t k = (room + inc - 1) / inc;
    return k < n ? static_cast<std::size_t>(k) : n;
}

// The walk's positions and fractions for lanes k0 .. k0 + vl - 1, exactly: k·inc_frac's 64-bit product
// in two halves (vmul, vmulhu) and the carry out of adding frac.
// (RVV types cannot be members: the two results through references.)
inline void walk_lanes(std::int32_t pos, std::uint32_t frac, std::int32_t inc_int, std::uint32_t inc_frac,
                       std::uint32_t k0, std::size_t vl, vint32m2_t& pos_out, vuint32m2_t& frac_out) {
    const vuint32m2_t k = __riscv_vadd_vx_u32m2(__riscv_vid_v_u32m2(vl), k0, vl);
    const vuint32m2_t lo = __riscv_vmul_vx_u32m2(k, inc_frac, vl);
    const vuint32m2_t hi = __riscv_vmulhu_vx_u32m2(k, inc_frac, vl);
    const vuint32m2_t f = __riscv_vadd_vx_u32m2(lo, frac, vl);
    const vbool16_t carry = __riscv_vmsltu_vx_u32m2_b16(f, frac, vl);
    vuint32m2_t whole = __riscv_vadd_vv_u32m2(__riscv_vmul_vx_u32m2(k, static_cast<std::uint32_t>(inc_int), vl), hi, vl);
    whole = __riscv_vadd_vx_u32m2_m(carry, whole, 1, vl);
    pos_out = __riscv_vadd_vx_i32m2(__riscv_vreinterpret_v_u32m2_i32m2(whole), pos, vl);
    frac_out = f;
}

// Unsigned 16-bit samples at byte offsets `off` from `base`, as floats in [-1, 1).
inline vfloat32m2_t pcm16(const std::uint8_t* base, vuint32m2_t off, std::size_t vl) {
    const vuint16m1_t u = __riscv_vluxei32_v_u16m1(reinterpret_cast<const std::uint16_t*>(base), off, vl);
    const vint16m1_t s = __riscv_vreinterpret_v_u16m1_i16m1(__riscv_vxor_vx_u16m1(u, 0x8000, vl));
    return __riscv_vfmul_vf_f32m2(__riscv_vfwcvt_f_x_v_f32m2(s, vl), 1.0f / 32768.0f, vl);
}

// Unsigned 8-bit samples at byte offsets `off` from `base`, as floats in [-1, 1). RVV 1.0 gathers the
// bytes at LMUL 1/2 and widens them twice; RVV 0.7.1 has no fractional LMUL, so there the bytes are read
// one by one (8-bit waves are the few; the 16-bit path carries the load).
inline vfloat32m2_t pcm8(const std::uint8_t* base, vuint32m2_t off, std::size_t vl) {
#if defined(__riscv_xtheadvector)
    alignas(64) std::uint32_t offs[256];
    alignas(64) std::int16_t s16[256];
    __riscv_vse32_v_u32m2(offs, off, vl);
    for (std::size_t j = 0; j < vl; ++j) s16[j] = static_cast<std::int16_t>(static_cast<int>(base[offs[j]]) - 0x80);
    const vint16m1_t s = __riscv_vle16_v_i16m1(s16, vl);
#else
    const vuint8mf2_t u = __riscv_vluxei32_v_u8mf2(base, off, vl);
    const vint8mf2_t b = __riscv_vreinterpret_v_u8mf2_i8mf2(__riscv_vxor_vx_u8mf2(u, 0x80, vl));
    const vint16m1_t s = __riscv_vwadd_vx_i16m1(b, 0, vl);   // sign-extended
#endif
    return __riscv_vfmul_vf_f32m2(__riscv_vfwcvt_f_x_v_f32m2(s, vl), 1.0f / 128.0f, vl);
}

// Unsigned 16-bit samples as floats in [-1, 1) (exact: a sign flip, a widening, a power of two).
inline vfloat32m2_t to_f32(vuint16m1_t u, std::size_t vl) {
    const vint16m1_t s = __riscv_vreinterpret_v_u16m1_i16m1(__riscv_vxor_vx_u16m1(u, 0x8000, vl));
    return __riscv_vfmul_vf_f32m2(__riscv_vfwcvt_f_x_v_f32m2(s, vl), 1.0f / 32768.0f, vl);
}

#if !defined(__riscv_xtheadvector)
// Unsigned 8-bit samples as floats in [-1, 1) (exact).
inline vfloat32m2_t u8_to_f32(vuint8mf2_t u, std::size_t vl) {
    const vint8mf2_t b = __riscv_vreinterpret_v_u8mf2_i8mf2(__riscv_vxor_vx_u8mf2(u, 0x80, vl));
    return __riscv_vfmul_vf_f32m2(__riscv_vfwcvt_f_x_v_f32m2(__riscv_vwadd_vx_i16m1(b, 0, vl), vl), 1.0f / 128.0f, vl);
}
#endif

// The reference's Hermite, its operations in its order, unfused: c1 = 0.5 (x1 - xm1); c2 = xm1 - 2.5 x0 +
// 2 x1 - 0.5 x2; c3 = 0.5 (x2 - xm1) + 1.5 (x0 - x1); ((c3 t + c2) t + c1) t + x0.
inline vfloat32m2_t hermite4(vfloat32m2_t xm1, vfloat32m2_t x0, vfloat32m2_t x1, vfloat32m2_t x2, vfloat32m2_t t, std::size_t vl) {
    const vfloat32m2_t c1 = __riscv_vfmul_vf_f32m2(__riscv_vfsub_vv_f32m2(x1, xm1, vl), 0.5f, vl);
    vfloat32m2_t c2 = __riscv_vfsub_vv_f32m2(xm1, __riscv_vfmul_vf_f32m2(x0, 2.5f, vl), vl);
    c2 = __riscv_vfadd_vv_f32m2(c2, __riscv_vfmul_vf_f32m2(x1, 2.0f, vl), vl);
    c2 = __riscv_vfsub_vv_f32m2(c2, __riscv_vfmul_vf_f32m2(x2, 0.5f, vl), vl);
    const vfloat32m2_t c3 = __riscv_vfadd_vv_f32m2(__riscv_vfmul_vf_f32m2(__riscv_vfsub_vv_f32m2(x2, xm1, vl), 0.5f, vl),
                                                   __riscv_vfmul_vf_f32m2(__riscv_vfsub_vv_f32m2(x0, x1, vl), 1.5f, vl), vl);
    vfloat32m2_t y = __riscv_vfadd_vv_f32m2(__riscv_vfmul_vv_f32m2(c3, t, vl), c2, vl);
    y = __riscv_vfadd_vv_f32m2(__riscv_vfmul_vv_f32m2(y, t, vl), c1, vl);
    return __riscv_vfadd_vv_f32m2(__riscv_vfmul_vv_f32m2(y, t, vl), x0, vl);
}

inline vfloat32m2_t sample(const Walk50& w, const std::uint8_t* base, vuint32m2_t idx, std::size_t vl) {
    // idx: the sample's index from `base` (sample -pre); bytes = idx for 8-bit, 2·idx for 16-bit
    return w.pcm8 ? pcm8(base, idx, vl) : pcm16(base, __riscv_vsll_vx_u32m2(idx, 1, vl), vl);
}

// Lane k's position (k from the walk's current position).
inline std::int32_t lane_pos(const Walk50& w, std::int32_t inc_int, std::uint32_t inc_frac, std::size_t k) {
    const std::uint64_t inc = static_cast<std::uint64_t>(static_cast<std::uint32_t>(inc_int)) << 32 | inc_frac;
    return w.pos + static_cast<std::int32_t>((static_cast<std::uint64_t>(w.frac) + k * inc) >> 32);
}
// The samples lanes k .. k + vl - 1 read for linear interpolation (the first x0 to the last x1).
inline std::size_t span_of(const Walk50& w, std::int32_t inc_int, std::uint32_t inc_frac, std::size_t k, std::size_t vl) {
    return static_cast<std::size_t>(lane_pos(w, inc_int, inc_frac, k + vl - 1) - lane_pos(w, inc_int, inc_frac, k) + 2);
}

// m samples (all with their points inside [-pre, U)) into out, from w's position; w is not advanced.
void segment(const Walk50& w, std::int32_t inc_int, std::uint32_t inc_frac, bool hermite, float* out, std::size_t m) {
    const std::size_t bytes = w.pcm8 ? 1 : 2;
    const std::uint8_t* base = w.wave + w.loop_start - static_cast<std::ptrdiff_t>(bytes) * w.pre;   // sample -pre
    for (std::size_t k = 0, vl; k < m; k += vl) {
        vl = __riscv_vsetvl_e32m2(m - k);
        vint32m2_t lpos;
        vuint32m2_t lfrac;
        walk_lanes(w.pos, w.frac, inc_int, inc_frac, static_cast<std::uint32_t>(k), vl, lpos, lfrac);
        const vuint32m2_t i0 = __riscv_vreinterpret_v_i32m2_u32m2(__riscv_vadd_vx_i32m2(lpos, w.pre, vl));
        const vfloat32m2_t t = __riscv_vfmul_vf_f32m2(__riscv_vfcvt_f_xu_v_f32m2(lfrac, vl), 1.0f / 4294967296.0f, vl);
        vfloat32m2_t y;
#if !defined(__riscv_xtheadvector)
        if (w.pcm8) {
            // 8-bit samples: the same, the bytes at LMUL 1/2
            if (hermite) {
                const vuint8mf2x4_t p4 = __riscv_vluxseg4ei32_v_u8mf2x4(base, __riscv_vsub_vx_u32m2(i0, 1, vl), vl);
                y = hermite4(u8_to_f32(__riscv_vget_v_u8mf2x4_u8mf2(p4, 0), vl), u8_to_f32(__riscv_vget_v_u8mf2x4_u8mf2(p4, 1), vl),
                             u8_to_f32(__riscv_vget_v_u8mf2x4_u8mf2(p4, 2), vl), u8_to_f32(__riscv_vget_v_u8mf2x4_u8mf2(p4, 3), vl), t, vl);
            } else if (const std::size_t span = span_of(w, inc_int, inc_frac, k, vl); span && span <= __riscv_vsetvlmax_e8m1()) {
                // The chunk's points within `span` consecutive bytes: one contiguous load, each lane's pair
                // picked out of the register (vrgather), no memory gather.
                const std::int32_t p0 = lane_pos(w, inc_int, inc_frac, k);
                const vuint8m1_t win = __riscv_vle8_v_u8m1(base + (p0 + w.pre), span);
                const vuint16m2_t rel = __riscv_vnsrl_wx_u16m2(__riscv_vreinterpret_v_i32m4_u32m4(__riscv_vlmul_ext_v_i32m2_i32m4(
                                            __riscv_vsub_vx_i32m2(lpos, p0, vl))), 0, vl);
                const vuint8m1_t g0 = __riscv_vrgatherei16_vv_u8m1(win, rel, vl);
                const vuint8m1_t g1 = __riscv_vrgatherei16_vv_u8m1(win, __riscv_vadd_vx_u16m2(rel, 1, vl), vl);
                const vfloat32m2_t x0 = u8_to_f32(__riscv_vlmul_trunc_v_u8m1_u8mf2(g0), vl);
                const vfloat32m2_t x1 = u8_to_f32(__riscv_vlmul_trunc_v_u8m1_u8mf2(g1), vl);
                y = __riscv_vfadd_vv_f32m2(x0, __riscv_vfmul_vv_f32m2(__riscv_vfsub_vv_f32m2(x1, x0, vl), t, vl), vl);
            } else {
                const vuint8mf2x2_t p2 = __riscv_vluxseg2ei32_v_u8mf2x2(base, i0, vl);
                const vfloat32m2_t x0 = u8_to_f32(__riscv_vget_v_u8mf2x2_u8mf2(p2, 0), vl);
                const vfloat32m2_t x1 = u8_to_f32(__riscv_vget_v_u8mf2x2_u8mf2(p2, 1), vl);
                y = __riscv_vfadd_vv_f32m2(x0, __riscv_vfmul_vv_f32m2(__riscv_vfsub_vv_f32m2(x1, x0, vl), t, vl), vl);
            }
            __riscv_vse32_v_f32m2(out + k, y, vl);
            continue;
        }
        {
            // 16-bit samples: the points of each position in one segmented gather (x0, x1 -- or xm1 .. x2),
            // half the addresses of separate gathers.
            if (hermite) {
                const vuint32m2_t off = __riscv_vsll_vx_u32m2(__riscv_vsub_vx_u32m2(i0, 1, vl), 1, vl);
                const vuint16m1x4_t p4 = __riscv_vluxseg4ei32_v_u16m1x4(reinterpret_cast<const std::uint16_t*>(base), off, vl);
                const vfloat32m2_t xm1 = to_f32(__riscv_vget_v_u16m1x4_u16m1(p4, 0), vl);
                const vfloat32m2_t x0 = to_f32(__riscv_vget_v_u16m1x4_u16m1(p4, 1), vl);
                const vfloat32m2_t x1 = to_f32(__riscv_vget_v_u16m1x4_u16m1(p4, 2), vl);
                const vfloat32m2_t x2 = to_f32(__riscv_vget_v_u16m1x4_u16m1(p4, 3), vl);
                y = hermite4(xm1, x0, x1, x2, t, vl);
            } else {
                const vuint16m1x2_t p2 = __riscv_vluxseg2ei32_v_u16m1x2(reinterpret_cast<const std::uint16_t*>(base),
                                                                          __riscv_vsll_vx_u32m2(i0, 1, vl), vl);
                const vfloat32m2_t x0 = to_f32(__riscv_vget_v_u16m1x2_u16m1(p2, 0), vl);
                const vfloat32m2_t x1 = to_f32(__riscv_vget_v_u16m1x2_u16m1(p2, 1), vl);
                y = __riscv_vfadd_vv_f32m2(x0, __riscv_vfmul_vv_f32m2(__riscv_vfsub_vv_f32m2(x1, x0, vl), t, vl), vl);
            }
            __riscv_vse32_v_f32m2(out + k, y, vl);
            continue;
        }
#endif
        if (hermite) {
            const vfloat32m2_t xm1 = sample(w, base, __riscv_vsub_vx_u32m2(i0, 1, vl), vl);
            const vfloat32m2_t x0 = sample(w, base, i0, vl);
            const vfloat32m2_t x1 = sample(w, base, __riscv_vadd_vx_u32m2(i0, 1, vl), vl);
            const vfloat32m2_t x2 = sample(w, base, __riscv_vadd_vx_u32m2(i0, 2, vl), vl);
            y = hermite4(xm1, x0, x1, x2, t, vl);
        } else {
            const vfloat32m2_t x0 = sample(w, base, i0, vl);
            const vfloat32m2_t x1 = sample(w, base, __riscv_vadd_vx_u32m2(i0, 1, vl), vl);
            y = __riscv_vfadd_vv_f32m2(x0, __riscv_vfmul_vv_f32m2(__riscv_vfsub_vv_f32m2(x1, x0, vl), t, vl), vl);
        }
        __riscv_vse32_v_f32m2(out + k, y, vl);
    }
}

} // namespace

void resample50(Walk50& w, std::int32_t inc_int, std::uint32_t inc_frac, bool hermite, float* out, std::size_t n) {
    const std::uint64_t inc = static_cast<std::uint64_t>(static_cast<std::uint32_t>(inc_int)) << 32 | inc_frac;
    const std::int32_t lo = hermite ? 1 : 0, hi = hermite ? 2 : 1;   // the points around a position
    const std::size_t bytes = w.pcm8 ? 1 : 2;
    // Samples the wave holds from the loop start (for a one-shot, whose walk ends at position 0 but
    // whose last points may reach past it).
    const std::int64_t avail = w.wave_size > w.loop_start ? static_cast<std::int64_t>((w.wave_size - w.loop_start) / bytes) : 0;
    std::size_t k = 0;
    while (k < n) {
        if (w.ended) {
            for (; k < n; ++k) out[k] = 0.0f;
            return;
        }
        // The run whose points all lie inside [-pre, U): read directly, no wrap. A loop: U = loop_len
        // (as the reference's direct path); a one-shot: positions below 0 (it ends there), and the
        // points inside the wave.
        std::size_t m = 0;
        if (w.pos - lo >= -w.pre) {
            if (w.loop_len > 0) {
                m = run_below(w.pos, w.frac, inc, static_cast<std::int64_t>(w.loop_len) - hi, n - k);
            } else {
                const std::int64_t cap = avail - hi < 0 ? avail - hi : 0;   // pos < 0 and pos + hi < avail
                m = run_below(w.pos, w.frac, inc, cap, n - k);
            }
        }
        if (m == 0) {   // a position at an edge: the reference, one sample
            ref::resample50(w, inc_int, inc_frac, hermite, out + k, 1);
            ++k;
            continue;
        }
        segment(w, inc_int, inc_frac, hermite, out + k, m);
        // the walk after m steps, then the reference's wrap / end
        const std::uint64_t acc = static_cast<std::uint64_t>(w.frac) + static_cast<std::uint64_t>(m) * inc;
        w.frac = static_cast<std::uint32_t>(acc);
        w.pos += static_cast<std::int32_t>(acc >> 32);
        if (w.loop_len == 0) {
            if (w.pos >= 0) w.ended = true;
        } else {
            while (w.pos >= w.loop_len) {
                const std::uint64_t g = static_cast<std::uint64_t>(w.frac) + w.loop_frac;
                w.frac = static_cast<std::uint32_t>(g);
                w.pos += static_cast<std::int32_t>(g >> 32) - w.loop_len;
            }
        }
        k += m;
    }
}

void mix_ramps(const float* x, float* const* dst, const float* g, const float* dg, std::size_t buses, std::size_t n) {
    if (!buses) return;
    for (std::size_t i = 0, vl; i < n; i += vl) {
        vl = __riscv_vsetvl_e32m2(n - i);
        const vfloat32m2_t xv = __riscv_vle32_v_f32m2(x + i, vl);
        const vfloat32m2_t iv = __riscv_vfcvt_f_xu_v_f32m2(__riscv_vadd_vx_u32m2(__riscv_vid_v_u32m2(vl), static_cast<std::uint32_t>(i), vl), vl);
        for (std::size_t b = 0; b < buses; ++b) {
            // g[b] + i·dg[b], then dst += x · that
            const vfloat32m2_t gv = __riscv_vfmacc_vf_f32m2(__riscv_vfmv_v_f_f32m2(g[b], vl), dg[b], iv, vl);
            float* d = dst[b] + i;
            __riscv_vse32_v_f32m2(d, __riscv_vfmacc_vv_f32m2(__riscv_vle32_v_f32m2(d, vl), xv, gv, vl), vl);
        }
    }
}

void svf50_lanes(float* x, std::size_t stride, float* const* band, float* const* low, const float* f, const float* df,
                 const float* q, std::size_t lanes, std::size_t n) {
    // A voice a lane, the samples in time: lane l's sample i at x[l·stride + i] (a strided load and
    // store). Each lane does the reference's operations in its order, unfused, f stepping by df after
    // each sample as the reference's does: exact.
    const std::ptrdiff_t sb = static_cast<std::ptrdiff_t>(stride * sizeof(float));
    for (std::size_t l0 = 0, vl; l0 < lanes; l0 += vl) {
        vl = __riscv_vsetvl_e32m2(lanes - l0);
        alignas(64) float bb[kBatch], ll[kBatch];
        for (std::size_t j = 0; j < vl; ++j) bb[j] = *band[l0 + j], ll[j] = *low[l0 + j];
        vfloat32m2_t b = __riscv_vle32_v_f32m2(bb, vl), lo = __riscv_vle32_v_f32m2(ll, vl);
        vfloat32m2_t fv = __riscv_vle32_v_f32m2(f + l0, vl);
        const vfloat32m2_t dfv = __riscv_vle32_v_f32m2(df + l0, vl), qv = __riscv_vle32_v_f32m2(q + l0, vl);
        float* col = x + l0 * stride;
        for (std::size_t i = 0; i < n; ++i, ++col) {
            const vfloat32m2_t xv = __riscv_vlse32_v_f32m2(col, sb, vl);
            // b += f (x - q b - l); l += f b; x = l; f += df
            const vfloat32m2_t e = __riscv_vfsub_vv_f32m2(__riscv_vfsub_vv_f32m2(xv, __riscv_vfmul_vv_f32m2(qv, b, vl), vl), lo, vl);
            b = __riscv_vfadd_vv_f32m2(b, __riscv_vfmul_vv_f32m2(fv, e, vl), vl);
            lo = __riscv_vfadd_vv_f32m2(lo, __riscv_vfmul_vv_f32m2(fv, b, vl), vl);
            __riscv_vsse32_v_f32m2(col, sb, lo, vl);
            fv = __riscv_vfadd_vv_f32m2(fv, dfv, vl);
        }
        __riscv_vse32_v_f32m2(bb, b, vl);
        __riscv_vse32_v_f32m2(ll, lo, vl);
        for (std::size_t j = 0; j < vl; ++j) *band[l0 + j] = bb[j], *low[l0 + j] = ll[j];
    }
}

} // namespace hvyxg::accel::rvv

#endif
