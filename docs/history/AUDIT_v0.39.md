# Code audit — v0.39

> Development history from before the first public release (0.43), when
> the project was named konax-ffgl; see [README.md](README.md) in this folder.

Full review of the v0.38 code base, done in three independent passes
(capture path against the AJA SDK and driver sources; GL pipeline and
YADIF against FFmpeg/libplacebo; support code, build, tests, docs and
licensing), followed by a review of the resulting diff. Every finding
below was checked against the code and, where the SDK is involved,
against the libajantv2 sources (`ajantv2/src`, `driver/`). Nothing here
was measured on the Kona X yet: v0.39 needs a hardware check.

Severity: **bug** = wrong behaviour today; **risk** = wrong behaviour
under conditions that are plausible but unconfirmed; **cleanup** = no
behaviour change.

## 1. The YADIF combing on motion (open issue from v0.38)

Symptom: with two instances on interlaced SDI, YADIF output combs in
moving areas only, alternating clean / combed between output frames;
static content is clean.

### Mechanism (verified in code; trigger not yet measured)

In field mode each ring publish carries the whole capture accumulator:
the field that just arrived plus the other field's rows from the last
time that field arrived. For a TFF source and source frame N:

| Publish | Even rows (top) | Odd rows (bottom) | Renderer action |
|---|---|---|---|
| F0(N) | F0(N) | F1(N-1) | rotate; copy into new `next` |
| F1(N) | F0(N) | F1(N) | re-copy into the same `next` (completes it) |

The ring is latest-wins. If F1(N) is replaced by F0(N+1) before
Resolume renders, the history slot for N never receives F1(N). It
rotates into `cur` holding F1(N-1). The first-field output of N keeps
F0(N) and is mostly clean; the second-field output keeps the stale
F1(N-1) rows while the interpolated rows are built from F0(N) and
F0(N+1): about 2.5 fields of time between neighbouring rows, so hard
combs exactly where things move, on every other output field. A lost
F0 heals itself (the F1 publish carries both fields), which is why only
one output parity is affected. Two instances make missed publishes
more frequent (more render jitter, more GPU load).

The shader itself matches FFmpeg's CPU YADIF for both fields (sampler
binding, `FieldParity`, `TopFieldFirst`, 1080i / 576i / 480i).

### Fix (v0.39)

`src/FieldHistoryGate.h` holds the decision logic. When the F0 publish
of frame N+1 arrives and the outgoing slot (N) never got its F1, the
publish's odd rows ARE F1(N) (AutoCirculate delivers fields in strict
F0/F1 alternation; the driver's "do not repeat the same field when
dropping" rule in `ntv2autocirc.c` guarantees it), so those rows are
copied into the slot before rotating. The copy is a small shader pass
(`texelFetch` + `discard` on the other row parity). No repair after a
longer gap: then the rows belong to a later frame.

Evidence:
- `tests/test_FieldHistoryGate.cpp` models the whole pipeline
  (accumulator, latest-wins ring, three slots). With the repair off
  (old behaviour) the `cur` slot holds two different frames in about
  2 renders per missed F1; with the repair on, never (8 seeds, 4000
  fields each, 25% missed publishes).
- The copy shader was run on Mesa (llvmpipe, GL 4.5 core): odd/even
  rows copied exactly, other rows untouched, no GL error.

### How to confirm on hardware

1. Two instances, interlaced SDI, motion. Watch the Resolume log line
   `KonaXSource: diag ...`: `ringMissed` and the new `fieldRepairs`.
2. If combing is gone and `fieldRepairs` grows: confirmed.
3. If combing persists:
   - `fieldRepairs` stays 0 while `ringMissed` grows: the misses are
     F0s or longer gaps; report both numbers.
   - Driver pair drops (F1(N) and F0(N+1) dropped together) splice two
     frames; they show as Stats / Drops. Rare.
   - GPU write-after-read across contexts was a third candidate; v0.39
     closes it with read fences (section 2).

## 2. Fixed in v0.39

