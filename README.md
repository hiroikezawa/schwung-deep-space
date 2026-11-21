# Deep Space Convolution

**English** | [日本語](README.ja.md)

An original Audio FX module for Schwung on Ableton Move (id `deep-space`).

Convolution with an IR (impulse response) creates the early reflections and the
character of a space, and its output excites a 16-line FDN (Feedback Delay Network)
that carries on into a late reverb of up to 30 seconds. The goal is not a stereo
widener but **depth: a cathedral, a vast cavern, an underground structure, a dark
endless space stretching far away**.

- 8 knobs on one page (Schwung's standard knob grid; knob 1 is MIX, knob 8 is SPACE).
  Works in a track slot and as **Master FX**
- All six IRs are **synthetic IRs** (generated at run time from fixed seeds). No recorded
  or third-party IRs are included
- DSP and FFT are entirely original code, MIT License (author Hiro Ikezawa). No external
  libraries
- Current version: 0.1.2

| Knob | Parameter | Range | Default |
|---|---|---|---|
| 1 | MIX | 0–100 % (equal power) | 40 % |
| 2 | DEPTH | 0–100 % | 60 % |
| 3 | SIZE | 0.50–4.00 x | 1.50 x |
| 4 | DECAY | 1.0–30.0 s | 8.0 s |
| 5 | DIFFUSION | 0–100 % | 70 % |
| 6 | DARKNESS | 0–100 % | 40 % |
| 7 | FREEZE | OFF / ON | OFF |
| 8 | SPACE | CATHEDRAL / CAVERN / TUNNEL / HANGAR / ABYSS / VOID | CATHEDRAL |

The defaults keep the input clearly audible with a huge depth behind it.

## Installation

### From a release (recommended)

1. Download the latest `deep-space-module.tar.gz` from [Releases](../../releases).
2. Upload it in Schwung Manager (`http://<Move's IP>:7700`) under Modules → "Install custom".
   From the command line:

   ```bash
   python3 scripts/install.py http://<Move's IP>:7700 --package deep-space-module.tar.gz
   ```

3. **Power-cycle Move once.** A DSP that has been loaded is not replaced by reloading
   the slot.

Tested on Schwung 1.7.3 (Ableton Move).

### Building from source

You need an aarch64 cross compiler (`gcc-aarch64-linux-gnu`) and Python 3. The Schwung
plugin API header is bundled in `third_party/schwung/`, so the Schwung source is not
needed (only the UI grid test uses a Schwung checkout, and it is skipped if there is none).

```bash
bash scripts/test.sh                                   # all tests (about 1 minute)
bash scripts/build.sh arm64                            # creates dist/deep-space-module.tar.gz
python3 scripts/install.py http://<Move's IP>:7700     # upload through Schwung Manager
bash scripts/render_demo.sh                            # writes listening WAVs to build/demo/
```

## Usage

1. Put **Deep Space Convolution** in the Audio FX of a track slot (Signal Chain), or in
   Master FX (Shift+Vol+Menu)
2. Play it with the defaults first. Raising DEPTH moves the source itself further away
3. SPACE picks the kind of space, SIZE its size, and DECAY the length of the late reverb
4. FREEZE ON holds the reverb of that moment; later input no longer enters the space
   (the dry sound still passes)

When SPACE / SIZE change, or DECAY is set shorter than the IR's own decay, a new IR is
generated and transformed a little at a time on the audio thread (about 0.2 s) and,
once ready, crossfaded in over about 0.3 s. While a knob is turning the generation keeps
restarting, and it switches only once, at the value where the knob stops.

## What each knob does

- **MIX**: equal-power dry / wet. 0 % is a complete bypass (bit-identical); 100 % has no
  dry signal. Once MIX has gone all the way to 0 (about 0.4 s) the reverb processing
  itself stops and the CPU load drops to almost zero. Raising MIX again clears the
  reverb history (about 20 ms, wet silent meanwhile) and restarts in an empty space, so
  input played during MIX 0 does not linger in the reverb. With FREEZE ON it does not
  stop, so MIX 0 → back up returns the same frozen sound. If SPACE / SIZE change during
  MIX 0, the IR is still built and used when MIX comes back up.
- **DEPTH**: not just a wet amount but "distance". It moves all of the following together:
  - Pre-delay 3 → 180 ms (× 0.85–1.9 by SIZE)
  - The early / late ratio (early reflection gain 1.0 → 0.3, late 0.5 → 1.3)
  - Distant air absorption (a wet low-pass: off → about 2.6 kHz)
  - Reflection density (the diffusion all-pass coefficients of the early reflections and
    the FDN input rise)
  - Direct / reverb ratio (the direct sound down by up to −12 dB, with its highs
    reduced; nothing happens at MIX 0)
  - Loudness compensation (wet up to +4.1 dB), so moving away does not just mean getting
    quieter
- **SIZE**: the scale of the space. It rebuilds the IR's time structure (the times of
  the early reflections, the gap before the first one, the bloom, the flutter period,
  the IR length, its decay time, the frequencies of its metallic resonances) and also
  stretches the FDN's 16 delay lengths (× SIZE^0.75) and the pre-delay. It is not a
  pitch change (the FDN lengths follow smoothly over about 0.35 s, so a slight
  Doppler-like wobble appears only while turning).
