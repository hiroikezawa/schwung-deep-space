# Third-party code in Deep Space Convolution

Deep Space Convolution's DSP is entirely original (Copyright (c) 2026 Hiro
Ikezawa, MIT, see `LICENSE`). The textbook primitives it uses — a radix-2
FFT, uniformly / non-uniformly partitioned overlap-save convolution, one-pole
filters, Schroeder all-passes, a Hadamard feedback-delay network with Jot-style
absorption filters, cubic Hermite interpolation, a xorshift noise source — were
written here from the mathematics; no DSP code was copied from any project and
no FFT library is linked.

The six impulse responses are SYNTHETIC: generated at run time from fixed
seeds by `dsp/ds_irgen.c` (also MIT, ours). No recorded or third-party IR is
shipped, so there are no IR licence or redistribution conditions.

Nothing GPL, LGPL or AGPL is compiled in, linked or copied: `deep-space.so`
needs only `libc` and `libm` (`scripts/check_abi.py` fails the build
otherwise), and `scripts/check_licenses.py` fails if copyleft text appears in
the source.

The only third-party files are Schwung's plugin API headers, used unmodified
to compile against the host ABI.

## Schwung plugin API headers — MIT

| | |
|---|---|
| Files | `third_party/schwung/host/audio_fx_api_v2.h`, `third_party/schwung/host/plugin_api_v1.h` |
| Upstream | https://github.com/charlesvestal/schwung (`src/host/`) |
| Copyright | Copyright (c) 2025-2026 Charles Vestal |
| Licence | MIT, reproduced below (also `third_party/schwung/LICENSE`) |

They are compiled into `deep-space.so` only as declarations (struct layouts,
constants); no Schwung code is linked.

```
MIT License

Copyright (c) 2025-2026 Charles Vestal

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

---

## Fork Provenance

Schwung (formerly Move Everything) is a ground-up rewrite that began as a fork
of [Move Anything](https://github.com/bobbydigitales/move-anything) by Bobby
Digitales. The original SPI communication techniques from that project provided
the foundation for accessing Move's hardware. Thanks to @talktogreg, @impbox,
@deets, and especially @bobbyd for those contributions.

---

This project includes third-party code under various licenses.
See THIRD_PARTY_LICENSES.md for details.
```
