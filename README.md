# AJA Input for Resolume

An open-source FFGL **source** plugin for [Resolume](https://resolume.com/)
Arena and Avenue 7.x that brings live video from AJA capture cards (KONA,
Corvid, Io) into Resolume with as little latency as the hardware allows.
It shows up in Resolume as **AJA Input**.

It captures straight from the card with AJA's SDK, owns the capture
thread, the DMA and the GPU upload, and shares each input between every
clip that shows it, the way Resolume's own live inputs do.

> Not affiliated with or endorsed by AJA Video Systems or Resolume.
> AJA, KONA, Corvid and Io are trademarks of AJA Video Systems, Inc.;
> Resolume is a trademark of Resolume B.V.

## Status: beta (0.43)

- **Tested on hardware: AJA KONA X**, with Resolume Arena 7.26 on
  Windows 11: it ran a live show for hours with two inputs at different
  resolutions (same frame rate) without a problem. Latency, measured with
  an earlier build against a DeckLink Quad 2 in the same composition: one
  frame ahead of it on progressive sources, one frame behind on
  interlaced sources with YADIF de-interlacing.
- **Not yet tried on hardware:** every other AJA card (table below),
  which is why this release is a beta. On the KONA X, the parts a show
  does not necessarily exercise have only been checked in the test host:
  Resize in a composition of another aspect, the No Signal modes, and
  Colorimetry Auto following HDR signalling.
- **Experimental:** 4K on cards without 12G routing (quad-link SDI, 4K
  HDMI on KONA HDMI / Io 4K Plus) and 10-bit capture.
- Windows only (Resolume's Windows build). The Linux build exists for the
  test suite.

Reports are very welcome, from any card: open an issue with the card
model, driver version and the log lines starting with `[AJAInput:caps]`
and `AJAInput:` (Resolume > Preferences > Log), which say what the plugin
found and did.

## Why not Resolume's built-in AJA input?

- **Low latency.** The capture thread wakes on the card's interrupt,
  transfers the newest complete frame straight into page-locked, GPU-mapped
  buffers and publishes it to Resolume's render thread; there is no queue
  of frames in between. An optional Low Latency Mode drives the card's
  frame buffers by hand.
- **One capture per input, shared by every clip** (like Resolume's own
  live inputs), with FrameStores lent to the inputs that are on screen
  when a card has fewer FrameStores than inputs in use.
- **Interlaced sources de-interlaced with YADIF** (motion adaptive),
  field by field for lower latency.
- **Robust signal handling:** cable pulls, format changes, 3G Level B
  sources, bidirectional SDI connectors left as outputs, and other
  applications using the card are detected and shown, not just "no
  signal".
- **Shows what is going on:** a Format readout in the inspector and an
  optional on-screen stats overlay.

## Supported cards

What the plugin offers on each card, from AJA's SDK capability tables.
"One format" cards run a single video format on all their inputs at a
time (no multi-format mode). Analog inputs are not captured.

| Card | Inputs offered | FrameStores | 4K | Notes |
|------|----------------|-------------|----|-------|
| KONA X | SDI 1-2, HDMI | 2 | one cable (12G) | **tested** |
| KONA X 4CH, KONA XM | SDI 1-2, HDMI | 4 | one cable (12G) | |
| KONA 5 with the 4x12G (8K), 2x4K or 8K MK firmware | SDI 1-4 | 4 (2x4K: 2) | one cable (12G) | |
| KONA 5 (standard firmware) | SDI 1-4, SDI 1-4 (4K) | 4 | quad-link 2SI | 4K experimental |
| KONA 5 3D LUT | SDI 1-2 | 2 | one cable (12G) | |
| KONA 4 | SDI 1-4, SDI 1-4 (4K) | 4 | quad-link 2SI | 4K experimental |
| KONA HDMI | HDMI 1-4 | 4 | HDMI 1-2 | 4K experimental |
| KONA 1, KONA 3G, KONA 4 UFC | SDI 1 (KONA 3G / 4 UFC: SDI 1-2) | 2 | - | one format |
| KONA LHi, KONA LHe Plus | SDI 1 (+ HDMI on LHi) | 2 | - | one format |
| KONA 3G Quad, Corvid 24 | SDI 1-4, SDI 1-4 (4K) | 4 | quad-link squares, up to 30p | one format; 4K experimental |
| Corvid 44 | SDI 1-4, SDI 1-4 (4K) | 4 | quad-link 2SI | 4K experimental |
| Corvid 44 12G | SDI 1-4 | 4 | one cable (12G) | |
| Corvid 88 | SDI 1-8, SDI 1-4 (4K), SDI 5-8 (4K) | 8 | quad-link 2SI | 4K experimental |
| Corvid HEVC | SDI 1-4, SDI 1-4 (4K) | 8 | quad-link 2SI | 4K experimental |
| Corvid 1, Corvid 22, Corvid 3G | SDI 1 (Corvid 22: SDI 1-2) | 2 | - | one format |
| Io 4K Plus | SDI 1-4, SDI 1-4 (4K), HDMI | 4 | quad-link 2SI, HDMI | 4K experimental |
| Io 4K | SDI 1-4, SDI 1-4 (4K), HDMI | 4 | quad-link 2SI | 4K experimental; no 4K over HDMI |
| Io X3 | SDI 1-4, HDMI | 4 | - | |
| Corvid HB-R | HDMI | 4 | - | one format |
| Io XT, Io Express | SDI 1-2 / SDI 1, HDMI | 2 / 1 | - | one format |
| Io 4K UFC | SDI 1-2, HDMI | 2 | - | one format |

Not supported: output-only devices (T-TAP, T-TAP Pro, left out of the
Device menu), 8K, and analog inputs. The SMPTE 2110 / 2022 IP cards
(KONA IP, Io IP) are listed but untested: the plugin does no network
setup (the streams would have to be configured with AJA's tools first)
and offers no quad-link on them.

On cards whose HDMI input only has a YCbCr output in AJA's routing
tables (KONA LHi, Io XT, Io X3, Io 4K, Io Express, Corvid HB-R) an RGB
HDMI source is taken from that output, as the card's receiver delivers
it (untested).

The Input menu lists the inputs of the cards installed on the machine
(all of them, since the Device parameter can pick any card). Without an
AJA card it shows SDI 1, SDI 2 and HDMI, and the clip shows a test
pattern.

### 4K

- **On one cable:** cards with 12G routing (KONA X family, KONA 5 with a
  12G firmware, Corvid 44 12G) take UHD / 4K DCI up to 60p on a single
  12G-SDI cable or on HDMI, through one FrameStore. A 4K signal on one
  cable of a card without 12G routing is refused with a reason.
- **Quad-link (experimental):** on cards without 12G routing, pick
  `SDI 1-4 (4K)` (or `SDI 5-8 (4K)` on a Corvid 88) for a 4K picture on
  four 3G (or 1.5G) cables. Cards with 425 muxes take two-sample
  interleave (2SI, the usual quad-link standard) into a FrameStore pair;
  KONA 3G Quad and Corvid 24 take square division into four FrameStores.
  The plugin checks that the four links carry the same signal and, when
  the source sends a payload ID, that it uses the interleave the card
  takes. Routing follows AJA's own 4K capture demo; it has not been tried
  on hardware. KONA 3G Quad and Corvid 24 take quad-link up to 30p.
- **4K HDMI on KONA HDMI / Io 4K Plus (experimental):** HDMI 1 uses
  FrameStores 1+2, HDMI 2 (KONA HDMI) FrameStores 3+4 (AJA's demo only
  routes HDMI 1; HDMI 2 follows the same pattern). The older HDMI input
  of the Io 4K has no route for it.

## Installing

1. Download the release ZIP from the
   [releases](../../releases) page (or build the DLL, below).
2. Copy `AJAInput.dll` to `Documents\Resolume Arena\Extra Effects\` (or
   `Resolume Avenue`). Close Resolume first if you replace a running copy.
3. Install AJA's driver for your card (AJA Desktop Software); the plugin
   is built against SDK 18.0 and was developed with driver 18.0.
4. Start Resolume and find **AJA Input** among the Sources.

Resolume's own AJA output can run on the same card: the plugin never
touches a FrameStore or connector another application plays out on, nor
changes the video format of a one-format card while another application
plays out on it, and says so in the Format field. Do not capture the
same inputs with Resolume's own AJA input as well.

## Using it

Drop **AJA Input** into a clip, pick the **Device** and the **Input**, and
trigger the clip. The capture of an input starts when its first clip is
shown and closes with its last clip; every clip of the same input, in any
layer or deck, shows the same frames.

The **Format** field in the inspector says what is happening, without
the overlay: `1080p50 | SDI1 FS1 | 2 clips`, or why there is no picture
(`no signal`, `SDI2 waiting for a free FrameStore`,
`card runs one format: another input differs`, ...). **Show Stats**
draws the same and more on the picture.

### Parameters

| Parameter | Description |
|-----------|-------------|
| **Input** | The input to capture (see the table above). |
| **Device** | The AJA card, by model and serial number. |
| **Format** | Read-only readout (see above). |
| **Resize** | How the picture fills the clip: **Fit** (default; the whole picture with its own aspect, transparent bars where it does not reach), **Fill** (covers the clip with its own aspect, the overflow is cut), **Stretch** (fills the clip, deformed if the aspects differ), **Original** (its own size in pixels, centred). See below. |
| **No Signal** | What the clip shows while its input has no picture: Black, Transparent (the layers below show through) or Hold Last Frame. |
| **Colorimetry** | Auto, BT.709 / BT.601 limited or full, PQ, HLG. Auto picks BT.601 for SD and BT.709 for HD and up, or PQ / HLG when the source signals it (SDI payload ID, HDMI HDR InfoFrame). PQ and HLG are tone-mapped to SDR. |
| **Deinterlace** | YADIF on interlaced sources (bob in 10-bit mode). No effect on progressive ones. |
| **Low Latency Mode** | Per input. Progressive: the card's frame buffers are driven by hand instead of AutoCirculate. Interlaced: YADIF without look-ahead, about one source frame less latency. |
| **Ring Depth** | Per input. AutoCirculate frame buffers (2-7). More absorbs hiccups without adding latency: the newest complete frame is always taken. |
| **10-bit capture (v210)** | Per input, experimental. 10-bit 4:2:2 frame buffer (bob de-interlacing). |
| **Show Stats** | On-screen overlay: capture state, input and FrameStore, frame rate, frame age, drops, warnings. |
| **Stats / Frames, Drops, Recoveries, Idle wakeups** | Read-only counters. |

"Per input" settings apply to every clip of the input: changing them in
one clip changes them in all, as with Resolume's own live inputs.

### Picture size and aspect

An FFGL source cannot choose its own size: Resolume renders it at the
size of its clip, which for a new source clip is the composition's, not
the input's. Resolume's own clip Resize (Fit / Fill / Stretch / Original)
then has nothing to fix, since the clip already has the composition's
size. The plugin's **Resize** does it inside the clip instead, with the
same names and meanings, in whatever size the clip has: by default
(**Fit**) a 4:3 SD camera on a 16:9 composition is pillarboxed, a DCI 4K
(4096 x 2160) input on a UHD composition letterboxed, and an HD input on
a 16:10 composition gets bars above and below, all without deforming the
picture. When the input and the composition have the same aspect, every
mode but Original simply fills the clip.

- The bars are transparent, as around a smaller clip in Resolume: the
  layers below show through. Effects on the clip see them as part of the
  clip; choose **Fill** if an effect should only see picture.
- SD (720 x 486 / 576) has non-square pixels and is shown at 4:3. For
  anamorphic 16:9 SD on a 16:9 composition choose **Stretch**.
- **No Signal: Black** blackens the picture's area; the bars stay
  transparent, so the layout does not jump when a cable is pulled.

### Several clips and inputs on one card

A card can capture as many inputs at once as it has FrameStores (table
above). SDI N uses FrameStore N; HDMI takes the highest free one (on a
KONA HDMI, HDMI N uses FrameStore N). With more inputs in use than
FrameStores, an input none of whose clips has been on screen for a
quarter of a second lends its FrameStore to an input that is on screen
(a short black moment when it is shown again). A 4K picture on a card
without 12G routing needs its FrameStore pair or four, and only inputs
holding one of those lend.

Frame rates on one card must belong to the same family (29.97 / 59.94,
30 / 60, 25 / 50, 23.98, 24): AJA multi-format cards stutter otherwise,
and the Format field warns `! rate clash`. "One format" cards run one
format on all inputs: an input whose format differs from the one already
running waits (`! format clash`) instead of switching the card under it.

## Troubleshooting

- **AJA Input does not appear in Resolume:** check the DLL is in `Extra
  Effects` and Resolume was restarted. `dumpbin /EXPORTS AJAInput.dll`
  must list `plugMain`.
- **The Device menu says `(no AJA detected)`:** the AJA driver does not
  see a card; check AJA Control Panel.
- **Test bars instead of video:** the card could not be opened (another
  application holds it exclusively, or the driver is missing); the log
  says why.
- **The Format field shows a reason instead of a format:**
  `waiting for a free FrameStore`: every FrameStore is used by inputs on
  screen. `routing failed` / `routing not allowed on this card`: the card
  refused a route (the log's `routing ROM:` line lists what it allows).
  `connector is another app's SDI output`: someone outputs on that
  connector. `4K on one SDI cable needs a card with 12G routing`: use the
  quad-link input or a 12G firmware. `quad-link ...`: see 4K above.
- **Bars around the picture, or the picture looks stretched:** the input
  and the composition have different aspects; see *Picture size and
  aspect* and the Resize parameter.
- **`RGB HDMI: no colour converter`:** the card has no colour converter
  free for that FrameStore; set the source to YCbCr.
- **`card's format is another app's output`:** a one-format card whose
  format another application's output uses; that input waits.
- **Thunderbolt devices (Io):** after unplugging and replugging while
  Resolume runs, re-trigger the clips or restart Resolume if the picture
  does not come back.

## Building from source (Windows)

Requirements: Windows 10/11, Visual Studio 2022 with *Desktop development
with C++*, CMake 3.15+, Git. `build.cmd` does the rest: it finds Visual
Studio and CMake, clones the FFGL SDK, libajantv2 (tag `ntv2_18_0_0`) and
vcpkg when missing (`-Setup`), installs GLEW, builds, runs the unit tests
and with `-Install` copies the DLL into Resolume.

```
build.cmd -Setup -Install  first time: clone ffgl / libajantv2 / vcpkg, build, install
build.cmd                  Release build + unit tests
build.cmd -Install         ... and copy the DLL into Resolume Arena (close Resolume first)
build.cmd -Clean           full rebuild
```

Other options: `-Config Debug`, `-Resolume Avenue`, `-SkipTests`,
`-Generator Ninja`, `-Workspace <folder>` (`Get-Help .\build.ps1 -Full`).
The expected layout is

```
<workspace>/
  AJA-Input-FFGL/  this repository (any folder name)
  ffgl/            github.com/resolume/ffgl
  libajantv2/      github.com/aja-video/libajantv2 (tag ntv2_18_0_0)
  vcpkg/           github.com/microsoft/vcpkg
```

Manual build, from a Developer Command Prompt in `AJA-Input-FFGL`:

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
      -DVCPKG_TARGET_TRIPLET=x64-windows-static ^
      -DCMAKE_TOOLCHAIN_FILE=..\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
```

The plugin is `build\Release\AJAInput.dll`, one self-contained file
(libajantv2 and GLEW linked statically).

Linux (development and CI): the same CMake project builds
`AJAInput.so`, the unit tests (`AJAInputTests`) and, with
`-DAJAINPUT_BUILD_HOST_SMOKE=ON`, a headless FFGL host test on EGL / Mesa
(`ffgl_host_smoke`).

## How it works

```
                 +------------------+          +--------------------+
card input ----> | capture thread   |  DMA     | texture ring       |  GL texture
(SDI / HDMI)     | (AutoCirculate   | -------> | (persistent-mapped | ----------> every clip
                 |  or manual LL)   |          |  PBOs, 3 slots)    |             of the input
                 +------------------+          +--------------------+
```

Each input in use is one `SharedCapture` (src/SharedCapture.h): the card
handle, its FrameStore(s), the capture thread and a texture ring. The
capture thread waits for the card's interrupt, transfers the newest
complete frame straight into a page-locked, persistently mapped pixel
buffer and publishes it to a triple-buffered texture ring through a GL
context shared with Resolume's; each clip draws the newest texture with
its own YCbCr-to-RGB shader, colorimetry and de-interlacing. Which
FrameStores and routes an input needs on a given card is decided in
src/CaptureTopology.h.

What is planned next is in [docs/ROADMAP.md](docs/ROADMAP.md), what
changed in [CHANGELOG.md](CHANGELOG.md). The plugin was developed for a
KONA X before its first public release; that history (design notes, the
latency work, a code audit) is kept in [docs/history](docs/history), and
the version tags in code comments refer to it.

Audio: FFGL gives plugins no way to hand audio to the host, so embedded
audio is not captured.

## License

Copyright (c) 2026 Joaquin Villodre.

AJA Input is free software: you can redistribute it and/or modify it
under the terms of the GNU Lesser General Public License, version 2.1 or
(at your option) any later version ([LICENSE](LICENSE)). It is
distributed in the hope that it will be useful, but without any
warranty; see the licence for details. The source code of every release
is published with it on GitHub.

The YADIF de-interlace shader is a port of FFmpeg's `vf_yadif.c` /
`vf_yadif_cuda.cu` and libplacebo's GLSL version, which are under the
same licence. The DLL links the FFGL SDK (BSD-3-Clause), libajantv2
(MIT) and GLEW statically; their notices are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), which must accompany
distributed copies of the DLL.

## Acknowledgements

Thanks to AJA Video Systems for publishing `libajantv2` as open source,
and to Resolume for the FFGL SDK.
