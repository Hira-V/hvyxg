// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
#pragma once
// What hvyxg's control kernels in RVV share (control_le_rvv.cpp, control_50_rvv.cpp): 32-bit lanes at
// LMUL 1, selects, truncating conversions, exact powers of two, the attenuation → gain law of both
// engines, and the voices' fields reached as runs of consecutive words (docs/ACCELERATION.md). RVV 1.0
// and XTheadVector (RVV 0.7.1) from one source. Included by the _rvv.cpp sources alone.

#include <riscv_vector.h>

#include <cstddef>
#include <cstdint>

namespace hvyxg::accel::rvv {
namespace {

using vi = vint32m1_t;
using vu = vuint32m1_t;
using vf = vfloat32m1_t;
using vb = vbool32_t;

inline vi ld(const std::int32_t* p, std::size_t vl) { return __riscv_vle32_v_i32m1(p, vl); }
inline vf ldf(const float* p, std::size_t vl) { return __riscv_vle32_v_f32m1(p, vl); }
inline void st(std::int32_t* p, vi v, std::size_t vl) { __riscv_vse32_v_i32m1(p, v, vl); }
inline void stf(float* p, vf v, std::size_t vl) { __riscv_vse32_v_f32m1(p, v, vl); }
inline vi sel(vb m, vi yes, vi no, std::size_t vl) { return __riscv_vmerge_vvm_i32m1(no, yes, m, vl); }
inline vf self(vb m, vf yes, vf no, std::size_t vl) { return __riscv_vmerge_vvm_f32m1(no, yes, m, vl); }
inline vi splat(std::int32_t x, std::size_t vl) { return __riscv_vmv_v_x_i32m1(x, vl); }
inline vf splatf(float x, std::size_t vl) { return __riscv_vfmv_v_f_f32m1(x, vl); }
inline vb eqx(vi a, std::int32_t x, std::size_t vl) { return __riscv_vmseq_vx_i32m1_b32(a, x, vl); }

// Truncating conversions (the reference's static_cast<int32_t> / __ftol). RVV 0.7.1 has no .rtz forms:
// there the rounding mode is set to towards-zero around the conversion.
#if defined(__riscv_xtheadvector)
struct Rtz {
    unsigned long old;
    Rtz() { __asm__ volatile("fsrmi %0, 1" : "=r"(old)); }
    ~Rtz() { __asm__ volatile("fsrm %0" ::"r"(old)); }
};
inline vi trunc_f(vf x, std::size_t vl) { Rtz r; return __riscv_vfcvt_x_f_v_i32m1(x, vl); }
inline vi trunc_d(vfloat64m2_t x, std::size_t vl) { Rtz r; return __riscv_vfncvt_x_f_w_i32m1(x, vl); }
#else
inline vi trunc_f(vf x, std::size_t vl) { return __riscv_vfcvt_rtz_x_f_v_i32m1(x, vl); }
inline vi trunc_d(vfloat64m2_t x, std::size_t vl) { return __riscv_vfncvt_rtz_x_f_w_i32m1(x, vl); }
#endif
/// ftol(double(a) · double(b)) for floats a, b: the exact product, truncated.
inline vi ftol_mul(vf a, vf b, std::size_t vl) { return trunc_d(__riscv_vfwmul_vv_f64m2(a, b, vl), vl); }
/// 2^e as a float, for e in the normal range.
inline vf pow2(vi e, std::size_t vl) {
    return __riscv_vreinterpret_v_i32m1_f32m1(__riscv_vsll_vx_i32m1(__riscv_vadd_vx_i32m1(e, 127, vl), 23, vl));
}

/// The attenuation → gain law of both engines (the 2006LE's eg_gain, the S-YXG50's lin): 1 below 0,
/// 0 from 0xFFFFF, else mant / (2^octave · 131071).
inline vf eg_gain(vi x, std::size_t vl) {
    const vi mant = __riscv_vor_vx_i32m1(__riscv_vand_vx_i32m1(__riscv_vnot_v_i32m1(x, vl), 0xFFFF, vl), 0x10000, vl);
    const vi oct = __riscv_vand_vx_i32m1(__riscv_vsra_vx_i32m1(x, 16, vl), 15, vl);   // in range where used
    const vf den = __riscv_vfmul_vf_f32m1(pow2(oct, vl), 131071.0f, vl);
    vf g = __riscv_vfdiv_vv_f32m1(__riscv_vfcvt_f_x_v_f32m1(mant, vl), den, vl);
    g = self(__riscv_vmsge_vx_i32m1_b32(x, 0xFFFFF, vl), splatf(0.0f, vl), g, vl);
    return self(__riscv_vmslt_vx_i32m1_b32(x, 0, vl), splatf(1.0f, vl), g, vl);
}

/// Lane access to the voices: runs of consecutive 32-bit words at an offset in each lane's voice
/// (base + at), as one indexed segment load or store (RVV 1.0), or a word at a time (XTheadVector).
inline vu idx(vu at, std::uint32_t off, std::size_t vl) { return __riscv_vadd_vx_u32m1(at, off, vl); }
inline vi vgi(const std::uint8_t* base, vu at, std::uint32_t off, std::size_t vl) {
    return __riscv_vluxei32_v_i32m1(reinterpret_cast<const std::int32_t*>(base), idx(at, off, vl), vl);
}
inline vf asf(vi v) { return __riscv_vreinterpret_v_i32m1_f32m1(v); }
inline vi asi(vf v) { return __riscv_vreinterpret_v_f32m1_i32m1(v); }

inline void load_run(const std::uint8_t* base, vu at, std::uint32_t off, std::size_t vl, vi& r0, vi& r1) {
#if defined(__riscv_xtheadvector)
    r0 = vgi(base, at, off + 0, vl);
    r1 = vgi(base, at, off + 4, vl);
#else
    const vint32m1x2_t t = __riscv_vluxseg2ei32_v_i32m1x2(reinterpret_cast<const std::int32_t*>(base), idx(at, off, vl), vl);
    r0 = __riscv_vget_v_i32m1x2_i32m1(t, 0);
    r1 = __riscv_vget_v_i32m1x2_i32m1(t, 1);
#endif
}

inline void load_run(const std::uint8_t* base, vu at, std::uint32_t off, std::size_t vl, vi& r0, vi& r1, vi& r2) {
#if defined(__riscv_xtheadvector)
    r0 = vgi(base, at, off + 0, vl);
    r1 = vgi(base, at, off + 4, vl);
    r2 = vgi(base, at, off + 8, vl);
#else
    const vint32m1x3_t t = __riscv_vluxseg3ei32_v_i32m1x3(reinterpret_cast<const std::int32_t*>(base), idx(at, off, vl), vl);
    r0 = __riscv_vget_v_i32m1x3_i32m1(t, 0);
    r1 = __riscv_vget_v_i32m1x3_i32m1(t, 1);
    r2 = __riscv_vget_v_i32m1x3_i32m1(t, 2);
#endif
}

inline void load_run(const std::uint8_t* base, vu at, std::uint32_t off, std::size_t vl, vi& r0, vi& r1, vi& r2, vi& r3) {
#if defined(__riscv_xtheadvector)
    r0 = vgi(base, at, off + 0, vl);
    r1 = vgi(base, at, off + 4, vl);
    r2 = vgi(base, at, off + 8, vl);
    r3 = vgi(base, at, off + 12, vl);
#else
    const vint32m1x4_t t = __riscv_vluxseg4ei32_v_i32m1x4(reinterpret_cast<const std::int32_t*>(base), idx(at, off, vl), vl);
    r0 = __riscv_vget_v_i32m1x4_i32m1(t, 0);
    r1 = __riscv_vget_v_i32m1x4_i32m1(t, 1);
    r2 = __riscv_vget_v_i32m1x4_i32m1(t, 2);
    r3 = __riscv_vget_v_i32m1x4_i32m1(t, 3);
#endif
}

inline void load_run(const std::uint8_t* base, vu at, std::uint32_t off, std::size_t vl, vi& r0, vi& r1, vi& r2, vi& r3, vi& r4) {
#if defined(__riscv_xtheadvector)
    r0 = vgi(base, at, off + 0, vl);
    r1 = vgi(base, at, off + 4, vl);
    r2 = vgi(base, at, off + 8, vl);
    r3 = vgi(base, at, off + 12, vl);
    r4 = vgi(base, at, off + 16, vl);
#else
    const vint32m1x5_t t = __riscv_vluxseg5ei32_v_i32m1x5(reinterpret_cast<const std::int32_t*>(base), idx(at, off, vl), vl);
    r0 = __riscv_vget_v_i32m1x5_i32m1(t, 0);
    r1 = __riscv_vget_v_i32m1x5_i32m1(t, 1);
    r2 = __riscv_vget_v_i32m1x5_i32m1(t, 2);
    r3 = __riscv_vget_v_i32m1x5_i32m1(t, 3);
    r4 = __riscv_vget_v_i32m1x5_i32m1(t, 4);
#endif
}

inline void load_run(const std::uint8_t* base, vu at, std::uint32_t off, std::size_t vl, vi& r0, vi& r1, vi& r2, vi& r3, vi& r4, vi& r5) {
#if defined(__riscv_xtheadvector)
    r0 = vgi(base, at, off + 0, vl);
    r1 = vgi(base, at, off + 4, vl);
    r2 = vgi(base, at, off + 8, vl);
    r3 = vgi(base, at, off + 12, vl);
    r4 = vgi(base, at, off + 16, vl);
    r5 = vgi(base, at, off + 20, vl);
#else
    const vint32m1x6_t t = __riscv_vluxseg6ei32_v_i32m1x6(reinterpret_cast<const std::int32_t*>(base), idx(at, off, vl), vl);
    r0 = __riscv_vget_v_i32m1x6_i32m1(t, 0);
    r1 = __riscv_vget_v_i32m1x6_i32m1(t, 1);
    r2 = __riscv_vget_v_i32m1x6_i32m1(t, 2);
    r3 = __riscv_vget_v_i32m1x6_i32m1(t, 3);
    r4 = __riscv_vget_v_i32m1x6_i32m1(t, 4);
    r5 = __riscv_vget_v_i32m1x6_i32m1(t, 5);
#endif
}

inline void load_run(const std::uint8_t* base, vu at, std::uint32_t off, std::size_t vl, vi& r0, vi& r1, vi& r2, vi& r3, vi& r4, vi& r5, vi& r6) {
#if defined(__riscv_xtheadvector)
    r0 = vgi(base, at, off + 0, vl);
    r1 = vgi(base, at, off + 4, vl);
    r2 = vgi(base, at, off + 8, vl);
    r3 = vgi(base, at, off + 12, vl);
    r4 = vgi(base, at, off + 16, vl);
    r5 = vgi(base, at, off + 20, vl);
    r6 = vgi(base, at, off + 24, vl);
#else
    const vint32m1x7_t t = __riscv_vluxseg7ei32_v_i32m1x7(reinterpret_cast<const std::int32_t*>(base), idx(at, off, vl), vl);
    r0 = __riscv_vget_v_i32m1x7_i32m1(t, 0);
    r1 = __riscv_vget_v_i32m1x7_i32m1(t, 1);
    r2 = __riscv_vget_v_i32m1x7_i32m1(t, 2);
    r3 = __riscv_vget_v_i32m1x7_i32m1(t, 3);
    r4 = __riscv_vget_v_i32m1x7_i32m1(t, 4);
    r5 = __riscv_vget_v_i32m1x7_i32m1(t, 5);
    r6 = __riscv_vget_v_i32m1x7_i32m1(t, 6);
#endif
}

inline void load_run(const std::uint8_t* base, vu at, std::uint32_t off, std::size_t vl, vi& r0, vi& r1, vi& r2, vi& r3, vi& r4, vi& r5, vi& r6, vi& r7) {
#if defined(__riscv_xtheadvector)
    r0 = vgi(base, at, off + 0, vl);
    r1 = vgi(base, at, off + 4, vl);
    r2 = vgi(base, at, off + 8, vl);
    r3 = vgi(base, at, off + 12, vl);
    r4 = vgi(base, at, off + 16, vl);
    r5 = vgi(base, at, off + 20, vl);
    r6 = vgi(base, at, off + 24, vl);
    r7 = vgi(base, at, off + 28, vl);
#else
    const vint32m1x8_t t = __riscv_vluxseg8ei32_v_i32m1x8(reinterpret_cast<const std::int32_t*>(base), idx(at, off, vl), vl);
    r0 = __riscv_vget_v_i32m1x8_i32m1(t, 0);
    r1 = __riscv_vget_v_i32m1x8_i32m1(t, 1);
    r2 = __riscv_vget_v_i32m1x8_i32m1(t, 2);
    r3 = __riscv_vget_v_i32m1x8_i32m1(t, 3);
    r4 = __riscv_vget_v_i32m1x8_i32m1(t, 4);
    r5 = __riscv_vget_v_i32m1x8_i32m1(t, 5);
    r6 = __riscv_vget_v_i32m1x8_i32m1(t, 6);
    r7 = __riscv_vget_v_i32m1x8_i32m1(t, 7);
#endif
}

inline void store_run(std::uint8_t* base, vu at, vb keep, std::uint32_t off, std::size_t vl, vi v0, vi v1) {
#if defined(__riscv_xtheadvector)
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 0, vl), v0, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 4, vl), v1, vl);
#else
    vint32m1x2_t t = __riscv_vundefined_i32m1x2();
    t = __riscv_vset_v_i32m1_i32m1x2(t, 0, v0);
    t = __riscv_vset_v_i32m1_i32m1x2(t, 1, v1);
    __riscv_vsuxseg2ei32_v_i32m1x2_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off, vl), t, vl);
