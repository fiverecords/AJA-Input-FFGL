# Latency Strategy

> Development history from before the first public release (0.43), when
> the project was named konax-ffgl; see [README.md](README.md) in this folder.

The goal of `konax-ffgl` is end-to-end SDI-to-Resolume-output latency on
the order of 4–8 ms on a 60 Hz timeline — well under one frame, and
roughly an order of magnitude better than the 2–4 frames the stock AJA
plugin in Resolume currently delivers. This document tracks what we
built to get there, what worked, and what remains.

## Targets

| Stage                          | Latency target (1080p59.94)        | Status                     |
|--------------------------------|------------------------------------|----------------------------|
| Stock AJA Resolume integration | ~50–83 ms (2–4 frames @ 60 Hz)     | Baseline being displaced.  |
| Off-thread upload (no DMA)     | ≤ 16 ms (one frame)                | **Shipped (synth path).**  |
| Off-thread + AutoCirculate     | ≤ 16 ms                            | Shipped; HW pending.       |
| Racing the beam (streaming DMA)| ≤ 8 ms                             | Future (v0.20+).           |

Latency is measured from "last scanline enters the SDI cable" to "pixel
visible in the Resolume output framebuffer", excluding the downstream
display's own latency.

## What we built

### Off-thread GPU upload

The texture upload runs on a **producer thread** that owns a secondary
OpenGL context, created via `wglCreateContextAttribsARB` with
Resolume's primary context passed as the share-list parent. The
producer issues `glTexSubImage2D` via PBO into one of three textures
in a triple-buffer `SharedTextureRing`. Each upload is fenced with
`glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE)`. Resolume's render thread,
when called via `ProcessOpenGL`, picks the latest ready texture
(`glClientWaitSync` with a zero timeout for the fence), binds it, and
draws — no upload work happens on the render thread.

This is the key win. With on-thread upload, every frame waited in the
queue until Resolume next called us. With off-thread upload, fresh
frames are GPU-resident before the render thread even asks.

### Texture ring depth: three, not two

We settled on a depth-3 ring after observing two effects on a depth-2:
- Tearing window when the producer was writing slot N while the
  consumer was sampling slot N — narrow but visible at 100+ FPS render
  rates.
- A surprising scenario where a render rate just above the producer
  rate produced ramped latency growth as the consumer "lapped" the
  producer.

Depth-3 absorbs both. The third slot is free margin, not added
latency: the consumer always picks the most recent published slot,
never the oldest. We do **not** delay-line frames through the ring.

### Phase-lock discovery: no backpressure

Initial design had the producer wait for the consumer to finish
reading slot N before writing into slot N+1, on the theory that this
would prevent loss. In practice this caused a phase-lock between the
60 Hz producer and the ~100 Hz consumer: producer would wait a
fraction of a vblank for the consumer's read to complete, gradually
falling behind real time. End-to-end age would slowly creep from 5 ms
to 30 ms over several minutes of running.

The fix was to remove producer backpressure entirely. The producer
publishes ASAP, every time. If the consumer is too slow to pick up a
frame, it gets overwritten — and that's exactly what the broadcast
world calls a "drop" anyway. The metric we surface in the overlay
(`drops`) counts these consumer-missed publications, which maps
directly onto AutoCirculate's output-drop concept.

After this change, end-to-end age stabilises around 5–12 ms with
occasional dips to 3–4 ms (the producer publishing right before the
consumer reads). That's already sub-frame on a 60 Hz timeline.

### Ring depth in AutoCirculate is margin, not delay

Related insight, found while implementing the AJA capture path: the
AutoCirculate ring depth parameter (we use 7) is **not** a delay line.
It does not add 6/60 second to the latency of the latest frame. The
ring's purpose is to absorb capture-side slowdowns without losing
frames: if the consumer (us) stalls for 100 ms, AutoCirculate keeps
queueing into ring slots, and we catch up later by reading the most
recent one. The latency of the latest frame from cable to host buffer
is roughly one frame period regardless of ring depth, dominated by
the time AutoCirculate waits to assemble the next complete frame.

Two corrections to that picture, found in the v0.40 latency pass:

