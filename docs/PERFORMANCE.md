# Performance

CPU cost and memory of hvyxg's rendering, the optimisations made, and the polyphony cap.
Measured with `hvyxg-bench` (`tools/hvyxg_bench.cpp`).

## How it is measured

- Release build (`-O3`), Apple clang 17, Apple M1 Max (performance cores), macOS 15, 44.1 kHz,
  linear interpolation unless noted.
- Times are the rendering thread's CPU time (`CLOCK_THREAD_CPUTIME_ID`), the best of 3 runs.
  "Load" = CPU time / audio time: 100 % = one core in real time; real-time factor = 100 / load.
- The machine was shared with other jobs (load average 6 … 15 during the final runs, up to
  100 earlier); thread CPU time is fairly robust against that, and repeated runs agreed within
  about 3 %.
- Sustained voices are organ notes (program 16) spread over the melodic channels, with
  reverb, chorus and variation off unless the test turns them on.

```sh
hvyxg-bench --plugin-path DLLS --bin B --data D voices 0 1 8 32 128 512 1024   # load per voice count
hvyxg-bench --plugin-path DLLS --bin B --data D effects                       # extra load per effect type
hvyxg-bench --plugin-path DLLS --bin B --data D worst 1024                    # everything on + note-on bursts
hvyxg-bench --plugin-path DLLS --bin B --data D song *.mid                    # real-time factor, worst block
```

## Voices

Load grows linearly with the voices sounding, about **350 µs of CPU per voice per second of
audio (0.035 % of a core)**, the same for both engines and the MU bank:

| Voices | S-YXG50 | S-YXG2006LE | MU1000 bank |
|---:|---:|---:|---:|
| 1 | 0.04 % | 0.05 % | — |
| 32 | 1.13 % | 1.14 % | 1.08 % |
| 128 | 4.46 % | 4.59 % | 4.31 % |
| 256 | 9.02 % | 9.25 % | 8.95 % |
| 512 | 18.2 % | 19.4 % | 18.3 % |
| 1024 | 37.1 % | 36.5 % | 37.4 % |

Interpolation (128 voices): S-YXG50 Hermite (also used for `Lanczos3` on that engine) 4.53 %,
the same as linear after the fast path below (5.80 % before); S-YXG2006LE Hermite 4.71 %,
Lanczos-3 5.18 %.