#endif
}

inline void store_run(std::uint8_t* base, vu at, vb keep, std::uint32_t off, std::size_t vl, vi v0, vi v1, vi v2) {
#if defined(__riscv_xtheadvector)
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 0, vl), v0, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 4, vl), v1, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 8, vl), v2, vl);
#else
    vint32m1x3_t t = __riscv_vundefined_i32m1x3();
    t = __riscv_vset_v_i32m1_i32m1x3(t, 0, v0);
    t = __riscv_vset_v_i32m1_i32m1x3(t, 1, v1);
    t = __riscv_vset_v_i32m1_i32m1x3(t, 2, v2);
    __riscv_vsuxseg3ei32_v_i32m1x3_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off, vl), t, vl);
#endif
}

inline void store_run(std::uint8_t* base, vu at, vb keep, std::uint32_t off, std::size_t vl, vi v0, vi v1, vi v2, vi v3) {
#if defined(__riscv_xtheadvector)
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 0, vl), v0, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 4, vl), v1, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 8, vl), v2, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 12, vl), v3, vl);
#else
    vint32m1x4_t t = __riscv_vundefined_i32m1x4();
    t = __riscv_vset_v_i32m1_i32m1x4(t, 0, v0);
    t = __riscv_vset_v_i32m1_i32m1x4(t, 1, v1);
    t = __riscv_vset_v_i32m1_i32m1x4(t, 2, v2);
    t = __riscv_vset_v_i32m1_i32m1x4(t, 3, v3);
    __riscv_vsuxseg4ei32_v_i32m1x4_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off, vl), t, vl);