- **"We catch up later" was not true before v0.40.** The loop did one
  transfer per interrupt, always the OLDEST ready frame (that is how
  the driver picks), so once behind it stayed behind: one frame in,
  one frame out. The loop now drains the ring after a transfer that
  leaves frames waiting, without waiting for the next interrupt.
- **The plugin kept one frame in hand.** `HasAvailableInputFrame()`
  is `GetBufferLevel() > 1`, i.e. two complete frames; the transfer
  then took the older one. The SDK documents the rule as an audio
  safeguard (the last audio chunk of a frame may not be in memory yet
  at level 1). No audio is captured here, so the plugin transfers at
  level >= 1, the frame completed at the interrupt it woke from, the
  same frame the Low Latency path DMAs.

With both, ring depth really is margin only: the delivered frame is
always the newest complete one.

### De-interlacing lookahead

YADIF (shader mode 1) reconstructs a field using the field after it,
so it shows the field one source frame behind the newest (two fields,
~33 ms at 1080i59.94). Low Latency Mode on interlaced sources selects
shader mode 2 (v0.40): the newest field is shown and its missing rows
come from the field before it, with the same motion test and bracket
clamp. Static detail is woven exactly as in mode 1; in motion both fall
back to the edge-directed spatial prediction. The cost is a temporal
predictor centred half a field earlier (a little more trailing on slow
motion).

### Capture state machine

The AJA capture thread runs from `Start()` to `Stop()` regardless of
input state. Internally it follows a small state machine:

```
                                 cable plugged,
                                 supported format
            +---------+    ok    +----------------+
   Start -> | Idle    |--------->|  Initializing  |
            +---------+          +----------------+
                                  |                |
                          ok      |                |  bad signal
                                  v                v
                            +----------+      +-----------+
                            | Running  |<-----| NoSignal  |
                            +----------+      +-----------+
                                  |  ^               ^
                  cable pulled or |  | signal back   |
                  format changed  |  +---------------+
                                  v
                            +-----------+
                            | NoSignal  |
                            +-----------+
```

`Failed` is a terminal sink reserved for unrecoverable driver errors
(card removed from the bus, kernel claim revoked) — it requires a
plugin reload to leave.

The state is exposed via an atomic `GetState()` that the render
thread reads every frame. When the state is anything other than
`Running`, the render thread paints a neutral grey fill instead of
sampling stale texels and the diagnostic overlay surfaces the cause
in colour.

### CPU affinity

The capture thread runs at `THREAD_PRIORITY_HIGHEST`, registers with
MMCSS as a "Capture" task (v0.40; real-time priority range while it
runs, CPU still reserved for the rest of the system by the service),
and is NOT pinned to a core (v0.39). Up to v0.38 it pinned itself to the
highest-numbered logical core as a stand-in for "performance core".
That heuristic was backwards on Intel hybrid CPUs, where Windows
numbers the P-cores first and the E-cores last, and with two plugin
instances both capture threads competed for the same core. The thread
now opts out of EcoQoS power throttling
(`SetThreadInformation(ThreadPowerThrottling)`, Windows 10 1709+),
which is the documented way to keep it off efficiency-class
scheduling, and otherwise leaves placement to the scheduler.

`TIME_CRITICAL` was explicitly rejected: under sustained load it can
freeze the mouse and audio subsystem with no observed benefit over
`HIGHEST` for a thread that blocks on a hardware interrupt.

## What still to validate against real hardware

These are the unknowns that need a Kona X plugged in to resolve:

- **Vblank interrupt latency.** The capture thread blocks on
  `WaitForInputVerticalInterrupt`. The latency from "frame complete
  in card memory" to "thread wakes" should be sub-millisecond, but
  this is driver-dependent.

- **AutoCirculate transfer overhead.** The DMA itself is fast (PCIe
  Gen3 x4 sustains ~3.5 GB/s; a 1080p UYVY frame is ~4 MB, so ~1 ms),
  but the SDK adds some setup per call. We mitigate the per-transfer
  cost by page-locking the host-side DMA destination buffer once at
  capture-thread start via `CNTV2Card::DMABufferLock`; the driver
  then reuses a single cached scatter-gather mapping across every
  `AutoCirculateTransfer` instead of walking the page table on each
  one. Matches AJA's `NTV2_BUFFER_LOCK` pattern in the NTV2Capture
  demo. Removes a known source of per-frame jitter; the remaining
  setup overhead is small and worth measuring on hardware before any
  further optimisation.

