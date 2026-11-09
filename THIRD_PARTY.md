# Third-party material

hvyxg is BSD-3-Clause. Besides its own code it holds a C++ translation of one BSD-licensed
project (SXG-Create-NF's MU ROM converter), number tables and code taken from that project and
from MAME through it, two vendored libraries (xxHash, doctest), and copies of three files of
Hira-V, by hvyxg's author. Each is listed below with its licence, as the licences ask.

## SXG-Create-NF

`src/fx/fx_mu.cpp` holds two number tables taken from SXG-Create-NF's
`utility/dllpatch.py` (`SYXG50_FX`): the effect-type substitution table of the patched
`syxg50.dll` and its per-type return-level factors (measured by SXG-Create-NF on the S-MU2000).
The big table layout read by `src/syxg50/bank_data.cpp` follows SXG-Create-NF's
`utility/makeSYXG50.py` and `docs/models-and-output.md`.

The MU ROM converter in `src/syxg50/mu_rom/` (and `include/hvyxg/mu_rom.hpp`) is a C++
translation of SXG-Create-NF's converter at its commit 0086663: `utility/convert.py`,
`decBase.py`, `decMU90.py`, `decMU1000.py`, `elemreduce.py`, `scaling.py`, `drumtrim.py`,
`voicetrim.py`, `drumvel.py`, `SampleConvert.py`, `makeSYXG50.py`, `tableconvert.py`,
`table.py`, `dataCRCs.py`, `template.py` (file names) and `utils.py`, in the parts its default
conversion uses. `src/syxg50/mu_rom/tables.hpp` holds data tables
generated from those sources: the DPCM delta / limit tables, the measured drum level steps and
drum / voice level trims (keyed by CRC-32 of ROM records), the drum velocity conversion's cutoff
table (generated from its two linear segments; its FEG velocity curve is the S-YXG50's, read from
the DLL) and the measured drum decay slopes. The ROM CRC-32 list in `mu_rom.cpp` comes from `dataCRCs.py`.

```
BSD 3-Clause License

Copyright (c) 2026, Soundshock

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## MAME (via SXG-Create-NF)

SXG-Create-NF's `utility/SampleConvert.py` bases its DPCM delta table, limits table and
decoding code on MAME (`license:BSD-3-Clause`, `copyright-holders:Olivier Galibert`); hvyxg's
translation in `src/syxg50/mu_rom/sample_convert.cpp` and the `kDpcmTable` /
`kDpcmLimits` tables in `src/syxg50/mu_rom/tables.hpp` carry that part over (`kDpcmTable` is
generated there from its segment structure).

```
BSD 3-Clause License

Copyright (c) Olivier Galibert (MAME copyright holder of the DPCM code)

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## Hira-V

`tools/render/lib/midi.hpp` and `tools/render/lib/midifile.hpp` / `.cpp` are copies of
[Hira-V](https://hira-v.com)'s instrument interface and Standard MIDI File reader
(`software/lib/`; the reader as Hira-V builds it with `MIDIFILE_STANDALONE`), by Kevin H.
Patterson, released with hvyxg under hvyxg's BSD 3-Clause License. They are not part of the
library; the renderers use them (`render_yxg` here, `render_opl3` in hvopl3). Their comments'
references to `docs/` and `lib/` files are to Hira-V's tree.

## xxHash

`third_party/xxhash/xxhash.h` is xxHash 0.8.3 (https://github.com/Cyan4973/xxHash), unmodified,
used for the XXH3 128-bit hashes that identify the engine tables in a DLL (`src/dll/tables.cpp`).
Its licence is `third_party/xxhash/LICENSE`:

```
xxHash Library
Copyright (c) 2012-2021 Yann Collet
All rights reserved.

BSD 2-Clause License (https://www.opensource.org/licenses/bsd-license.php)

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions and the following disclaimer in the documentation and/or
  other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## doctest

`third_party/doctest/doctest.h` is doctest 2.4.11 (https://github.com/doctest/doctest),
unmodified, the test framework of `tests/`. Its licence is `third_party/doctest/LICENSE.txt`:

```
The MIT License (MIT)

Copyright (c) 2016-2023 Viktor Kirilov

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
