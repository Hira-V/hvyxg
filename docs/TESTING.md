# Testing

```sh
cmake -S . -B build -DHVYXG_WERROR=ON
cmake --build build -j
./build/hvyxg_tests        # the unit and engine tests (doctest)
./build/hvyxg_alloc_test   # rendering, MIDI and resets never allocate
(cd build && ctest)        # both, and the core linked alone (hvyxg_core_link)
```

## The assets

hvyxg ships none of Yamaha's data, so the tests that run an engine need your DLLs (for their
tables) and the bank-dependent tests your banks. Without them those tests skip, with a message;
everything else runs. They are looked for in a local asset tree, `_untracked/` in the source tree
(git-ignored: never commit its contents), or `$HVYXG_ASSETS`:

```
roms/yxg50/       sxgbin41.tbl, sxgwave4.tbl (and the 2 MB pair)
roms/yxg2006le/   SXGBNW6L.TBL, SXGDAT6L.TBL
roms/mu1000/ …    MU ROMs (searched recursively), e.g. mu1000-v2.01-h/l.bin, mu2000_flash.bin, wave ROMs
tbl/mu1000/       SXG-Create-NF's SXGMU1KX.TBL / .UPCM (MU2000: tbl/mu2000/SXGMU2KX.*)
tbl/mu1000-embed/ syxg1000.dll made by SXG-Create-NF --embed (optional)
dll/              syxg50.dll and S-YXG2006LE.DLL: the engine tables (any PE file here is searched)
midi/             songs
```

Environment variables override single files (use `export`; zsh does not word-split
`env $VARS`):

```sh
export HVYXG_DLLS=…/dll                   # a folder of DLLs for the engine tables (default dll/)
export HVYXG_TBL_BIN=…/sxgbin41.tbl HVYXG_TBL_DATA=…/sxgwave4.tbl
export HVYXG_2006LE_BIN=…/SXGBNW6L.TBL HVYXG_2006LE_DATA=…/SXGDAT6L.TBL
export HVYXG_MU_BIN=…/SXGMU1KX.TBL HVYXG_MU_DATA=…/SXGMU1KX.UPCM   # an SXG-Create-NF MU bank
export HVYXG_MU_ROMS=…/roms/mu1000:…/roms/waves   # MU ROM files / dirs (':'-separated); default roms/
export HVYXG_MU2000_BIN=…/SXGMU2KX.TBL HVYXG_MU2000_DATA=…/SXGMU2KX.UPCM   # optional: MU2000 check
export HVYXG_MU_PLUGIN=…/syxg1000.dll   # optional: an SXG-Create-NF --embed plugin of the MU bank
```

## Build checks

- `hvyxg_header_check_files` and `hvyxg_header_check_nofiles` compile every public header on its
  own, with and without `HVYXG_NO_FILES`.
- `hvyxg_core_check` compiles the core (what an embedded host builds) with `-fno-exceptions
  -fno-rtti -DHVYXG_NO_FILES=1`, and `hvyxg_core_link` links it alone against every embedded entry
  point (ENGINE.md, "Embedded hosts").
- `-DHVYXG_SANITIZE=ON` builds everything with AddressSanitizer and UndefinedBehaviorSanitizer
  (`=thread` for ThreadSanitizer; `tests/threading_test.cpp` renders four synths on one bank on
  four threads).

## Fuzzing

`-DHVYXG_BUILD_FUZZERS=ON` adds a fuzz target for every input hvyxg reads from outside: bank
files, MU ROMs, MIDI messages and byte streams, and the renderers' MIDI file reader. Each
regression found is kept and replayed by `ctest`: [fuzz/README.md](../fuzz/README.md).

## Against the DLLs

The reference renders come from the real DLLs, run as black boxes by tools kept outside this
repository; `render_yxg` renders the same MIDI file through hvyxg, and pitch, level, spectra and
envelopes are compared. The results: [ENGINE.md](ENGINE.md), "Current match against the DLLs".
