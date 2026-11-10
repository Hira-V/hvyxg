# hvyxg

hvyxg is a new XG MIDI synthesizer in the form of a BSD-licensed C++20 library, modeled on the
Yamaha S-YXG50 and S-YXG2006LE software synthesizers. hvyxg is capable of using original Yamaha
sound banks with high accuracy and fidelity, with a design supporting accelerated RISC-V paths for
[Hira-V](https://hira-v.com).

For even more flexibility and fidelity, hvyxg can also utilize MU-series sound banks. hvyxg adds
XG2.0 insertion effects, variation types, part and drum EQs, gentler voice stealing and greater
polyphony over the original Yamaha S-YXG-series soft-synths. A DLL-exact mode turns these additions
off.

hvyxg contains no Yamaha data. You must supply sound banks you own. Engine tables are read at run
time from your own DLLs. DLLs and stock sound banks are identified by content and checked by hash,
so any build of them will do.

> hvyxg is not an official Yamaha project, and is not affiliated with Yamaha. Yamaha and XG are
> trademarks of Yamaha Corporation; S-YXG50, S-YXG2006LE, MU1000, MU2000 and MU128 refer to original
> Yamaha products.

## Yamaha's data: not included, not to be shared

**This repository publishes only source code.** It contains no Yamaha sound banks, DLLs, ROMs, wave
data or firmware, nor any table copied from them, and it never will. To use hvyxg with Yamaha sound
banks, you must supply your own: the S-YXG50 and S-YXG2006LE banks and DLLs from your own copies of
those products, or the MU ROMs extracted from your own MU1000, MU2000 or MU128 hardware.

Notice:

* Do not redistribute Yamaha sound banks (`.TBL`), Yamaha DLLs, ROM images or wave data anywhere,
  including GitHub.
* Do not post them, or anything made from them, in Issues, Pull Requests, Discussions, Releases or
  attachments: that also includes tables converted from MU ROMs (`.TBL` / `.UPCM`) by tools like
  SXG-Create-NF, patched DLLs (they contain Yamaha's code), hvyxg's MU caches, or renders of whole
  banks. Hashes (CRC-32, XXH3), logs, MIDI files and short recordings are enough for bug reports.
* These files are handled separately from hvyxg's source code and are not covered by its licence
  below.

## Highlights

- **Modelled on original synthesizers, and measured against them.** Envelopes, filters, pitch,
  levels, pan, LFOs, the voice manager and every effect follow original references, and are checked
  against their renders: isolated notes render within 0.5 dB and 1 cent, whole songs at spectral
  correlations of 0.996 to 0.99999.
- **Two engines, multiple sound banks.** Models both the S-YXG50 (4 MB and 2 MB banks, 128 voices)
  and the S-YXG2006LE (32 DSP voices, its XGlite subset or full XG), and also supports MU-series
  banks, second voice engine included, via a built-in converter.
- **Full XG effects.** Reverb, chorus and 43 variation types, in system or insertion connection,
  plus the XG2.0 insertion blocks and multi EQ. Reverb, chorus, and every variation type matches
  original references at 0.99999 or better.
- **Real-time safe.** `render`, `midi` and `reset` never block, lock, throw or allocate. One loaded
  bank serves any number of synths, on any threads.
- **Embeddable.** The core builds without exceptions, RTTI or files, keeps no writable globals, and
  works over memory the caller owns: the engine tables, the banks and the synth. It takes Universal
  MIDI Packets and renders 32-bit integers or floats, at any rate from 8 to 192 kHz.

## Performance

Cost on one Apple M1 Max core at 44.1 kHz (100 % is one core in real time). Methods and the figures
for each song and effect are in [docs/PERFORMANCE.md](docs/PERFORMANCE.md).

| Voices sounding | YXG50 | YXG2006LE | MU1000 bank |
|---:|---:|---:|---:|
| 32 | 1.1 % | 1.1 % | 1.1 % |
| 128 | 4.5 % | 4.6 % | 4.3 % |
| 1024 (the cap) | 37 % | 37 % | 37 % |

Ten real songs cost 0.7 to 1.7 % (YXG50) and 0.8 to 1.3 % (YXG2006LE) of a core; reverb, chorus and
variation add 0.03 to 0.2 % each. A synth takes about 3.5 MB (2 MB of it the XG2.0 insertion units)
and 1.2 to 2 KB a voice; the bank is shared.

## Quick start

```cpp
#include "hvyxg/hvyxg.hpp"

void play_a_note() {
    hvyxg::Synth::Options o;
    o.bin_file = "sxgbin41.tbl";        // the S-YXG50 bank (or SXGBNW6L.TBL: S-YXG2006LE)
    o.data_file = "sxgwave4.tbl";       //   ... and its wave table (SXGDAT6L.TBL)
    o.plugin_files = {"syxg50.dll"};    // the engine tables (or S-YXG2006LE.DLL)
    o.sample_rate = 48000;
    hvyxg::Synth synth(o);              // throws hvyxg::BankError if anything is missing

    const uint8_t note_on[] = {0x90, 60, 100};   // channel 1, middle C
    synth.midi(note_on);
    static float out[2 * 480];                   // stereo interleaved: L, R, L, R, ...
    for (int block = 0; block < 100; ++block) {  // one second, 10 ms at a time
        synth.render(out, 480);                  // full scale +-1.0: hand it to the audio output
    }
}
```

Build it with CMake (`add_subdirectory(hvyxg)` or `find_package(hvyxg)`, then link `hvyxg::hvyxg`).

## API

In `include/hvyxg/` (all of it: `hvyxg/hvyxg.hpp`), namespace `hvyxg`:

| Call | |
|---|---|
| `Synth(options)` | A synth: loads the bank and the engine tables the options name (files, folders searched by content, DLLs) |
| `Synth(bank, settings)` | A synth on a bank loaded before; never throws |
| `midi(bytes, offset, port)` | One MIDI message or SysEx, at a frame of the next render |
| `midi_stream(bytes, offset, port)` | A raw byte stream: running status, real-time bytes, SysEx split across calls |
| `ump(words, offset)` | One Universal MIDI Packet: MIDI 1.0 channel voice, 7-bit SysEx, System Reset; the group is the port |
| `render(float*, frames)` | Interleaved stereo, full scale ±1.0 |
| `render(int32_t*, frames)` | The same at a 16-bit sample's scale, not clipped |
| `reset()` | Power-on: XG mode, pending messages dropped |
| `load_bank(...)` | A bank from files, vectors, or memory the caller owns (`BankMemory`), shared by any number of synths |
| `load_engine_tables(tables, dll)` | The engine tables from a DLL image into an `EngineTables` the caller owns |
| `convert_mu_roms(...)`, `load_mu_roms(...)` | An MU bank from MU1000 / MU2000 ROM dumps |
| `model()`, `voice_count()`, `active()`, `part_program(i)` | What the synth is playing |

| Option | |
|---|---|
| `sample_rate` | 8 to 192 kHz (default 44.1) |
| `max_polyphony` | Up to 1024 voices (reference synth default: 128 or 32) |
| `interpolation` | `Linear` (reference), `Hermite`, `Lanczos3` |
| `ports` | 1 to 4 MIDI ports of 16 parts |
| `enable_mu` | An MU-series sound bank |
| `full_xg_2006le` | Full XG on the S-YXG2006LE bank instead of its XGlite subset |
| `steal_fade_ms`, `mu_part_eq` | Supersets: a stolen voice fades out; the part EQ at the MU2000's strength |
| `dll_exact` | Every superset off: the output follows the reference synth |

Errors are `hvyxg::BankError` in the API that reads files, and `hvyxg::Error` in the core's, which
builds with `HVYXG_NO_FILES=1` for hosts without exceptions or files. The version is in
`hvyxg/version.hpp` (`HVYXG_VERSION_STRING`, `"0.2.0"`). Threads, the embedded core and how each
engine follows its reference: [docs/ENGINE.md](docs/ENGINE.md).

## The command-line renderer: render_yxg

`render_yxg` renders a Standard MIDI File to a WAV:

```sh
render_yxg --plugin syxg50.dll --bin sxgbin41.tbl --data sxgwave4.tbl song.mid song.wav
render_yxg --plugin-path dlls/ --bank-path banks/ song.mid song.wav       # the bank found by content
render_yxg --plugin syxg50.dll --bank-path roms/ --mu-model mu2000 song.mid song.wav   # MU2000 ROMs
render_yxg --plugin S-YXG2006LE.DLL --2006full --bin SXGBNW6L.TBL --data SXGDAT6L.TBL song.mid song.wav
```

It is also the shortest example of the library. `tools/render_yxg.cpp` puts `Synth` behind Hira-V's
instrument interface, `midi::Instrument`: one Universal MIDI Packet at a sample offset into the next
block, then the block. `tools/render/` does the rest: it reads the file, plays each event at its
sample, and writes the WAV. hvopl3's `render_opl3` is built on the same files, so the two take the
same common options: `--rate`, `--format f32|s16|s24`, `--gain`, `--tail`, `--ports`,
`--base-level`, `--solo`, `--quiet`. `tools/render/lib/` holds copies of Hira-V's UMP interface and
MIDI file reader; neither project depends on Hira-V. `render_yxg --help` lists every option.

## Building

```sh
cmake -S . -B build
cmake --build build -j
```

C++20 and CMake 3.24 or later; Apple clang, LLVM clang or GCC, on macOS and Linux. No network is
needed (doctest and xxHash are vendored). The targets: the static library `hvyxg`, `render_yxg`,
`hvyxg-bench` (CPU cost) and the tests. `-DHVYXG_WERROR=ON` makes warnings errors,
`-DHVYXG_SANITIZE=ON` builds with the address and undefined-behaviour sanitizers, and
`-DHVYXG_BUILD_FUZZERS=ON` adds the fuzz targets. `cmake --install build` installs the library, its
headers, a CMake package and `render_yxg`.

## Testing

```sh
./build/hvyxg_tests        # unit and engine tests
./build/hvyxg_alloc_test   # rendering never allocates
(cd build && ctest)        # both, and the embedded core linked alone
```

The engine tests need your DLLs and sound banks, in a local asset tree (`_untracked/`, never
committed) or named by environment variables; without them they skip. Every input from outside is
fuzzed. The asset tree, the build checks and the fuzzers: [docs/TESTING.md](docs/TESTING.md).

## Status

0.2.0 adds accelerated paths for Hira-V's RISC-V machines, built where the target defines them:
RVV 1.0 (the X280) and XTheadVector (the C920) with `HVYXG_ACCEL_RVV`, and the A25's in-order scalar
core with `HVYXG_ACCEL_DSP`. They cover the resamplers, filters, mixing and the S-YXG2006LE's
control across voices. Every engine also does less work a block: a voice keeps its controls while no
message has changed them, and without a vector unit the voices go in small batches. A host build
renders exactly what 0.1.1 did. The design, the checks and the figures:
[docs/ACCELERATION.md](docs/ACCELERATION.md).

0.1.1 revises the README; the code is unchanged from 0.1.0.

0.1.0, the first release: both engines, their effects, the MU banks and the MU ROM converter, the
supersets and DLL-exact mode, the embedded core, and the renderer.

Verified against renders from original Yamaha synths. MU128 table converting and usage are currently
untested. Sample rates other than 44.1 and 48 kHz have not been compared against the reference
synths.

## Repository layout

| Path | |
|---|---|
| `include/hvyxg/` | The public API |
| `src/` | The library: the MIDI front end, the two engines (`syxg50/`, `syxg2006le/`), the effects (`fx/`), the MU ROM converter (`syxg50/mu_rom/`), the engine table loader (`dll/`), the accelerated kernels (`accel/`) |
| `tools/` | `render_yxg`, its host `render/` (shared with hvopl3), and `hvyxg-bench` |
| `tests/` | Unit and engine tests, the allocation check, the embedded core's link check |
| `fuzz/` | Fuzz targets and their regressions |
| `third_party/` | doctest, xxHash |
| `docs/` | [The engine](docs/ENGINE.md), [performance](docs/PERFORMANCE.md), [the accelerated paths](docs/ACCELERATION.md), [testing](docs/TESTING.md) |

## License

Copyright (C) 2026 Kevin H. Patterson. Released under the BSD 3-Clause License: see
[LICENSE](LICENSE).

The built-in MU bank converter is a C++ port of
[SXG-Create-NF](https://github.com/NightFright2k19/SXG-Create-NF) (BSD 3-Clause), whose sample
decoding follows MAME (BSD 3-Clause). xxHash (BSD 2-Clause) and doctest (MIT) are vendored. Their
notices are in [THIRD_PARTY.md](THIRD_PARTY.md).

S-YXG50 and S-YXG2006LE sound banks, DLLs, and MU-series sound banks are property of Yamaha and are
not part of this project or covered by its licence: see "Yamaha's data" above.