#endif
}

inline void store_run(std::uint8_t* base, vu at, vb keep, std::uint32_t off, std::size_t vl, vi v0, vi v1, vi v2, vi v3, vi v4) {
#if defined(__riscv_xtheadvector)
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 0, vl), v0, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 4, vl), v1, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 8, vl), v2, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 12, vl), v3, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 16, vl), v4, vl);
#else
    vint32m1x5_t t = __riscv_vundefined_i32m1x5();
    t = __riscv_vset_v_i32m1_i32m1x5(t, 0, v0);
    t = __riscv_vset_v_i32m1_i32m1x5(t, 1, v1);
    t = __riscv_vset_v_i32m1_i32m1x5(t, 2, v2);
    t = __riscv_vset_v_i32m1_i32m1x5(t, 3, v3);
    t = __riscv_vset_v_i32m1_i32m1x5(t, 4, v4);
    __riscv_vsuxseg5ei32_v_i32m1x5_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off, vl), t, vl);
#endif
}

inline void store_run(std::uint8_t* base, vu at, vb keep, std::uint32_t off, std::size_t vl, vi v0, vi v1, vi v2, vi v3, vi v4, vi v5) {
#if defined(__riscv_xtheadvector)
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 0, vl), v0, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 4, vl), v1, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 8, vl), v2, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 12, vl), v3, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 16, vl), v4, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 20, vl), v5, vl);
#else
    vint32m1x6_t t = __riscv_vundefined_i32m1x6();
    t = __riscv_vset_v_i32m1_i32m1x6(t, 0, v0);
    t = __riscv_vset_v_i32m1_i32m1x6(t, 1, v1);
    t = __riscv_vset_v_i32m1_i32m1x6(t, 2, v2);
    t = __riscv_vset_v_i32m1_i32m1x6(t, 3, v3);
    t = __riscv_vset_v_i32m1_i32m1x6(t, 4, v4);
    t = __riscv_vset_v_i32m1_i32m1x6(t, 5, v5);
    __riscv_vsuxseg6ei32_v_i32m1x6_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off, vl), t, vl);
