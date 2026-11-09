# Accelerated paths

Design for hvyxg's accelerated rendering on Hira-V's machines: the X280 (RVV 1.0), the C920
(RVV 0.7.1, XTheadVector) and the A25 (Andes DSP), in three numeric forms (f32, i32, i16).
Measured motivation in [PERFORMANCE.md](PERFORMANCE.md) and below.

## Status

**Built: f32 on RVV** (`src/accel/kernels_rvv.cpp`, the X280 and, from the same source, the C920).
Both engines batch their voices (up to `kBatch` = 32) through the kernels: resample50, svf50_lanes and
mix_ramps for the S-YXG50 (and MU mode, which runs the same voice), resample_le (linear, Hermite and
Lanczos-3), svf_le_lanes and peak_ramp for the S-YXG2006LE. Every kernel is checked against the
reference by `tools/accel_check.cpp` under qemu at VLEN 128, 256 and 512; the resamplers and filters are
exact, mix_ramps and peak_ramp within float rounding.

The S-YXG2006LE on the Blackhole (`xgbench voices 32 128`, 48 kHz, 1.64 GHz), a voice's share of a core:

| Interpolation | Scalar resample | RVV |
|---|---:|---:|
| linear | -- | 0.162 % |
| Hermite | -- | 0.172 % |
| Lanczos-3 | 0.637 % | 0.200 % |

(The Lanczos column's scalar figure is the same build with Lanczos sent to the reference: the same
digest, 3.2× the time.)

