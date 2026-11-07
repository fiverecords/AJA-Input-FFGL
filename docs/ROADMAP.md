# Roadmap

Where AJA Input stands and what comes next. For what each release
changed see [CHANGELOG.md](../CHANGELOG.md); the design history before
the first public release is in [history](history).

## Now: 0.43 beta

It ran a live show for hours on a KONA X, with two inputs at different
resolutions. To leave beta:

- On the KONA X, what a show does not necessarily exercise: 4K on one
  12G cable, cable pulls and input changes, the No Signal modes, Resize
  in a composition of another aspect, HDR signalling.
- Reports from other cards (KONA 4 / 5, Corvid, Io, KONA HDMI), above
  all for one-format cards and 4K over several FrameStores, which follow
  AJA's demos but have not met hardware.
- Interlaced sources with two clips on full-motion video: an earlier
  build showed combing there; the likely cause was fixed, hardware has
  to confirm it (the log's `fieldRepairs` counter).

## Next

- Windows CI job that builds the release DLL and runs the unit tests,
  so release binaries come from a clean machine.
- Whatever the beta reports bring up.

## Later, if there is a need

- YADIF for 10-bit (v210) capture (bob today).
- Resolume clip Width / Height: check how they interact with Resize.

## Out of scope

- Embedded audio: FFGL gives plugins no way to hand audio to the host.
- AJA video output: a lot of work, and FFGL plugins cannot be used in
  Resolume's Advanced Output.
- GPUDirect RDMA: AJA's Windows driver does not expose it.
- 8K, analog inputs, and network set-up of SMPTE 2110 / 2022 IP cards.
