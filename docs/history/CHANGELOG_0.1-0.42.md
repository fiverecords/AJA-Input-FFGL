# Changelog until 0.42

> Development history from before the first public release (0.43), when
> the project was named konax-ffgl; see [README.md](README.md) in this folder.

All notable changes to `konax-ffgl` are documented here. The format
is loosely based on [Keep a Changelog](https://keepachangelog.com/);
each entry highlights user-visible behaviour, not internal refactors.
For implementation notes and design rationale, see `docs/ROADMAP.md`.

## v0.42 — 2026

**Highlights**: works with every AJA card that has an input, not only
the KONA X; shown in Resolume as **AJA Input**; experimental 4K over
several FrameStores on cards without 12G routing; a No Signal option;
a Resize option that keeps the input's aspect in the clip; Colorimetry
Auto follows HDR signalling. Pending hardware check; only the KONA X has
been tested.

### Added
- **Every input of every AJA card.** The Input menu lists the inputs of
  the AJA cards on the machine: SDI 1-8 (Corvid 88), HDMI 1-4 (KONA
  HDMI), and the quad-link inputs below. Without a card it keeps the
  original SDI 1, SDI 2, HDMI. The option values of the earlier menus
  (SDI 1-4 = 0-3, HDMI = 4) keep naming the same inputs, so saved
  compositions load unchanged; a value no input has falls back to
  SDI 1. HDMI 2-4 read their own colour space; SDI 5-8 pair with
  FrameStores 5-8; on a KONA HDMI, HDMI N pairs with FrameStore N.
- **Output-only devices** (T-TAP, T-TAP Pro) are left out of the Device
  menu instead of failing when picked; the Device parameter accepts
  cards 0-15 (was 0-3).
- **HDMI on more cards:** an HDMI input prefers FrameStores that have a
  CSC (KONA XM: 4 FrameStores, 3 CSCs). On cards whose HDMI input only
  has a YCbCr output in AJA's routing tables (KONA LHi, Io XT, Io X3,
  Io 4K, Io Express, Corvid HB-R) an RGB source is taken from that
  output instead of failing to route (untested).