- **Multi-channel exclusivity.** Switching SDI Input mid-show with
  another application also using the card may or may not behave
  cleanly. Untested.

- **Format detection timing.** `GetInputVideoFormat` may take a
  variable number of vblanks to lock onto a new signal. The recovery
  loop's 500 ms retry cadence is a guess that may need tuning.

## Future: racing the beam

The Kona X exposes streaming DMA: instead of waiting for a full frame
interrupt, transfers can begin as scanlines arrive in card memory. If
we upload to GPU in halves (or quarters) gated by progress
notifications, the rendered output can be ready up to half a frame
earlier than naive whole-frame capture — pushing us into the 4 ms
territory that motivated the project.

Risks:
- Partial-transfer API in `libajantv2 18` needs review against the
  SDK source. The documentation is sparse.
- Visible tearing if the upload boundary falls inside the visible
  frame and Resolume's output is not vsync-bypassed. Mitigation:
  use the bottom-up scanline order of UYVY so the partial upload
  always advances from the top of the frame, and document the
  Resolume output configuration required.
- Complexity is high. Only worth pursuing once everything below is
  measured and stable on hardware.

## Decisions made and their rationale

- **Off-thread upload via shared GL context, not separate display
  context.** Plugins that fully own GL tend to deadlock with
  Resolume's own threads. Borrowing the share-list slot is the
  supported escape hatch and has worked reliably across NVIDIA
  drivers 470–596 in practice.
- **Triple-buffer texture ring, not double.** Double-buffer is enough
  for correctness but has measurable tearing artifacts at high render
  rates with 60 Hz producer; triple is free margin.
- **`THREAD_PRIORITY_HIGHEST`, not `TIME_CRITICAL`.** See note above.
- **No producer backpressure.** See phase-lock discussion above.
- **All raster sizes up to and including 4K DCI accepted.** 8K and
  quad-link channel-bonded formats are deferred until the multi-
  channel AutoCirculate setup needed for them lands. The pipeline
  components (`SharedTextureRing`, `TextureUploader`, `FrameQueue`)
  are sized at InitGL from the detected format and have no upper
  bound below the GL implementation's `GL_MAX_TEXTURE_SIZE`.
- **Interlaced accepted in weave mode.** AutoCirculate delivers
  interlaced frames as single full-height buffers with both fields
  woven; the upload and shader paths are unchanged. The visible
  artefact is combing during motion. Proper bob / yadif de-interlace
  is a v0.19 milestone; field-rate processing requires switching
  AutoCirculate into per-field delivery, which is a non-trivial
  change to the capture loop.
- **Full AJA SDK setup discipline at device open and channel
  configure.** The plugin follows the exact pattern AJA's own
  NTV2Capture demo uses: acquire the stream for our process,
  switch to NTV2_OEM_TASKS (saving the previous mode for restore on
  close), enable the FrameStore widget explicitly, subscribe to
  input and output vertical interrupt events (documented as
  critical on Windows so WaitForInputVerticalInterrupt fires
  reliably), wait the recommended 10 VBIs after flipping an SDI
  BNC to receive so the SDI receiver block has time to lock before
  the format probe, and disable VANC capture so the active raster
  is not contaminated by ancillary data. These calls are not
  optional in any realistic capture scenario; the symptom of
  missing any one of them tends to be "works at home, breaks at
  the venue" intermittent failures that are nearly impossible to
  debug without the SDK source in hand. Doing it right at the
  first pass eliminates them.