| # | Area | Sev. | Finding | Fix |
|---|---|---|---|---|
| 1 | YADIF | bug | Missed second-field publish leaves a stale field in history (section 1) | `FieldHistoryGate` + row-parity repair pass |
| 2 | Ring / queue | bug | Index and fresh flag were two atomics; a publish between the consumer's two steps returned an older frame marked fresh (model: 789 of 5005 interleavings; stress: ~1.8% of fresh reads) | One packed atomic per side; concurrent test that fails on the old code |
| 3 | YADIF | bug | `YadifSlots::Advance` accepted older sequences, rotating frames out of order | Reject `<=` last; tests |
| 4 | Ring | risk | GL does not order commands across contexts: the producer could overwrite a slot texture before the renderer's queued blit read it; and the CPU memcpy / DMA into a persistent PBO did not wait for the previous upload that read it | `EndRead` fence after the last read, producer `glWaitSync` before `glTexSubImage2D`; bounded CPU wait on the slot's own upload fence before writing its PBO (`WriterPersistentPtrForWrite`). Checked on Mesa with two shared contexts, memcpy and DMA-style paths |
| 5 | Pixel kind | bug | 10-bit toggle reopened the card at v210 size while the ring stayed UYVY: DMA wrote ~33% past each persistent PBO | One pixel kind for capture, ring, queue, shader; pipeline re-sized on change; sink-size guard in both producers |
| 6 | Capture | bug | v0.38 interrupt-count gate turned wait timeouts into an endless `continue`: cable pull never reached NoSignal | Gate only when the wait returned true |
| 7 | Capture | bug | LL `DMAReadFrame` without FrameStore used the base frame size; wrong memory for quad/TSI 4K | 4-argument overload |
| 8 | Capture | bug | Field mode enabled without the texture ring: half-frames to the screen | Field mode requires the ring |
| 9 | Device | bug | Each instance saved/restored task mode and stream claim; closing one of two instances released the card under the other (`ReleaseStreamForApplication` forces refcount 0) | Process-wide claim registry: first Open takes, last Close restores |
| 10 | Capture | risk | Settings changed during Initializing (>=170 ms) were lost | Configuration generation, checked after configuring and at every Running iteration; CAS state bump |
| 11 | Threads | risk | Every capture thread pinned to the highest logical core (an E-core on Intel hybrid; shared by both instances) | No pinning; EcoQoS opt-out |
| 12 | YADIF | risk | Capture restart reset the source sequence but not the history | Reset when the sequence goes backwards |
| 12b | Pixel kind | risk | Device switch started the capture with the new card's kind, which page-locked the PBOs at the wrong size before the size guard ran | Start with the allocated kind; the next frame re-sizes if needed |
| 13 | GL state | risk | History blits subject to host scissor; texture units 1/2 left bound | Scissor guarded; units unbound; repair pass forces the colour mask (alpha carries Y1) |
| 14 | Overlay | bug | Glyphs `L [ ] ?` missing | Added |
| 15 | Stats | bug | Synthetic off-thread Frames/Drops read the queue | Read the ring |
| 16 | Version | cleanup | Version literal in several places | One constant |
| 17 | Shader | cleanup | Preserved-row test divided a negative int (undefined in GLSL; result happened to be right) | `(yo & 1) == FieldParity` |
| 18 | Docs / build | cleanup | Stale README/ROADMAP/LATENCY_STRATEGY/comments, FFGL licence named MIT (it is BSD-3), missing copyright lines, CMake version 0.1.0, CI only on main with an unpinned FFGL | Updated; CI on all branches, FFGL pinned |

## 3. Not changed in v0.39 — proposals

Rows marked **v0.40** were implemented in v0.40 (see CHANGELOG). v0.40
also found and fixed issues not listed here: the AutoCirculate loop
transferred only at buffer level 2, one frame behind the newest; a
2-slot frame-mode ring drops every other interrupt in the driver; and
interlaced starts treated AutoCirculate's STARTING state as a failure
(see ROADMAP, v0.40 section). Left open from the v0.40 review: the lazy
page lock of the field-mode buffer is keyed on pointer and size only
(a re-allocation at the same address would be missed; not reachable
today), and toggling Low Latency on a field-mode source still re-inits
AutoCirculate although it now only changes the shader.

