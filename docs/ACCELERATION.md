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

## Control across voices

**Measured (2026-11-09, the Blackhole, 128 voices, linear, kernels skipped in turn):** with the signal on
the vector unit, a voice's time is mostly its scalar control code -- the S-YXG2006LE 0.114 % of a core
of 0.164 % (70 %), the S-YXG50 0.033 % of 0.067 % (50 %). The X280 runs it at about three times the
cycles a Zen 4 core takes (in order, dual issue); no one function dominates (LFO, three EGs, pitch,
cutoff, the output glides and gains, each a few hundred cycles), so the fix is the vector unit, across
voices, as the filter already runs.

**The shape.** A block's voices go in batches (`kBatch`, as today), each in three passes:

1. **Gather** (scalar, per voice, in voice order): what is branchy or touches shared state -- the
   pending note-off, the start gate and key-on delay, the voice's draws from the shared random generator
   (the per-voice draw, and an S&H LFO's draw, taken here in the same order as before), the part's
   controls (`controls()`, its sums shared per part) -- then the voice's per-block state copied into
   the batch's **control lanes**: one array per field (structure of arrays), lane = voice.
2. **Control** (a kernel across the lanes): LFO, the EGs, pitch → phase increment, cutoff glide →
   filter coefficient and damping, the output stage's glides and the per-output gain ramps. Its results
   go back to the voices (scatter), and the signal passes read them as today.
3. **Signal** (as today): samples and peak per voice, the filter across the batch, EQ, fade and the mix
   per voice.

**Exact by construction.** The reference is the engines' own per-voice code, unchanged: a build
without a target's control kernel runs it voice by voice, bit for bit as today. A target's kernel takes
the steady states (an EG stepping toward its target, the LFO running, the glides) with the reference's
operations in the reference's order; any lane that would leave them this block -- an EG reaching a
stage end, a recompute, an LFO delay or fade, a wave the kernel does not do -- is flagged, not
written, and runs the reference code on its untouched voice. The lanes are a copy, so a flagged lane
costs its gather and nothing else. The one inexact step allowed (docs: speed over exactness) is a
vector sine for the 2006LE's sine LFO; the reference's `std::sin` stays the reference.

**Shared between the engines** (`src/accel/control*.hpp`): the step toward a target with its done mask
(the 2006LE's `step`, the S-YXG50's `glide`: the same shape, a different landing rule), the attenuation
→ gain (`mant / (131071 · 2^octave)`, both engines' `lin` / `eg_gain`), the gain-ramp builder (start,
step and the "receives anything" mask per output), and the scalar fallback plumbing. Each engine keeps
its own lanes and kernel; what differs is the EGs' stages and the LFOs.

**Per target.**

- **X280 (RVV 1.0, VLEN 512):** e32 at m1 is 16 lanes, so a 32-voice batch is two groups; the 2006LE's
  x87-double products at e64 m2. Tables (pitch mantissas, filter ceilings) by indexed loads. The
  scalar core reads the results once per batch (a scalar read waits for the vector queue to drain, so
  once, not per voice).
- **C920 (RVV 0.7.1, VLEN 128):** the same source: 4 lanes at e32 m1, so m4 for 16; no fractional
  LMUL is needed (control is all 32- and 64-bit); e64 for the doubles. Its vector unit sits in the
  out-of-order core, so the scalar read-back costs less than on the X280.
- **A25 (Andes DSP, RV32, no vector unit):** the lanes run the reference code in a lane loop -- no
  SIMD gain for 32-bit control, but straight-line, cache-friendly passes. Its gain comes with the
  integer forms (i32 / i16): gains from a 256-entry mantissa table and a shift instead of a float
  division, packed 16-bit gains (`smmwb`, `kmda`) in the mix.

**Built so far (2026-11-09), the 2006LE:** the voice's block split into begin / control / signal /
end, the batch's control through `accel::control_le` (`src/accel/control_le*.{hpp,cpp}`), reading and
writing the voices' fields in place by indexed loads and stores (offsets from `DspVoice::fields_le`), the
lanes carrying only the block's inputs and the byte flags. Checked lane by lane against the reference
(`HVYXG_CHECK_CONTROL`, a RISC-V build under qemu at VLEN 512: 0 differences in 8 songs; the kernel keeps
84-99.7 % of lanes) and on the Blackhole (6 song digests identical to the reference build). hvyxg is now
built without FMA contraction everywhere, so the reference is the same on every machine.

**What it bought, and why so little:** a voice 0.153 % → 0.146 % of an X280 core (128 voices, linear).
The full profile (every perf sample, not the top 24) of control alone says why: the kernel itself costs
about half the scalar code it replaces, but most of that is its indexed loads and stores (about one
element a cycle on the X280: some 150 fields a voice), and the rest of control -- the module's voice
loop, `controls()`, `begin`, filling the lanes -- is scalar and now the larger part, and it waits for
each batch's kernel rather than overlapping it.

**The X280's costs, measured** (`vmembench`, Hira-V's `apps/bench`: 32 voices of 256, 1 KiB records,
cycles an element): indexed 2.58, indexed segment of 8 fields 0.45, unit-stride 0.25, strided 1.39,
indexed store 2.34. So the kernel now reads and writes the voices' fields as **runs** -- each voice's
fields reordered into runs of up to 8 consecutive words (`FieldsLE`, checked at compile time), one
segment load or store a run -- and only the active voices are touched (a structure-of-arrays layout over
the whole pool would pay for every pooled object: with ~34 voices sounding of 256, more than segments).
**Vector register spills are expensive** (a spill's address is computed from `vlenb`): the kernel runs
in four phases (the amp EG first, as it decides which lanes stay; the LFO; pitch and filter; the output
stage), a loop over the lanes each, passing values through the lanes (unit-stride). A 2006LE voice:
0.153 % → 0.141 % of a core, exact.

**What is left, and why:** a scalar load after vector stores waits for the vector queue to drain, and
the scalar side reads the kernel's results straight away (`render_end`'s mix, `signal`'s increment,
`from_lanes`), so the scalar and vector work still run one after the other; and `controls()`, `begin`,
filling the lanes and the module's voice loop stay scalar. Next: the batch's whole block -- control,
samples, filter, mix -- issued without a scalar read of a vector result in between (the scalar side
working on the next batch's controls meanwhile), and `controls()` in lanes.

**The vector unit's budget a 2006LE voice and 64-sample block** (Hira-V's `xgkbench`, X280, cycles):
control_le 340; resample linear 425, Hermite 722, Lanczos 1123; peak_ramp 163; svf_le_lanes 176; mix_ramps
108 (2 buses) to 288 (7). A dependent `vfdiv` is about 127 cycles; a scalar load waits for the vector
unit only where it reads a word a vector store has not yet written (`vmembench`'s sync rows), so scalar
and vector work do overlap where the scalar side does not read what is in flight.

**Two batches in flight** (ToneModule2006::render): a full batch's control is issued, then the batch
before it finished, voice order and the generator's draws unchanged (exact: 16 renders bit for bit,
0 differences lane by lane, the Blackhole's digests identical). Songs, where the scalar side was the
limit: `c.mid` 8.08 % → 6.44 % of an X280 core (linear). 128 sustained voices, where the vector unit is
now the limit: 0.141 % → 0.150 % a voice (0.153 % without the kernel). Next: less vector work (the
resampler and the peak are the largest), and `controls()` / filling the lanes, still scalar.

**The S-YXG50 (2026-11-09):** the same restructure -- ChipVoice's block in begin / control / signal /
end, its control fields in runs, two batches in flight -- took a voice from about 0.085 % to 0.065 % of
an X280 core (128 voices, 48 kHz), exact. Its control kernel (`accel::control_50`, exact: 0 differences
lane by lane at 44.1 and 48 kHz and in MU mode) does not pay there -- the S-YXG50's control is light,
and its time is in the resampler's 8-bit gathers and conversion and the filter -- so it is opt-in
(`HVYXG_CONTROL_50`), kept for targets where it may (the C920). At 48 kHz a cutoff that moves still
costs a scalar asin and sin a block (svf_f, remembered while it holds still). The shared RVV helpers are
`src/accel/lanes_rvv.hpp`.

**The resamplers at LMUL 4 (2026-11-09).** The X280's vector unit is in order: a chunk's work is one
dependent chain, and its latency, not its operations, set a block's time. At LMUL 4 a block takes half
the chains (a 2006LE block one): cycles a voice and block (xgkbench), before → after -- S-YXG50 8-bit
linear 901 → 669, Hermite 1674 → 1500, 16-bit linear 1021 → 888, Hermite 1674 → 1500; 2006LE linear
424 → 389, Hermite 722 → 662. (LMUL 8 was slower: the registers spill.) The 8-bit linear run also takes
each lane's pair in one gather of 16-bit words of consecutive bytes. An S-YXG50 voice 0.064 % → 0.051 %
of a core (128 voices); exact throughout (accel_check, the Blackhole's digests).

**(Superseded) the voices' state in lanes for good.** The per-block state lives in structure-of-arrays form in
the module (one array per field, indexed by voice object), DspVoice reaching its fields there: the
kernel then reads and writes unit-stride (16 lanes in a couple of cycles, not 16), over all objects at
once with the inactive masked, and nothing is filled or copied. Then `controls()` and `begin` in the
same lanes, so the scalar side is the events alone. The same for the S-YXG50's ChipVoice.

**Order of work:** the 2006LE (the larger share) -- gather / control / scatter with the reference
voice by voice, bit for bit; then its RVV kernel, checked lane by lane against the reference
(`tools/accel_check.cpp`: random voice states, every field after a block); then the S-YXG50 the same
way; then the C920 and the A25.

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
