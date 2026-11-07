# Development log (until 0.42)

> Development history from before the first public release (0.43), when
> the project was named konax-ffgl; see [README.md](README.md) in this folder.

Status at v0.42: compiles clean, 85 unit tests + host smoke test green, loads in
Resolume, and validated end-to-end against a real Kona X up to v0.37
(latency measured against a DeckLink Quad 2: +1 frame ahead on
progressive, -1 frame behind on interlaced with YADIF). v0.39 carries
the fixes from the full code audit (`docs/AUDIT_v0.39.md`); v0.40 the
latency work, v0.41 the multi-clip fixes, v0.42 support for every AJA
card with an input (see below). All four still need a hardware check.
v0.19 (embedded audio) is documented as out of scope for an FFGL plugin.

The per-milestone sections below are kept as the historical design
record; their "pending hardware validation" notes describe the state
at the time each milestone was written.

## Milestones at a glance

**Done and hardware-validated:**
- v0.18 — Hardware bring-up (RGB HDMI, raster change, DMABufferLock)
- v0.20 — Colorimetry (BT.601/709, full/limited, auto)
- v0.21 — Field-separation de-interlace (superseded on UYVY by v0.35)
- v0.22 — Low Latency Mode (manual ping-pong opt-in)
- v0.23 to v0.28 — Format text, capture-loop refactor, load-time
  diagnostics, live stats, regression tests + Linux CI
- v0.29.1 — HDR transfer functions (PQ + HLG, 8-bit shader-only)
- v0.32 — Per-field DMA delivery for interlaced (AC field mode)
- v0.33 — Configurable buffer count for Low Latency Mode (2 or 3)
- v0.35 — YADIF de-interlace (motion-adaptive UYVY) + YadifSlots
- v0.37 — Interlaced UYVY always routed through AC field mode

**Done in code, pending hardware check:**
- v0.29.2 — 10-bit packed v210 capture (EXPERIMENTAL opt-in)
- v0.38 — Interrupt-count gate for the Windows wait quirk
- v0.39 — Audit fixes (see CHANGELOG and docs/AUDIT_v0.39.md)
- v0.40 — Latency: AC transfer at buffer level 1, backlog drain,
  no-lookahead YADIF under Low Latency Mode (see the v0.40 section)
- v0.41 — Shared capture: one capture per input for all its clips,
  natural FrameStore pairing, FrameStores lent to inputs on screen,
  on-screen failure reasons (see the v0.41 section)
- v0.42 — Every AJA card: SDI 1-8 / HDMI 1-4, one-format cards,
  experimental 4K over several FrameStores (v0.30), No Signal and
  Resize options, HDR-signalled Auto colorimetry, renamed "AJA Input"
  (see the v0.42 section)

**Open issues:**
- YADIF combing on full-motion interlaced video, observed with two
  instances running. v0.39 repairs the most likely cause (missed
  second-field publish leaving a stale field in the YADIF history,
  counted as `fieldRepairs` in the diag log); confirm on hardware as
  described in docs/AUDIT_v0.39.md.

**Out of scope:**
- v0.19 — Embedded audio (FFGL spec lacks plugin → host audio path)
- GPUDirect RDMA on Windows (AJA Windows driver does not expose it)
- 8K; analog inputs; SMPTE 2110 / 2022 IP cards
- (v0.30 quad-link 4K: experimental since v0.42, untested)

**Backlog:**
- v0.36 — YADIF for V210 (no use case yet)
- Windows CI job running the unit tests and building the release DLL
  (the host smoke test is Linux/Mesa only).
- YADIF licensing: the shader's upstream files are LGPL-2.1+; decide
  before the first public release (THIRD_PARTY_NOTICES.md, v0.42,
  states the question).
- Square-division quad-link on cards with 425 muxes (v0.42 takes 2SI
  there); 4K HDMI on cards without 425 muxes (Corvid HB-R).

**Cross-cutting side tasks:**
- AJA support ticket: ask explicitly about Windows GPUDirect / streaming
  DMA path on GeForce.
- Documentation kept in sync with code on every milestone.


## v0.18 — First hardware bring-up (highest priority)

The Kona X is the only thing standing between us and "actually working".
Everything else is theoretical until the first capture comes back
through the wire.

**Deliverables**
- Real capture validated end-to-end on SDI1 of a Kona X, at least at
  1080p59.94 and 720p60. Visible picture in Resolume, FPS counter on
  the overlay matches input rate, no driver-side drops over a 60s test.
- Diagnostic dump at plugin load confirms expected card identity:
  model name, serial, firmware version, FrameStore count (2 for base
  Kona X / 4 for Kona XM or KONAX_4CH SKU), inputs (SDI1, SDI2, HDMI).
- Recovery cycle proven: yank the cable, overlay flips to NoSignal,
  re-plug, capture comes back without plugin reload.
- Channel switch proven: change Input parameter SDI1 → SDI2 mid-show,
  capture re-arms cleanly within 200-300 ms.

**Done in code, pending hardware verification:**
- RGB HDMI support via CSC. The capture's routing builder detects
  the HDMI input colour space at probe time and inserts the
  channel's CSC widget between the HDMI input and the FrameStore
  when the source signals RGB. Reference: the
  `!isFrameRGB && isInputRGB` branch of
  `CNTV2DemoCommon::GetInputRouting` in `ntv2democommon.cpp`. Two
  `Connect` calls per route, no explicit CSC enable needed.
- Live raster-change reallocation. The capture thread detects the
  cable carrying a different resolution than the one the pipeline
  was sized for and publishes a pending change atomically. The
  render thread (which owns the GL context) reads the pending
  change at the top of the next `ProcessOpenGL`, tears the capture
  down, calls `Initialise()` on each size-dependent component
  (`TextureUploader`, `SharedTextureRing`, `FrameQueue`) at the new
  dimensions, and spins up a fresh `AJACaptureSource` on the same
  device and FrameStore. No plugin reload needed; the user sees
  one or two black frames during the rebuild.
- HDMI runtime selection bug-fix. The previous `kMaxChannelOneBased`
  constant was 4, silently rejecting any `SetChannelOneBased(5)`
  call and keeping the selected SDI when the user picked HDMI in
  the dropdown. Now correctly bounded at 5.
- DMA destination buffer page-locking via `CNTV2Card::DMABufferLock`
  at capture-thread start, matching AJA's `NTV2_BUFFER_LOCK`
  pattern. Reduces per-frame DMA overhead and removes a source of
  jitter from the page-table walk.

**Non-obvious gotcha to watch for on first boot**
- The build asserts that the card has CanDo12gRouting for 4K via TSI.
  If the user's specific firmware/SKU does not advertise this (the
  base Kona X SKU might not), the code falls back to
  `Set4kSquaresEnable` which is wrong for single-link 4K. The 4K
  capture would either fail to init or deliver one quadrant of the
  image. If this happens, the fix is to inspect the diagnostic log
  for the actual capabilities and adjust the conditional.


## v0.19 — Embedded audio capture [out of scope]

**Verdict: out of scope for an FFGL plugin.** FFGL 2.0+ allows a
plugin to *receive* audio FFT data *from* the host (for building
audio-reactive visualizers), but provides **no mechanism for a plugin
to deliver audio TO the host**. This was confirmed by direct
testimony on the Resolume support forum:

> "Could you add the ability to use the audio of the Composition or
>  individual clips in Resolume rather than the External Audio FFT
>  input? Afraid there is no way to do this inside a FreeFrame
>  plugin"

Audio-reactive FFGL plugins that exist (BigFug etc.) work around this
by reading directly from the system's default sound device, not from
the host. That is a workaround, not a sanctioned path, and it has
nothing to do with the Kona X's embedded SDI audio.

**The three things we could have done with the Kona X audio:**
1. **Ignore it** — what we do today. The FrameStore is configured for
   video-only capture via `NTV2_AUDIOSYSTEM_INVALID`.
2. **Capture and discard** — pull it into `AutoCirculateTransfer` to
   keep the driver-side sync state consistent, but never expose it.
   No user-visible benefit over option 1.
3. **Capture and route to a virtual audio device** — implement (or
   integrate with) a WDM-virtual or ASIO-virtual driver that
   Resolume (or any other app) can then take as an audio input.
   This is a whole separate project: kernel-space WDM driver, or
   integration with a third-party tool such as VB-Audio Cable,
   ASIO4ALL, or AJA's own AJANTV2 audio device driver. Out of scope
   for this FFGL plugin.

