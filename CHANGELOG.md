# Changelog

All notable changes to AJA Input are documented here. The format is
loosely based on [Keep a Changelog](https://keepachangelog.com/).
Versions before 0.43 were not published; their history is kept in
[docs/history](docs/history).

## 0.43.0-beta — 2026-11-06

First public release. Tested on a KONA X, where it ran a live show for
hours with two inputs; a beta because the other AJA cards are untested
on hardware (see the README's Status).

### Features
- Live input from AJA capture cards with SDI or HDMI inputs (KONA,
  Corvid, Io): SDI 1-8, HDMI 1-4 and, on cards without 12G routing,
  experimental quad-link 4K (`SDI 1-4 (4K)`, `SDI 5-8 (4K)`) and 4K HDMI
  (KONA HDMI, Io 4K Plus). The Input menu lists the inputs of the cards
  installed; output-only devices are left out of the Device menu.
- Low latency: the capture thread wakes on the card's interrupt and
  transfers the newest complete frame into page-locked, persistently
  mapped buffers; the GPU upload runs off Resolume's render thread. An
  optional Low Latency Mode drives the frame buffers by hand.
- One capture per input, shared by every clip that shows it; when a card
  has fewer FrameStores than inputs in use, inputs that are not on screen
  lend theirs to the ones that are. Cards that run one video format on
  all inputs, and other applications playing out on the card, are
  respected instead of reprogrammed.
- YADIF de-interlacing, field by field (bob in 10-bit mode).
- Colorimetry: BT.601 / BT.709 limited or full, PQ and HLG tone-mapped to
  SDR. Auto follows the source's signalling (SDI payload ID, HDMI HDR
  InfoFrame).
- Resize: Fit (default), Fill, Stretch or Original, so a picture whose
  aspect differs from the composition's is not deformed.
- No Signal: Black, Transparent or Hold Last Frame.
- Format readout in the inspector (format, input, FrameStores, clips
  sharing the input, or why there is no picture) and an optional stats
  overlay.
- 10-bit 4:2:2 capture (experimental).

### Stability promises from this release on
- The plugin ID (`AJIN`), the DLL name (`AJAInput.dll`), the parameter
  order and the Input option values stay as they are, so compositions
  saved with this version keep opening with later ones.

### Known limitations
- Windows only. No embedded audio: FFGL has no way to hand audio to the
  host.
- Untested on hardware: every card but the KONA X, and 4K over several
  FrameStores. On the KONA X, Resize in a composition of another aspect,
  No Signal and HDR signalling have only been checked in the test host.
