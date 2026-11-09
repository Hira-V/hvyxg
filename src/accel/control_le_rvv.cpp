// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// The S-YXG2006LE voice's control across lanes in RVV (control_le.hpp): RVV 1.0 (the X280) and
// XTheadVector (the C920) from one source. The voices' state by indexed loads and stores, in place. Exact against syxg2006le::DspVoice::control for the lanes
// it keeps (ok): the integer steps as the reference's, the float operations unfused in its order
// (-ffp-contract=off), its x87 products (float × float in double, truncated) as widening multiplies --
// exact, since a float product always fits a double -- and its power-of-two scalings (ldexp) as a
// multiply by an exact power of two.

#if defined(HVYXG_ACCEL_RVV) && (defined(__riscv_vector) || defined(__riscv_xtheadvector))

#include "accel/control_le.hpp"

#include <riscv_vector.h>

#include <cstddef>
#include <cstdint>

namespace hvyxg::accel::rvv {

namespace {

using vi = vint32m1_t;
using vu = vuint32m1_t;
using vf = vfloat32m1_t;
using vb = vbool32_t;

constexpr std::int32_t kSilent = 0x100000;
constexpr float kBusScale = -1.0f / 8192.0f;

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

/// The reference's step (dsp_voice.cpp): p toward target by inc; done once there (landing exactly on the
/// target reports not done; already there reports done without moving).
inline vi step(vi p, vi target, vi inc, vb& done, std::size_t vl) {
    const vb eq = __riscv_vmseq_vv_i32m1_b32(p, target, vl);
    const vb lt = __riscv_vmslt_vv_i32m1_b32(p, target, vl);
    const vi up = __riscv_vadd_vv_i32m1(p, inc, vl);
    const vi dn = __riscv_vsub_vv_i32m1(p, inc, vl);
    const vb over = __riscv_vmand_mm_b32(lt, __riscv_vmslt_vv_i32m1_b32(target, up, vl), vl);
    const vb gt = __riscv_vmnor_mm_b32(lt, eq, vl);
    const vb under = __riscv_vmand_mm_b32(gt, __riscv_vmslt_vv_i32m1_b32(dn, target, vl), vl);
    vi r = sel(lt, up, dn, vl);
    r = sel(__riscv_vmor_mm_b32(over, under, vl), target, r, vl);
    r = sel(eq, p, r, vl);
    done = __riscv_vmor_mm_b32(eq, __riscv_vmor_mm_b32(over, under, vl), vl);
    return r;
}
/// The reference's step_up: upward only; done at or past the target.
inline vi step_up(vi p, vi target, std::int32_t inc, vb& done, std::size_t vl) {
    const vb at = __riscv_vmsge_vv_i32m1_b32(p, target, vl);
    const vi up = __riscv_vadd_vx_i32m1(p, inc, vl);
    const vb over = __riscv_vmslt_vv_i32m1_b32(target, up, vl);
    done = __riscv_vmor_mm_b32(at, over, vl);
    return sel(at, p, sel(over, target, up, vl), vl);
}

/// eg_gain: 1 below 0, 0 from 0xFFFFF, else mant / (131071 · 2^octave).
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

/// One mod EG (mod_tick and mod_advance), from its two runs; its new state, position and delay counter
/// replace theirs in run a (the rest of it stays: stored back as it is).
inline void mod_eg(const std::uint8_t* base, vu at, const FieldsLE::Eg& e, vb gated, vi& stt, vi& pos, vi& delay, vi& inc0, vi& inc1,
                   vi& inc2, vi& inc_sus, vi& inc_rel, vi& state_out, std::size_t vl) {
    load_run(base, at, e.a, vl, stt, pos, delay, inc0, inc1, inc2, inc_sus, inc_rel);
    vi l0, l1, l2, l3, rel, delay_target;
    load_run(base, at, e.b, vl, l0, l1, l2, l3, rel, delay_target);
    // state 0: the delay
    const vb s0 = eqx(stt, 0, vl);
    vb ddone;
    const vi dnew = step_up(delay, delay_target, kBlockLE, ddone, vl);
    delay = sel(s0, dnew, delay, vl);
    // states 1 / 3 / 5 / 7 / 8: one step of pos, its target and step by state
    const vb s1 = eqx(stt, 1, vl), s3 = eqx(stt, 3, vl), s5 = eqx(stt, 5, vl), s7 = eqx(stt, 7, vl), s8 = eqx(stt, 8, vl);
    const vi target = sel(s1, l1, sel(s3, l2, sel(s8, rel, l3, vl), vl), vl);
    const vi inc = sel(s1, inc0, sel(s3, inc1, sel(s5, inc2, sel(s7, inc_sus, inc_rel, vl), vl), vl), vl);
    const vb moving = __riscv_vmor_mm_b32(__riscv_vmor_mm_b32(s1, s3, vl), __riscv_vmor_mm_b32(__riscv_vmor_mm_b32(s5, s7, vl), s8, vl), vl);
    vb pdone;
    const vi pnew = step(pos, target, inc, pdone, vl);
    pos = sel(moving, pnew, pos, vl);
    // advance: a done delay or stage (1 / 3 / 5), the gate open
    const vb staged = __riscv_vmand_mm_b32(__riscv_vmor_mm_b32(s1, __riscv_vmor_mm_b32(s3, s5, vl), vl), pdone, vl);
    const vb adv = __riscv_vmandn_mm_b32(__riscv_vmor_mm_b32(__riscv_vmand_mm_b32(s0, ddone, vl), staged, vl), gated, vl);
    // mod_advance: the first later level that differs from where pos is
    const vb le3 = __riscv_vmsle_vx_i32m1_b32(stt, 3, vl), le1 = __riscv_vmsle_vx_i32m1_b32(stt, 1, vl);
    vi ns = sel(__riscv_vmand_mm_b32(le3, __riscv_vmsne_vv_i32m1_b32(pos, l3, vl), vl), splat(5, vl), splat(7, vl), vl);
    ns = sel(__riscv_vmand_mm_b32(le1, __riscv_vmsne_vv_i32m1_b32(pos, l2, vl), vl), splat(3, vl), ns, vl);
    ns = sel(__riscv_vmand_mm_b32(s0, __riscv_vmsne_vv_i32m1_b32(pos, l1, vl), vl), splat(1, vl), ns, vl);
    state_out = sel(adv, ns, stt, vl);
}

} // namespace

void control_le(LanesLE& L, const FieldsLE& F, const ControlTablesLE& t) {
    // In phases, a loop over the lanes each, so few vector registers are live at once (the X280 pays
    // dearly for spills); the amp EG first, since it decides which lanes the kernel keeps. Values a
    // later phase needs go through the lanes (unit-stride).
    const std::size_t n = L.n;
    std::uint8_t* const base = L.base;
#define HVYXG_LANES for (std::size_t k = 0, vl; k < n; k += vl)
#define HVYXG_SETUP                                                                                    \
    vl = __riscv_vsetvl_e32m1(n - k);                                                                    \
    const vu at = __riscv_vle32_v_u32m1(L.voice + k, vl);                                              \
    const vf one = splatf(1.0f, vl), mone = splatf(-1.0f, vl), half = splatf(0.5f, vl), zero = splatf(0.0f, vl); \
    (void)one, (void)mone, (void)half, (void)zero;                                                      \
    vb d;                                                                                              \
    (void)d;
    // ── amp EG: which lanes stay ───────────────────────────────────────────────────────────────────
    HVYXG_LANES {
        HVYXG_SETUP
        vb flag = __riscv_vmclr_m_b32(vl);
        vi stt, x, hold, xstep, gp_old, g_old, slope_i, icpt_i;
        load_run(base, at, F.amp_a, vl, stt, x, hold, xstep, gp_old, g_old, slope_i, icpt_i);
        vi ai0, ai1, ai2, ai3, al0, al1, al2, al3;
        load_run(base, at, F.amp_b, vl, ai0, ai1, ai2, ai3, al0, al1, al2, al3);
        vi inc_rel, d1rate, d2rate;
        load_run(base, at, F.amp_c, vl, inc_rel, d1rate, d2rate);
        const vf gain_prev = asf(g_old);
        vf gain;
        vb fin;
        {
            const vb s0 = eqx(stt, 0, vl), s1 = eqx(stt, 1, vl), s2 = eqx(stt, 2, vl), s3 = eqx(stt, 3, vl), s4 = eqx(stt, 4, vl),
                     s5 = eqx(stt, 5, vl), s6 = eqx(stt, 6, vl), s7 = eqx(stt, 7, vl), s8 = eqx(stt, 8, vl), s9 = eqx(stt, 9, vl);
            // hold states 0 / 2 / 4 step the hold toward 0; the others step x
            const vb holding = __riscv_vmor_mm_b32(s0, __riscv_vmor_mm_b32(s2, s4, vl), vl);
            const vi hinc = sel(s0, ld(L.amp_inc_attack + k, vl), sel(s2, ai1, ai2, vl), vl);
            vb hdone;
            const vi hnew = step(hold, splat(0, vl), hinc, hdone, vl);
            hold = sel(holding, hnew, hold, vl);
            const vi silent = splat(kSilent, vl);
            const vi target = sel(s1, al1, sel(s3, al2, sel(__riscv_vmor_mm_b32(s5, s7, vl), al3, silent, vl), vl), vl);
            const vi xinc = sel(__riscv_vmor_mm_b32(s1, s9, vl), xstep, sel(s3, ai1, sel(s5, ai2,
                                sel(s6, ai3, sel(s7, splat(t.inc_sustain_glide, vl), inc_rel, vl), vl), vl), vl), vl);
            const vb xmoving = __riscv_vmor_mm_b32(__riscv_vmor_mm_b32(__riscv_vmor_mm_b32(s1, s3, vl), __riscv_vmor_mm_b32(s5, s6, vl), vl),
                                                   __riscv_vmor_mm_b32(__riscv_vmor_mm_b32(s7, s8, vl), s9, vl), vl);
            vb xdone;
            const vi xnew = step(x, target, xinc, xdone, vl);
            x = sel(xmoving, xnew, x, vl);
            // a stage end (states 0..5) leaves the steady states; so does an instant decay (rate 127)
            const vb staged = __riscv_vmor_mm_b32(__riscv_vmand_mm_b32(holding, hdone, vl),
                                                  __riscv_vmand_mm_b32(__riscv_vmor_mm_b32(s1, __riscv_vmor_mm_b32(s3, s5, vl), vl), xdone, vl), vl);
            const vb instant = __riscv_vmor_mm_b32(__riscv_vmand_mm_b32(s2, eqx(d1rate, 127, vl), vl),
                                                   __riscv_vmand_mm_b32(s4, eqx(d2rate, 127, vl), vl), vl);
            flag = __riscv_vmor_mm_b32(flag, __riscv_vmor_mm_b32(staged, instant, vl), vl);
            // the gain: linear stages from the line, the others by the law
            const vb linear = __riscv_vmor_mm_b32(s1, s9, vl);
            const vf line = __riscv_vfsub_vv_f32m1(asf(icpt_i), __riscv_vfmul_vv_f32m1(__riscv_vfcvt_f_x_v_f32m1(x, vl), asf(slope_i), vl), vl);
            gain = self(linear, line, eg_gain(x, vl), vl);
            fin = __riscv_vmor_mm_b32(__riscv_vmfle_vf_f32m1_b32(gain, 0.0f, vl), __riscv_vmand_mm_b32(s9, __riscv_vmsge_vx_i32m1_b32(x, kSilent, vl), vl), vl);
            gain = self(fin, zero, gain, vl);
        }
        vi ticks, timer;
        load_run(base, at, F.ticks, vl, ticks, timer);
        const vb restart = __riscv_vmsge_vx_i32m1_b32(timer, 4096, vl);

        const vb keep = __riscv_vmandn_mm_b32(__riscv_vmsne_vx_i32m1_b32(ld(L.ok + k, vl), 0, vl), flag, vl);
        st(L.ok + k, sel(keep, splat(1, vl), splat(0, vl), vl), vl);
        st(L.finished + k, sel(fin, splat(1, vl), splat(0, vl), vl), vl);
        st(L.peak_reset + k, sel(restart, splat(1, vl), splat(0, vl), vl), vl);
        stf(L.gain_prev + k, gain_prev, vl);
        stf(L.gain + k, gain, vl);
        store_run(base, at, keep, F.amp_a, vl, stt, x, hold, xstep, asi(gain_prev), asi(gain), slope_i, icpt_i);
        store_run(base, at, keep, F.ticks, vl, __riscv_vadd_vx_i32m1(ticks, 1, vl),
                  sel(restart, splat(0, vl), __riscv_vadd_vx_i32m1(timer, kBlockLE, vl), vl));
    }
    // ── LFO (running: the fade factor 1) ───────────────────────────────────────────────────────
    HVYXG_LANES {
        HVYXG_SETUP
        const vb keep = __riscv_vmsne_vx_i32m1_b32(ld(L.ok + k, vl), 0, vl);
        vi w, inv, phase_i, lfo_inc, pd_i, ad_i, fd_i, amod_i;
        load_run(base, at, F.lfo_a, vl, w, inv, phase_i, lfo_inc, pd_i, ad_i, fd_i, amod_i);
        vi sh_i, op_old, oa_old, of_old, ob_old;
        load_run(base, at, F.lfo_b, vl, sh_i, op_old, oa_old, of_old, ob_old);
        vf amod = asf(amod_i);
        {
            const vf tgt = ldf(L.amod_target + k, vl);
            const float glide = static_cast<float>(t.inc_sustain_glide);
            const vf upv = __riscv_vfmin_vv_f32m1(__riscv_vfadd_vf_f32m1(amod, glide, vl), tgt, vl);
            const vf dnv = __riscv_vfmax_vv_f32m1(__riscv_vfsub_vf_f32m1(amod, glide, vl), tgt, vl);
            amod = self(__riscv_vmflt_vv_f32m1_b32(amod, tgt, vl), upv, self(__riscv_vmflt_vv_f32m1_b32(tgt, amod, vl), dnv, amod, vl), vl);
        }
        // the depths: element + controller (integer-valued floats below 2^24: the reference's double sums
        // are exact, so are these), clamped
        const vf dp = __riscv_vfmin_vf_f32m1(__riscv_vfmax_vf_f32m1(__riscv_vfadd_vv_f32m1(asf(pd_i), ldf(L.pmod + k, vl), vl), 0.0f, vl), 21845.0f, vl);
        const vf da = __riscv_vfmin_vf_f32m1(__riscv_vfmax_vf_f32m1(__riscv_vfadd_vv_f32m1(asf(ad_i), amod, vl), 0.0f, vl), static_cast<float>(128 << 12), vl);
        const vf df = __riscv_vfmin_vf_f32m1(__riscv_vfmax_vf_f32m1(__riscv_vfadd_vv_f32m1(asf(fd_i), ldf(L.fmod + k, vl), vl), 0.0f, vl), 262144.0f, vl);
        const vu ph = __riscv_vreinterpret_v_i32m1_u32m1(phase_i);
        const vf sgn_p = self(__riscv_vmsne_vx_i32m1_b32(__riscv_vand_vx_i32m1(inv, 4, vl), 0, vl), mone, one, vl);
        const vf sgn_a = self(__riscv_vmsne_vx_i32m1_b32(__riscv_vand_vx_i32m1(inv, 2, vl), 0, vl), mone, one, vl);
        const vf sgn_f = self(__riscv_vmsne_vx_i32m1_b32(__riscv_vand_vx_i32m1(inv, 1, vl), 0, vl), mone, one, vl);
        const vb w0 = eqx(w, 0, vl), w1 = eqx(w, 1, vl), w5 = eqx(w, 5, vl), w6 = eqx(w, 6, vl), w7 = eqx(w, 7, vl), w8 = eqx(w, 8, vl);
        const vb is_saw = __riscv_vmor_mm_b32(__riscv_vmor_mm_b32(w0, w5, vl), eqx(w, 9, vl), vl);
        const vb is_tri = __riscv_vmor_mm_b32(__riscv_vmor_mm_b32(w1, w6, vl), eqx(w, 10, vl), vl);
        const vb is_sq = __riscv_vmor_mm_b32(__riscv_vmor_mm_b32(eqx(w, 2, vl), w7, vl), eqx(w, 11, vl), vl);
        vf wp, wa, wf;
        {   // saw: (int((ph + off) & 0xFFFFF) - 0x80000) / 2^20
            const auto saw = [&](vu off) {
                const vi u = __riscv_vreinterpret_v_u32m1_i32m1(__riscv_vand_vx_u32m1(__riscv_vadd_vv_u32m1(ph, off, vl), 0xFFFFF, vl));
                return __riscv_vfmul_vf_f32m1(__riscv_vfcvt_f_x_v_f32m1(__riscv_vsub_vx_i32m1(u, 0x80000, vl), vl), 1.0f / 1048576.0f, vl);
            };
            const vf so = saw(__riscv_vreinterpret_v_i32m1_u32m1(sel(w0, splat(0x80000, vl), splat(0, vl), vl)));
            wp = __riscv_vfmul_vv_f32m1(sgn_p, saw(__riscv_vreinterpret_v_i32m1_u32m1(splat(0x80000, vl))), vl);
            wa = __riscv_vfadd_vf_f32m1(__riscv_vfmul_vv_f32m1(self(w0, __riscv_vfneg_v_f32m1(sgn_a, vl), sgn_a, vl), so, vl), 0.5f, vl);
            wf = __riscv_vfadd_vv_f32m1(__riscv_vfmul_vv_f32m1(sgn_f, so, vl), self(w5, half, zero, vl), vl);
        }
        {   // triangle: u = (ph + off) & 0xFFFFF, folded; (int(u) - 0x40000) / 2^19
            const auto tri = [&](vu off) {
                vu u = __riscv_vand_vx_u32m1(__riscv_vadd_vv_u32m1(ph, off, vl), 0xFFFFF, vl);
                const vb hi = __riscv_vmsne_vx_u32m1_b32(__riscv_vand_vx_u32m1(u, 0x80000, vl), 0, vl);
                u = __riscv_vxor_vx_u32m1_mu(hi, u, u, 0xFFFFF, vl);
                return __riscv_vfmul_vf_f32m1(__riscv_vfcvt_f_x_v_f32m1(__riscv_vsub_vx_i32m1(__riscv_vreinterpret_v_u32m1_i32m1(u), 0x40000, vl), vl), 1.0f / 524288.0f, vl);
            };
            const vf to = tri(__riscv_vreinterpret_v_i32m1_u32m1(sel(w1, splat(0x40000, vl), splat(0, vl), vl)));
            const vf tp = __riscv_vfmul_vv_f32m1(sgn_p, tri(__riscv_vreinterpret_v_i32m1_u32m1(splat(0x40000, vl))), vl);
            const vf ta = __riscv_vfadd_vf_f32m1(__riscv_vfmul_vv_f32m1(self(w1, __riscv_vfneg_v_f32m1(sgn_a, vl), sgn_a, vl), to, vl), 0.5f, vl);
            const vf tf = __riscv_vfadd_vv_f32m1(__riscv_vfmul_vv_f32m1(sgn_f, to, vl), self(w6, half, zero, vl), vl);
            wp = self(is_tri, tp, wp, vl), wa = self(is_tri, ta, wa, vl), wf = self(is_tri, tf, wf, vl);
        }
        {   // square: the half the phase is in
            const vb second = __riscv_vmsne_vx_u32m1_b32(__riscv_vand_vx_u32m1(ph, 0x80000, vl), 0, vl);
            const vb ia = __riscv_vmsne_vx_i32m1_b32(__riscv_vand_vx_i32m1(inv, 2, vl), 0, vl);
            const vb iff = __riscv_vmsne_vx_i32m1_b32(__riscv_vand_vx_i32m1(inv, 1, vl), 0, vl);
            const vf qp = __riscv_vfmul_vv_f32m1(self(second, splatf(-0.5f, vl), half, vl), sgn_p, vl);
            const vf qa = self(__riscv_vmxor_mm_b32(second, ia, vl), one, zero, vl);
            const vf qf = self(w7, self(__riscv_vmxor_mm_b32(second, iff, vl), one, zero, vl),
                               __riscv_vfmul_vv_f32m1(self(second, half, splatf(-0.5f, vl), vl), sgn_f, vl), vl);
            wp = self(is_sq, qp, wp, vl), wa = self(is_sq, qa, wa, vl), wf = self(is_sq, qf, wf, vl);
        }
        {   // S&H: the value drawn (begin)
            const vf sh = asf(sh_i);
            const vf hp = __riscv_vfmul_vv_f32m1(__riscv_vfmul_vf_f32m1(sh, 0.5f, vl), sgn_p, vl);
            const vf ha = __riscv_vfrsub_vf_f32m1(__riscv_vfmul_vv_f32m1(__riscv_vfmul_vf_f32m1(sh, 0.5f, vl), sgn_a, vl), 0.5f, vl);
            const vf hf = __riscv_vfadd_vv_f32m1(__riscv_vfmul_vv_f32m1(__riscv_vfmul_vf_f32m1(sh, -0.5f, vl), sgn_f, vl), self(w8, half, zero, vl), vl);
            const vb is_sh = __riscv_vmnor_mm_b32(__riscv_vmor_mm_b32(is_saw, is_tri, vl), is_sq, vl);
            wp = self(is_sh, hp, wp, vl), wa = self(is_sh, ha, wa, vl), wf = self(is_sh, hf, wf, vl);
        }
        const vi out_pitch = ftol_mul(dp, wp, vl);
        const vi out_amp = ftol_mul(da, wa, vl);
        const vi out_filter = ftol_mul(df, wf, vl);
        const vb biased = __riscv_vmnor_mm_b32(__riscv_vmand_mm_b32(__riscv_vmsge_vx_i32m1_b32(w, 5, vl), __riscv_vmsle_vx_i32m1_b32(w, 8, vl), vl), eqx(w, 12, vl), vl);
        const vi out_bias = sel(biased, trunc_f(__riscv_vfmul_vf_f32m1(df, 0.5f, vl), vl), splat(0, vl), vl);
        const vu next = __riscv_vand_vx_u32m1(__riscv_vadd_vv_u32m1(ph, __riscv_vreinterpret_v_i32m1_u32m1(lfo_inc), vl), 0xFFFFF, vl);
        const vb quad = __riscv_vmsne_vx_u32m1_b32(__riscv_vand_vx_u32m1(__riscv_vxor_vv_u32m1(next, ph, vl), 0xC0000, vl), 0, vl);
        const vi sh_flag = sel(quad, splat(1, vl), ld(L.sh_flag + k, vl), vl);

        st(L.sh_flag + k, sh_flag, vl);
        st(L.out_pitch + k, out_pitch, vl);
        st(L.out_amp + k, out_amp, vl);
        st(L.out_filter + k, out_filter, vl);
        st(L.out_bias + k, out_bias, vl);
        store_run(base, at, keep, F.lfo_a, vl, w, inv, __riscv_vreinterpret_v_u32m1_i32m1(next), lfo_inc, pd_i, ad_i, fd_i, asi(amod));
        store_run(base, at, keep, F.lfo_b, vl, sh_i, out_pitch, out_amp, out_filter, out_bias);
    }
    // ── pitch and filter EGs, portamento, the phase increment, the cutoff ──────────────────────────
    HVYXG_LANES {
        HVYXG_SETUP
        const vb keep = __riscv_vmsne_vx_i32m1_b32(ld(L.ok + k, vl), 0, vl);
        const vb gated = __riscv_vmsne_vx_i32m1_b32(ld(L.gated + k, vl), 0, vl);
        const vi out_pitch = ld(L.out_pitch + k, vl), out_filter = ld(L.out_filter + k, vl), out_bias = ld(L.out_bias + k, vl);
        vi pe_s, pe_pos, pe_delay, pe_i0, pe_i1, pe_i2, pe_is, pe_ir, pe_state;
        mod_eg(base, at, F.eg[0], gated, pe_s, pe_pos, pe_delay, pe_i0, pe_i1, pe_i2, pe_is, pe_ir, pe_state, vl);
        vi pb_old, porta_old, porta_step, inc_old, cutoff_old, ct_old, fprev_i, fnew_old;
        load_run(base, at, F.pitch, vl, pb_old, porta_old, porta_step, inc_old, cutoff_old, ct_old, fprev_i, fnew_old);
        const vi porta = step(porta_old, splat(0, vl), porta_step, d, vl);
        const vi pitch_base = ld(L.pitch_base + k, vl);
        vi inc;
        {
            vi b = __riscv_vmax_vx_i32m1(__riscv_vmin_vx_i32m1(__riscv_vadd_vv_i32m1(pitch_base, porta, vl), 0x80000, vl), -0x80000, vl);
            b = __riscv_vadd_vv_i32m1(__riscv_vadd_vv_i32m1(__riscv_vadd_vv_i32m1(b, pe_pos, vl), out_pitch, vl), ld(L.loop_tune + k, vl), vl);
            b = __riscv_vmax_vx_i32m1(__riscv_vmin_vx_i32m1(b, 0x80000, vl), -0x80000, vl);
            const vi v = __riscv_vsra_vx_i32m1(b, 6, vl);
            const vu off = __riscv_vsll_vx_u32m1(__riscv_vreinterpret_v_i32m1_u32m1(__riscv_vand_vx_i32m1(v, 0x3FF, vl)), 2, vl);
            const vf mant = __riscv_vluxei32_v_f32m1(t.pitch, off, vl);
            inc = trunc_f(__riscv_vfmul_vv_f32m1(mant, pow2(__riscv_vadd_vx_i32m1(__riscv_vsra_vx_i32m1(v, 10, vl), 15, vl), vl), vl), vl);
        }

        // ── filter EG, cutoff glide, the coefficient ─────────────────────────────────────────────
        vi fe_s, fe_pos, fe_delay, fe_i0, fe_i1, fe_i2, fe_is, fe_ir, fe_state;
        mod_eg(base, at, F.eg[1], gated, fe_s, fe_pos, fe_delay, fe_i0, fe_i1, fe_i2, fe_is, fe_ir, fe_state, vl);
        const vi cutoff_target = ld(L.cutoff_target + k, vl);
        const vi cutoff = step(cutoff_old, cutoff_target, splat(t.inc_cutoff_glide, vl), d, vl);
        vf f_new, f_prev;
        {
            vi lc = __riscv_vmax_vx_i32m1(__riscv_vmin_vx_i32m1(__riscv_vadd_vv_i32m1(cutoff, fe_pos, vl), 0x100000, vl), 0x80000, vl);
            lc = __riscv_vmax_vx_i32m1(__riscv_vmin_vx_i32m1(__riscv_vadd_vv_i32m1(lc, out_bias, vl), 0x100000, vl), 0x80000, vl);
            lc = __riscv_vmin_vv_i32m1(lc, ld(L.ceiling + k, vl), vl);
            lc = __riscv_vmax_vx_i32m1(__riscv_vsub_vv_i32m1(lc, out_filter, vl), 0, vl);
            const vi m = __riscv_vadd_vx_i32m1(__riscv_vand_vx_i32m1(__riscv_vsra_vx_i32m1(lc, 8, vl), 255, vl), 256, vl);
            f_new = __riscv_vfmin_vf_f32m1(__riscv_vfmul_vv_f32m1(__riscv_vfcvt_f_x_v_f32m1(m, vl),
                                                                   pow2(__riscv_vsub_vx_i32m1(__riscv_vsra_vx_i32m1(lc, 16, vl), 24, vl), vl), vl), 1.0f, vl);
            const vf fp = asf(fprev_i);
            f_prev = self(__riscv_vmflt_vf_f32m1_b32(fp, 0.0f, vl), f_new, fp, vl);
        }

        store_run(base, at, keep, F.eg[0].a, vl, pe_state, pe_pos, pe_delay, pe_i0, pe_i1, pe_i2, pe_is, pe_ir);
        store_run(base, at, keep, F.eg[1].a, vl, fe_state, fe_pos, fe_delay, fe_i0, fe_i1, fe_i2, fe_is, fe_ir);
        store_run(base, at, keep, F.pitch, vl, pitch_base, porta, porta_step, inc, cutoff, cutoff_target, asi(f_prev), asi(f_new));
    }
    // ── output stage: glides, gains, the mix ramps ──────────────────────────────────────────────────
    HVYXG_LANES {
        HVYXG_SETUP
        const vb keep = __riscv_vmsne_vx_i32m1_b32(ld(L.ok + k, vl), 0, vl);
        const vi out_amp = ld(L.out_amp + k, vl);
        const vf gain_prev = ldf(L.gain_prev + k, vl), gain = ldf(L.gain + k, vl);
        vi level_old, pan0_old, pan1_old, s0_old, s1_old, s2_old, s3_old;
        load_run(base, at, F.out, vl, level_old, pan0_old, pan1_old, s0_old, s1_old, s2_old, s3_old);
        vi gp0, gp1, gp2, gp3, gp4, gp5, gp6;
        load_run(base, at, F.gains, vl, gp0, gp1, gp2, gp3, gp4, gp5, gp6);
        const vi level = step(level_old, ld(L.level_t + k, vl), splat(t.inc_sustain_glide, vl), d, vl);
        const vi pan0 = step(pan0_old, ld(L.pan_t[0] + k, vl), splat(t.inc_pan, vl), d, vl);
        const vi pan1 = step(pan1_old, ld(L.pan_t[1] + k, vl), splat(t.inc_pan, vl), d, vl);
        const vi sx0 = step(s0_old, ld(L.send_t[0] + k, vl), splat(t.inc_send, vl), d, vl);
        const vi sx1 = step(s1_old, ld(L.send_t[1] + k, vl), splat(t.inc_send, vl), d, vl);
        const vi sx2 = step(s2_old, ld(L.send_t[2] + k, vl), splat(t.inc_send, vl), d, vl);
        const vi sx3 = step(s3_old, ld(L.send_t[3] + k, vl), splat(t.inc_send, vl), d, vl);
        // the gains, left / right, of a send: g(pan + send + level + amp LFO), muted from 255 units
        const auto gains_of = [&](vi s, vf& gl, vf& gr) {
            const vi tt = __riscv_vadd_vv_i32m1(__riscv_vadd_vv_i32m1(s, level, vl), out_amp, vl);
            const vb mute = __riscv_vmsge_vx_i32m1_b32(tt, 0xFF000, vl);
            gl = self(mute, zero, eg_gain(__riscv_vadd_vv_i32m1(pan0, tt, vl), vl), vl);
            gr = self(mute, zero, eg_gain(__riscv_vadd_vv_i32m1(pan1, tt, vl), vl), vl);
        };
        vf g0, g1, g2, g3, g4, g5, g6, rl, rr;
        gains_of(sx0, g0, g1);
        gains_of(sx1, rl, rr);
        g2 = __riscv_vfmul_vf_f32m1(__riscv_vfadd_vv_f32m1(rl, rr, vl), 0.5f, vl);
        gains_of(sx2, g3, g4);
        gains_of(sx3, g5, g6);
        const vb first = eqx(ld(L.mixed + k, vl), 0, vl);
        // a ramp from the previous block's gain (its own on the first block) to this one's
        const auto from = [&](vf g, vi gp) {
            return asi(__riscv_vfmul_vf_f32m1(__riscv_vfmul_vv_f32m1(gain_prev, self(first, g, asf(gp), vl), vl), kBusScale, vl));
        };
        const auto to = [&](vf g) { return asi(__riscv_vfmul_vf_f32m1(__riscv_vfmul_vv_f32m1(gain, g, vl), kBusScale, vl)); };

        store_run(base, at, keep, F.out, vl, level, pan0, pan1, sx0, sx1, sx2, sx3);
        store_run(base, at, keep, F.gains + 28, vl, from(g0, gp0), from(g1, gp1), from(g2, gp2), from(g3, gp3), from(g4, gp4), from(g5, gp5), from(g6, gp6));
        store_run(base, at, keep, F.gains + 56, vl, to(g0), to(g1), to(g2), to(g3), to(g4), to(g5), to(g6));
        store_run(base, at, keep, F.gains, vl, asi(g0), asi(g1), asi(g2), asi(g3), asi(g4), asi(g5), asi(g6));
    }
#undef HVYXG_LANES
#undef HVYXG_SETUP
}

} // namespace hvyxg::accel::rvv

#endif