- **Single-link 12G-SDI 4K via TSI on a single FrameStore.** A 4K
  UHD or DCI signal arriving on a single 12G-SDI cable is routed to
  a single FrameStore exactly like a 1080p signal would be (one
  Connect call between the SDI output crosspoint and the FrameStore
  input crosspoint), and the FrameStore is told to interpret its
  raster as a 4K image via SetTsiFrameEnable(true). This is what
  AJA's NTV2Capture4K demo does for cards with CanDo12gRouting,
  which the Kona X family has across all three SKUs. The Squares
  alternative (Set4kSquaresEnable) is wired up too for cards
  without 12G routing, kept for portability even though no Kona X
  variant takes that path. The practical consequence is that a
  single Kona X 4K capture takes one FrameStore, not four, so a
  Kona X base can still host another simultaneous 1080p capture on
  its second FrameStore while one instance is bringing in 4K.
  Quad-link 4K (4xHD bonded across four SDI BNCs) is not supported
  on Kona X because the card only has two SDI BNCs, and not yet
  supported in this plugin on cards that do have four BNCs because
  the channel-bonding routing is a v0.30 milestone.
- **MultiFormat mode enabled and device reference set to free-run.**
  By default AJA cards run in "UniFormat" mode where every FrameStore
  is locked to the same clock and the same format. That works for
  the simple case of "one input, one output, same rate" but produces
  the classic "second channel refuses to lock" failure when the user
  switches between two SDI inputs running at different formats, or
  when another application on the same card has cached state at a
  different format. Enabling MultiFormat mode at device open gives
  every FrameStore an independent clock derived from its own SDI
  input. Pinning the device reference to free-run (rather than the
  default of slaving to whatever SDI was last configured)
  additionally prevents signal loss on one input from disturbing
  the others. This pair of settings is what separates "switch SDI
  in Resolume mid-show and it just works" from "switch SDI and
  spend twenty minutes power-cycling the card".
- **Physical input source decoupled from FrameStore via the
  on-board signal routing matrix.** Three concepts that AJA hardware
  exposes separately and that earlier versions of this plugin
  conflated: the physical BNC connector (1..4 SDI plus 1 HDMI), the
  NTV2InputSource enum that the format detector and routing matrix
  talk about, and the FrameStore (the internal DMA engine that
  AutoCirculate drives). The Kona X base has 4 BNCs but only 2
  FrameStores; the routing matrix lets any BNC feed any FrameStore.
  TryConfigureChannel issues an explicit `card.Connect()` call to
  wire `GetInputSourceOutputXpt(SDI N) → GetFrameStoreInputXptFromChannel(FS K)`
  before AutoCirculate runs, so SDI3 and SDI4 work just as cleanly
  as SDI1 and SDI2 even though there is no NTV2_CHANNEL3/4 FrameStore
  for them to "natively" map to on this hardware. This is what
  AJA's own NTV2Capture demo does; the historical Resolume native
  integration appears to have skipped this step.
- **FrameStore claims arbitrated by a process-wide pool.** Several
  plugin instances coexisting in the same Resolume composition each
  need their own FrameStore so their AutoCirculate sessions do not
  collide. The pool hands out the lowest-numbered free FrameStore at
  InitGL and reclaims it at DeInitGL. The card's FrameStore count
  (queried via `cap.GetNumFrameStores()`) is the hard ceiling: a
  third instance trying to load against a Kona X base will not get
  a FrameStore and falls back to the synthetic generator with a
  clear log message rather than fighting the second instance for
  the same hardware engine.
- **3G-SDI Level B → Level A auto-conversion enabled on every SDI
  input at device open.** The card supports both transport sub-formats
  of 3G-SDI (a single 3 Gbps stream vs two multiplexed 1.5 Gbps
  sub-streams, both carrying the same 1080p50/59.94/60 raster).
  Upstream gear is inconsistent about which it emits: some cameras
  default to A, some converters to B, some routers translate, some
  pass through. Enabling the hardware-level conversion means the
  rest of the pipeline only ever sees the Level A enum values
  (`NTV2_FORMAT_1080p_*_A`), so the multi-format decoder, the
  AutoCirculate setup, and the AGC state machine cannot get
  confused by the Level B variants. The diagnostic dump surfaces
  whether the cable physically carried A or B.
- **Format detected only at InitGL, not at runtime.** The GL
  resources (texture ring, PBOs, host buffer) are sized once when
  the plugin starts and are NOT reallocated when the SDI input
  switches to a different raster mid-show. The capture state
  machine surfaces this as `NoSignal` so the user knows what
  happened; reloading the plugin (or closing/reopening the
  composition) renegotiates against the new format. Runtime
  reallocation is the v0.16 milestone.