#endif
}

inline void store_run(std::uint8_t* base, vu at, vb keep, std::uint32_t off, std::size_t vl, vi v0, vi v1, vi v2, vi v3, vi v4, vi v5, vi v6) {
#if defined(__riscv_xtheadvector)
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 0, vl), v0, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 4, vl), v1, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 8, vl), v2, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 12, vl), v3, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 16, vl), v4, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 20, vl), v5, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 24, vl), v6, vl);
#else
    vint32m1x7_t t = __riscv_vundefined_i32m1x7();
    t = __riscv_vset_v_i32m1_i32m1x7(t, 0, v0);
    t = __riscv_vset_v_i32m1_i32m1x7(t, 1, v1);
    t = __riscv_vset_v_i32m1_i32m1x7(t, 2, v2);
    t = __riscv_vset_v_i32m1_i32m1x7(t, 3, v3);
    t = __riscv_vset_v_i32m1_i32m1x7(t, 4, v4);
    t = __riscv_vset_v_i32m1_i32m1x7(t, 5, v5);
    t = __riscv_vset_v_i32m1_i32m1x7(t, 6, v6);
    __riscv_vsuxseg7ei32_v_i32m1x7_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off, vl), t, vl);
#endif
}

inline void store_run(std::uint8_t* base, vu at, vb keep, std::uint32_t off, std::size_t vl, vi v0, vi v1, vi v2, vi v3, vi v4, vi v5, vi v6, vi v7) {
#if defined(__riscv_xtheadvector)
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 0, vl), v0, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 4, vl), v1, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 8, vl), v2, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 12, vl), v3, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 16, vl), v4, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 20, vl), v5, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 24, vl), v6, vl);
    __riscv_vsuxei32_v_i32m1_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off + 28, vl), v7, vl);
#else
    vint32m1x8_t t = __riscv_vundefined_i32m1x8();
    t = __riscv_vset_v_i32m1_i32m1x8(t, 0, v0);
    t = __riscv_vset_v_i32m1_i32m1x8(t, 1, v1);
    t = __riscv_vset_v_i32m1_i32m1x8(t, 2, v2);
    t = __riscv_vset_v_i32m1_i32m1x8(t, 3, v3);
    t = __riscv_vset_v_i32m1_i32m1x8(t, 4, v4);
    t = __riscv_vset_v_i32m1_i32m1x8(t, 5, v5);
    t = __riscv_vset_v_i32m1_i32m1x8(t, 6, v6);
    t = __riscv_vset_v_i32m1_i32m1x8(t, 7, v7);
    __riscv_vsuxseg8ei32_v_i32m1x8_m(keep, reinterpret_cast<std::int32_t*>(base), idx(at, off, vl), t, vl);
#endif
}

} // namespace
} // namespace hvyxg::accel::rvv