- **Experimental 4K on cards without 12G routing** (KONA 4, KONA 5 with
  its standard firmware, Corvid 44 / 88, Io 4K / 4K Plus, KONA HDMI,
  KONA 3G Quad, Corvid 24), routed as AJA's ntv2capture4k demo does,
  untested on hardware:
  - `SDI 1-4 (4K)` and `SDI 5-8 (4K)` (Corvid 88): a 4K picture on four
    cables. Cards with 425 muxes take two-sample interleave (2SI) into a
    FrameStore pair; KONA 3G Quad and Corvid 24 take square division
    into four FrameStores. The four links must carry the same signal,
    and a source that sends a payload ID must use the interleave the
    card takes; otherwise the Format field says which check failed.
  - 4K HDMI on KONA HDMI (HDMI 1 on FrameStores 1+2, HDMI 2 on 3+4) and
    Io 4K Plus, through 425 muxes (and four CSCs for an RGB source: on
    the KONA HDMI 1, 2, 5, 6 for HDMI 1 and 3, 4, 7, 8 for HDMI 2, never
    the CSC of a FrameStore an HD RGB capture uses). Only HDMI inputs
    whose widget has the quadrant outputs (not the Io 4K's); AJA's demo
    routes HDMI 1 only, HDMI 2 follows the same pattern.
  - A 2SI link with a payload ID reports the 4K picture itself (the SDK
    returns the VPID's 4x format); taken as is. Links without one, and
    square-division links, report 1080p and are mapped as AJA's demo
    does. KONA 3G Quad and Corvid 24 take quad-link up to 30p.
  - FrameStore pairs / fours are claimed, lent and released as a whole;
    a request for them only makes captures holding one of them lend. An
    input that goes from HD to 4K (or back) hands its FrameStores back
    and claims what the new format needs. TSI / quad mode is switched
    off again when such a capture stops or fails to start (the SDK's
    SetVideoFormat for an HD format also does it for its pair / group).
    Another application's capture on the other FrameStores of a set is
    stopped, as AJA's demo does. No quad-link on IP cards.
- **No Signal** parameter: what a clip shows while its input has no
  picture (no signal, starting, waiting for a FrameStore): Black (the
  default; up to v0.41 a near-black grey), Transparent, or Hold Last
  Frame, which keeps the clip's last picture of that input until the
  signal returns (dropped when the input or device changes; a 16-bit
  copy in 16-bit compositions).
- **Resize** parameter: how the picture fills the clip. An FFGL source
  cannot set its own size, so Resolume renders the clip at the
  composition's size and, up to v0.41, a picture of another aspect was
  stretched over it (a 4:3 SD camera on a 16:9 composition, DCI 4K on
  UHD, HD on 16:10). Fit (the default) keeps the input's aspect with
  transparent bars, Fill covers the clip and cuts the overflow, Stretch
  is the old behaviour, Original shows the picture at its own size,
  centred: the names and meanings of Resolume's own clip Resize. SD is
  shown at 4:3. Nothing changes when the input and the composition have
  the same aspect; compositions made with v0.41 open at Fit (set Stretch
  for the old look). No Signal Black blackens the picture's area only.
- **Colorimetry Auto follows HDR signalling:** PQ or HLG signalled in
  the SDI payload ID (SMPTE ST 352) or in the HDMI Dynamic Range and
  Mastering InfoFrame (HDMI 1-2) selects the PQ / HLG mode. The Format
  field adds `| HLG` / `| PQ`; the log says when the signalling changes.

### Changed
- **Shown as "AJA Input"** in Resolume (was "AJA Kona X"). The plugin's
  unique ID stays KNX1 and the DLL keeps its name, so compositions find
  it as before.
- **Cards without multi-format** (KONA 1, KONA LHi, KONA 3G, Corvid 24,
  Io XT, ...) run one video format on all their FrameStores (the SDK
  programs channel 1 whatever channel is passed). An input whose format
  differs from the one another input already runs now waits with
  `card runs one format: another input differs` instead of switching
  the card's format under the first; the frame-rate warning becomes
  `! format clash` on those cards. A second input of the same format
  does not reprogram the card, and no input changes the format while
  another application plays out on the card (`card's format is another
  app's output`).
- **A 4K signal on one cable of a card without 12G routing** is refused
  with a reason. Up to v0.41 it switched quad (squares) mode on for its
  FrameStore, which also copied that FrameStore's format to the other
  three of its group and broke their captures.
- The Format field and the overlay name FrameStore sets (`SDI1-4 FS1+2`);
  the load-time log lists the inputs the plugin can capture on the card
  (`[KonaXSource:caps] inputs: ...`) and surveys all of them.
- A FrameStore running AutoCirculate in TSI or quad mode on a card
  without 12G routing counts its whole pair / group as in use (another
  application's 4K capture).

### Docs
- README rewritten for publication: supported cards (from AJA's SDK
  capability tables), 4K, installing, parameters, troubleshooting,
  building, trademark notice.
- `THIRD_PARTY_NOTICES.md`: the FFGL SDK, libajantv2 and GLEW notices
  that must accompany the DLL, and the open question about the YADIF
  shader's LGPL origin.

### Tests
- 85 unit tests (was 58): input numbers and saved option values,
  FrameStore plans for each card family (KONA HDMI CSCs, HDMI widgets
  without quadrant outputs, IP cards), all-or-nothing FrameStore set
  claims, pairing for every input (CSC preference), the one-format rule,
  targeted FrameStore requests, the quad-link format table including a
  2SI payload ID, and the Resize picture rect (Fit / Fill / Original for
  SD, DCI 4K, 16:10 and portrait clips, centring, no one-pixel bars, the
  GL's viewport limit). The new SDK calls also compile against SDK
  18.0.0, the tag the Windows build uses. The host smoke test checks the
  plugin name and ID, the parameters, the new inputs, an unknown Input
  value, Hold Last Frame, and Resize in a 4:3 clip: transparent bars on
  the right lines, nothing drawn outside the clip or the host's scissor
  box, the host's viewport, scissor and clear colour restored.

## v0.41 — 2026

**Highlights**: one capture per input, shared by every clip that shows
it, as Resolume's own live inputs work; each input on its own
FrameStore; the reason a capture does not run is shown on screen.
Pending hardware check.

### Changed
- **Clips of the same input share one capture.** Up to v0.40 every
  clip was a capture of its own: two clips on SDI1 meant two
  FrameStores routed from one connector (reported as "picture for a
  second, then frozen" and "the other SDI1 clip always says no
  signal"), and the two FrameStores of a Kona X were gone with two
  clips. Now the first clip of an input starts its capture (card,
  FrameStore, capture thread, texture ring) and every other clip of
  that input shows the same frames; the capture closes with its last
  clip. Any number of clips, layers and decks can show SDI1 at once.
- **Capture settings belong to the input**, as in Resolume: Ring
  Depth, Low Latency Mode and 10-bit capture apply to every clip of
  the input; changing them in one clip changes them in the others. The
  other clips report the input's value whenever the host reads it, also
  while they sit in the deck, and clips on screen ask the host to
  refresh the inspector. A clip joining an input takes that input's
  settings. Colorimetry, Deinterlace and Show Stats stay per clip.
- The Format readout says when a capture is shared
  (`1080p50 | SDI1 FS1 | 3 clips`); the overlay status line adds
  `x3`.
- **A clip starts capturing when it is first shown**, not when it is
  loaded. The host applies a clip's saved values only after its
  InitGL, so attaching there put every clip of a composition on the
  default input first, and its saved settings then landed on SDI1's
  capture. Clips that sit in the deck and were never shown hold no
  capture or FrameStore and read `SDI2 idle (starts when shown)`; the
  first trigger of an input that nothing else captures takes a moment
  to start. An Input or Device change on a clip that is not on screen
  likewise takes effect when it is next shown.
- Switching a clip to another input of the same card reuses the open
  card handle instead of closing and reopening the card.

### Fixed
- **Each input uses its own FrameStore** (SDI1 -> FrameStore 1,
  SDI2 -> FrameStore 2), as AJA's own software and the card's default
  routing do. Up to v0.40 the first free FrameStore was taken, so a
  lone SDI2 clip ran on a crossed route (SDI2 -> FrameStore 1), the
  setup reported as "no signal / init flickering, never a format".
  FrameStores the card's routing ROM cannot reach from the input are
  skipped (the ROM is listed in the log). A capture that stays offline
  on a crossed route moves to its own FrameStore once that is free.
- **Inputs whose clips are not on screen lend their FrameStore.** With
  more inputs in use than FrameStores (SDI1, SDI2 and HDMI on a
  two-FrameStore Kona X), an input none of whose clips has been
  rendered for a quarter of a second gives its FrameStore to an input
  that is on screen and needs one, and takes one back when shown again
  (a short grey moment while it restarts). Before, the extra input ran
  the synthetic test bars for the rest of the session; now it waits
  (`HDMI waiting for a free FrameStore`).
- The input connector check of v0.40 refused to switch ANY connector
  in output mode while another application played out on the card
  (e.g. Resolume's AJA output on HDMI blocked SDI2 as an input). It now
  only refuses when that connector's own SDI output carries the other
  application's playout (routing traced back to the FrameStore).
- A restarted capture no longer flashes the last picture it showed
  before it stopped.
- The text readouts are read from a locked snapshot of the capture,
  and the host's GUI and parameter threads reach the capture only under
  the clip's lock: a capture is always destroyed on the render thread
  (where its GL objects can be deleted), never on the thread that
  happened to read the inspector last.
- FrameStore claims are per card: with two cards, card 2's SDI1 no
  longer competes with card 1's SDI1 for "FrameStore 1".
- A capture waiting for a FrameStore no longer logs the other
  application's FrameStores ten times a second.
- Lending no longer mistakes a stalled render thread (an input starting
  up, a host hitch) for "not on screen": an input lends its FrameStore
  only when other clips kept rendering while its own did not.
- A FrameStore the card's routing ROM rules out for an input is never
  taken for it (the capture would loop on "routing failed" while
  holding it). An input the ROM cannot route anywhere says
  `card cannot route SDI2 to a FrameStore` and asks nobody to lend.
- A FrameStore that captured 4K before no longer stays in TSI mode for
  an HD raster (TSI is now switched off for non-4K formats, as AJA's
  demos do).

### Known, to settle on hardware
- On a two-FrameStore Kona X, HDMI takes FrameStore 2 when it is free,
  which is SDI2's own. HDMI first, then SDI2, puts SDI2 on a crossed
  route (SDI2 -> FrameStore 1) for as long as HDMI is on screen. If
  crossed routes turn out to fail on the card, the fix is to move HDMI
  to the other FrameStore.
- Starting an input's capture (first time it is shown) and changing a
  shown clip's Input happen on the render thread: the output pauses
  while the card opens and the pipeline is built. To measure; if it is
  noticeable, the card will be opened at load and an Input change will
  re-target the running capture instead of rebuilding it.

### Added
- **The reason a capture is not running is shown** in the Format
  readout and, with Show Stats on, as a red line: `routing failed`,
  `connector is another app's SDI output`, `AutoCirculate init
  failed`, ... (plain `no signal` when the cable simply has none). The
  readout also says which input and FrameStore the clip uses
  (`1080p50 | SDI1 FS1`), and the overlay status line reads
  `aja running sdi1 fs1`. Retries no longer flicker between the reason
  and `init`.
- Log: `routing ROM: SDI1 -> FS1 FS2; SDI2 -> ...` when an input's
  capture starts, the pairing chosen and why, clips attaching to new or
  shared captures, captures closing, FrameStores lent and taken back.
  FrameStore numbers in the log are 1-based throughout (FrameStore 1 =
  AJA's FS1).
- The overlay font has the full upper-case alphabet and some
  punctuation, so these texts render completely.
- Optional offline FFGL host smoke test (`-DKONAX_BUILD_HOST_SMOKE=ON`,
  Linux, EGL + Mesa): loads the plugin through `plugMain` and checks
  sharing, shared settings, input switching and teardown.

### Tests
- 58 unit cases (FrameStore pairing order, single-FrameStore and
  per-card claims, FrameStore requests) plus the host smoke test
  (also: lazy attach, deck clips, captures destroyed on the render
  thread).

## v0.40 — 2026

**Highlights**: lower latency. Pending hardware check (as is v0.39).

### Changed
- **AutoCirculate delivers the newest frame.** The capture used to
  wait until two captured frames were ready (`HasAvailableInputFrame`)
  and then transfer the older one, always one frame behind. The SDK
  keeps that rule for audio, which this plugin does not capture; it
  now transfers as soon as one frame is complete. One frame less on
  progressive sources without Low Latency Mode, one field less on
  interlaced sources.
- **Low Latency Mode on interlaced sources now does something:** it
  selects a YADIF variant without lookahead that shows the newest
  field instead of the one a source frame behind it (about 33 ms less
  at 1080i59.94). Static detail is identical; slow motion trails a
  little more. With the toggle off, YADIF is unchanged.
- Capture thread registered with Windows MMCSS ("Capture" task) for
  prompt wake-ups under load; falls back silently if unavailable.
- Format names: DCI rasters get a `DCI` prefix (`DCI 2160p24`) and SD
  uses `525i59.94` / `625i50`, as the docs already did.

### Fixed
- Loading the plugin switched EVERY bidirectional SDI connector to
  input while surveying the inputs, which silently cut an SDI output
  another application had on the other connector. The survey is now
  read-only; only the connector the instance captures is switched.
- A connector left (or put back) in output mode read as "no signal"
  forever: the input format was probed before switching the connector
  to receive. Switched first now.
- **Latency creep after a late wake-up.** One late capture-thread
  wake left the capture a frame behind for the rest of the session
  (one frame in, one frame out per interrupt). The loop now drains the
  driver ring; the diagnostic line counts it as `drains=`.
- Field-mode DMA buffer was not page-locked, so the driver pinned it
  on every field; now locked once.
- Low Latency Mode published one frame of stale card memory on every
  start, channel switch and recovery.
- Low Latency Mode with two instances: another instance could be given
  this instance's idle frame buffers by the SDK's allocator (AutoCirculate
  is now left in its INIT state, which keeps the range reserved).
- One-time log line if the driver ever delivers a field-mode transfer
  without a field flag.
- **Ring Depth 2 without Low Latency (the default for progressive
  sources):** in the driver source, a 2-slot AutoCirculate ring finds
  its next slot still full on every other interrupt and drops, so only
  every other frame was delivered. If the Windows driver behaves the
  same, v0.39 and earlier showed half the frame rate at that setting
  with Drops rising steadily. Frame mode now always uses at least 3
  slots (no latency cost).
- A transfer that returns the slot the card is still recording into
  (possible right after a driver drop) is discarded instead of shown;
  counted as `activeSkips=` in the diagnostic line.
- Interlaced sources: about half of all capture starts bounced through
  "no signal" (and counted a recovery) because AutoCirculate waits for
  field 0 before it runs and the plugin treated that STARTING state as
  a failure.

### Added
- **Warning when two inputs on one card use frame rates it cannot mix.**
  AJA multi-format cards accept a different format on each input, but
  only within one frame-rate family (29.97/59.94, 30/60, 25/50, 23.98,
  24; the SDK's `IsMultiFormatCompatible`). Raster size does not
  matter. When the formats on the card clash, the Format readout shows
  `! rate clash: SDI1 1080i50` (the other input), the stats overlay
  adds a red line, and the log says which inputs and formats clash.
- **Sharing the card with other applications.** The plugin now checks,
  before configuring a capture, whether the AJA retail services
  (standard task mode) or another application (for example Resolume's
  own AJA output) is using what it needs, and says so in the Format
  readout, on the stats overlay and in the log:
  - AJA services in control: OEM tasks are re-asserted (also on every
    instance's open again, as before v0.39).
  - A FrameStore playing out for someone else is never taken or
    stopped; the FrameStore claim prefers FrameStores nobody is using.
  - An SDI connector used as somebody's output is not switched to
    input behind their back.
- The log now says why a capture cannot be configured (no signal,
  unsupported or mismatching format, routing or AutoCirculate failure),
  once per distinct reason, instead of cycling between "init" and
  "no signal" silently.
- `build.cmd` / `build.ps1`: one-command Windows build (finds Visual
  Studio and CMake, installs GLEW through vcpkg, builds, runs the unit
  tests, `-Install` copies the DLL into Resolume, `-Setup` clones the
  sibling SDKs on a new machine).

### Tests
- Format name, multi-format compatibility and FrameStore-avoidance
  tests; `EXPECT_EQ` / `EXPECT_NE` failures print both values. 53
  cases.

## v0.39 — 2026

**Highlights**: full code audit. Fix for the most likely cause of the
YADIF combing seen on motion with two instances, plus a set of
capture, GL and threading fixes. Needs a hardware check.

### Fixed
- **YADIF: combing on motion, every other output field.** In field
  mode a history frame only becomes complete when the renderer sees
  the publish of its second field. The texture ring is latest-wins,
  so under render jitter (more likely with two instances) that
  publish could be replaced before Resolume picked it up; the frame
  then kept the previous frame's second field, and every
  second-field output showed those stale rows next to newer
  interpolated rows. Now, when the next frame's first field arrives,
  the missing field is copied from it (it carries that field
  unchanged) before the history rotates. Each repair is counted as
  `fieldRepairs` in the diagnostic log line. Decision logic extracted
  to `FieldHistoryGate.h` with unit tests, including a model of the
  whole field pipeline that reproduces the old bug.
- **Frame handoff could return an older frame marked as fresh.** The
  ready-slot index and the fresh flag were two separate atomics in
  both the texture ring and the byte queue; a publish landing between
  the consumer's two steps handed the next acquire an older slot. Now
  one atomic exchange per side. Covered by a new concurrent test that
  fails on the old code.
- **Texture ring: renderer reads and producer writes are now fenced
  in both directions.** The producer waits (GPU side) for the
  renderer's last read of a slot before overwriting it; GL does not
  order commands across contexts, so under GPU load a history blit
  could previously copy a later field. The CPU copy / DMA into a
  persistent PBO also waits (bounded) for the previous upload that
  read it.
- **Toggling 10-bit capture live overran the ring buffers.** The
  toggle reopened the card at the new frame size while the GL ring
  stayed sized for 8-bit. The pixel kind is now one value shared by
  the capture, the ring, the queue and the shader, changes are
  applied by re-sizing the whole pipeline, and both producers refuse
  to publish into a mis-sized sink.
- **Pulling the cable left the instance on a frozen frame** (v0.38
  regression on the AutoCirculate path). Wait timeouts now reach the
  no-signal handling again.
- **Low Latency mode at 4K read the wrong card memory**: DMA now uses
  the FrameStore's own frame size.
- **Field mode without the off-thread ring** sent half-frames to the
  screen; field mode now requires the ring.
- **Closing one of two instances on the same card** released the
  card's stream claim and restored the task mode while the other was
  still capturing. One process-wide claim per card now.
- **Settings changed while the capture was initialising** (input,
  ring depth, Low Latency) were silently dropped.
- **YADIF history after a capture restart** mixed in frames from the
  previous session for a few renders; it is now reset.
- Overlay glyphs `L`, `[`, `]`, `?` were missing.
- Synthetic mode Frames/Drops stats read the wrong counters.
- History blits are no longer affected by a host scissor state;
  texture units 1/2 are unbound after drawing.

### Changed
- The capture thread is no longer pinned to the highest logical core
  (on Intel hybrid CPUs that is an E-core, and two instances shared
  it); it opts out of EcoQoS throttling instead.
- Plugin version is a single constant used by the FFGL info and the
  overlay.
- YADIF history: older sequences are rejected by `YadifSlots`.
- Docs: README parameters, licence and status; ROADMAP status;
  LATENCY_STRATEGY affinity section; new `docs/AUDIT_v0.39.md`.

## v0.38 — 2026

**Highlights**: fix for noisy multi-instance stats on Windows.

### Fixed
- **Inspector `Drops` and `Idle Wakeups` no longer inflate on the
  second instance when two captures run on the same card.** Cause
  was a Windows-specific quirk in the AJA SDK documented in
  `ntv2card.h:2976`: the vertical-interrupt wait uses an event
  handle whose semantics differ from Linux/macOS. With one instance
  the wait works correctly; with two, an event signalled by
  instance A's vsync can wake up instance B's wait too, leaving
  instance B to see `!HasAvailableInputFrame()` on its channel.
  Previously this counted as an idle wakeup (and could also lead
  to driver-drop counter inflation through AC timing jitter).
  Fix: per the SDK's documented workaround, read the per-channel
  interrupt count before and after the wait; if it didn't advance,
  the wake was spurious — retry without counting anything. Applied
  to both the AutoCirculate vsync wait and the Low-Latency-mode
  field-ID wait. Linux/macOS unaffected (the event is cleared
  before each wait on those platforms, so the gate never triggers).

## v0.37 — 2026

**Highlights**: Low Latency toggle now Just Does The Right Thing for
interlaced sources. Stats overlay tidied for publication.

### Changed
- **Low Latency Mode + interlaced UYVY: toggle is now internally a
  no-op.** Both `LL ON` and `LL OFF` route through the same AutoCirculate
  field-mode capture path, which publishes at field rate (60 Hz for
  1080i59.94) and matches Resolume's compositor cadence. The previous
  behaviour routed `LL ON` through the manual frame-rate LL path,
  which was structurally ~3 display frames slower on interlaced
  content because of the 30 Hz publish vs 60 Hz compose mismatch.
  Validated against DeckLink Quad 2 reference: identical latency with
  toggle ON or OFF, both at −1 display frame vs DeckLink. The toggle
  still works as expected on progressive sources.
- Stats overlay version label updated to `v0.37`.

### Removed
- **Removed five debug controls from the inspector** that were used
  during bring-up and no longer add value to a published plugin:
  `Brightness`, `Show Overlay`, `Sim Jitter`, `Consumer Delay`,
  `Inline Upload`. Compositions saved with older versions will
  silently ignore those parameter values on load.
- Stats overlay no longer displays the `avg` and `max` upload-latency
  lines: those measured render-thread inline upload latency, which is
  always 0 since off-thread upload became the default path.

### Added
- 7 new unit tests for `SyntheticFrameGenerator` covering frame size,
  BT.709 limited-range black, SMPTE bars luma monotonicity, UYVY byte
  order. Total test count: 35.

## v0.35 — 2026

**Highlights**: motion-adaptive de-interlace replaces field-separation
bob on UYVY. Significantly better edge preservation on text, graphics,
and broadcast-style content vs the v0.21 bob fallback.

### Added
- **YADIF de-interlacer** (motion-adaptive) on UYVY 8-bit sources.
  Combines an edge-directed spatial predictor with a temporal predictor
  using previous / current / next interlaced frames. Replaces the v0.21
  field-separation bob shader for UYVY; V210 still uses bob pending
  v0.36.
- New CPU-testable YADIF slot-rotation state machine with 9 unit tests
  covering buffer fill, sequence dedupe, advance-after-duplicate
  reset semantics.

## v0.33 — 2026

### Added
- **Ring Depth parameter now applies to Low Latency Mode.** Previously
  LL mode used a fixed 2-slot rotation; the dropdown is now honoured
  in both modes (clamped 2..3 internally for LL).

## v0.32 — 2026

**Highlights**: half-frame latency reduction on interlaced UYVY through
per-field DMA delivery.

### Added
- **Per-field DMA capture for interlaced UYVY** via AutoCirculate's
  `AUTOCIRCULATE_WITH_FIELDS` mode. Each AutoCirculateTransfer delivers
  one field's worth of contiguous pixel data, which the host scatters
  into a full-frame buffer at every-other-line layout. The YADIF
  render path picks up the freshly-arrived field earlier than under
  full-frame DMA, shaving ~1 display frame off the end-to-end latency
  on interlaced sources (validated on hardware).

## v0.29.2 — 2026 (EXPERIMENTAL)

### Added
- **Opt-in 10-bit packed v210 capture** (`10-bit (v210)` toggle).
  Switches the FrameStore to `NTV2_FBF_10BIT_YCBCR` and the render
  shader to a packed-10-bit unpack. Silently falls back to 8-bit UYVY
  if the card reports `canDoFBF10Bit=no`. Triggers a device reopen.
  Note: V210 currently uses simple field-separation bob (not YADIF)
  on interlaced sources; YADIF for V210 is planned for v0.36.

## v0.29.1 — 2026

### Added
- **HDR transfer-function support: PQ (ST.2084) and HLG**. The render
  shader detects the encoding via colorimetry-aware paths and applies
  the correct inverse OETF, tone-maps from the source's nominal
  brightness range to SDR for compositing, and converts from BT.2020
  to BT.709 gamut with hue-preserving out-of-gamut clamping.

## v0.28 — 2026

### Added
- **Regression test suite for the synthetic path** (28 tests covering
  FrameQueue, FrameStorePool, VideoFormat, YadifSlots).
- Linux CI build target.

## v0.27 — 2026

### Added
- **Visual status badge in the inspector** — coloured indicator showing
  capture state (running / no signal / initializing / fault) at a
  glance, no need to open the stats overlay.

## v0.26 — 2026

### Added
- **Live capture stats in the inspector**: frames captured, drops,
  recoveries, idle wakeups exposed as read-only parameters that the
  host can show in the parameter strip.

## v0.25 — 2026

### Added
- Verbose load-time diagnostic logging: card identity, framestore
  count, supported pixel formats, input crosspoint routing, HDMI
  input colour-space detection. Surfaces config issues at plugin
  load rather than during first frame transfer.

## v0.24 — 2026

### Changed
- Capture loop's `Running` state refactored for cleanliness: split the
  hot-path transfer into a dedicated function, made the no-signal
  retry cadence configurable, separated AC status query from the
  transfer call. No user-visible change; cleaner code for future
  hardware bring-up work.

## v0.23 — 2026

### Added
- **`Format` text parameter in the inspector** — live readout of the
  detected raster string (`1080p59.94`, `no signal`, etc.), updated
  on every input format change without needing the on-image overlay.

## v0.22 — 2026

### Added
- **`Low Latency Mode` opt-in toggle**. Switches the AJA capture path
  from AutoCirculate to a manual ping-pong / cycle rotation modelled
  after the SDK's `ntv2llburn` demo. Drops end-to-end latency from
  ~1 frame variable to ~1 frame deterministic on progressive sources.
  Trade-off: less tolerance to consumer stalls; the user picks the
  trade per composition.

## v0.21 — 2026

### Added
- **`Deinterlace` toggle (field-separation bob)** in the render shader.
  When the source is interlaced and the toggle is on, the shader picks
  one field per macropixel row and produces a progressive frame at the
  field rate. Force-clamped to weave on progressive sources. Superseded
  on UYVY by YADIF in v0.35 (bob still used on V210 pending v0.36).
