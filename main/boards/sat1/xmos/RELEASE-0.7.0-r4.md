# Satellite1 0.7.0-r4 release validation

This release updates native Satellite1 only. Voice PE, ReSpeaker XVF3800 and
ESP32-S3-BOX-3 retain their published 0.7.0 device entries, artifact URLs and
SHA-256 values from `native-0.7.0-r3`. The merged manifest remains usable by
both Tater OTA and the USB flasher. The release tag is
`native-satellite1-0.7.0-r4`.

The standard `sat1` build now embeds the tested `aec4_track` XMOS profile,
version 1.2.0-dev.13. Rebuilding after the default-profile changes reproduced
the exact tested DSP factory image. The normal build excludes USB audio capture
and bench instrumentation. See [TRACKING-CLEANUP.md](TRACKING-CLEANUP.md) for
the processing architecture and earlier hardware observations.

## Prepared local artifacts

| Image | Bytes | SHA-256 |
| --- | ---: | --- |
| Native OTA application | 2,820,592 | `397198d91af9dbea0017fb4901a5e9cd66ce0db4952b7b0cfd63545bc75dce5e` |
| Native factory image | 6,097,392 | `2fac99b59ac20ad8ccd66d4bf85a6a5d394f1836194fe86702a1e7bde0441faa` |
| Embedded XMOS factory image | 270,336 | `2af046f6d95372f332cdc48236c74098ce4f8bcf8608efe8c04925122c75865f` |

The native image checksum and validation hash pass esptool inspection. It has
325,136 bytes of headroom in the 3 MiB application partition. The factory image
contains the exact OTA application at 0x320000. These hashes identify the local
qualification build; CI generates manifest checksums from its own build outputs.

## Checks

- Standard `pio run -e sat1`: passed, with the intended profile and exactly one
  matching XMOS payload. Previous Sat1 and other-board payloads are absent.
- All 24 native host regression scripts and three native direction UI tests:
  passed. The 22 audio bench host tests and DoA/beam host tests also passed.
- All 21 pipeline-contract, beam arithmetic and speech-protection policy tests
  passed on their host and XS3A simulator backends, without skipped tests.
- Seven release packaging regressions: passed. They cover preserving every
  unaffected board, USB-flasher manifest selection, local/all-board packaging,
  and rejection of stale native images, wrong DSP payloads, oversized images
  and incomplete/corrupt carry-forward metadata.
- Actual OTA: restored published r3/dev.8, upgraded to r4/dev.13, rolled back
  from r4 to r3/dev.8, then upgraded to r4/dev.13 again. Each transition reached
  the expected native and DSP versions with advancing DSP frames. This verifies
  an explicit OTA recovery path; bootloader automatic rollback is not enabled.
- The user confirmed music, voice commands from both sides and direction LEDs
  working after the requested final full power cycle.
- Additional release-image playback observation: 6,093 DSP frames advanced over
  91.3 seconds of observed music, with no stalls, status-read errors, missing
  counters or increasing playback errors. The extended 30-minute run was stopped
  at the user's request because these checks had already been repeated. It is
  not a completed 30-minute soak.

Qualification is on one Satellite1 with its internal speaker. Far-field
recognition gains, moving-talker accuracy, ERLE, false wakes per hour and stereo
AUX/Both output have not been quantified by these checks. The release does not
claim dedicated dereverberation or parity with commercial smart speakers.
