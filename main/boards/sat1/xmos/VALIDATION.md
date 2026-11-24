# Sat1 XMOS integration validation

Software validated locally on November 24, 2026. Subsequent physical testing
found a DSP stall in both four-mic AEC candidates; their passing simulator tests
do not qualify them for use. The migrated legacy topology passed a basic voice
check. See [HARDWARE-VALIDATION.md](HARDWARE-VALIDATION.md) for the measured results.
No release or upstream PR was published.

## Automated checks

| Check | Result |
| --- | --- |
| SPI 0x10/0x11 framing, acknowledgments, errors, retries and size guards | Pass |
| Direction estimator and beamformer host regressions | Pass |
| Raw calibration remains unchanged when processing cleaned silence | Pass |
| Native wake gain and all 65,536 legacy speech samples | Pass |
| XMOS updater failures, repeated/concurrent calls and cleanup | Pass |
| Direction activity/memory and playback wake policy | Pass |
| Version parser tests | 11 passed |
| Channel/frame contract tests, including four XTC simulator cases | 11 passed |
| fwk_voice 0.9.0 compatibility | 24 serial AEC frames, pool/reset checks and 8 output-VNR frames passed |
| Four-mic AEC schedule and allocation checks | 32 serial-reference/scheduled frames passed |
| Real four-mic AEC plus raw DoA/calibration and residual beam | 48 frames and three initializations passed; output remained live |
| Linked XMOS firmware identity, factory MD5, compression and native embedded payload | Pass for each candidate |
| Raw four-channel USB lab firmware | Compiles and links; enumeration not tested |
| Production 1.1.1 compressed image | SHA-256 unchanged |

The combined DSP simulation measured a maximum of **919,626 reference timer
ticks** across its tested frames. This is a synthetic single-thread DSP result;
it does not establish the physical RTOS scheduling budget or I2S reliability.

All twelve external dependency roots match `submodules.lock` and have no tracked
local modifications. Imported build/test code retains upstream notices.

## Built candidates

Each native application fits the 3 MiB application partition. XMOS linker
constraints passed; runtime stack/heap and audio deadlines still require bench
validation.

| Native environment | XMOS version | XMOS factory bytes | ESP app bytes | Build |
| --- | --- | --- | --- | --- |
| `sat1_xmos_aec4` | 1.2.0-dev.2 | 245,760 | 2,807,216 | Pass |
| `sat1_xmos_aec4_beam` | 1.2.0-dev.3 | 245,760 | 2,808,336 | Pass |
| `sat1_xmos_legacy` | 1.2.0-dev.1 | 241,664 | 2,804,016 | Pass |

The packager reads the DFU version bytes from the linked ELF symbol, rather
than trusting the last generated source header. Each ESP application was checked
for the exact expected compressed XMOS payload, and each merged factory image
was checked for its application at 0x320000. Full checksums and captured logs
are in `release_assets/sat1-xmos-integration/`.

See [INTEGRATION.md](INTEGRATION.md) for installation, rollback, the physical
test matrix and the planned upstream contribution split. Audio quality,
restarts, power cycles, music/barge-in, AUX stereo, and USB enumeration are
**pending**. The four-mic startup/liveness check failed on physical hardware.
Passing simulation does not establish that FPH issue #129 is fixed.