In the typical VJ workflow, show audio does not pass through
Resolume at all - it goes from a separate mixing console direct to
the PA. Resolume needs audio only for FFT visualisers (already
handled by FFGL 2.0) or for playing back clips with embedded audio
(handled by Resolume's file playback path). The Kona X audio is
therefore best ignored at the plugin level.

If a user has a hard requirement to bring SDI-embedded audio into
Resolume specifically, the recommended path is a virtual audio
device outside this plugin. We can document that approach but it is
not code that lives here.

**Original v0.19 plan, archived for reference:**
The path through the AJA SDK would have been: pick
`NTV2_AUDIOSYSTEM_N` matched to the FrameStore index, call
`SetAudioSystemInputSource`, `SetEmbeddedAudioInput`,
`SetNumberAudioChannels`, `SetAudioBufferSize`, `StartAudioInput`
before `AutoCirculateStart`, set `xfer.acAudioBuffer` and
`acAudioByteCount` in every transfer, read back
`acTransferStatus.acAudioTransferSize`. This is well-trodden
territory (the `ntv2capture` demo does it) — what it cannot do is
get the bytes from there to Resolume's audio engine without going
through a sound device.


## v0.20 — Colorimetry options

Until v0.19 the shader hardcoded BT.709 limited-range YCbCr → RGB
conversion. This was wrong for:
- **SD captures (525i, 625i)**: SMPTE specifies BT.601 for SD; using
  BT.709 produces a slight tint (greens lean yellow, reds lean orange).
- **Full-range sources (computer / graphics card HDMI)**: the matrix
  expects limited-range Y (16..235) but the source delivers full-range
  Y (0..255), so the picture comes out washed and clipped.
- **HDR sources (HDR10, HLG)**: the linear gamma assumption is wrong;
  needs ST.2084 PQ or HLG transfer function inverse.

**Implemented in code (pending visual validation on real sources):**
- A "Colorimetry" parameter exposed as an option dropdown with five
  values: "Auto", "BT.709 limited", "BT.709 full", "BT.601 limited",
  "BT.601 full". Default is "Auto", which resolves to BT.601 limited
  for SD (height ≤ 576) and BT.709 limited for everything HD and
  above, based on the active `CapturedFormat`.
- Single shader program with two uniform vec4s. `ColorNormalize` packs
  `(yOffset, yScale, cOffset, cScale)` so the same matrix expression
  handles both limited and full range with a uniform swap;
  `ColorMatrix` packs `(krCr, kgCb, kgCr, kbCb)` for the per-coefficient
  matrix. Free helper functions in the cpp's anonymous namespace
  resolve mode → uniform tuples; the render path uploads them every
  frame so the user can A/B the dropdown live.
- HDR (PQ / HLG) deferred to v0.20.1 or later; needs 10-bit capture
  first which is its own change.

The AJA SDK does not help here; this is entirely a shader concern.


## v0.21 — Proper de-interlace [done — superseded by v0.35 on UYVY]

1080i50/59.94/60 and 525i / 625i currently capture and display, but
AutoCirculate delivers each interlaced frame as a single weave (both
fields woven into one full-height raster). On motion this produces
visible combing.

**Two approaches discussed, separately or together:**
- **Bob de-interlace** in the shader: render each field as a
  full-height frame by duplicating each scanline (or with vertical
  interpolation). Doubles the effective frame rate (50i becomes
  effectively 50p) and is cheap on GPU. Visible quality is acceptable
  for live VJing.
- **Yadif-style temporal de-interlace**: combines spatial and temporal
  filters using the current field plus the previous/next field's data.
  Better quality at the cost of a one-field latency penalty (one extra
  vblank). Heavier shader but still real-time.

For VJ work, bob was enough to ship first. Yadif eventually landed in
v0.35 once the operator asked for better static-detail rendering on
broadcast HD; the v0.21 bob path is still the fallback for V210 (see
v0.36 for the plan to lift that).

**Implemented in code (pending visual validation on real interlaced
source):**
- A "Deinterlace" parameter exposed as a boolean toggle. Off
  (default) keeps the historical Weave behaviour. On enables the
  active mode for the current pixel kind: YADIF on UYVY (v0.35),
  field-separation bob on V210 (legacy v0.21 path).
- Bob mode in the shader does **field separation at double-rate
  display**: every ProcessOpenGL call alternates between sampling the
  top field's scanlines (parity 0) and the bottom field's scanlines
  (parity 1), with linear interpolation between adjacent rows of the
  chosen field. Each captured frame's two fields appear at their own
  temporal moments, doubling the effective frame rate against the
  source.  This is the same trick vMix uses when its master frame
  rate is 50p / 59.94p / 60p against 50i / 59.94i / 60i input - the
  reason vMix has a reputation for "the best soft deinterlace" is
  not a clever algorithm but precisely this field-separation
  behaviour, which we now match. The user-visible result: motion
  looks smooth, no combing, no apparent resolution penalty as long
  as the render rate is >= 2x the source rate (60Hz Resolume on
  59.94i source = perfect 2x match).
- The render path force-clamps the InterlaceMode uniform back to 0
  whenever `m_currentFormat.isInterlaced == false`, so the user can
  leave the toggle on permanently and progressive sources still
  render correctly. The FieldParity uniform is uploaded
  unconditionally; the shader simply does not consult it on the
  Weave branch.

**Future (per-field delivery from AutoCirculate)**: switch
AutoCirculate to per-field delivery via `xfer.SetVideoBuffer()` plus
the interlaced-field flag. This would give us each field as a
separate transfer at twice the wake-up rate, reducing field-to-display
latency by half a frame. The shader-side Bob we have today already
achieves the same visual quality assuming the render rate keeps up;
per-field DMA is a latency optimisation, not a quality one. Tracked
as v0.32.


## v0.22 — Low Latency Mode [done]

Optional manual ping-pong capture path that bypasses AutoCirculate
entirely, mirroring the AJA SDK's `ntv2llburn` demo. Exposed via a
`Low Latency Mode` boolean toggle (default off). When on:

- `TryConfigureChannel` reserves two device frame slots via
  `AutoCirculateInitForInput(2)` + `AutoCirculateGetStatus` (reading
  `acStartFrame`) + `AutoCirculateStop`. The slots stay reserved for
  manual use after AC is stopped.
- A warm-up sequence (`SetInputFrame` slot 0 → `WaitForInputFieldID`
  → flip → `SetInputFrame` slot 1) primes the hardware so the
  first iteration of the capture loop reads a complete frame.
- The `Running` state in the capture loop bifurcates:
  - LL: `WaitForInputFieldID(NTV2_FIELD0)` → flip `m_currentInFrame`
    → `DMAReadFrame(slot, host buffer, bytes)` → `SetInputFrame` for
    the next iteration → `GetRP188Data(channel, rp188)` for timecode
    → upload to texture ring.
  - AC: as v0.21 (unchanged path).
- `WaitForInputFieldID` returning false triggers the same NoSignal
  recovery as the AC path's `HasAvailableInputFrame()==false`
  branch, sharing format-probe / raster-change logic.
- The overlay version label appends ` [LL]` when this mode is active
  on a live AJA capture, so the user can tell at a glance which
  transfer path is in use.

Result: capture latency drops from "~1 frame promedio with AC
ring=2..7 jitter" to "~1 frame deterministic". Sub-vblank streaming
is **not** what this delivers; that would require either GPUDirect
RDMA (AJA-supported only on Linux per the NTV2 17.5 streaming-DMA
documentation) or our own partial-DMA + `ReadLineCount` polling
loop, which has no SDK precedent for capture. The realistic Windows
ceiling for this hardware sits here.

Trade-off documented in the param's UI comment and the demo's
header: less tolerance to consumer stalls. If the render thread
cannot keep up with the SDI clock, captured frames are silently
overwritten by the hardware at the next vblank rather than counted
as drops at an observable driver counter. The toggle is opt-in so
the AC path remains the safe default.


## v0.23 — Format info in the inspector [done]

Read-only text parameter `Format` exposed in Resolume's parameter
inspector. The operator can see at a glance what the capture is
seeing — `1080p59.94`, `720p60`, `no signal`, `synthetic 1280x720
@60.00`, `no device`, etc. — without having to leave the overlay
on top of the rendered output, which is invasive in a live show.

Implementation notes:
- New parameter `PID_FormatInfo` registered with `FF_TYPE_TEXT`.
- `GetTextParameter(PID_FormatInfo)` composes the string on demand
  from atomic accessors of `AJACaptureSource` (`GetState()`,
  `LastVideoFormatRaw()`), so it is safe to call from any thread
  and always reflects the live state.
- `SetTextParameter(PID_FormatInfo, ...)` is a no-op success: the
  field is read-only from the operator's perspective, but FFGL has
  no native read-only text type, so Resolume can call SetText when
  the user clicks into the field or when a composition is loaded;
  we silently accept and let GetText restore our value on the next
  poll.
- The returned `char*` points to a 64-byte per-instance buffer that
  is repopulated on every call.
- Four-case decoding:
  1. AJA Running + raw format decodable → format name from `TryDecodeNTV2Format`
  2. AJA Running + raw not decodable → `WxH (raw fmt N)` fallback
  3. AJA NoSignal → `no signal`
  4. AJA Idle/Initializing/Failed → state name verbatim
  5. Synthetic producer → `synthetic WxH@FPS`
  6. Neither → `no device`


## v0.24 — Refactor the capture loop's Running state [done]

**What was done**: the AC and LL branches in `AJACaptureSource::CaptureLoop`'s
Running state previously duplicated two blocks of ~25 lines each
(the format-probe / raster-change / NoSignal transition, and the
1 Hz SDI statistics sample). Extracted into two private member
helpers on `AJACaptureSource`:

- `HandleNoFrameAvailable(card, inputSourceRaw, lastNoSignalRetry)`
  — runs the format probe, publishes the raster-change atomic when
  the input came back with a different size, transitions to
  NoSignal otherwise, always bumps the idle-wakeup counter. Both
  paths call it identically on their "no frame available" sentinel
  (`!status.HasAvailableInputFrame()` for AC,
  `!WaitForInputFieldID(...)` for LL).
- `SampleSDIStatisticsIfDue(card, isSDI, canSdiChecks,
  inputSourceRaw, framesSinceSdiSample)` — same body as before,
  including the 60-frame interval gate.

**Why not three helpers**: the original plan included a timecode
publisher helper. The AC and LL paths read RP188 from different
sources (`xfer.acTransferStatus.acFrameStamp.GetInputTimeCode` vs
`GetRP188Data` direct), and the "decode + atomic publish" tail is
only 10 lines. Wrapping it would have meant either pulling AJA
headers into the plugin header or passing the rp188 around with
forward-declared types — more friction than the duplication is
worth. Left inline in each branch.

**Signature note**: the helpers take the `NTV2InputSource` enum as a
raw `int` so the public header does not need to include AJA SDK
headers. The cast back happens inside the implementations.

**Effect**: when future features touch the Running state (HDR
detection, raster-change variants, per-field DMA), the lock-loss
contract lives in one place. Net code size roughly unchanged; the
maintenance value is in having one source of truth, not in line
count.


## v0.25 — Verbose load-time diagnostic logging [done]

**What was done**: extended the device-open path in `AJADevice::Open`
to populate ~25 new capability fields on the `Status` struct and to
emit a structured dump tagged `[KonaXSource:caps]` once per
successful device open. The dump is greppable in the Resolume log
and is intended to be the first stop in any troubleshooting
session.

The dump lines, in order:
```
[KonaXSource:caps] === <ModelName> (SN <serial>) ===
[KonaXSource:caps] firmware=... driver=... id=0x...
[KonaXSource:caps] connectors: SDI in/out/biDir/HDMI in/out/gen/SDIflags
[KonaXSource:caps] internals: FrameStores/CSCs/DMA/MSI/memoryMB
[KonaXSource:caps] rasters: SD/HD/2K/4K/8K/12G-SDI/HFR-RGB
[KonaXSource:caps] pixfmt: 8bYCbCr/10bYCbCr
[KonaXSource:caps] routing: 12g/multiFmt/3GLvlConv/RGBLvlA/LUTs/EnhCSC
[KonaXSource:caps] anc/timecode: customAnc/RP188
[KonaXSource:caps] diag: SDI-err-checks/HDMI-MultiView
[KonaXSource:caps] streamingDMA=...  (Linux-only path; not Windows)
[KonaXSource:caps] === end ===
```

**New Status fields**: `numHDMIOutputs`, `numCSCs`, `numDMAEngines`,
`activeMemoryMB`, `hdmiVersion`, `canDoFBF10Bit`,
`canDoSDVideo`/`canDoHDVideo`/`canDo2KVideo`/`canDo4KVideo`/
`canDo8KVideo`/`canDo12GSDI`/`canDoHFRRGB`,
`canDo12gRouting`/`canDoMultiFormat`/`canDo3GLevelConv`/
`canDoRGBLevelA`/`canDoColorCorr`/`canDoEnhancedCSC`,
`canDoCustomAnc`/`canDoRP188`,
`canDoStreamingDMA`/`canDoMSI`, `canDoHDMIMultiView`.

**The streamingDMA line** is explicit about being Linux-only because
that flag is the GPUDirect-relevant one and the operator's eye will
land on it first when latency is the question. The line tells the
truth: the silicon supports it, the Windows driver does not expose
the path.

**Test on hardware**: load the plugin with a Kona X present, grep
Resolume's log for `[KonaXSource:caps]`. All 11 lines should appear
exactly once per `AJADevice::Open` call. Values like `streamingDMA`,
`12g-SDI`, `RP188` should be `yes` on Kona X.

**Risk**: zero. Read-only operations; failure modes are limited to
"a flag query returned false because the SDK doesn't recognise it
on this firmware", which is the desired behaviour.


## v0.26 — Live capture stats in the inspector [done]

**What was done**: four new read-only text parameters in the
inspector that mirror the stats counters the on-screen overlay
shows, so the operator can see them without leaving the overlay
on in production.

- `Stats / Frames` — total frames captured since the plugin loaded.
  Maps to `FramesCaptured()` on AJA, `queue.ProducedCount()` on
  synthetic.
- `Stats / Drops` — driver-side drops (AC path) or queue drops
  (synthetic). LL path silently overwrites frames and does not
  count them, so this reads zero in that mode and the operator
  uses `Idle wakeups` instead.
- `Stats / Recoveries` — count of NoSignal → Running transitions.
  AJA only; synthetic always reads zero (no signal-loss path).
- `Stats / Idle wakeups` — VSyncs / FieldID waits that returned
  without a frame. Useful tell for "card is alive but the consumer
  is too fast / too slow". AJA only; synthetic always reads zero.

**Implementation**: same pattern as v0.23 (`PID_FormatInfo`).
`FF_TYPE_TEXT`, `SetTextParameter` no-op success for all five
read-only text params, `GetTextParameter` switches on the index
and formats the corresponding counter into one of four 32-byte
per-instance buffers. Each parameter has its own buffer so the
host can capture pointers to several of them in succession without
one buffer overwriting another's content.

**Why not collapse into one multi-line stats param**: FFGL's
`FF_TYPE_TEXT` accepts embedded newlines, but Resolume's inspector
renders text params as single-line edit fields. Four separate
labelled fields are easier to scan than one stuffed with
`\n`-delimited rows.

**Test on hardware**: open the inspector, watch the four counters.
`Frames` should tick at the SDI rate. `Drops` stays at zero on a
healthy capture. `Recoveries` increments on each cable yank+plug
cycle. `Idle wakeups` ticks slowly if the consumer outruns the
capture; large values point at a render-thread / SDI-rate
mismatch.

**Risk**: low. Same mechanism as v0.23, mostly mechanical.


## v0.27 — Visual status badge in the inspector [done]

**What was done**: three new read-only float parameters
`Status R`, `Status G`, `Status B`, registered with FFGL types
`FF_TYPE_RED`, `FF_TYPE_GREEN`, `FF_TYPE_BLUE` consecutively.
Resolume's inspector groups consecutive RGB parameters into a
single colour swatch, so what the operator sees is one square
that changes hue based on the capture path's live state.

**State → colour mapping**:
- AJA Running              → green  (R=0,   G=1,    B=0)
- AJA Initializing/NoSignal → amber  (R=1,   G=0.65, B=0)
- AJA Failed               → red    (R=1,   G=0,    B=0)
- AJA Idle (not started)   → grey   (R=0.5, G=0.5,  B=0.5)
- Synthetic producer alive → blue   (R=0.3, G=0.4,  B=1)
- Neither path alive       → dim    (R=0.2, G=0.2,  B=0.2)

**Implementation**: a single `ResolveStatusColor(R, G, B)` helper
on `KonaXSource` returns all three components from one state
snapshot, then `GetFloatParameter(PID_StatusR/G/B)` picks the
component for the requested PID. Computing all three on every Get
is trivially cheap (just reads, no atomics incremented) and
guarantees the three values come from the same state — important
when a cable yank happens between two of Resolume's polls.

`SetFloatParameter` for the three PIDs is a silent no-op success
(same pattern as `PID_FormatInfo` and the four stats params), so
Resolume's composition-load behaviour does not stomp on our
computed colour.

**Fallback behaviour if Resolume renders the params as three
separate sliders** (older versions, third-party hosts): the
mapping is still legible. A slider at 1.0 / 0.0 / 0.0 reads as
"error"; 0.0 / 1.0 / 0.0 reads as "fine". Worst case the
operator has three numbers in front of them instead of a
coloured square — still informative.

**Test on hardware**:
- Plug a cable, wait for lock → swatch goes green within ~2 frames.
- Yank the cable → swatch goes amber within one vsync.
- Cause a hard failure (e.g. another app grabs the card) → swatch
  goes red.
- Load the plugin without a card → swatch goes blue (synthetic).

**Risk**: low. Worst case Resolume does not visually group the
three params, and the operator sees three sliders. Still works.


## v0.28 — Regression test for the synthetic path [done]

**What was done**: minimal in-tree test framework and a test suite
covering the building blocks of the synthetic capture path that do
not require an OpenGL context.

**Test infrastructure**:
- `tests/test_framework.h`: header-only mini framework with
  `TEST_CASE(Suite, Name) { ... }`, `EXPECT_EQ/NE/TRUE/FALSE`,
  `ASSERT_TRUE`, `TEST_RETURN_OK`. Tests register themselves via
  static initialisation; `main()` runs them and exits with the
  failure count.
- `tests/test_main.cpp`: 8-line runner entry point.
- `konax_tests` executable target added to CMakeLists.txt, registered
  with CTest via `add_test()`. Links against the `ajantv2` interface
  target solely to inherit the platform compile definitions (`AJA_LINUX`
  etc.) that the SDK headers gate on; the test binary touches no
  AJA runtime code.

**Tests in this round**:
- `FrameQueue` (4 cases): init + write + read, producer overrun
  produces drop, repeated read produces repeat, ResetStats clears
  drops/repeats but preserves ProducedCount per the contract.
- `FrameStorePool` (4 cases): lowest free index, release frees the
  slot, max-stores ceiling respected, releasing an unclaimed index
  is harmless.
- `VideoFormat` (5 cases): 1080p59.94 / 720p60 / 1080i59.94 decode
  to the right raster, NTV2_FORMAT_UNKNOWN returns false and leaves
  output untouched, UYVYBytes matches the raster.

**13 cases, all green**.

**Discovered during implementation**: an initial assertion in
`FrameQueue.ResetStats` was wrong — it expected ProducedCount to be
zeroed, but the documented contract is that ResetStats clears only
drops/repeats and deliberately preserves ProducedCount to keep the
producer/consumer sequence handshake monotonic. The test was fixed
to match the contract. This is the kind of subtle regression that
the test suite is meant to catch in the future.

**CI workflow** (`.github/workflows/ci.yml`):
- Linux-only for now (Windows is the deployment target, but its CI
  setup is heavier than the gate is worth).
- Caches the AJA SDK and FFGL clones across runs so a re-run is
  fast (<2 minutes typical).
- Builds plugin + tests, runs `ctest --output-on-failure`.
- Uploads the plugin .so as a build artefact on success.

**Out of scope for this milestone**:
- GL-bearing tests (FrameProducer, SharedTextureRing texture ops,
  KonaXSource end-to-end). These need a headless GL context (EGL
  surfaceless, llvmpipe, xvfb) which is feasible but adds 30-60s
  of setup per run. Deferred until those paths develop a
  regression-prone surface.
- Windows / macOS CI jobs.

**Test on hardware**: not applicable. This is the gate that runs
without hardware to begin with.

**Risk**: low. Pure addition; no production code touched beyond
version bump.


## v0.29.1 — HDR transfer functions (PQ + HLG, 8-bit) [done]

**Scope decision**: full HDR support (10-bit packed v210 capture +
transfer functions + tone mapping + gamut conversion) is too large
for one iteration. Split into v0.29.1 (shader-only, 8-bit) now and
v0.29.2 (device-format switch to 10-bit) later. Doing it this way
lets the shader architecture for HDR land first; v0.29.2 only has
to change the device pixel format and the shader's unpack code,
leaving the PQ/HLG logic untouched.

**What was done**: extended the Colorimetry dropdown with two new
options, "PQ (ST.2084)" and "HLG", over BT.2020 colour primaries.
The capture path still requests 8-bit YCbCr (UYVY); only the shader
changes.

**Shader changes** (`KonaXSource.cpp` fragment shader source):
- New uniform `int HDRMode` (0 = SDR, 1 = PQ, 2 = HLG).
- Four new helper functions in the shader: `pq_inverse_eotf`,
  `hlg_inverse_oetf`, `bt2020_to_bt709`, `tone_map_reinhard`,
  `srgb_encode`. All are constant-branch elided on the SDR path.
- The end-of-`main()` logic branches on `HDRMode`: SDR clamps R'G'B'
  into [0, 1] as before; HDR applies inverse EOTF → tone map →
  gamut convert → sRGB encode.
- BT.2020 → BT.709 matrix from ITU-R BT.2087.
- Reinhard tone map (per-channel ratio derived from max component);
  preserves hue, costs some saturation in clipped highlights.
  Adequate first pass; can swap for BT.2390 or ACES later without
  touching anything else.

**C++ changes**:
- `ColorimetryMode::PQ_BT2020 = 5` and `HLG_BT2020 = 6` added to
  the enum.
- `NormalizeForMode`: PQ/HLG fall into the narrow-range branch
  (SDI carries them at 16..235 / 16..240 codes, not full-range).
- `MatrixForMode`: PQ/HLG select the BT.2020 → R'G'B' matrix
  (coefficients 1.4746 / -0.1646 / -0.5714 / 1.8814 from kR=0.2627,
  kG=0.6780, kB=0.0593).
- New free helper `HDRModeFor(ColorimetryMode)` returns the int
  the shader uniform consumes.

**Caveat documented in source + UI**: 8-bit PQ maps code 235 to ~93
nits, so highlights brighter than that on the original source are
clipped at the cable. The result is still useful — midtone colour
science lands roughly right, way better than treating the signal as
Rec.709 — but a proper HDR pipeline needs 10-bit capture, which is
v0.29.2.

**Test on hardware**: feed a PQ source over SDI, switch Colorimetry
to "PQ (ST.2084)". The image should look noticeably less crushed
than under "BT.709 limited" (the wrong-but-default choice). Same
for HLG. With an actual HDR source, compare against AJA Control
Room's HDR-aware preview as the ground truth.

**Risk**: shader-only, low. The new code path is opt-in (selecting
PQ or HLG in the dropdown); the SDR path is byte-for-byte
unchanged.


## v0.29.2 — 10-bit packed capture (v210) [done — EXPERIMENTAL, pending hardware]

**Why**: 8-bit PQ/HLG (the v0.29.1 starting point) clips at ~93 nits
peak. The Kona X supports `NTV2_FBF_10BIT_YCBCR` (v210 packed: three
10-bit samples in each 32-bit word, six pixels per 16 bytes). This
gives the full HDR code range and removes the clipping ceiling.

**Shipped**
- New `PixelKind` enum in `PixelFormat.h` plus `V210FrameSize()`,
  `TextureWidthTexels()`, `FrameSize()` helpers anchored by 7
  dedicated unit tests (1080p / 720p sizes, UYVY < V210 invariant,
  texture-width dispatch, FrameSize dispatch). Tests run in CI.
- `SharedTextureRing` and `TextureUploader` `Initialise()` and
  `Upload()` now take `PixelKind` (default UYVY), so the existing
  8-bit path is byte-for-byte unchanged. Internally they pick the
  right RGBA8 texture width and PBO stride. Both `m_kind` members
  are private and read at upload time.
- `AJACaptureSource` constructor takes `PixelKind` and stores it in
  the same atomic-with-capture-thread-mirror pattern that ring depth
  and low-latency mode already use. `TryConfigureChannel` snapshots
  the kind, resizes `m_transferBuffer` via `FrameSize()`, picks
  `NTV2_FBF_10BIT_YCBCR` vs `NTV2_FBF_8BIT_YCBCR` accordingly. If
  the card lacks `canDoFBF10Bit`, the capture path logs a fallback
  notice and silently degrades to 8-bit (the toggle becomes a no-op).
- New UI toggle `PID_TenBitMode = 19` (FF_TYPE_BOOLEAN, default 0,
  labelled "10-bit capture (v210)"). The setter triggers
  `ReopenAJADevice` because the FrameStore format is set at
  AutoCirculate init time and cannot be flipped mid-stream. All
  three `AJACaptureSource` construction sites (InitGL, raster
  change, ReopenAJADevice) and all three texture pipeline init
  sites pass `CurrentPixelKind()`. `m_queue.Initialise()` now uses
  `FrameSize()` instead of `fmt.UYVYBytes()`.
- New shader uniform `PixelFormatMode` (0 = UYVY, 1 = V210). The
  fragment shader's `main()` branches on it: UYVY keeps the
  existing macropixel reconstruction; V210 computes
  `groupIdx = col/6, inGroupIx = col%6, pairIdx = inGroupIx/2`,
  fetches 4 RGBA8 texels at `(groupIdx*4 + [0..3], row)` via
  `texelFetch`, reconstructs each 32-bit word from its 4 bytes
  (LSB-first byte order), and extracts Y / Cb / Cr per the SMPTE
  v210 bit layout (documented inline). `NormalizeForMode` now
  takes `PixelKind` and returns the (yOffset, yScale, cOffset,
  cScale) tuple in 8-bit code-space for UYVY (16, 1/219, 128,
  1/224 limited; 0, 1/255, 128, 1/255 full) or 10-bit code-space
  for V210 (64, 1/876, 512, 1/896 limited; 0, 1/1023, 512, 1/1023
  full). The downstream colour-matrix and HDR code stays
  bit-depth-agnostic.
- Overlay version line gains a `[10b]` suffix when V210 is active
  on an AJA capture session; combined with `[LL]` it reads
  `v0.34 [LL 10b]`. Synthetic and 8-bit sessions are unaffected.

**Known limitations / caveats**
- **Not yet validated on hardware.** The shader's v210 unpack
  assumes standard SMPTE byte/bit layout (3 × 10-bit samples per
  32-bit word, LSB-first packing) and little-endian byte order
  inside each RGBA8 texel. AJA's `NTV2_FBF_10BIT_YCBCR` is
  documented to use the standard layout, but no AJA card has been
  available during development to confirm. If the operator's
  capture appears colour-tinted or shows horizontal stripes when
  the toggle is on, the workaround is to leave the toggle off and
  fall back to the v0.29.1 8-bit path.
- Bob de-interlace + V210 degrades to nearest-row field separation
  because `texelFetch` doesn't interpolate. Operators wanting Bob
  on HDR-interlaced sources should leave the 10-bit toggle off
  until a future revision moves V210 to a more elaborate sampler
  that re-introduces vertical interpolation.
- The toggle is a hard reopen of the AJA device — a few hundred ms
  of black flashes during the swap. Acceptable for a setup-time
  decision; not for mid-show toggling.

**Test on hardware (when available)**
- 10-bit HDR source visibly cleaner highlights than the same source
  in v0.29.1 (8-bit). Bright skies and specular reflections retain
  detail instead of stair-stepping.
- 4K UHD at 60p in 10-bit pushes ~1.3 GB/s through the PCIe bus —
  needs validation that the threadripper workstation sustains it
  without drops.

**Risk**: medium. The C++ scaffolding is fully tested and the 8-bit
path is unchanged by construction (UYVY is the default everywhere).
The remaining unknown is the shader's v210 layout assumption, which
will reveal itself the first time the operator toggles 10-bit on a
real card.


## v0.30 — Quad-link 4K / 8K for cards with 4 SDI BNCs [quad-link 4K: experimental in v0.42]

v0.42 implements quad-link 4K (and 4K HDMI through 425 muxes) as an
experimental feature for publication; see the v0.42 section. 8K stays
out of scope. The original assessment follows.

Kona X has only 2 SDI BNCs so this is **not applicable to the user's
hardware**. For Kona 5 / Corvid 44 / IO 4K Plus type cards with 4
BNCs:
- 4K via quad-link: 4 BNCs each carrying 1080p, internally bonded
  into a single 4K raster via 2SI (TSI) or Squares mode across 4
  FrameStores.
- 8K via dual quad-link: 8 BNCs (no card has these) or quad 12G.

This adds significant complexity to the `FrameStorePool` (claim sets
of FrameStores) and to the routing (multi-path `Connect`). **Decision:
not implemented**. Will be revisited only if someone with a 4-BNC
card asks for it; the FFGL plugin's whole-Kona-X focus does not
warrant building it speculatively. Kept in the roadmap so the
version numbering reads consistently.

**Risk**: high (lots of corner cases in 2SI vs Squares, format
detection across 4 inputs, mid-show topology changes). Defer.


## v0.31 — Yadif de-interlace [done — landed as v0.35]

**Status reversed**: originally listed as out of scope because the
v0.21 field-separation bob matched vMix's broadcast contract. The
operator later asked for better static-detail rendering, especially
on type cards and screen-graphic overlays where Bob's interpolation
between same-field rows leaves visible stair-step artefacts on fine
horizontal lines.

Implemented as v0.35; see that section for the deliverable, the
algorithm references, and the testing notes. v0.31 is kept here only
so the roadmap version numbering reads in order.


## v0.32 — Per-field DMA delivery for half-frame interlaced latency [done — partially hardware-validated]

**Hardware validation status (de la sesión actual):**
- **LL OFF + UYVY interlaced**: validado. Medida del usuario contra
  DeckLink Quad 2 + Resolume: v0.35 baseline era −2 frames behind
  DeckLink; v0.32 da −1 frame behind DeckLink. **Ganancia medida de
  1 display frame**, exactamente lo que el modelo predijo.
- **LL ON + UYVY interlaced**: NO mejorado. v0.32 no toca el path LL
  (gate explícito: `!m_activeLowLatencyMode`). LL ON en interlaced
  marca −4 frames behind DeckLink, identico al baseline pre-v0.32 en
  LL ON. Este NO es un bug de v0.32 — es una limitación estructural
  pre-existente del path LL en interlaced que solo se ha medido por
  primera vez en esta sesión.

**Razón estructural de la limitación LL+interlaced**:
- LL ON publica al ring a frame-rate de fuente (30 Hz para 1080i59.94),
  vía `WaitForInputFieldID(NTV2_FIELD0)` + `DMAReadFrame` del frame
  completo.
- YADIF en field-rate aplica read-ahead de 1 source frame = 33.4 ms
  = 2 display frames a 60 Hz.
- Plus: descuadre de cadencias entre publish a 30 Hz y composición
  Resolume a 60 Hz añade stalls de compositor (~1-2 display frames
  adicionales).
- Total: ~4 display frames behind, que es lo que medimos.

**Why**: pre-v0.32 the AutoCirculate path delivered full frames at the
frame rate, even when the source was interlaced. For a 1080i59.94
source, the plugin waited for both fields (one whole frame = 16.7 ms
at 30 Hz frame rate) before publishing to the ring. The YADIF path
then needed a third buffered frame as "next", landing the rendered
output ~2 display frames (~33 ms at 60 Hz) behind the source — vs ~1
frame ahead of DeckLink in progressive mode, the swing in interlaced
was a measured 3 frames slower than DeckLink+Resolume.

**What was done**

1. Capture path (`AJACaptureSource`): when the active format is
   interlaced AND the pixel kind is UYVY AND low-latency mode is OFF,
   `AutoCirculateInitForInput` is now called with the
   `AUTOCIRCULATE_WITH_FIELDS` flag. Each `AutoCirculateTransfer`
   delivers ONE field's worth of contiguous half-frame data
   (`m_frameBytes / 2` bytes). The capture loop branches on
   `m_fieldModeActive`:
   - In frame mode (progressive sources, V210, LL): unchanged.
   - In field mode: DMA target is always `m_transferBuffer`
     (full-frame-sized but only half is overwritten per transfer).
     After the transfer, the contiguous field rows are scattered into
     `m_fieldAccumBuffer` at every-other-line offset (offset 0 for top
     field, +linePitch for bottom). The mapping from AC's
     F0/F1 (`acPeerToPeerFlags & AUTOCIRCULATE_FRAME_FIELD0`) to
     top/bottom rows depends on field order: TFF source → F0=top,
     F1=bottom; BFF source (NTSC 480i) → F0=bottom, F1=top.

2. Ring (`SharedTextureRing`): `Upload` now takes optional
   `FieldHint hint = FullFrame` and `bool isFirstFieldOfFrame = false`
   parameters. Per-slot storage of both, exposed via `ReadResult`.
   Existing callers (frame mode, synthetic producer) pass no extra
   args and get the v0.31 behaviour. Field-mode capture passes
   `FreshTop` / `FreshBottom` and `isFirstFieldOfFrame = isF0` so the
   render thread can drive YADIF history rotation and the
   FieldParity uniform correctly.

3. Render path (`KonaXSource`): on each fresh ring acquire, the new
   `m_lastFreshFieldHint` member captures the publish's `fieldHint`.
   The yadif-history blit branches on the hint:
   - `FullFrame` or `isFirstFieldOfFrame=true`: rotate (call
     `YadifSlots::Advance`) then blit the whole ring texture into
     the new "next" slot via `BlitRingFrameIntoHistory`.
   - Field publish that is NOT a frame-start: `BlitRingFieldIntoCurrentNextSlot`
     re-blits the ring texture into the *existing* next slot (no
     rotation). The shared FBO-blit code is extracted into
     `BlitRingTextureToHistorySlot` so both paths share state-save/
     restore. The `FieldParity` uniform is now overridden directly
     from `m_lastFreshFieldHint` (FreshTop → 0, FreshBottom → 1) so
     the shader's field choice matches the freshly-written field
     row-set every render. The counter-based scheme stays in effect
     for FullFrame publishes.

4. Counter semantics: `m_framesCaptured` increments only on F0 (not
   every field), so `FramesCaptured()` remains semantically "captured
   source frames". Field-rate work is observable via
   `SharedTextureRing::UploadedCount()`.

**Expected savings**: in steady state, the renderer can now display
F0 of frame N as soon as F0 of frame N+1 has been published (one
field interval = ~16.7 ms at 30 Hz frame rate / 60 Hz field rate),
instead of waiting for F1 of frame N+1 too. That's ~1 display frame
at 60 Hz in the rendered output — taking the user from "DeckLink + 2
frames" to "DeckLink + 1 frame" in interlaced.

**Scope limits intentionally left for follow-ups**:
- V210 stays in frame mode. The scatter is UYVY-byte-stride-specific;
  V210's packed 10-bit layout would need its own scatter pattern, and
  the V210 interlaced path uses simple bob anyway, where the latency
  win is smaller.
- LL mode stays in frame mode (`AUTOCIRCULATE_WITH_FIELDS` requires
  the AC dispatcher; LL uses raw DMAReadFrame). Field-mode-in-LL would
  pair `WaitForInputFieldID(FIELD0/FIELD1)` with per-field DMA reads
  — a separate work item.
- The first publish of a session may briefly land an F1 (if AC starts
  mid-frame) before a proper F0 arrives. The renderer shows weave
  fallback during the first 3 source frames anyway (yadif gate at
  `framesBuffered >= 3`), so the misalignment self-heals within
  ~50 ms with no user-visible artefact.

**Test on hardware**
- 1080i59.94 source with a clear motion subject (panning shot,
  scrolling ticker). Measure latency vs the v0.31 build at the same
  Ring Depth: should drop by ~1 display frame at 60 Hz.
- Compare against DeckLink + Resolume on the same source: expectation
  is "DeckLink + 1 frame" instead of "+2 frames".
- BFF-class source (480i NTSC) if available: confirm field-row
  scatter offsets (F0=bottom, F1=top) read correctly.

**Risk**: medium. Touches AC init, capture loop, the texture ring's
publish API, and the render-side yadif history rotation. The frame-
mode and field-mode code paths are kept side-by-side with a single
`m_fieldModeActive` gate so non-interlaced and V210 paths are
provably untouched (the gate is built into the capture session's
init and stays stable for the session).


## v0.33 — Configurable buffer count for Low Latency Mode [done]

**What was done**: the LL path's hardcoded `kLowLatencyFrameCount = 2`
constant is replaced by `std::min(m_activeRingFrameCount, 3)`, so the
user-facing Ring Depth parameter now applies to LL mode as well as
to the AutoCirculate mode. Values above 3 are silently clamped:
beyond a 3-slot rotation the manual SetInputFrame cadence cannot
benefit (each WaitForInputFieldID reads exactly one slot back).

**Algorithm change**: the v0.22 `m_currentInFrame ^= 1` (XOR-1
ping-pong, N=2-only) is replaced by a cycle-based rotation that
works for both N=2 and N=3:
- New capture-thread-local member `m_llCycleSlot` (0..N-1) tracks
  the slot the hardware is currently writing in this iteration's
  interval (= the slot we last passed to `SetInputFrame`).
- After each `WaitForInputFieldID`, the "just completed" slot is
  `(m_llCycleSlot + N - 1) % N` — one position BEFORE the cycle
  slot in the rotation. That is what `DMAReadFrame` reads.
- After the DMA, advance `m_llCycleSlot = (m_llCycleSlot + 1) % N`
  and `SetInputFrame` to the new slot. The hardware will switch
  to it at the next vblank.

**Why this design vs. simpler "advance slot by +1 and use same for
DMA and SetInputFrame"**: the simpler design works for N=2 because
the cycle has only two members (DMA target and SetInputFrame target
collapse to "the other slot"). For N=3 they DIFFER — DMA target is
the slot just completed (one position behind hw); SetInputFrame
target is the slot two positions ahead (so when hw arrives at it
the next vblank, it doesn't overwrite the slot we are still DMA-
reading). Without the separate cycle tracker, N=3 ends up reading
slots two intervals old (2-frame steady-state latency) instead of
one. The cycle-slot variable adds 4 bytes of per-instance state and
keeps N=3 latency identical to N=2 in steady state. The third slot
is pure buffer slack — at any moment one slot is being written,
one is being DMA-read, and one is idle and available as cushion
if the consumer stalls.

**Test on hardware**:
- Ring Depth = 2 in LL: identical behavior to v0.22 (N=2 ping-pong).
- Ring Depth = 3 in LL: identical latency, but a stalled
  `ProcessOpenGL` (e.g. an occasional render-thread hiccup) has
  one frame of safety margin before the rotation wraps and an
  in-flight DMA risks racing the active write.
- Ring Depth = 4..7 in LL: silently treated as 3.
- Toggle Ring Depth mid-stream: state machine briefly flashes
  Initializing → NoSignal → Running (same as channel switch
  handling), no crashes.

**Risk**: medium. The algorithm change passes mental tracing for
both N=2 and N=3, but the LL path is the one that exercises the
hardware most directly and the first place to look if the picture
flickers or tears in production. The N=2 sub-case is byte-for-byte
equivalent to v0.22's behavior under the cycle formula, so worst
case the regression is N=3-specific.


## v0.35 — YADIF de-interlace [done — pending hardware validation]

**What was done**: the v0.21 field-separation bob is replaced on the
UYVY path by YADIF (Yet Another DeInterlacing Filter), a motion-
adaptive deinterlacer that combines an edge-directed spatial predictor
with a plausible-motion temporal clamp built from three captured
frames (previous / current / next).

**Algorithm references**: the GLSL shader is ported from FFmpeg's
`vf_yadif_cuda.cu` (Philip Langdale, LGPL-2.1, 2018) via libplacebo's
GLSL adaptation (Niklas Haas). The spatial predictor operates
vector-wise on RGBA8 macropixels — each of the four channels (U, Y0,
V, Y1) independently selects its best edge direction over horizontal
offsets ±1..±3 macropixels. The temporal predictor binds its samples
the way libplacebo and the libavfilter C code do (temporal average
from the field-adjacent frames).

Correction (v0.39 audit): `tdiff0` is NOT halved in our shader, and
this was documented as matching the CUDA original. Current upstream
does halve it in all three references (`vf_yadif_cuda.cu`:
`max3(tdiff0 >> 1, ...)`, `vf_yadif.c`, libplacebo: `abs(D - I) / 2.0`),
so this is a divergence of ours, not of upstream. The effect is a
wider temporal clamp under motion (output leans towards bob). It is
kept for now because the current output is the validated one; see
docs/AUDIT_v0.39.md.

**C++ infrastructure**:
- Three local history textures owned by `KonaXSource`, sized to match
  the capture raster in UYVY macropixel layout. Allocated lazily on
  first InitGL or on raster change via `EnsureYadifHistory`.
- Two FBOs used for `glBlitFramebuffer`: one bound to
  `GL_READ_FRAMEBUFFER` with the ring's texture as source, one bound
  to `GL_DRAW_FRAMEBUFFER` with the chosen history slot as
  destination. ~4 MB blit per arrived frame, sub-millisecond on the
  4080. The blit is the price we pay for field-pure access to three
  frames; the ring's slots cannot be retained across acquires. Note:
  the initial implementation used a single FBO with attachments on
  both READ and DRAW targets — a misunderstanding of how
  GL_{READ,DRAW}_FRAMEBUFFER work (they are binding points, not
  separate attachment storage). Hardware validation revealed it
  immediately: the blit silently copied the destination onto itself,
  the history textures stayed at their initial zeros, and the
  YADIF output was a flat ~(0, 0.3, 0) green (limited-range YCbCr
  normalisation of all-zero codes converted via Rec.709). Fixed by
  splitting into two distinct FBO objects.
- Slot bookkeeping factored into `konax::YadifSlots` (pure struct,
  unit-tested independent of GL). `Advance(sequence)` returns the
  destination slot and rotates prev/cur/next; `Reset()` is called
  on history reallocation and on format-change matching-dimensions
  early-out.
- Render path samples `history[curSlot]` (one frame behind the most
  recently arrived one), with `history[prevSlot]` and
  `history[nextSlot]` straddling it in time. This adds ~1/sourceRate
  of display latency relative to weave (≈33 ms at 30 Hz captured /
  1080i60). Documented latency stat under-reports by this offset.

**Activation gate** (`yadifPath` in render code):
1. Deinterlace toggle is on.
2. Source format is interlaced.
3. Pixel kind is UYVY (V210 falls back to v0.21 simple bob; see v0.36).
4. Off-thread upload path is active (inline upload does not blit into
   history).
5. `m_yadifSlots.framesBuffered >= 3` (so all three role textures
   contain valid data).
6. History textures are allocated.

Until all six are true the render path silently falls back to plain
weave on the ring's most recent texture; the user toggle is honoured
but cannot engage YADIF.

**Field-parity tracking**: the FieldParity uniform alternates 0 ↔ 1
within each captured frame. The tick is reset to 0 on every new
captured frame (sampled from `AJACaptureSource::FramesCaptured()` or
`SharedTextureRing::UploadedCount()`) so that drift between Resolume's
render rate and the source's frame rate cannot accumulate. The
TopFieldFirst uniform is auto-derived: 0 for NTSC-class
`height <= 486`, 1 otherwise (HD 1080i, PAL 576i).

**Tests** (9 new cases, `test_YadifSlots.cpp`):
- Initial state is the canonical {0,1,2} permutation, framesBuffered
  = 0, lastBlittedSequence at its sentinel.
- First Advance writes into the old prevSlot, then that slot becomes
  the new nextSlot.
- After three Advances the slot chronology (prev / cur / next maps to
  oldest / middle / newest sequence) holds.
- framesBuffered saturates at 3.
- Duplicate sequences are absorbed without rotating.
- Advance after a duplicate still works.
- Reset restores initial state.
- Permutation invariant holds across 1000 advances.
- Reset clears lastBlittedSequence (regression guard against a
  matched-sequence no-op after a format change).

**Test on hardware**:
- 1080i59.94 source with fine-detail content (a stationary type
  card; a still graphic overlay with thin horizontal lines).
  Comparison against the v0.21 bob output should show measurably
  cleaner static detail with no introduced flicker. Latency
  measurement (camera at 1000 fps) should show +33 ms vs the v0.21
  bob path, matching the documented one-frame read-ahead cost.
- Live broadcast content (panning shot, sports footage) compared
  against the DeckLink reference. The output should sit between
  bob and DeckLink's filter in motion clarity; the documented
  `tdiff0` divergence means we accept that.
- Toggle between Deinterlace ON and OFF mid-stream: clean transition,
  no flash of garbage on the engage / disengage frame.
- Toggle the 10-bit V210 mode while Deinterlace is on: V210 falls
  back to v0.21 bob (see v0.36); the gate flips silently.

**Risk**: medium. The C++ slot rotation is exhaustively unit-tested,
so the moving parts that are not GL are pinned down. The shader
math reproduces an externally-validated reference (except the
un-halved `tdiff0`, which the v0.39 audit found is our own divergence,
not upstream's).
The remaining failure modes are GL-side: a driver that does not
honour the FBO + glBlitFramebuffer dance between contexts, or a
sampler unit binding race against the upload thread's write into the
ring. Both would manifest as wrong-frame artefacts that are easy to
diagnose visually.


## v0.37 — Low-Latency mode field-rate publish for interlaced [done — pending hardware validation]

**Solution chosen**: instead of implementing per-field DMA inside the
manual LL slot rotation (which would require AJA SDK behaviour
neither the docs nor the demos confirm — whether `SetInputFrame`
respects field-granularity advances in non-AC mode), we reroute
LL+interlaced through the AC WITH_FIELDS path that v0.32 already
delivered. The user-facing "Low Latency" toggle keeps its meaning
("give me minimum latency") but the IMPLEMENTATION for interlaced
UYVY is now AC field mode regardless of toggle state, because AC
field mode is structurally faster than manual LL frame-rate for
that content.

**What was done**

1. `AJACaptureSource::m_useACPath` (new): derived flag computed at
   every session re-init. Replaces `m_activeLowLatencyMode` as the
   capture loop's dispatch gate. Rule:
   - If `m_fieldModeActive` (interlaced + UYVY): force `true`
     (always route through AC, regardless of LL toggle).
   - Else: `!m_activeLowLatencyMode` (legacy semantic).

2. `m_fieldModeActive` gate dropped the `!m_activeLowLatencyMode`
   restriction: now interlaced + UYVY enables field mode regardless
   of LL toggle.

3. Capture loop and `TryConfigureChannel` LL-setup block both
   dispatch on `m_useACPath` instead of `m_activeLowLatencyMode`.
   Progressive sources with LL toggle ON still take the manual LL
   path; interlaced sources with LL toggle ON take the AC field mode
   path (the override).

4. Log line emitted once per Initializing pass when the override
   applies, so the redirection is visible in the diagnostic log.

**Expected savings**: matches the v0.32 LL-OFF measurement at
−1 frame vs DeckLink in interlaced. With this change, LL ON in
interlaced should reach the same latency floor as LL OFF in
interlaced (both go through the same AC field-mode path).

**Scope limits intentionally left for follow-ups**:
- LL toggle still controls progressive path behaviour (no change).
- V210 interlaced stays on the path chosen by the LL toggle: LL ON
  goes through manual LL frame-mode, LL OFF goes through AC frame
  mode. Neither benefits from field-rate publishing because V210
  doesn't have a field-mode capture path yet.

**Test on hardware**
- 1080i59.94 LL ON + Deinterlace ON: measure vs LL OFF + Deinterlace
  ON in the same source/setup. Expected: identical latency (both go
  through AC field mode).
- Confirm the log line "routing through AC field mode" appears once
  per session start when LL is on and source is interlaced UYVY.
- Progressive source with LL ON: confirm log line does NOT appear
  and behaviour is unchanged (manual LL path).


## v0.37-original-plan — Low-Latency mode field-rate publish for interlaced

**Why**: discovered during v0.32 hardware validation. LL ON in
interlaced runs ~3 display frames slower than LL OFF + v0.32 against
DeckLink Quad 2 + Resolume in the same test conditions. The current
LL path publishes frames at source-frame rate (30 Hz for 1080i59.94)
via `WaitForInputFieldID(NTV2_FIELD0)` + `DMAReadFrame` of the full
frame. The YADIF render then reads-ahead by one source frame (33 ms
= 2 display frames at 60 Hz), and the publish/composition cadence
mismatch (30 Hz publish, 60 Hz Resolume compose) adds another ~1-2
display frames of compositor stall.

**Deliverables**
- LL path field-rate variant when source is interlaced + UYVY:
  alternate `WaitForInputFieldID(NTV2_FIELD0)` and
  `WaitForInputFieldID(NTV2_FIELD1)` (each fires at half-frame
  cadence = 60 Hz for 1080i59.94). On each wake, DMAReadFrame just
  one field's worth from the device — needs `DMAReadFrameSegmented`
  with the correct host/device strides to extract every-other-line
  rows, or DMAReadFrame the half-slot and scatter host-side.
- Apply the same host-side scatter accumulator + FieldHint metadata
  the AC field-mode path uses (`m_fieldAccumBuffer`, ring `FieldHint`,
  source-frame-seq). The render-side YADIF rotation logic from v0.32
  already handles this correctly via the sourceFrameSequence gate.
- Expected gain: same as v0.32 in AC mode (~1 display frame),
  plus elimination of the publish-cadence compositor stall by
  publishing at 60 Hz native.

**Risk**: medium. LL path slot rotation is delicate (the
`SetInputFrame` + `WaitForInputFieldID` cycle has tight timing
assumptions). Need to validate that the field-rate slot rotation
doesn't have its own race with the hardware's writer position.

**Test on hardware**
- 1080i59.94 LL ON: measure vs LL OFF + v0.32. Expected: LL ON
  catches up to within 1 display frame of LL OFF.


## v0.36 — YADIF for V210 (10-bit packed)

**Why**: v0.35 ships with YADIF on UYVY only; the 10-bit V210 path
falls back to the v0.21 field-separation bob. The blocker is
geometric: each output fragment in V210 mode depends on a group of 4
RGBA8 texels (six image pixels packed into four 32-bit words), not
on a single macropixel. The v0.35 `yadif_spatial` function takes
±3 macropixel taps; for V210 these would need to be redefined in
image-pixel space, and each tap would have to walk back through the
v210-unpack path to pull the right luma / chroma codes. Doable but
not trivial.

**Deliverables**
- Rewrite `yadif_spatial` to operate in image-pixel coordinates,
  with branch on `PixelFormatMode` to pick between the existing
  macropixel-coord version (UYVY) and the new pixel-coord version
  (V210). Tap count stays at 14 per row pair; the per-tap fetch
  invokes the v210 unpack the same way the weave / bob path does.
- `yadif_temporal` already operates on per-row samples and does not
  see the macropixel structure, so it should carry over with only a
  PixelFormatMode-aware fetch helper.
- Lift the `kindNow == UYVY` clause from the `yadifPath` activation
  gate in `KonaXSource::ProcessOpenGL`, and remove the V210 early-
  return from `BlitRingFrameIntoHistory`. The history texture
  layout needs to match the V210 ring slot size
  (`alignedWidth/6 * 4` texels), so `EnsureYadifHistory` grows a
  branch on the active pixel kind.

**Test on hardware**
- A 10-bit interlaced source through `NTV2_FBF_10BIT_YCBCR` with
  Deinterlace ON. The YADIF output should match what v0.35 produces
  on the same content captured as 8-bit UYVY, with the additional
  10-bit highlight range preserved.

**Risk**: medium. Two paths through the shader that have to match
visually. Until v0.29.2 (V210) is itself hardware-validated, this
milestone is blocked: the V210 path could ship with a layout error
that v0.36's shader rewrite would compound. Sequence: hardware-
validate v0.29.2 first, then take this one.


## Cross-cutting concerns

These are not version-numbered milestones; they're things to keep
in mind during any of the above work.

- **Test scenarios for every milestone**:
  1. Plugin loads in Resolume.
  2. With no AJA card present, falls back to synthetic SMPTE bars.
  3. With AJA card present but no cable: NoSignal overlay state.
  4. With cable + signal: real capture, FPS matches input rate, no
     driver drops, overlay shows AJA state in green.
  5. Cable yank: graceful transition to NoSignal.
  6. Cable re-plug: graceful transition back to Running, no leak.
  7. Plugin reload in Resolume: cleanup runs, FrameStore claim
     released, second load succeeds.
  8. Two plugin instances on different SDI inputs: both work, each
     uses its own FrameStore.

- **Documentation must stay accurate**. Every milestone that changes
  what works should update both README and LATENCY_STRATEGY. The
  README is the "what's supported today" doc; LATENCY_STRATEGY is
  the "why is it done this way" doc.

- **No new third-party deps unless absolutely required**. Current
  deps: libajantv2 (mandatory), FFGL SDK (mandatory), vcpkg toolchain
  on Windows (build-only). Adding more deps means more burden on
  the user's build setup.

- **Resolume Arena compatibility envelope**: tested against 7.26.1
  on Windows 11. FFGL 2.x. OpenGL 4.1 core profile. We do not use
  GL features past 4.1 so older Resolume builds should also work.

- **Side task: AJA SDK support ticket re: Windows GPUDirect.** As of
  this writing the Kona X advertises sub-frame latency via GPUDirect
  RDMA, but the documented path (NTV2 17.5 streaming DMA APIs, AJA's
  GStreamer plugin's NVMM mode, Clara Holoscan integration) is
  Linux-only and requires the driver to be built with `AJA_RDMA=1`.
  Open a ticket at https://sdksupport.aja.com asking explicitly
  whether the Windows NTV2 driver exposes GPUDirect RDMA, GPUDirect
  for Video (DVP), or any equivalent PCIe peer-to-peer DMA path to
  an NVIDIA GeForce RTX 40-series GPU. Outcomes:
  - Yes, and here is the API → revisit v0.22 / v0.32 with a third
    transfer mode `GPU Direct` that DMAs straight into a GL texture
    backed by a CUDA-OpenGL interop buffer.
  - No, and it is on the roadmap → keep an eye on subsequent SDK
    releases.
  - No, and no plan → close the question with confidence. The user's
    Threadripper + RTX 4080 workstation can either accept the Windows
    ceiling we have today or migrate to a Linux + Holoscan workflow
    that is fundamentally not Resolume.


## v0.40 — Latency

Where the interlaced and AutoCirculate latency was going, and what
v0.40 changes. Pending hardware check.

**1. AutoCirculate kept one complete frame in hand.** The capture
loop transferred only when `HasAvailableInputFrame()`, which is
`GetBufferLevel() > 1`. In the driver (`AutoCircGetBufferLevel`,
`ntv2autocirc.c`) the level counts complete frames, so level 1 means
the frame finished at the interrupt we woke from; requiring 2 always
delivered the frame completed one interrupt earlier, and the driver
transfers the oldest valid frame first (`AutoCircFindNextAvailFrame`).
The SDK header explains the rule exists for audio (the last 512-byte
audio chunk may not be written yet at level 1). The plugin captures no
audio, so it now transfers at level >= 1: one frame less on the AC
path, one field less in field mode. The LL path already worked this
way.

**2. A late wake-up was never caught up.** One transfer per
interrupt meant a single late wake left the capture one frame behind
for the rest of the session. The loop now drains: when a transfer
reports more frames ready (`acTransferStatus.acBufferLevel`), it
transfers again without waiting, capped at the ring size. Counted as
`drains=` in the diag log.

**3. YADIF needs a field of lookahead.** Mode 1 reconstructs the
field one source frame behind the newest, because its temporal
predictor uses the field after the reconstructed one. With Low
Latency Mode on, field-mode sources use shader mode 2: the same
spatial predictor and bracket clamp, but the missing rows come from
the field just before the shown one and motion is measured over the
last frame only. It shows the newest field: two fields (one source
frame) less. Static detail is still woven exactly; slow motion trails
slightly more. Checked offline on Mesa against synthetic scenes
(static texture, moving edges): both modes de-comb equally, mode 2
shows a frame newer.

**Guards found in review.** In the driver, a drop (next slot still
full) keeps the hardware recording into the active slot and marks that
slot valid, so a level 1 transfer could return the slot under the
write head; such a transfer is now discarded (`activeSkips=`). A
2-slot frame-mode ring drops on every other interrupt by construction,
so frame mode now uses at least 3 slots. A driver-model simulation
(3+ slots, with the guard) gives no drops, no discards and always the
newest complete frame in steady state, and catches up within one
interrupt after a stall.

**4. Smaller items.** Field-mode DMA buffer page-locked once (it was
pinned per transfer by the driver); capture thread registered with
MMCSS as a "Capture" task; LL warm-up frame no longer published.

**How to measure on hardware:** same DeckLink comparison as v0.37,
progressive with LL off (AC) and on, interlaced with LL off and on.
Expected: progressive AC now equal to LL; interlaced about one field
better with LL off and about three fields better with LL on, against
v0.39. The diag line should show `drains=` and `activeSkips=` staying small
and idle wake-ups near zero. Quick check of the 2-slot finding: v0.39
with Low Latency off and Ring Depth 2 on a 1080p source should show
Drops rising at about half the frame rate if the Windows driver matches
the published driver source.


## v0.41 — Shared capture

Reported on the Kona X (v0.40 build, no log available): two clips on
SDI1 showed a picture for about a second and then froze; another SDI1
clip always read "no signal"; a lone SDI2 clip flickered between
"no signal" and "init" and never showed a format.

Root cause in the architecture: every clip (FFGL instance) was a capture
of its own. Two clips of SDI1 were two FrameStores routed from one
connector; and the FrameStore pool handed out the lowest free index, so
a lone SDI2 clip, or a second clip, ran on a crossed route (SDI2 -> FS1,
SDI1 -> FS2). Every failing case had one or both; the working setups
(one SDI1 clip; v0.37 with SDI1 + SDI2) had neither. Resolume's own live
inputs open each input once and share it between its clips; the plugin
now does the same.

`konax::SharedCapture` (src/SharedCapture.{h,cpp}) owns everything that
exists once per (device, input): AJADevice, FrameStore claim,
AJACaptureSource or the synthetic FrameProducer, SharedTextureRing and
SharedGLContext, FrameQueue + TextureUploader (inline path), the current
format and pixel kind, and the capture settings (Ring Depth, Low Latency,
10-bit). A process-wide registry of weak_ptrs keyed by (device, input,
current GL context) hands every KonaXSource the shared_ptr of its
input; the last clip's release destroys it on the render thread
(DeInitGL / ProcessOpenGL, host context current). KonaXSource keeps the
shader, colorimetry / de-interlace settings, YADIF history, overlay.

- A clip attaches in its first ProcessOpenGL, not in InitGL: the FFGL
  SDK's FF_INSTANTIATE_GL sets the parameter defaults and calls InitGL
  in one go (ffgl/FFGL.cpp instantiateGL), so saved values arrive after
  InitGL. ProcessOpenGL re-attaches whenever the attached (device,
  input) differs from the parameters; setters forward capture settings
  only to the capture of the current parameters.
- Other threads reach `m_source` only under the clip's mutex
  (`SourceForParamsLocked`), never with a strong copy, so the last
  reference is always dropped on the render thread. Lock order: clip
  mutex, then the capture's status mutex.
- A destroyed capture parks its open AJADevice; the next capture of the
  same card takes it (no reopen on an input switch);
  `SharedCapture::CloseSpareDevices()` closes what was not taken. Each
  capture still has its own AJADevice (CNTV2Card) while it runs.
- FrameStorePool masks are per card.
- Per frame each clip calls `Update()` (idempotent within a host frame:
  marks the input as on screen, applies settings, re-sizes on a new
  format or pixel kind and bumps `FormatGeneration()`, lending, natural
  pairing, rate-clash check), then `Acquire()` / `EndRead()`. Several
  clips acquiring from one ring in one host frame is safe: a repeat
  returns the same reader slot, a newer publish swaps it, the old slot
  stays fenced by the earlier clip's EndRead. The ring's `isFresh` goes
  to the first acquirer only, so each clip judges freshness by the
  publish sequence for its YADIF history. The byte-queue path uploads
  each new frame once and every clip samples the same texture.
- Clips adopt the input's capture settings (`SettingsGeneration()`);
  a change raises `FF_EVENT_FLAG_VALUE` so the inspector follows.
- `Snapshot()` copies the status under a mutex that also guards the
  render thread swapping the capture / producer pointers, for the
  host's GUI thread (text parameters).
- Natural pairing: `FrameStorePool::PreferenceOrder` / `TryClaim`
  (SDI N -> FS N first, HDMI highest first); candidates the crosspoint
  ROM cannot reach are skipped (`AJADevice::CanRouteInputToFrameStore`).
  A capture offline for 2 s on a crossed route moves when its natural
  FrameStore is free.
- Lending: an input on screen without a FrameStore posts
  `MultiFormatRegistry::WantFrameStore`; a capture none of whose clips
  rendered for 250 ms yields (`AJACaptureSource::MaybeYield`): stops
  AutoCirculate, withdraws its format and releases its FrameStorePool
  bit from the capture thread, and parks. The SharedCapture sees
  `HasYielded()` after the join, forgets the index without releasing
  it again, and claims a FrameStore anew when a clip renders. A request
  is consumed by one capture, not all idle ones.
- `SdiOutputCarriesForeignPlayout`: the transmit-connector refusal
  only applies when the connector's SDI output is fed (traced back
  through CSC / mux widgets) by a FrameStore running another
  application's AutoCirculate playout.
- Failure reasons (`AJACaptureSource::StatusReason`) in the Format
  readout and as a red overlay line; overlay font completed.
- An independent review (second reading) found the GUI-thread last
  owner, the pre-InitGL assumption, a stale-value race in the setters,
  log spam while waiting for a FrameStore, the card reopen on input
  switch and the process-wide FrameStore mask; all fixed above, the
  first two reproduced by the smoke test against the earlier build. A
  third, shorter reading of those fixes found the card-handle reuse
  never taking effect (member initialisation order), a setting edited
  while a new capture was starting being lost, and a spurious log
  line; fixed. The parameter mirrors other threads touch are atomics.
- A final full review found nothing blocking a first hardware test and
  three things fixed since: lending now also requires other clips to
  have rendered meanwhile (`MultiFormatRegistry::RenderTick`, so a
  render-thread stall is not "not on screen"); FrameStores the routing
  ROM rules out are never claimed, and an input with no routable
  FrameStore stops asking for one; TSI is switched off for non-4K.
  Open, to settle on hardware: HDMI on a two-FrameStore card takes FS2
  (SDI2's pair), so HDMI-then-SDI2 runs SDI2 crossed; and the capture
  start-up / Input change runs on the render thread (card open,
  survey, shared GL context, ring allocation), a pause to measure.
- Verified offline: `tests/host/ffgl_host_smoke.cpp` (plugMain host on
  EGL + Mesa, synthetic inputs: sharing, shared settings, input
  switching, teardown, a GUI thread polling text parameters during
  60 input switches; clean under ASan + UBSan), and a three-consumer
  ring test (no tearing, every clip sees every publish, never an older
  one within a host frame).

To confirm on hardware: one SDI2 clip alone (log:
`SDI2 -> FrameStore 2 ..., natural pairing`); SDI1 + SDI2 clips; two or
more SDI1 clips in different layers (all show the picture, Format says
`| 2 clips`); Low Latency toggled in one clip shows in the other;
SDI1 + SDI2 + HDMI on screen alternately (lending).


## v0.42 — Every AJA card

Asked for so the plugin can be published: support every AJA card that
has an input, not only the Kona X it was written for. The capture code
was already capability-driven (no device IDs); what tied it to the
Kona X was the input encoding (1..4 SDI, 5 HDMI), a three-entry Input
menu, the assumption of multi-format and of 12G routing for 4K.

Capabilities were read from the SDK's tables (ntv2devicefeatures) for
every card with an input; the README's card table comes from that.

- **Input numbers** (`src/InputId.h`, header-only, no SDK;
  `src/InputNTV2.h` maps to NTV2InputSource / NTV2Channel). The number is
  the Input option value + 1, and option values are stored in
  compositions, so the original ones keep their inputs and the new
  inputs take the values after them: SDI 5-8 = 6-9, HDMI 2-4 = 10-12,
  quad-link SDI 1-4 / 5-8 = 13 / 14. HDMI n's channel is n-1 (the
  SDK's HDMI input calls take it). The Input menu is the union of the
  detected cards' inputs (`AJADevice::DeviceEntry::inputMask`), in the
  order SDI, quad-link, HDMI; without a card, the original three.
  Output-only devices are left out of the Device menu.
- **One-format cards.** `CNTV2Card::SetVideoFormat` programs channel 1
  when multi-format is not active (ntv2register.cpp), so on those cards
  every FrameStore runs one format. `MultiFormatRegistry::ClaimCardFormat`
  records the format of the input that configured the card (atomically
  with the check); another input with a different format fails to
  configure with a reason and retries; `ReleaseCardFormat` on teardown
  and on a failed configure. `FindConflict` treats any difference as a
  clash there (`! format clash`). The card is not reprogrammed for a
  second input of the same format.
- **FrameStore plans** (`src/CaptureTopology.{h,cpp}`, pure, tested):
  `PlanFrameStores(input, signalIs4K, caps)` gives Single (any one
  FrameStore: SD/HD/3G, and 4K on 12G-routing cards), Tsi2 (a fixed
  FrameStore pair through two 425 muxes) or Squares4 (four FrameStores),
  or a reason the input cannot be captured. Quad-link on 425-mux cards
  is 2SI; on cards without muxes (Corvid 24, KONA 3G Quad) squares;
  4K HDMI on 425-mux cards Tsi2 on pair 1+2 (HDMI 1) or 3+4 (HDMI 2).
  Mux base: the FrameStore index on four-FrameStore cards, half of it on
  the Corvid 88 (the demo's mux = fb / 2). CSC base for RGB 4K HDMI:
  2 x the first FrameStore.
- **Routing and FrameStore setup** follow
  `CNTV2DemoCommon::GetInputRouting4K` and `NTV2Capture4K::SetupVideo`:
  SetVideoFormat (the quad 4x formats; the SDK lists only those for
  non-12G cards) and SetTsiFrameEnable / Set4kSquaresEnable on the first
  FrameStore, VANC off and frame buffer format on all of them,
  AutoCirculate and interrupts on the first. The ROM is consulted for
  every connection of those routes. The four links must report the same
  format, and a link-1 VPID must match the interleave (2160-line
  quad-link standards are 2SI, `CNTV2VPID::IsStandardTwoSampleInterleave`).
  Teardown (and a failed start) switches the mode off. SetVideoFormat
  for an HD format already switches TSI / quad off for the pair / group
  (SetQuadFrameEnable(false), ntv2register.cpp); the plugin's own
  clearing only matters on a one-format card whose format it did not
  reprogram. `AJADevice::BusyFrameStores` counts the pair / group of a
  FrameStore running in TSI / quad mode.
- **SharedCapture** claims a plan's fixed set with
  `FrameStorePool::TryClaimMask` (never over a playout), releases it
  whole, and on a format change re-plans: an input going from HD to 4K
  (or back) on such a card gives its FrameStores back and claims the new
  set. A waiting 4K input asks for exactly its set
  (`MultiFormatRegistry::WantFrameStore(..., mask)`), and only a capture
  holding one of them lends (`ConsumeFrameStoreWant(..., ownMask)`).
  The capture thread re-plans from the live format and sends a mismatch
  back as a format change.
- Fixed on the way: a 4K signal on a non-12G card set squares mode on
  its single FrameStore, which copies the format to the whole group of
  four (ntv2register.cpp) and breaks the other captures. Now refused with
  a reason, checked before the format capability so the message says why.
- **No Signal** (`PID_NoSignal`): Black / Transparent / Hold Last Frame.
  Hold copies the host framebuffer's viewport into a texture of the clip
  after each drawn frame (before the overlay) and blits it back while
  there is no picture; dropped when the input / device changes or the
  viewport resizes. Copying the drawn result keeps it independent of the
  capture ring (re-allocated on a format change) and of de-interlacing.
- **HDR-signalled Auto colorimetry**: the capture thread samples the
  link-1 VPID transfer characteristics (SDI) or the HDMI DRM InfoFrame
  EOTF (HDMI 1-2; CTA-861 2 = PQ, 3 = HLG) once a second; Auto resolves
  to PQ / HLG when signalled.
- Renamed "AJA Input" in the FFGL info; ID KNX1 and the DLL name stay.
- **Resize** (`PID_Resize`, `src/PictureFit.h`, pure, tested), asked for
  after the user saw the clip take the composition's size. FFGL has no
  way for a source to choose its size (FF_RESIZE only goes host to
  plugin, no capability for it), and Resolume renders a source clip at
  the composition's size, so its own clip Resize cannot correct the
  aspect. The plugin draws the picture into a rect of the host's
  viewport instead: Fit (default) / Fill / Stretch (v0.41) / Original,
  Resolume's names. `PictureRect` keeps the display aspect (SD at 4:3),
  centres, snaps a rect within one pixel of the viewport to it (no
  one-pixel bars from rounding) and stays within GL_MAX_VIEWPORT_DIMS.
  When the rect is the viewport the draw is the v0.41 one, untouched;
  otherwise `ClipAreaScope` saves viewport, scissor test / box and clear
  colour, clips to the host's viewport (and the host's scissor box),
  clears the bars to transparent and sets the viewport to the rect. The
  main shader works in `uv` space (only the history-slot copy uses
  gl_FragCoord, at the slot's size), so de-interlacing is unaffected.
  No Signal Black blackens the picture's rect only; Hold Last Frame
  copies the whole viewport, bars included; the overlay is drawn after,
  over the whole clip. The host viewport is read from GL (what a
  full-clip draw fills), not from `currentViewport`.

Tested offline: 85 unit tests (InputId, CaptureTopology for every card
family, mask claims, pairing, the one-format rule, masked wants, the
quad-link format table, the Resize rect) and the host smoke test (name /
ID, 15 parameters, menu without a card, SDI 5 / SDI 8 / HDMI 2 / HDMI 4 /
quad-link inputs on the synthetic generator, unknown Input value, Hold
Last Frame without GL errors, Resize in a 4:3 clip read back pixel by
pixel, host GL state restored). MinGW syntax check against the Windows
headers: no new warnings. The sandbox SDK is 18.1.0; the sources also
compile against the ntv2_18_0_0 tag the Windows build clones.

Second reading (independent review before publishing) found and fixed:
- Major: a 2SI quad-link link with a payload ID (every ST 425-5 source)
  reports the 4K picture itself, `GetSDIInputVideoFormat` returning the
  VPID's 4x format; the quad-link mapping refused anything already 4K,
  so 2SI quad-link never started. 4x formats now pass through (Level B
  folded to A), tested with a real 2SI VPID.
- KONA HDMI: a 4K RGB picture on HDMI 1 used CSCs 1-4, two of them the
  CSCs of FrameStores 3 / 4 that HD RGB captures use; now 1, 2, 5, 6
  (HDMI 2: 3, 4, 7, 8) on cards with two CSCs per FrameStore
  (`TsiCscIndex`), the demo's 1-4 elsewhere.
- One-format cards: changing the card's format also changes another
  application's playout; refused while one runs.
- KONA XM (3 CSCs, 4 FrameStores): HDMI took FrameStore 4, which has no
  CSC; HDMI now prefers FrameStores with a CSC.
- The Io 4K's HDMI widget has no quadrant (nor RGB) outputs in the SDK
  routing tables: 4K HDMI is only planned where the quadrant outputs
  exist (`CardCaps::hdmiQuadrantMask`), and an RGB source on a
  YCbCr-only HDMI widget is taken from the YCbCr output
  (`hdmiRgbMask`).
- TSI / quad mode left on when AutoCirculate failed to start after it
  was set; cleared in the failure path. Another application's capture
  on the other FrameStores of a set is stopped (ntv2capture4k does).
- No quad-link offered on IP cards; README card table completed (Corvid
  HEVC, Io 4K UFC, KONA 3G Quad / Corvid 24 quad-link up to 30p, IP
  cards untested); Hold Last Frame copies at 16 bits in 16-bit
  compositions and does not retry an incomplete target every frame.


To confirm on hardware: no regression on the Kona X (SDI1, SDI2, HDMI,
4K 12G); No Signal modes; Resize in a composition of another aspect
(and how Resolume's clip Width / Height interacts); HLG / PQ signalling. Everything on other cards
needs their owners; 4K quad-link / HDMI is untested routing.
