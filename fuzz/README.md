# Fuzzing

libFuzzer targets with AddressSanitizer and UndefinedBehaviorSanitizer for every input hvyxg
reads from outside: bank files, MU ROMs, MIDI messages, MIDI byte streams and SMF files.

| Target | Input |
|--------|-------|
| `fuzz_syxg50_bank` | S-YXG50 bin / wave tables (classic layout), then programs, drum kits and notes played on the loaded bank |
| `fuzz_mu_bank` | SXG-Create-NF MU tables (`enable_mu`, big and classic layouts, MAP2 voice-map appendix, forced layouts) |
| `fuzz_syxg2006le_bank` | S-YXG2006LE voice table (header, segments, voice / element records, block references) and encrypted wave table |
| `fuzz_mu_rom` | MU ROM set detection (`find_mu_roms`) on fuzzed files, the S-MU2000 flash split, interleave / dejumble, MU2000 scaling and the table decoder on fuzzed images |
| `fuzz_midi` | `Synth::midi`: channel messages, XG parameter changes and bulk dumps (with checksums), GS DT1, universal SysEx, malformed messages, frame offsets, ports, resets, renders, on all three engines and several synth options |
| `fuzz_midi_stream` | `Synth::midi_stream`: running status, real-time bytes, SysEx split across calls, stray bytes |
| `fuzz_smf` | the renderers' Standard MIDI File reader (`tools/render/lib/midifile`, Hira-V's, copied); every event must be a well-formed UMP message (type 2 or 3) at a non-decreasing time |

The banks are synthetic (`fuzz_support.cpp` builds small S-YXG50, MU big-layout and
S-YXG2006LE banks from scratch), so the targets need no Yamaha files and the seeds contain no
bytes of them. `fuzz_midi` and `fuzz_midi_stream` play the real banks instead when
`HVYXG_TBL_BIN` / `_DATA`, `HVYXG_2006LE_BIN` / `_DATA` and `HVYXG_MU_BIN` / `_DATA` are set
(better coverage of the tone modules). Rendered samples must stay finite. The engines read their
tables from the DLLs: every target that plays a bank needs `HVYXG_DLLS` set to a folder with
syxg50.dll and the S-YXG2006LE DLL, and stops with a message when it is not (also when the
regressions are replayed by `ctest`).

## Build and run

libFuzzer needs LLVM clang (Apple clang has none; use Homebrew's `llvm`):

```sh
cmake -S . -B build-fuzz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++ -DHVYXG_BUILD_FUZZERS=ON
ninja -C build-fuzz
build-fuzz/hvyxg_fuzz_seeds seeds                 # seed corpora, one folder per target
mkdir -p corpus/fuzz_midi
build-fuzz/fuzz_midi -max_total_time=3600 -timeout=20 -max_len=4096 corpus/fuzz_midi seeds/fuzz_midi
ctest --test-dir build-fuzz                       # replays fuzz/regressions/
```

Bank targets want `-max_len=65536`. Without libFuzzer (Apple clang, GCC) the same targets
link a small standalone driver (`standalone_main.cpp`) that replays files and directories and,
with `-runs=N`, mutates them at random (`-seed=S`, `-max_len=L`; the input being run is kept
in `./crash-standalone` until the run ends).

`-DHVYXG_SANITIZE=ON` builds the library, tools and tests with ASan + UBSan without the
fuzzers (e.g. to run `hvyxg_tests` under them); `-DHVYXG_SANITIZE=thread` with
ThreadSanitizer (`tests/threading_test.cpp`).

## Regressions

`regressions/<target>/` holds the input of every bug found; `ctest` replays them when the
fuzzers are built, and `tests/robustness_test.cpp` covers each in the normal test suite.

## Campaign so far

LLVM clang 22 libFuzzer + ASan + UBSan on an Apple M1 Max, November 2026: per target a 3-min,
a 30-min and a 90-min run (fuzz_midi on the fixed code another 60 min, and 30 + 70 min with
the real banks; fuzz_midi_stream 90 min with the real banks). Runs in the last campaign:
fuzz_smf 37 M, fuzz_syxg2006le_bank 2.0 M, fuzz_syxg50_bank 1.5 M, fuzz_mu_bank 1.3 M,
fuzz_mu_rom 1.1 M, fuzz_midi_stream 0.9 M (+ 0.8 M real banks), fuzz_midi 0.5 M (+ 0.5 M real
banks). Bugs found (all fixed, each with a regression test):

| Target | Bug |
|---|---|
| fuzz_smf | an empty escaped SysEx (`F7 00`) became an empty message (hvyxg's former reader) |
| fuzz_mu_rom | MU2000 scaling read the image before its bounds check |
| fuzz_syxg2006le_bank | voice-table record offsets outside their segment (out-of-bounds read at load) |
| fuzz_syxg2006le_bank | a group pitch curve row missing from the bank threw out of `Synth::midi` |
| fuzz_midi (real MU bank) | S-YXG50 EG: float → int conversion of ±inf for a zero slope (also found by the test suite under UBSan) |
| fuzz_midi (synthetic and real banks) | room reverbs with a long time in the smallest room read past the comb gain table |

The test suite under ASan + UBSan found one more (the EG conversion above) and an
out-of-bounds read in a test helper; ThreadSanitizer found nothing.