**Not built yet:** XTheadVector run on the Pioneer (it compiles; the C920's 16-bit gathers are filled
one by one for want of 32-bit indices), the i16 and i32 forms, the A25's DSP kernels, and the 2006LE's
control code (now most of a voice's time).

## Where the time goes

`xgbench voices 128` on the Blackhole's X280 (1.64 GHz, 48 kHz, S-YXG50, effects off), sampled at
1 kHz for 10 s, 99 % of the samples in four functions of `syxg50::ChipVoice`:

| Function | Share | What it does per 128-sample block |
|---|---:|---|
| `direct` | 32 % | one PCM sample (8 or 16 bit) to float (inlined into `fetch`'s loop) |
| `render` | 28 % | control, then the Chamberlin SVF, EQ, steal fade |
| `fetch` | 23 % | the 32.32 position walk, loop wrap, linear / Hermite interpolation |
| `mix_ramps` | 18 % | the voice into up to 7 buses, each gain ramped |

The X280 build of xg.lib has no `v` in its `-march` today: not one vector instruction. Per voice it
costs 0.29 % of a core (S-YXG50) and 0.36 % (S-YXG2006LE): 9-10× a Zen 4 core at 48 kHz.

The 2006LE engine (`syxg2006le::DspVoice`) has the same shape: resample (linear 10-bit weights,
Hermite, Lanczos-3 6-tap), SVF, EQ, steal fade, a peak tracker, the ramped mix.

## The shape: control per voice, signal in kernels

Every voice block splits into

1. **Control** (scalar, per voice, once a block): envelopes, LFOs, pitch, glides, gains -- the
   engines' own code, unchanged. It ends by filling a **voice block descriptor**: the position
   walk (`pos`, `frac`, `inc`, loop start / length / fraction, pre-roll, one-shot), the wave
   pointer and format (pcm8 / pcm16), the interpolation, the filter's start and step (f, df, q)
   and state, the EQ path, the fade, and per bus the destination and the gain's start and step.
2. **Signal** (kernels, over a batch of voices): what the descriptors say, with no engine logic.

The kernels, each one function per (format, target):

| Kernel | Vectorised across | Notes |
|---|---|---|
| `resample` | samples | the walk computed per lane (64-bit position in two 32-bit halves where ELEN is 32), split at the loop end so a segment never wraps; gather (`vluxei`) of the points; linear, Hermite, Lanczos-3 |
| `svf` | voices (lanes) | recursive in time: the batch's state in lanes, the block read and written strided (or transposed once) |
| `eq` | voices | the biquad pair (shelves, HPF) per lane, same as `svf` |
| `fade`, `peak` | samples | elementwise; peak a max-reduction (integer formats only: exact in any order) |
| `mix` | samples | `dst[b][n] += x[n] · (g0 + n · dg)` for each live bus |
| `bus_to_f32` | samples | i16 / i32 formats only: the buses converted once a block for the effects |

Effects (reverb, chorus, variation, insertion) stay f32 and scalar in the first phases: at 128 voices
they are a few percent of the voices' cost (PERFORMANCE.md, "Effects"). They get kernels later only if
profiles say so.

## Numeric forms

| Form | Samples | Coefficients, gains | Buses | For |
|---|---|---|---|---|
| **f32** | float | float | float | today's output; HQ (Hermite, Lanczos); the reference against the DLLs |
| **i32** | Q1.30 | Q2.29 / Q1.30, 64-bit products | i32 (Q4.27, 15 dB headroom) | HQ without an FPU's cost: the A25 (scalar FPU, DSP 32×32 MAC), deterministic everywhere |
| **i16** | Q15 (the wave's own 16 bits) | Q15 | i32 accumulation | the fastest: twice i32's lanes; the A25's packed 16-bit SIMD |

`Synth::Options` gains `precision` (f32 / i32 / i16; default f32). The forms differ in output, and each
is checked against the DLL references with its own tolerance; i16 may come closest to what a DLL does where
the DLL works in integers (to be measured, not assumed).

## Accuracy, not exactness

**The accelerated paths are allowed tiny inaccuracies for speed.** The scalar code stays exactly as it
is, the reference; a kernel on a target may fuse multiply-adds, reassociate, evaluate a ramp in closed
form (`g0 + n · dg`, where the reference adds `dg` sample by sample), or use cheaper arithmetic, as long
as it stays within its form's error bound:

- **Per kernel:** random descriptors, the kernel against the scalar reference; the largest error
  relative to full scale, bounded per (kernel, form) -- f32 near float rounding (about -120 dBFS),
  i32 by its Q format, i16 by 16 bits (about -90 dBFS).
- **Whole renders:** `render_yxg` on songs, the target's build against the portable f32 build: the
  error's level against the signal's (SNR), with a floor per form.
- **Against the DLLs:** each form through the existing engine tests, with their tolerances.

Exact where it costs little: the position walk (integer), the integer forms where the operations
map one to one, and anything a vector unit computes the same way anyway; inexact only where
exactness would cost real speed (the ramps, FMA, reassociation). Integer reductions (a peak's max)
are exact in any order. The phase-1 restructuring itself
changes no arithmetic, and is checked bit for bit against the code it replaces.

## Targets

One kernel interface (`src/accel/kernels.hpp`), one source per target, chosen at compile time (Hira-V
builds one arch folder per target; a host build takes the portable one):

| Source | Built for | Extensions used |
|---|---|---|
| `kernels_scalar.cpp` | everything (the reference) | none |
| `kernels_rvv.cpp` | X280 (`-march=..._v_zvl512b`), C920 (GCC's XTheadVector patch: the same intrinsics, as hvopl3's `hvopl3_rvv.inc` does) | V: gathers (`vluxei`), widening (`vwcvt`, `vfwcvt.f.x`), `vnclip`, segment loads where they pay; 64-bit lanes on the X280 (ELEN 64), two 32-bit halves on targets with ELEN 32; VLEN-agnostic (`vsetvl` everywhere), LMUL 1-4 by register pressure; no fractional LMUL (0.7.1 has none) |
| `kernels_dsp.cpp` | A25 (`__riscv_dsp`, `-mext-dsp`) | packed 16-bit SIMD (`kmda`, `smmwb`, `kmmawb`, `sclip`, `pkbb`), `sunpkd810` for pcm8, 32×32→64 MAC for i32; f32 stays scalar FPU |

Scalar control code also takes each target's scalar extensions through the compiler: Zba / Zbb on the
X280 (`sh1add`, `clz` for the log/linear laws), XTheadBa / Bb / MemIdx / Mac on the C920, XAndesPerf on
the A25 -- by `-march` alone, no source changes.

What 0.7.1 lacks that the kernels must avoid or wrap (checked per kernel): fractional LMUL, `vzext`
/ `vsext`, some reductions' forms, `vfrec7` / `vfrsqrt7`, tail- and mask-agnostic policies.

## Batching

The kernels take a batch: up to `kLanes` descriptors (X280 e32 m1 = 16 lanes, m2 = 32). The engines'
`render` loops become: control for every active voice → descriptors → `resample` per voice (it writes
its row) → `svf` / `eq` across the batch → `fade` → `mix` per voice in voice order. A voice's block
rows live in a per-synth scratch area (128 × kLanes floats: 8 KB at 16 lanes).

## Testing

- **Host:** every form's scalar reference against the DLL references (the existing engine tests,
  tolerances per form), and the new kernel tests: random descriptors, each kernel against its
  reference.
- **Per target:** the kernel tests and `render_yxg` renders against the portable build's, within
  the bounds above -- RVV under qemu at VLEN 128, 256 and 512, XTheadVector on the Pioneer, the A25
  under rv32emu with the DSP extension and on the Tang.
- **Board:** `xgbench` (Hira-V's `apps/bench/xgbench.cpp`): load per voice and per song.

## Phases

1. **Kernels, scalar, f32.** The descriptor and kernel split in both engines, the arithmetic
   unchanged (bit for bit against today's renders); host kernel tests. No speed change yet.
2. **RVV f32** for the X280; xg.lib built with `v zvl512b`. Digests under qemu and on the Blackhole.
3. **XTheadVector f32** for the C920 (the same source); on the Pioneer.
4. **i16** (scalar reference, RVV, DSP), then **i32**.
5. **A25** (DSP i16 / i32): first whether xg.lib fits the Tang at all (memory, voices a core).
6. **Effects**, if the profiles then point there.