- **DECAY**: RT60 of the late reverb (low band). Each FDN line's loss is computed from
  its own length, so changing SIZE does not change the RT60. If the IR's own decay is
  longer than DECAY, the IR is shortened too.
- **DIFFUSION**: the FDN input diffusion all-passes (4 stages), the density of the early
  reflections, and the FDN's delay modulation. Low values let individual reflections be
  heard; high values give a smooth wall.
- **DARKNESS**: the FDN's frequency-dependent loss (high-band RT60 from 0.9× down to
  0.08× the low band) and the wet highs.
- **FREEZE**: see below.
- **SPACE**: switches between six synthetic stereo IRs (table below).

### How FREEZE works

When turned on, the input to the space (before the pre-delay) is closed over 0.25 s and
the input to the FDN over 0.12 s, while the FDN's RT60 moves to 30000 s over 0.5 s. The
FDN input stays closed while frozen so that the IR tail the convolution is still playing
cannot pile up in the now lossless loop and grow louder. An RT60 of 30000 s is a loss of
about −0.6 dB over 5 minutes, and that small loss is what guarantees the loop stays
stable (not a perfectly infinite hold; stability comes first). Turning it off reverses
the steps and the sound decays at the normal DECAY.

## IR presets (all synthetic)

Each IR is generated by `dsp/ds_irgen.c` from a fixed seed, so the same settings give
the same IR on any machine, every time.
The model: a mix of shared noise and per-channel noise (stereo width) is shaped by a
two-band exponential decay split at a crossover (the highs die first) × a bloom × a slow
amplitude fluctuation; a train of early reflection impulses, flutter echoes between
parallel walls and structural resonances (decaying sines) are added; a two-stage
low-pass plus a high-pass give the space its colour, and the last quarter fades out.
Energy normalisation evens out the level differences between presets.

| SPACE | Character (at SIZE 1) |
|---|---|
| CATHEDRAL | First reflection at 22 ms, 30 early reflections (to ~160 ms), low-band RT 4.2 s / high 2.2 s, 70 ms bloom, wide stereo |
| CAVERN | 44 dense, irregular reflections from 9 ms (to ~240 ms), dark (5.2 kHz), low-band RT 4.8 s / high 1.3 s, strong fluctuation |
| TUNNEL | Flutter echoes 41 ms apart, tube resonances at 187/374/561 Hz, narrow stereo, slightly reduced lows |
| HANGAR | 9 widely spaced slaps from 30 to 340 ms, bright (15 kHz, high-band RT 2.8 s), 6 metallic resonances |
| ABYSS | No early reflections, rises out of the dark over 480 ms, extremely dark (2.6 kHz), weighted to the deep lows, low-band RT 7.5 s |
| VOID | No early reflections, fully decorrelated stereo, nothing below 160 Hz, bright and long (high-band RT 4.5 s), 260 ms fade-in |