Memory: a synth takes 3.4 MB (S-YXG50) / 3.9 MB (S-YXG2006LE) at polyphony 1, of which 2 MB
are the four XG2.0 insertion units, plus about 1.2 KB (S-YXG50) / 2 KB (S-YXG2006LE) per voice:
4.6 / 5.9 MB at 1024 voices. The bank itself is shared (its size is the files' size).

## Effects

Extra load of each effect on 16 sustained voices (0.56 % without effects), sends at 100:

| Block | S-YXG50 | S-YXG2006LE | Most expensive |
|---|---|---|---|
| Reverb (7 types) | 0.10 … 0.12 % | 0.09 … 0.12 % | Hall 1, Tunnel |
| Chorus (5 types) | 0.05 … 0.11 % | 0.04 … 0.12 % | Chorus 1, Celeste 1 |
| Variation, system or insertion connection (26 types) | 0.05 … 0.18 % | 0.03 … 0.20 % | Phaser 1, Auto Wah |
| XG2.0 insertion block (26 types + Compressor, Noise Gate, Lo-Fi, Ring Mod) | 0.06 … 0.14 % | 0.04 … 0.14 % | Auto Wah, Phaser 1 |

Effects cost little next to the voices: every block on at once (reverb, chorus, variation as
insertion, four insertion blocks: distortion, phaser, rotary, auto wah; the parts of those
blocks render through their chains) adds 0.7 % of a core at 32 voices and 3.6 % at 1024
(*Worst cases* against *Voices*).

## Worst cases

Every effect and all four insertion blocks on, all voices sounding (load), and the slowest
block in which 64 note-ons arrive at once, each stealing a voice (a block's CPU time against
its real-time budget):

| Voices | Load | Burst block: S-YXG50 | S-YXG2006LE | MU1000 bank |
|---:|---:|---:|---:|---:|
| 32 | 1.8 % | 4 % | 4 % | 5 % |
| 128 | 5.5 % | 11 % | 11 % | 15 % |
| 256 | 10.5 % | 18 % | 21 % | 22 % |
| 512 | 20.4 % | 34 % | 38 % | 41 % |
| 1024 | 40.7 % | 75 % | 75 % | 80 % |

Before the steal-path changes below the burst blocks took 17 / 10 / 21 % (128 voices) and
237 / 218 / 207 % (1024 voices) of their budget: a dropout at high polyphony.

## Songs

The ten test songs as `render_yxg` plays them (128-frame host blocks), load and the
slowest block against its budget, before and after the optimisations (two development builds of 0.1.0):

| Song | Length | S-YXG50 voices ≤ | load before → after | worst block | S-YXG2006LE load before → after | worst block |
|---|---:|---:|---:|---:|---:|---:|
| moon_p | 92 s | 100 | 1.34 → 1.15 % | 4.4 % | 1.39 → 1.29 % | 2.9 % |
| TrekMedG | 487 s | 146 | 2.03 → 1.68 % | 10.6 % | 1.38 → 1.28 % | 4.2 % |
| Nes_Netopia_mix | 266 s | 99 | 1.83 → 1.51 % | 5.3 % | 1.39 → 1.30 % | 4.1 % |
| ORC4GM | 141 s | 47 | 1.24 → 1.04 % | 3.0 % | 1.36 → 1.27 % | 3.7 % |
| ORC2GM | 141 s | 40 | 0.95 → 0.83 % | 2.9 % | 1.18 → 1.11 % | 2.6 % |
| Be_with_me | 264 s | 37 | 0.84 → 0.71 % | 2.4 % | 0.90 → 0.84 % | 2.9 % |
| HUMAN3GM | 130 s | 40 | 0.97 → 0.84 % | 2.7 % | 1.11 → 1.07 % | 2.8 % |
| Chaotic_destiny | 271 s | 48 | 0.98 → 0.83 % | 3.3 % | 1.20 → 1.11 % | 3.4 % |
| Dubnesstopia | 317 s | 78 | 1.13 → 0.95 % | 5.7 % | 1.12 → 1.05 % | 3.3 % |
| Full_Memory | 384 s | 44 | 1.12 → 0.94 % | 5.2 % | 1.18 → 1.10 % | 4.5 % |

A dense song renders 50 … 140 times faster than real time on one core. The MU1000 bank
(`--enable-mu`) costs the same as the stock S-YXG50 bank (0.68 … 1.71 %). Whole
`hvyxg-render` runs (TrekMedG, SMF parsing and WAV writing included; now `render_yxg`): S-YXG50 9.9 → 8.2 s,
S-YXG2006LE 6.7 → 6.3 s of CPU time.

## Profile

macOS `sample` while rendering TrekMedG: the voices dominate. S-YXG50: `ChipVoice::render` +
`fetch` 85 % of the samples, reverb 3 %, chorus 2 %. S-YXG2006LE: `DspVoice::render` +
`render_samples` 75 %, reverb 4 %, the per-voice controller sums 3 %, chorus 2 %. Note-on
bursts at high polyphony were dominated by the steal search (below).

## Optimisations

Each keeps the output bit-identical: the WAVs of the ten songs on both banks, the MU1000 bank
(moon_p) and 14 option renders (polyphony 24 / 12 / 40 / 20 / 8 / 6 to force steals, steal
fade, Hermite, Lanczos-3, DLL-exact, MU at polyphony 48) have the same checksums before and
after.

- **S-YXG50 sample fetch** (`chip.cpp`): when every interpolation point lies inside
  `[−pre, loop length)`, the samples are read directly; `set_wave` has already checked that
  the wave covers that range (else the voice has ended), so the per-sample clamp, loop wrap
  and bounds checks of `sample_at` are skipped. Linear and Hermite.
- **Ramp mixing** (both engines): a voice mixes into up to seven buses, each with its own gain
  ramp (`g += dg` per sample, an add chain the compiler must keep in order). The buses that
  receive anything are now mixed in one pass (a loop per bus count), so the chains overlap;
  each bus still sees exactly the same operations.
- **Steal path** (S-YXG50 `make_room`): a note's score was found by a voice scan per voice
  (`group_of`), and the victim part by a voice scan per part, O(n²) per steal. Now the part
  owners come from one counting pass and the note scores from one sorted pass, O(n log n).
- **Note list pruning** (S-YXG2006LE `prune`, every note-on): each note object scanned every
  slot; now the live serials are collected and sorted once.

## Allocation and locks

`render` never allocates, from the first call on; `midi`, `midi_stream` and `reset` do not
either within their documented capacity (1024 pending messages / 16 KB, 4 KB SysEx). Checked
by `hvyxg_alloc_test`, which replaces the global `operator new` (ENGINE.md, "Threads and
real-time use"). Nothing in the library takes a lock.

## Polyphony cap

`Synth::Options::max_polyphony` is at most `Synth::kMaxPolyphony` = **1024**; larger values
make the constructor throw `std::invalid_argument` (`render_yxg --polyphony` rejects them).
At 1024 voices everything on costs about 41 % of a fast core and a 64-note steal burst 75 …
80 % of its block, so more voices could not be played in real time even here; slower machines
should stay well below (256 voices: 10 % load, bursts ≤ 22 %). The defaults are the DLLs' 128
(S-YXG50) and 32 (S-YXG2006LE). Memory is not the limit (about 2 KB per voice).