| Area | Sev. | Finding | Proposal |
|---|---|---|---|
| LL + 2 instances (**v0.40**: AC left in INIT) | bug | After `AutoCirculateStop` the SDK's memory auditor (`SDRAMAuditor::TagVideoFrames`) only marks the current input frame of an enabled FrameStore as used. Another instance's `AutoCirculateInitForInput` can take the LL instance's idle ping-pong slot, so both FrameStores write the same memory. Only progressive sources use LL (interlaced is forced to AC field mode). The SDK's `ntv2llburn` demo has the same pattern. | Keep AutoCirculate in INIT instead of stopping it (the driver ISR only timestamps in INIT, so the frame register is left alone), or pass explicit non-overlapping frame ranges. Needs a hardware test with two LL instances. |
| YADIF | cleanup | `tdiff0` is not halved; FFmpeg's CPU filter uses `FFMAX3(temporal_diff0 >> 1, temporal_diff1, temporal_diff2)` (`vf_yadif.c`). Not halving loosens the temporal clamp. | Revisit after the combing fix is confirmed; changes output slightly. |
| YADIF | cleanup | `spatial_score` lacks FFmpeg's `-1` bias (diagonal must win by 2, not 1) | Same: revisit later, equivalent is `-1.0/255.0`. |
| YADIF | cleanup | Edge rows: clamped `yU`/`yD` land on the row being rebuilt; FFmpeg mirrors to a row of the same field | One row affected. |
| YADIF | note | The local copy of `vf_yadif_cuda.cu` binds prev2/next2 differently from `vf_yadif.c`. The shader follows `vf_yadif.c`. Do not "fix" the shader to the CUDA file. | — |
| YADIF | note | Deinterlace silently draws weave until 3 frames are buffered, when the off-thread path is off, or when history allocation failed | Optional overlay hint. |
| Capture (**v0.40**: logged once) | cleanup | Field mode: `AUTOCIRCULATE_FRAME_FULL` in `acPeerToPeerFlags` would be treated as F1 | Log once and fall back to frame mode. |
| Capture (**v0.40**) | cleanup | Field mode DMAs into `m_transferBuffer`, which is not `DMABufferLock`ed | Lock it like the ring slots. |
| Capture (**v0.40**: drain) | cleanup | One transfer per wake: a backlog never drains; frame-mode interlaced (v210) counts every F1 interrupt as an idle wake and runs a format probe | Drain while `HasAvailableInputFrame`; ignore F1 wakes in frame mode. |
| Capture (**v0.40**: LL warm-up only) | cleanup | NoSignal recovery ignores the raw format code; LL publishes garbage during its first N-1 iterations; explicit `SetTsiFrameEnable` is redundant with `SetVideoFormat` | Small tidy-ups. |
| Overlay (**v0.40**: `[LL]` now meaningful on interlaced; buffer reused) | cleanup | `[LL]` shown even when LL is overridden for interlaced UYVY; `Render` allocates a vector per call | Show the effective path; reuse a member buffer. |
| VideoFormat (**v0.40**) | cleanup | UHD and DCI both named `2160p...`; SD named `486i59.94` / `576i50` while docs say 525i / 625i | Decide naming, add string tests. |
| Build | risk | Windows never compiled in CI (include-order rule, `DeviceCapabilities` in a file that includes FFGL); static CRT keyed on the triplet name; no `-Werror` option; AJA SDK version not checked (CI pins `ntv2_18_0_0`, the review sandbox used 18.1) | Windows CI job with vcpkg `glew:x64-windows-static`; warn on non-static triplet; version check on `AJA_NTV2_SDK_VERSION_MAJOR/MINOR`. |
| Tests (**v0.40**: values printed) | cleanup | `EXPECT_EQ` does not print values; pure helpers still inside `KonaXSource.cpp` (colorimetry, field parity tracker, field scatter, format string, input option mapping) | Move to headers and test, like `FieldHistoryGate`. |
| Licensing | decision | The YADIF shader says it is ported from LGPL-2.1+ code (FFmpeg / libplacebo) while the project is MIT. The CUDA kernel was relicensed to MIT by its sole author (FFmpeg patchwork 12108); `vf_yadif.c` and libplacebo are LGPL-2.1+. What notice applies depends on which source was actually ported. FFGL (BSD-3) requires its notice when the DLL ships. | Owner decision; add a THIRD_PARTY_NOTICES file. Not legal advice. |

Minor notes from the diff review, left as they are: `fieldRepairs`
also counts a repair whose copy was skipped (incomplete FBO, logged
separately); CI runs twice for same-repo pull requests; if the first
instance's stream claim fails, later instances do not retry it.
`ReopenAJADevice` (from a parameter change) and `ApplyPixelKindChange`
(from `ProcessOpenGL`) both restart the capture and assume the host
calls them on the same thread, which is what Resolume does.

## 4. Checked and OK

- Field identity: the driver sets `AUTOCIRCULATE_FRAME_FIELD0/1`; top =
  F0 except 486/514-line rasters, matching `m_tffSource`.
- AutoCirculate starts on F0 and never delivers the same field twice in
  a row; field-mode formats match the driver's `AutoCircCanDoFieldMode`.
- Scatter bounds for 486, 576 and 1080 lines.
- MultiFormat: per-FrameStore format/FBF/TSI writes; balanced
  subscribe/unsubscribe; frame allocation serialised by the SDK.
- v0.38 interrupt counter: per-FrameStore driver counter, safe on
  wraparound, no CPU spin; the "other instance woke my wait" theory is
  not supported by the user-space SDK code (per-card events).
- Shader sampler binding, `FieldParity`, `TopFieldFirst` vs FFmpeg CPU
  YADIF; lost F0 handled by the sequence gate; `YadifSlots` a valid
  permutation; distinct blit FBOs re-attached per blit.
- Producer fence created then flushed; consumer `glWaitSync` in
  Resolume's context; teardown joins the capture thread before the
  ring is released.
- FrameStorePool CAS loop; UYVY/v210 sizes vs `NTV2FormatDescriptor`;
  colour matrices derived from Kr/Kb; ASan/UBSan clean on the test
  suite.