IR length is 1.4–2.0 s × SIZE (capped at 2.0 s). For checking, `scripts/render_demo.sh`
writes the IRs themselves to `build/demo/irs/<SPACE>.wav` (SIZE 1.5, DECAY 30,
peak −1 dBFS).

## DSP signal path

```text
Stereo In ─┬──────────────────────────── direct sound (DEPTH sets distance: level + high cut) ─┐
           │                                                                                │
           └→ FREEZE gate → pre-delay (DEPTH / SIZE)                                        │
                 → partitioned convolution (stereo IR)                                      │
                 → density diffusion (2 all-passes) ───────┬──→ early reflections × gE(DEPTH)│
                 → input diffusion (4 all-passes) → FDN gate│                               │
                 → 16-line FDN (Hadamard, frequency-dependent loss, modulation) → late × gL(DEPTH)│
                       ↓                                                                    │
                 air absorption LP (DEPTH, DARKNESS) → loudness comp → DC removal → wet peak guard ─┤ MIX
                                                                                            ↓
                                                    final peak guard (only above −0.2 dBFS) → Out
```

### Convolution engine (`dsp/ds_conv.c`)

Non-uniformly partitioned overlap-save convolution.

| Stage | IR range | Partitions | FFT | Processing |
|---|---|---|---|---|
| HEAD | 0 – 4096 samples | 128 × 32 | 256-point real FFT | Every block. No added latency |
| BODY | 4096 – 96256 samples | 2048 × up to 45 | 4096-point real FFT | Spread over 16 blocks (one cycle) |

One BODY cycle (128 × 16 = 2048 samples) breaks down as: blocks 0/1 the left and right
forward FFTs, 2–13 the frequency-domain multiply-accumulate (45 partitions in 12 equal
parts), 14/15 the left and right inverse FFTs. BODY starts at IR sample 4096
(= 2 × 2048), which gives exactly the headroom this spreading needs, so the output is
correctly timed.

- All IR spectra are FFT'd ahead of time (with 1/N folded in at build time). FFT tables
  are built once at plugin initialisation and never initialised in the audio callback
- The input's frequency-domain delay line is shared by two filter sets, so an IR switch
  is "run the new set alongside for one cycle → equal-power crossfade over 6 cycles
  (about 0.28 s)" and loses no history
- Internally it always works in 128-sample units whatever the host block size (a
  128-sample FIFO; the pre-delay is shortened by that much, so the wet timing is exact
  and the dry path has zero latency)
- Error against direct convolution is below −129 dB (test 1)

### Late reverb (`dsp/ds_fdn.c`)

16 delay lines (17.3–96.7 ms at SIZE 1, × SIZE^0.75), a 16×16 Hadamard matrix (fast
Walsh–Hadamard transform; orthogonal = lossless), a first-order absorption filter per
line whose DC and Nyquist gains come from the RT60, independent slight modulation at
0.06–0.26 Hz (1.5–9 samples, cubic Hermite interpolation), output DC removal, and a
safety limiter that only acts above 4× the normal level (it never triggered in the tests).

### Safety

- NaN / Inf: checked every wet block; if one ever appears, the wet goes silent, memory is
  cleared and the IR is rebuilt (0 occurrences in the tests)
- Denormals: the FPCR FZ bit (FTZ/DAZ on x86) is set only during processing and restored
  afterwards
- No memory allocation, file I/O, locks or thread creation on the audio thread.
  Instances come from a static pool via CAS (`scripts/check_abi.py` rejects malloc / mutex
  references at build time)
