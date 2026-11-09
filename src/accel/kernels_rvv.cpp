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

// Unsigned 16-bit samples at byte offsets `off` from `base`, as floats in [-1, 1). RVV 0.7.1's indices
// are as wide as its elements (no 32-bit offsets to 16-bit samples), so there they are read one by one
// for now (to do on the C920: aligned 32-bit word gathers and a shift a lane).
inline vfloat32m2_t pcm16(const std::uint8_t* base, vuint32m2_t off, std::size_t vl) {
#if defined(__riscv_xtheadvector)
    alignas(64) std::uint32_t offs[256];
    alignas(64) std::uint16_t s16[256];
    __riscv_vse32_v_u32m2(offs, off, vl);
    for (std::size_t j = 0; j < vl; ++j) s16[j] = static_cast<std::uint16_t>(base[offs[j]] | base[offs[j] + 1] << 8);
    const vuint16m1_t u = __riscv_vle16_v_u16m1(s16, vl);
#else
    const vuint16m1_t u = __riscv_vluxei32_v_u16m1(reinterpret_cast<const std::uint16_t*>(base), off, vl);
#endif
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

#if !defined(__riscv_xtheadvector)
// 8-bit samples, linear, at LMUL 4 (64 lanes at VLEN 512): the in-order vector unit's latency per
// dependent chain is the cost here, so twice the lanes a chain halves it. The walk as walk_lanes; each
// lane's pair from the chunk's bytes (one contiguous load, the window as 16-bit words of consecutive
// bytes, one gather of words) where they fit a register group, else a segmented gather.
__attribute__((noinline)) void segment8_linear(const Walk50& w, std::int32_t inc_int, std::uint32_t inc_frac, float* out, std::size_t m) {
    const std::uint8_t* base = w.wave + w.loop_start - w.pre;   // sample -pre
    for (std::size_t k = 0, vl; k < m; k += vl) {
        vl = __riscv_vsetvl_e32m4(m - k);
        const vuint32m4_t kk = __riscv_vadd_vx_u32m4(__riscv_vid_v_u32m4(vl), static_cast<std::uint32_t>(k), vl);
        const vuint32m4_t lo = __riscv_vmul_vx_u32m4(kk, inc_frac, vl);
        const vuint32m4_t hi = __riscv_vmulhu_vx_u32m4(kk, inc_frac, vl);
        const vuint32m4_t f = __riscv_vadd_vx_u32m4(lo, w.frac, vl);
        const vbool8_t carry = __riscv_vmsltu_vx_u32m4_b8(f, w.frac, vl);
        vuint32m4_t whole = __riscv_vadd_vv_u32m4(__riscv_vmul_vx_u32m4(kk, static_cast<std::uint32_t>(inc_int), vl), hi, vl);
        whole = __riscv_vadd_vx_u32m4_m(carry, whole, 1, vl);
        const vint32m4_t lpos = __riscv_vadd_vx_i32m4(__riscv_vreinterpret_v_u32m4_i32m4(whole), w.pos, vl);
        const vfloat32m4_t t = __riscv_vfmul_vf_f32m4(__riscv_vfcvt_f_xu_v_f32m4(f, vl), 1.0f / 4294967296.0f, vl);
        vuint8m1_t b0, b1;
        const std::size_t span = span_of(w, inc_int, inc_frac, k, vl);
        if (span && span <= __riscv_vsetvlmax_e8m2()) {
            const std::int32_t p0 = lane_pos(w, inc_int, inc_frac, k);
            const vuint8m2_t win = __riscv_vle8_v_u8m2(base + (p0 + w.pre), span);
            const vuint8m2_t win1 = __riscv_vslidedown_vx_u8m2(win, 1, span);
            const vuint16m4_t pairs = __riscv_vor_vv_u16m4(__riscv_vwaddu_vx_u16m4(win, 0, span),
                                                          __riscv_vsll_vx_u16m4(__riscv_vwaddu_vx_u16m4(win1, 0, span), 8, span), span);
            const vuint16m2_t rel = __riscv_vnsrl_wx_u16m2(__riscv_vreinterpret_v_i32m4_u32m4(__riscv_vsub_vx_i32m4(lpos, p0, vl)), 0, vl);
            const vuint16m4_t g = __riscv_vrgatherei16_vv_u16m4(pairs, __riscv_vlmul_ext_v_u16m2_u16m4(rel), vl);
            const vuint16m2_t g2 = __riscv_vlmul_trunc_v_u16m4_u16m2(g);
            b0 = __riscv_vnsrl_wx_u8m1(g2, 0, vl);
            b1 = __riscv_vnsrl_wx_u8m1(g2, 8, vl);
        } else {
            const vuint8m1x2_t p2 = __riscv_vluxseg2ei32_v_u8m1x2(base, __riscv_vreinterpret_v_i32m4_u32m4(__riscv_vadd_vx_i32m4(lpos, w.pre, vl)), vl);
            b0 = __riscv_vget_v_u8m1x2_u8m1(p2, 0);
            b1 = __riscv_vget_v_u8m1x2_u8m1(p2, 1);
        }
        const auto to_f = [&](vuint8m1_t u) {
            const vint8m1_t b = __riscv_vreinterpret_v_u8m1_i8m1(__riscv_vxor_vx_u8m1(u, 0x80, vl));
            return __riscv_vfmul_vf_f32m4(__riscv_vfwcvt_f_x_v_f32m4(__riscv_vwadd_vx_i16m2(b, 0, vl), vl), 1.0f / 128.0f, vl);
        };
        const vfloat32m4_t x0 = to_f(b0), x1 = to_f(b1);
        __riscv_vse32_v_f32m4(out + k, __riscv_vfadd_vv_f32m4(x0, __riscv_vfmul_vv_f32m4(__riscv_vfsub_vv_f32m4(x1, x0, vl), t, vl), vl), vl);
    }
}
#endif

#if !defined(__riscv_xtheadvector)
// The other runs at LMUL 4 too (for the same latency's sake): each position's points in one segmented
// gather (x0, x1, or xm1 .. x2), 8- or 16-bit, the interpolation as the reference's, unfused.
__attribute__((noinline)) void segment_m4(const Walk50& w, std::int32_t inc_int, std::uint32_t inc_frac, bool hermite, float* out, std::size_t m) {
    const std::size_t bytes = w.pcm8 ? 1 : 2;
    const std::uint8_t* base = w.wave + w.loop_start - static_cast<std::ptrdiff_t>(bytes) * w.pre;   // sample -pre
    for (std::size_t k = 0, vl; k < m; k += vl) {
        vl = __riscv_vsetvl_e32m4(m - k);
        const vuint32m4_t kk = __riscv_vadd_vx_u32m4(__riscv_vid_v_u32m4(vl), static_cast<std::uint32_t>(k), vl);
        const vuint32m4_t lo = __riscv_vmul_vx_u32m4(kk, inc_frac, vl);
        const vuint32m4_t hi = __riscv_vmulhu_vx_u32m4(kk, inc_frac, vl);
        const vuint32m4_t f = __riscv_vadd_vx_u32m4(lo, w.frac, vl);
        const vbool8_t carry = __riscv_vmsltu_vx_u32m4_b8(f, w.frac, vl);
        vuint32m4_t whole = __riscv_vadd_vv_u32m4(__riscv_vmul_vx_u32m4(kk, static_cast<std::uint32_t>(inc_int), vl), hi, vl);
        whole = __riscv_vadd_vx_u32m4_m(carry, whole, 1, vl);
        // index from sample -pre (the first point: x0, or xm1 for Hermite)
        vuint32m4_t i0 = __riscv_vadd_vx_u32m4(whole, static_cast<std::uint32_t>(w.pos + w.pre - (hermite ? 1 : 0)), vl);
        if (!w.pcm8) i0 = __riscv_vsll_vx_u32m4(i0, 1, vl);
        const vfloat32m4_t t = __riscv_vfmul_vf_f32m4(__riscv_vfcvt_f_xu_v_f32m4(f, vl), 1.0f / 4294967296.0f, vl);
        const auto f8 = [&](vuint8m1_t u) {
            const vint8m1_t b = __riscv_vreinterpret_v_u8m1_i8m1(__riscv_vxor_vx_u8m1(u, 0x80, vl));
            return __riscv_vfmul_vf_f32m4(__riscv_vfwcvt_f_x_v_f32m4(__riscv_vwadd_vx_i16m2(b, 0, vl), vl), 1.0f / 128.0f, vl);
        };
        const auto f16 = [&](vuint16m2_t u) {
            const vint16m2_t s16 = __riscv_vreinterpret_v_u16m2_i16m2(__riscv_vxor_vx_u16m2(u, 0x8000, vl));
            return __riscv_vfmul_vf_f32m4(__riscv_vfwcvt_f_x_v_f32m4(s16, vl), 1.0f / 32768.0f, vl);
        };
        vfloat32m4_t y;
        if (hermite) {
            vfloat32m4_t xm1, x0, x1, x2;
            if (w.pcm8) {
                const vuint8m1x4_t p = __riscv_vluxseg4ei32_v_u8m1x4(base, i0, vl);
                xm1 = f8(__riscv_vget_v_u8m1x4_u8m1(p, 0)), x0 = f8(__riscv_vget_v_u8m1x4_u8m1(p, 1));
                x1 = f8(__riscv_vget_v_u8m1x4_u8m1(p, 2)), x2 = f8(__riscv_vget_v_u8m1x4_u8m1(p, 3));
            } else {
                const vuint16m2x4_t p = __riscv_vluxseg4ei32_v_u16m2x4(reinterpret_cast<const std::uint16_t*>(base), i0, vl);
                xm1 = f16(__riscv_vget_v_u16m2x4_u16m2(p, 0)), x0 = f16(__riscv_vget_v_u16m2x4_u16m2(p, 1));
                x1 = f16(__riscv_vget_v_u16m2x4_u16m2(p, 2)), x2 = f16(__riscv_vget_v_u16m2x4_u16m2(p, 3));
            }
            // the reference's Hermite: c1 = 0.5 (x1 - xm1); c2 = xm1 - 2.5 x0 + 2 x1 - 0.5 x2;
            // c3 = 0.5 (x2 - xm1) + 1.5 (x0 - x1); ((c3 t + c2) t + c1) t + x0
            const vfloat32m4_t c1 = __riscv_vfmul_vf_f32m4(__riscv_vfsub_vv_f32m4(x1, xm1, vl), 0.5f, vl);
            vfloat32m4_t c2 = __riscv_vfsub_vv_f32m4(xm1, __riscv_vfmul_vf_f32m4(x0, 2.5f, vl), vl);
            c2 = __riscv_vfadd_vv_f32m4(c2, __riscv_vfmul_vf_f32m4(x1, 2.0f, vl), vl);
            c2 = __riscv_vfsub_vv_f32m4(c2, __riscv_vfmul_vf_f32m4(x2, 0.5f, vl), vl);
            const vfloat32m4_t c3 = __riscv_vfadd_vv_f32m4(__riscv_vfmul_vf_f32m4(__riscv_vfsub_vv_f32m4(x2, xm1, vl), 0.5f, vl),
                                                           __riscv_vfmul_vf_f32m4(__riscv_vfsub_vv_f32m4(x0, x1, vl), 1.5f, vl), vl);
            y = __riscv_vfadd_vv_f32m4(__riscv_vfmul_vv_f32m4(c3, t, vl), c2, vl);
            y = __riscv_vfadd_vv_f32m4(__riscv_vfmul_vv_f32m4(y, t, vl), c1, vl);
            y = __riscv_vfadd_vv_f32m4(__riscv_vfmul_vv_f32m4(y, t, vl), x0, vl);
        } else {
            vfloat32m4_t x0, x1;
            if (w.pcm8) {
                const vuint8m1x2_t p = __riscv_vluxseg2ei32_v_u8m1x2(base, i0, vl);
                x0 = f8(__riscv_vget_v_u8m1x2_u8m1(p, 0)), x1 = f8(__riscv_vget_v_u8m1x2_u8m1(p, 1));
            } else {
                const vuint16m2x2_t p = __riscv_vluxseg2ei32_v_u16m2x2(reinterpret_cast<const std::uint16_t*>(base), i0, vl);
                x0 = f16(__riscv_vget_v_u16m2x2_u16m2(p, 0)), x1 = f16(__riscv_vget_v_u16m2x2_u16m2(p, 1));
            }
            y = __riscv_vfadd_vv_f32m4(x0, __riscv_vfmul_vv_f32m4(__riscv_vfsub_vv_f32m4(x1, x0, vl), t, vl), vl);
        }
        __riscv_vse32_v_f32m4(out + k, y, vl);
    }
}
#endif

// m samples (all with their points inside [-pre, U)) into out, from w's position; w is not advanced.
void segment(const Walk50& w, std::int32_t inc_int, std::uint32_t inc_frac, bool hermite, float* out, std::size_t m) {
#if !defined(__riscv_xtheadvector)
    if (w.pcm8 && !hermite) segment8_linear(w, inc_int, inc_frac, out, m);
    else segment_m4(w, inc_int, inc_frac, hermite, out, m);
    return;
#endif
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
                // Each lane's pair at once: the window as 16-bit words of consecutive bytes (byte j, then
                // byte j + 1 above it), one gather of words, the two bytes split by narrowing shifts.
                const std::int32_t p0 = lane_pos(w, inc_int, inc_frac, k);
                const vuint8m1_t win = __riscv_vle8_v_u8m1(base + (p0 + w.pre), span);
                const vuint8m1_t win1 = __riscv_vslidedown_vx_u8m1(win, 1, span);
                const vuint16m2_t pairs = __riscv_vor_vv_u16m2(__riscv_vwaddu_vx_u16m2(win, 0, span),
                                                              __riscv_vsll_vx_u16m2(__riscv_vwaddu_vx_u16m2(win1, 0, span), 8, span), span);
                const vuint16m2_t rel = __riscv_vnsrl_wx_u16m2(__riscv_vreinterpret_v_i32m4_u32m4(__riscv_vlmul_ext_v_i32m2_i32m4(
                                            __riscv_vsub_vx_i32m2(lpos, p0, vl))), 0, vl);
                const vuint16m2_t g = __riscv_vrgatherei16_vv_u16m2(pairs, rel, vl);
                const vfloat32m2_t x0 = u8_to_f32(__riscv_vnsrl_wx_u8mf2(__riscv_vlmul_trunc_v_u16m2_u16m1(g), 0, vl), vl);
                const vfloat32m2_t x1 = u8_to_f32(__riscv_vnsrl_wx_u8mf2(__riscv_vlmul_trunc_v_u16m2_u16m1(g), 8, vl), vl);
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

namespace {

// A recursive-in-time filter across voices: lane l is voice l, its block at x + l·stride, its two state
// words at *s1[l], *s2[l]. Step(x, s1, s2, f, q, vl) -> y updates the state and gives the output; f steps
// by df after each sample, as the references' do. RVV 1.0 reads and writes four consecutive samples of
// every lane in one strided segment access (vlsseg4 / vssseg4: a lane's samples are contiguous in its
// row), instead of an element per sample; 0.7.1 takes the plain strided path.
template <class Step>
void lanes_filter(float* x, std::size_t stride, float* const* s1, float* const* s2, const float* f, const float* df,
                  const float* q, std::size_t lanes, std::size_t n, Step step) {
    const std::ptrdiff_t sb = static_cast<std::ptrdiff_t>(stride * sizeof(float));
    for (std::size_t l0 = 0, vl; l0 < lanes; l0 += vl) {
        vl = __riscv_vsetvl_e32m2(lanes - l0);
        alignas(64) float a1[kBatch], a2[kBatch];
        for (std::size_t j = 0; j < vl; ++j) a1[j] = *s1[l0 + j], a2[j] = *s2[l0 + j];
        vfloat32m2_t v1 = __riscv_vle32_v_f32m2(a1, vl), v2 = __riscv_vle32_v_f32m2(a2, vl);
        vfloat32m2_t fv = __riscv_vle32_v_f32m2(f + l0, vl);
        const vfloat32m2_t dfv = __riscv_vle32_v_f32m2(df + l0, vl), qv = __riscv_vle32_v_f32m2(q + l0, vl);
        float* col = x + l0 * stride;
        std::size_t i = 0;
#if !defined(__riscv_xtheadvector)
        for (; i + 4 <= n; i += 4, col += 4) {
            vfloat32m2x4_t t = __riscv_vlsseg4e32_v_f32m2x4(col, sb, vl);
            vfloat32m2_t y0 = step(__riscv_vget_v_f32m2x4_f32m2(t, 0), v1, v2, fv, qv, vl);
            fv = __riscv_vfadd_vv_f32m2(fv, dfv, vl);
            vfloat32m2_t y1 = step(__riscv_vget_v_f32m2x4_f32m2(t, 1), v1, v2, fv, qv, vl);
            fv = __riscv_vfadd_vv_f32m2(fv, dfv, vl);
            vfloat32m2_t y2 = step(__riscv_vget_v_f32m2x4_f32m2(t, 2), v1, v2, fv, qv, vl);
            fv = __riscv_vfadd_vv_f32m2(fv, dfv, vl);
            vfloat32m2_t y3 = step(__riscv_vget_v_f32m2x4_f32m2(t, 3), v1, v2, fv, qv, vl);
            fv = __riscv_vfadd_vv_f32m2(fv, dfv, vl);
            t = __riscv_vset_v_f32m2_f32m2x4(t, 0, y0);
            t = __riscv_vset_v_f32m2_f32m2x4(t, 1, y1);
            t = __riscv_vset_v_f32m2_f32m2x4(t, 2, y2);
            t = __riscv_vset_v_f32m2_f32m2x4(t, 3, y3);
            __riscv_vssseg4e32_v_f32m2x4(col, sb, t, vl);
        }
#endif
        for (; i < n; ++i, ++col) {
            const vfloat32m2_t y = step(__riscv_vlse32_v_f32m2(col, sb, vl), v1, v2, fv, qv, vl);
            __riscv_vsse32_v_f32m2(col, sb, y, vl);
            fv = __riscv_vfadd_vv_f32m2(fv, dfv, vl);
        }
        __riscv_vse32_v_f32m2(a1, v1, vl);
        __riscv_vse32_v_f32m2(a2, v2, vl);
        for (std::size_t j = 0; j < vl; ++j) *s1[l0 + j] = a1[j], *s2[l0 + j] = a2[j];
    }
}

} // namespace

void svf50_lanes(float* x, std::size_t stride, float* const* band, float* const* low, const float* f, const float* df,
                 const float* q, std::size_t lanes, std::size_t n) {
    // b += f (x - q b - l); l += f b; y = l -- the reference's operations in its order, unfused: exact.
    lanes_filter(x, stride, band, low, f, df, q, lanes, n,
                 [](vfloat32m2_t xv, vfloat32m2_t& b, vfloat32m2_t& lo, vfloat32m2_t fv, vfloat32m2_t qv, std::size_t vl) {
                     const vfloat32m2_t e = __riscv_vfsub_vv_f32m2(__riscv_vfsub_vv_f32m2(xv, __riscv_vfmul_vv_f32m2(qv, b, vl), vl), lo, vl);
                     b = __riscv_vfadd_vv_f32m2(b, __riscv_vfmul_vv_f32m2(fv, e, vl), vl);
                     lo = __riscv_vfadd_vv_f32m2(lo, __riscv_vfmul_vv_f32m2(fv, b, vl), vl);
                     return lo;
                 });
}

void svf_le_lanes(float* x, std::size_t stride, float* const* lp, float* const* bp, const float* f, const float* df,
                  const float* q, std::size_t lanes, std::size_t n) {
    // qq = min(q, max(3 - 2f, 2 - f)); y = lp; bp += f (x - qq bp - lp); lp += f bp -- exact, as above.
    lanes_filter(x, stride, bp, lp, f, df, q, lanes, n,
                 [](vfloat32m2_t xv, vfloat32m2_t& b, vfloat32m2_t& lo, vfloat32m2_t fv, vfloat32m2_t qv, std::size_t vl) {
                     const vfloat32m2_t lim = __riscv_vfmax_vv_f32m2(__riscv_vfrsub_vf_f32m2(__riscv_vfmul_vf_f32m2(fv, 2.0f, vl), 3.0f, vl),
                                                                     __riscv_vfrsub_vf_f32m2(fv, 2.0f, vl), vl);
                     const vfloat32m2_t qq = __riscv_vfmin_vv_f32m2(qv, lim, vl);
                     const vfloat32m2_t y = lo;
                     const vfloat32m2_t e = __riscv_vfsub_vv_f32m2(__riscv_vfsub_vv_f32m2(xv, __riscv_vfmul_vv_f32m2(qq, b, vl), vl), lo, vl);
                     b = __riscv_vfadd_vv_f32m2(b, __riscv_vfmul_vv_f32m2(fv, e, vl), vl);
                     lo = __riscv_vfadd_vv_f32m2(lo, __riscv_vfmul_vv_f32m2(fv, b, vl), vl);
                     return y;
                 });
}

void peak_ramp(const float* x, float a, float d, float* peak, std::size_t n) {
    // max(*peak, |(a + i·d) · x[i]|), the ramp in closed form (the reference adds d sample by sample).
    // The reference's max(hi, -lo) -- hi from *peak, lo from 0 -- is max(*peak, max |y|): one reduction.
    // The result is stored from the vector unit: a scalar read (vfmv.f.s) would wait for every vector
    // instruction before it, the resampler's gathers included.
    vfloat32m1_t m = __riscv_vle32_v_f32m1(peak, 1);
    for (std::size_t i = 0, vl; i < n; i += vl) {
        vl = __riscv_vsetvl_e32m2(n - i);
        const vfloat32m2_t iv = __riscv_vfcvt_f_xu_v_f32m2(__riscv_vadd_vx_u32m2(__riscv_vid_v_u32m2(vl), static_cast<std::uint32_t>(i), vl), vl);
        const vfloat32m2_t av = __riscv_vfmacc_vf_f32m2(__riscv_vfmv_v_f_f32m2(a, vl), d, iv, vl);
        m = __riscv_vfredmax_vs_f32m2_f32m1(__riscv_vfabs_v_f32m2(__riscv_vfmul_vv_f32m2(av, __riscv_vle32_v_f32m2(x + i, vl), vl), vl), m, vl);
    }
    __riscv_vse32_v_f32m1(peak, m, 1);
}

namespace {
// The S-YXG2006LE's run: m samples of its walk, all inside the direct range, into out. Linear: s0, s1 in
// one segmented gather (16-bit, signed), the 10-bit weight; Hermite: the four points, the same
// polynomial as the S-YXG50's. Exact. Positions relative to the run's start fit 32 bits (the caller
// checks the step).
#if !defined(__riscv_xtheadvector)
// segment_le at LMUL 4 (a 64-sample block in one chunk at VLEN 512: the in-order unit's chain latency
// once, not twice): the points in one segmented gather, as below.
__attribute__((noinline)) void segment_le_m4(const WalkLE& w, std::int32_t inc, bool hermite, float* out, std::size_t m) {
    const std::int16_t* base = w.base + w.pos - (hermite ? 1 : 0);
    const auto frac = static_cast<std::uint32_t>(w.frac), uinc = static_cast<std::uint32_t>(inc);
    for (std::size_t k = 0, vl; k < m; k += vl) {
        vl = __riscv_vsetvl_e32m4(m - k);
        const vuint32m4_t acc = __riscv_vadd_vx_u32m4(__riscv_vmul_vx_u32m4(__riscv_vadd_vx_u32m4(__riscv_vid_v_u32m4(vl),
                                                                            static_cast<std::uint32_t>(k), vl), uinc, vl), frac, vl);
        const vuint32m4_t off = __riscv_vsll_vx_u32m4(__riscv_vsrl_vx_u32m4(acc, kPhaseBitsLE, vl), 1, vl);   // bytes
        const vuint32m4_t fr = __riscv_vand_vx_u32m4(acc, (1u << kPhaseBitsLE) - 1, vl);
        const auto cv = [&](vint16m2_t v) { return __riscv_vfwcvt_f_x_v_f32m4(v, vl); };
        vfloat32m4_t y;
        if (hermite) {
            const vint16m2x4_t p4 = __riscv_vluxseg4ei32_v_i16m2x4(base, off, vl);
            const vfloat32m4_t xm1 = cv(__riscv_vget_v_i16m2x4_i16m2(p4, 0)), x0 = cv(__riscv_vget_v_i16m2x4_i16m2(p4, 1));
            const vfloat32m4_t x1 = cv(__riscv_vget_v_i16m2x4_i16m2(p4, 2)), x2 = cv(__riscv_vget_v_i16m2x4_i16m2(p4, 3));
            const vfloat32m4_t t = __riscv_vfmul_vf_f32m4(__riscv_vfcvt_f_xu_v_f32m4(fr, vl), 1.0f / 32768.0f, vl);
            const vfloat32m4_t c1 = __riscv_vfmul_vf_f32m4(__riscv_vfsub_vv_f32m4(x1, xm1, vl), 0.5f, vl);
            vfloat32m4_t c2 = __riscv_vfsub_vv_f32m4(xm1, __riscv_vfmul_vf_f32m4(x0, 2.5f, vl), vl);
            c2 = __riscv_vfadd_vv_f32m4(c2, __riscv_vfmul_vf_f32m4(x1, 2.0f, vl), vl);
            c2 = __riscv_vfsub_vv_f32m4(c2, __riscv_vfmul_vf_f32m4(x2, 0.5f, vl), vl);
            const vfloat32m4_t c3 = __riscv_vfadd_vv_f32m4(__riscv_vfmul_vf_f32m4(__riscv_vfsub_vv_f32m4(x2, xm1, vl), 0.5f, vl),
                                                           __riscv_vfmul_vf_f32m4(__riscv_vfsub_vv_f32m4(x0, x1, vl), 1.5f, vl), vl);
            y = __riscv_vfadd_vv_f32m4(__riscv_vfmul_vv_f32m4(c3, t, vl), c2, vl);
            y = __riscv_vfadd_vv_f32m4(__riscv_vfmul_vv_f32m4(y, t, vl), c1, vl);
            y = __riscv_vfadd_vv_f32m4(__riscv_vfmul_vv_f32m4(y, t, vl), x0, vl);
        } else {
            const vint16m2x2_t p2 = __riscv_vluxseg2ei32_v_i16m2x2(base, off, vl);
            const vfloat32m4_t s0 = cv(__riscv_vget_v_i16m2x2_i16m2(p2, 0)), s1 = cv(__riscv_vget_v_i16m2x2_i16m2(p2, 1));
            // w1 = 32 (frac >> 5) / 32768; v = s0 + (s1 - s0) w1
            const vfloat32m4_t w1 = __riscv_vfmul_vf_f32m4(__riscv_vfcvt_f_xu_v_f32m4(__riscv_vsll_vx_u32m4(__riscv_vsrl_vx_u32m4(fr, 5, vl), 5, vl), vl),
                                                           1.0f / 32768.0f, vl);
            y = __riscv_vfadd_vv_f32m4(s0, __riscv_vfmul_vv_f32m4(__riscv_vfsub_vv_f32m4(s1, s0, vl), w1, vl), vl);
        }
        __riscv_vse32_v_f32m4(out + k, y, vl);
    }
}
#endif

void segment_le(const WalkLE& w, std::int32_t inc, bool hermite, float* out, std::size_t m) {
#if !defined(__riscv_xtheadvector)
    segment_le_m4(w, inc, hermite, out, m);
    return;
#endif
    const std::int16_t* base = w.base + w.pos - (hermite ? 1 : 0);
    const auto frac = static_cast<std::uint32_t>(w.frac), uinc = static_cast<std::uint32_t>(inc);
    for (std::size_t k = 0, vl; k < m; k += vl) {
        vl = __riscv_vsetvl_e32m2(m - k);
        const vuint32m2_t acc = __riscv_vadd_vx_u32m2(__riscv_vmul_vx_u32m2(__riscv_vadd_vx_u32m2(__riscv_vid_v_u32m2(vl),
                                                                            static_cast<std::uint32_t>(k), vl), uinc, vl), frac, vl);
        const vuint32m2_t off = __riscv_vsll_vx_u32m2(__riscv_vsrl_vx_u32m2(acc, kPhaseBitsLE, vl), 1, vl);   // bytes
        const vuint32m2_t fr = __riscv_vand_vx_u32m2(acc, (1u << kPhaseBitsLE) - 1, vl);
        vfloat32m2_t y;
#if defined(__riscv_xtheadvector)
        alignas(64) std::uint32_t offs[256];
        alignas(64) std::int16_t pts[4][256];
        __riscv_vse32_v_u32m2(offs, off, vl);
        const std::size_t np = hermite ? 4 : 2;
        for (std::size_t j = 0; j < vl; ++j)
            for (std::size_t p = 0; p < np; ++p) pts[p][j] = base[offs[j] / 2 + p];
        const auto pt = [&](std::size_t p) { return __riscv_vfwcvt_f_x_v_f32m2(__riscv_vle16_v_i16m1(pts[p], vl), vl); };
        if (hermite) {
            y = hermite4(pt(0), pt(1), pt(2), pt(3), __riscv_vfmul_vf_f32m2(__riscv_vfcvt_f_xu_v_f32m2(fr, vl), 1.0f / 32768.0f, vl), vl);
        } else {
            const vfloat32m2_t s0 = pt(0), s1 = pt(1);
#else
        if (hermite) {
            const vint16m1x4_t p4 = __riscv_vluxseg4ei32_v_i16m1x4(base, off, vl);
            y = hermite4(__riscv_vfwcvt_f_x_v_f32m2(__riscv_vget_v_i16m1x4_i16m1(p4, 0), vl),
                         __riscv_vfwcvt_f_x_v_f32m2(__riscv_vget_v_i16m1x4_i16m1(p4, 1), vl),
                         __riscv_vfwcvt_f_x_v_f32m2(__riscv_vget_v_i16m1x4_i16m1(p4, 2), vl),
                         __riscv_vfwcvt_f_x_v_f32m2(__riscv_vget_v_i16m1x4_i16m1(p4, 3), vl),
                         __riscv_vfmul_vf_f32m2(__riscv_vfcvt_f_xu_v_f32m2(fr, vl), 1.0f / 32768.0f, vl), vl);
        } else {
            const vint16m1x2_t p2 = __riscv_vluxseg2ei32_v_i16m1x2(base, off, vl);
            const vfloat32m2_t s0 = __riscv_vfwcvt_f_x_v_f32m2(__riscv_vget_v_i16m1x2_i16m1(p2, 0), vl);
            const vfloat32m2_t s1 = __riscv_vfwcvt_f_x_v_f32m2(__riscv_vget_v_i16m1x2_i16m1(p2, 1), vl);
#endif
            // w1 = 32 (frac >> 5) / 32768; v = s0 + (s1 - s0) w1
            const vfloat32m2_t w1 = __riscv_vfmul_vf_f32m2(__riscv_vfcvt_f_xu_v_f32m2(__riscv_vsll_vx_u32m2(__riscv_vsrl_vx_u32m2(fr, 5, vl), 5, vl), vl),
                                                           1.0f / 32768.0f, vl);
            y = __riscv_vfadd_vv_f32m2(s0, __riscv_vfmul_vv_f32m2(__riscv_vfsub_vv_f32m2(s1, s0, vl), w1, vl), vl);
        }
        __riscv_vse32_v_f32m2(out + k, y, vl);
    }
}

// The S-YXG2006LE's Lanczos run: m samples whose six points (pos - 2 .. pos + 3) all lie inside the
// direct range. The weight row is lround(frac / 128) (frac ≥ 0: (frac + 64) >> 7), its six weights one
// segmented gather, the points another; the sum in the reference's order, unfused, so exact. LMUL 1:
// six fields of 32 bits fit a segment there (EMUL · NFIELDS ≤ 8).
void segment_lanczos(const WalkLE& w, std::int32_t inc, const float* lanczos, float* out, std::size_t m) {
    const std::int16_t* base = w.base + w.pos - 2;
    const auto frac = static_cast<std::uint32_t>(w.frac), uinc = static_cast<std::uint32_t>(inc);
    for (std::size_t k = 0, vl; k < m; k += vl) {
        vl = __riscv_vsetvl_e32m1(m - k);
        const vuint32m1_t acc = __riscv_vadd_vx_u32m1(__riscv_vmul_vx_u32m1(__riscv_vadd_vx_u32m1(__riscv_vid_v_u32m1(vl),
                                                                            static_cast<std::uint32_t>(k), vl), uinc, vl), frac, vl);
        const vuint32m1_t off = __riscv_vsll_vx_u32m1(__riscv_vsrl_vx_u32m1(acc, kPhaseBitsLE, vl), 1, vl);   // bytes
        const vuint32m1_t row = __riscv_vsrl_vx_u32m1(__riscv_vadd_vx_u32m1(__riscv_vand_vx_u32m1(acc, (1u << kPhaseBitsLE) - 1, vl), 64, vl), 7, vl);
        const vuint32m1_t woff = __riscv_vmul_vx_u32m1(row, 6 * sizeof(float), vl);                          // bytes
#if defined(__riscv_xtheadvector)
        alignas(64) std::uint32_t offs[256], wofs[256];
        alignas(64) float pts[6][256], wts[6][256];
        __riscv_vse32_v_u32m1(offs, off, vl);
        __riscv_vse32_v_u32m1(wofs, woff, vl);
        for (std::size_t j = 0; j < vl; ++j)
            for (std::size_t p = 0; p < 6; ++p) {
                pts[p][j] = static_cast<float>(base[offs[j] / 2 + p]);
                wts[p][j] = lanczos[wofs[j] / sizeof(float) + p];
            }
#define HVYXG_PT(p) __riscv_vle32_v_f32m1(pts[p], vl)
#define HVYXG_WT(p) __riscv_vle32_v_f32m1(wts[p], vl)
#else
        const vint16mf2x6_t p6 = __riscv_vluxseg6ei32_v_i16mf2x6(base, off, vl);
        const vfloat32m1x6_t w6 = __riscv_vluxseg6ei32_v_f32m1x6(lanczos, woff, vl);
        const auto pt = [&](auto f) { return __riscv_vfwcvt_f_x_v_f32m1(f, vl); };
#define HVYXG_PT(p) pt(__riscv_vget_v_i16mf2x6_i16mf2(p6, p))
#define HVYXG_WT(p) __riscv_vget_v_f32m1x6_f32m1(w6, p)
#endif
        vfloat32m1_t y = __riscv_vfmul_vv_f32m1(HVYXG_WT(0), HVYXG_PT(0), vl);
        y = __riscv_vfadd_vv_f32m1(y, __riscv_vfmul_vv_f32m1(HVYXG_WT(1), HVYXG_PT(1), vl), vl);
        y = __riscv_vfadd_vv_f32m1(y, __riscv_vfmul_vv_f32m1(HVYXG_WT(2), HVYXG_PT(2), vl), vl);
        y = __riscv_vfadd_vv_f32m1(y, __riscv_vfmul_vv_f32m1(HVYXG_WT(3), HVYXG_PT(3), vl), vl);
        y = __riscv_vfadd_vv_f32m1(y, __riscv_vfmul_vv_f32m1(HVYXG_WT(4), HVYXG_PT(4), vl), vl);
        y = __riscv_vfadd_vv_f32m1(y, __riscv_vfmul_vv_f32m1(HVYXG_WT(5), HVYXG_PT(5), vl), vl);
#undef HVYXG_PT
#undef HVYXG_WT
        __riscv_vse32_v_f32m1(out + k, y, vl);
    }
}
} // namespace

void resample_le(WalkLE& w, std::int32_t inc, InterpLE interp, const float* lanczos, float* out, std::size_t n) {
    // A step whose 128 lanes' fractions could pass 32 bits stays the reference's.
    if (inc < 0 || inc > (1 << 24)) {
        ref::resample_le(w, inc, interp, lanczos, out, n);
        return;
    }
    const bool hermite = interp == InterpLE::Hermite;
    // the points each sample reads, below and above its position
    const std::int64_t lo = interp == InterpLE::Lanczos ? 2 : hermite ? 1 : 0, hi = interp == InterpLE::Lanczos ? 3 : hermite ? 2 : 1;
    std::size_t k = 0;
    while (k < n) {
        if (w.ended) {
            for (; k < n; ++k) out[k] = 0.0f;
            return;
        }
        // the run whose points all lie inside [-pre_loop, loop_len): positions below loop_len - hi
        std::size_t m = 0;
        if (w.pos - lo >= -w.pre_loop && w.pos < w.loop_len - hi && inc > 0) {
            const std::uint64_t room = (static_cast<std::uint64_t>(w.loop_len - hi - w.pos) << kPhaseBitsLE) - static_cast<std::uint64_t>(w.frac);
            const std::uint64_t r = (room + static_cast<std::uint64_t>(inc) - 1) / static_cast<std::uint64_t>(inc);
            m = r < n - k ? static_cast<std::size_t>(r) : n - k;
        } else if (w.pos - lo >= -w.pre_loop && w.pos < w.loop_len - hi && inc == 0) {
            m = n - k;
        }
        if (m == 0) {
            ref::resample_le(w, inc, interp, lanczos, out + k, 1);
            ++k;
            continue;
        }
        if (interp == InterpLE::Lanczos) segment_lanczos(w, inc, lanczos, out + k, m);
        else segment_le(w, inc, hermite, out + k, m);
        const std::uint64_t acc = static_cast<std::uint64_t>(w.frac) + static_cast<std::uint64_t>(m) * static_cast<std::uint64_t>(inc);
        w.pos += static_cast<std::int64_t>(acc >> kPhaseBitsLE);
        w.frac = static_cast<std::int32_t>(acc & ((1u << kPhaseBitsLE) - 1));
        while (w.pos >= w.loop_len) {
            if (w.one_shot) {
                w.ended = true;
                break;
            }
            w.pos -= w.loop_len;
        }
        k += m;
    }
}

} // namespace hvyxg::accel::rvv

#endif