- A reused slot's signal history (about 3 MB) is cleared 128 KB per block before use

## CPU, memory, latency

- Latency: **0 samples** (no added delay in the dry path or in the convolution HEAD)
- Memory: 6.6 MB per instance (two IR spectrum sets 3.1 MB, frequency-domain delay line
  1.5 MB, FDN 1.0 MB, IR generation buffers 0.73 MB, pre-delay 0.25 MB). The static pool
  of 8 (53 MB) is virtual space; only instances in use take real memory
- CPU: see "Measured on Move" below

## Files

```text
schwung-deep-space/
├── LICENSE                     MIT (Hiro Ikezawa)
├── README.md                   this file
├── README.ja.md                Japanese version
├── THIRD_PARTY_LICENSES.md     notice for the Schwung API header (MIT), statement that the IRs are synthetic
├── module.json                 id deep-space, audio_fx, 8 chain_params
├── help.json                   on-device help (ASCII)
├── dsp/
│   ├── ds_plugin.c             audio_fx_api_v2 adapter, ui_hierarchy, instance pool, state save
│   ├── ds_engine.c/.h          overall wiring, DEPTH / FREEZE / MIX, smoothing, staged IR build
│   ├── ds_conv.c/.h            non-uniformly partitioned stereo convolution, crossfade of two sets
│   ├── ds_fft.c/.h             real FFT (256 / 4096 points)
│   ├── ds_irgen.c/.h           the six synthetic stereo IRs
│   ├── ds_fdn.c/.h             16-line FDN
│   ├── ds_params.h             parameter table (checked against module.json at build time)
│   └── ds_dsp.h                first-order filter coefficients, random numbers, FTZ
├── third_party/schwung/        Schwung plugin API header (MIT, unmodified)
├── scripts/
│   ├── build.sh                native / asan / arm64 builds; arm64 puts a tarball in dist/
│   ├── test.sh                 all tests
│   ├── render_demo.sh          offline listening WAVs and IR WAVs
│   ├── install.py              install through Schwung Manager
│   ├── metadata.py             module.json → embedded in the DSP + consistency check
│   ├── check_abi.py            ABI check of the ARM64 .so
│   └── check_licenses.py       checks for copyleft code and copyright notices
└── tests/
    ├── test_engine.c           engine / plugin API tests
    ├── test_grid.mjs           standard grid test with the same page engine as the device
    ├── dump_contract.py        passes the built .so's answers to the grid test
    ├── render_demo.c           demo renderer
    ├── signals.h               synthetic test signals (drums, chords, pink noise, ...), WAV writer
    └── bench_device.c          CPU benchmark for Move (static aarch64 binary)
```

The chain host loads `audio_fx/<id>/<id>.so`, so the DSP is `deep-space.so`.

## Tests (`scripts/test.sh`)

| # | What | Result |
|---|---|---|
| 1 | Partitioned convolution = direct convolution (IR 3000 / 4096 / 4097 / 40000 / 96256 samples), impulse → the IR itself | Error −129 to −134 dB, impulse error 4e-7 |
| 2 | Host block size 1 / 37 / 64 / 300 / random 1–300 vs 128 | Output difference 0 (bit-identical) |
| 3 | Largest sample step and 20 ms level change while switching SPACE 6 times + SIZE + DECAY | Steps within 1.33× of steady state, level change +0.1 dB |
| 4 | DECAY 1 / 2 / 5 / 10 / 20 / 30 s → measured RT60 (below 400 Hz, EDC −10 to −40 dB) | 1.17 / 1.10 / 1.01 / 0.99 / 0.99 / 0.98× |
| 5 | DEPTH 0 → 100 % (6 presets): pre-delay, early/late ratio, spectral centroid, loudness | Table below |
| 6 | FREEZE for 5 minutes (input keeps playing) → release | No rise, within −1.7 dB over 5 minutes, DC 4.5e-6, no clicks, decays after release |
| 7 | Min/max of every continuous parameter, 64 combinations × full-scale input × FREEZE on/off | NaN/Inf 0, peak ≤ 1.0 |
| 8 | Left-only / mono input → decorrelated reverb on both sides, dry stays on its side | R/L −3.9 dB, correlation 0.02 / 0.05 |
| 9 | MIX 0 / 50 / 100 % | 0 % is a bit-identical bypass, 50 % dry −3 dB, 100 % no dry |
| — | Sleep at MIX 0: processing stops (49.5 → 1.5 µs/block on a desktop), still bit-identical from 40 % → 0, no click on raising it again, reverb present after 50–100 ms, no sleep during FREEZE | OK |
| 10 | 8 instances + a 9th refused, create/destroy 200 times, a reused slot starts from silence | OK (about 0.6 s until a reused slot is ready) |
| — | Parameters, enum names, state save (FREEZE is not saved), ui_hierarchy | OK |
| — | AddressSanitizer + UBSan, standard grid UI (the device's page engine), ARM64 ABI, licence checks | OK |

DEPTH 0 → 100 % (impulse response at MIX 100 %, loudness of pink noise at MIX 40 %):

| SPACE | Onset | Early/late | Spectral centroid | Loudness |
|---|---|---|---|---|
| CATHEDRAL | 40 → 246 ms | −10.0 → −20.6 dB | 4855 → 1211 Hz | +0.6 → −1.1 dB |
| CAVERN | 19 → 226 ms | −5.6 → −16.5 dB | 3051 → 966 Hz | +0.2 → −2.2 dB |
| TUNNEL | 12 → 216 ms | −4.7 → −16.0 dB | 3945 → 1070 Hz | +0.8 → −0.6 dB |
| HANGAR | 49 → 255 ms | −4.8 → −13.7 dB | 6813 → 1154 Hz | +0.9 → −0.8 dB |
| ABYSS | 74 → 286 ms | −19.9 → −30.0 dB | 984 → 564 Hz | +0.5 → −0.4 dB |
| VOID | 31 → 244 ms | −18.6 → −27.3 dB | 6376 → 1526 Hz | +0.8 → −2.0 dB |

## Measured on Move

2026-10-23, host 1.7.3, `build/bench-device-arm64` run on Move at normal priority with
`taskset 0x7` (Move's CPU is a Cortex-A72; one 128-sample block = 2902 µs).

| Setting | Mean | Worst |
|---|---|---|
| Defaults | 478 µs (16.5 %) | 1756 µs |
| Maximum (ABYSS, SIZE 4, DECAY 30) | 484 µs (16.7 %) | 2245 µs |
| SPACE switched every second (IR generation + crossfade) | 569 µs (19.6 %) | 1747 µs |
| FREEZE | 451 µs (15.6 %) | 7880 µs |
| MIX 0 (sleeping, 0.1.1) | 6 µs (0.2 %) | 380 µs |

Breakdown (mean): convolution 207 µs, FDN 177 µs. One 4096-point FFT is 138 µs.
The worst figures were measured as a normal-priority process and include preemption by
other processes (7880 µs is clearly preemption). For the real figures inside the SPI
thread, check Schwung's CPU page.

## Known limitations

- All IRs are synthetic. There is no way to load measured (recorded) IRs
- Changes to SPACE / SIZE / DECAY take effect about 0.5 s later, because of IR
  generation and the crossfade
- While SIZE is turning, the FDN delay lengths follow smoothly, causing a slight pitch
  wobble
- FREEZE is not a perfectly infinite hold (about −0.6 dB over 5 minutes; stability
  first). The FREEZE state is not saved (to avoid reopening a set frozen on an empty
  reverb)
- Preset levels are matched with pink noise. On low-heavy material such as kicks, VOID
  (which cuts the lows) sounds quieter and HANGAR (with strong early reflections) louder
- The longest IR is 2.0 s (the FDN takes over after that)
