# Sat1 hardware evaluation — November 24, 2026

**The combined 1.2.0-dev.3 candidate failed. Do not promote it.**

Tested one Satellite1 paired to Tater over Wi-Fi.
Each test used its existing setup, internal speaker, and the same power supply.
Installation used Tater's existing OTA connection and the native image's
embedded XMOS updater. Setup data was retained. No USB factory erase, release,
or upstream PR was performed.

## Observed results

| XMOS image | Result | Evidence |
| --- | --- | --- |
| Production 1.1.1, initial baseline | Basic liveness and playback pass | Changing processed mic samples; DoA frame counter advanced through an eight-second 440 Hz tone and afterward; four healthy mics |
| 1.2.0-dev.3, AEC4 plus residual beam | **Fail: DSP stalls** | Correct installed identity; DoA frame counter remained at 12; wake samples repeated -3056, with min = max = -3056, through fresh status updates and a playback request |
| 1.2.0-dev.2, AEC4 residual average | **Fail: DSP stalls** | Correct installed identity; frame counter remained at 196; wake samples repeated +261, with min = max = +261 |
| 1.2.0-dev.1, original topology plus new library/protocol | Basic liveness and voice pass | Frame counter advances; processed mic values change; local wake detections and audio reach Tater; user confirmed the answer to “Hey Tater, what time is it?” was clear |
| Published native 0.7.0 / XMOS 1.1.1, final rollback | Audio liveness and playback pass; telemetry discrepancies remain | Fourteen status snapshots; frame counter progressed from 2,202 to 6,914; processed samples remained live during and after a tone; subsequent frame counter 9,945 |

The ESP continued receiving I2S samples and reporting increasing
`samples_seen` after the four-mic DSP stopped. A nonzero RMS and a successful
SPI version read therefore do **not** establish a live microphone pipeline.
Four healthy mic flags likewise did not establish downstream DSP liveness.

Both failed candidates include FPH PR #132's compact frame changes. This test
does not establish that their failure has the same root cause as FPH issue
#129, nor does it isolate AEC computation, scheduling, queueing, allocation, or
stack usage. Disabling the post-AEC beam was insufficient to resolve it.

The legacy comparison also reports `direct=fail` in its optional direct GPIO
read diagnostics while the status-register button read succeeds. The new
exchange helper strips handler status, and the GPIO servicer declares one data
byte. A trial changing the caller from two data bytes to one passed the host
framing test but lost DoA telemetry on production XMOS after an ESP restart.
That change was reverted. Direct GPIO/control compatibility needs further
hardware investigation before the migrated native reader is promoted.

## Tested artifact identities

| Candidate | Native application SHA-256 | XMOS factory MD5 |
| --- | --- | --- |
| dev.1 | `d67dd55a2f7d357d72524774e5b30e231ee648f7733b158796c5cf7c750e06b3` | `6ccfbce2e189d7b3b0f5cc1d2dec0c0d` |
| dev.2 | `508bde701c0b9e378e771d4a440b9978a3ed6a31d2f5afec955bd89ca6895442` | `ef3f451ab0169de190b757db48551142` |
| dev.3 | `68a6b01e1f230dde44a69ee7994c02b3890880801c0c636a6dae6db8805b861b` | `0cd2790acd4871e720d75b8d95a2e31e` |

Local status timelines, OTA commands and server logs are retained in the ignored
`release_assets/sat1-xmos-integration/hardware/2026-11-24-sat1/` directory.
The final production check included two transient DoA telemetry discrepancies:
one state read had zero energy/frame counter and another diagnostic read had
zero mic mask/gains. Subsequent snapshots recovered while processed mic audio
remained live. Its strict assertion requiring a healthy mic mask in every
snapshot failed. These recovered telemetry reads are distinct from the
sustained frozen counters and constant processed samples in dev.2/dev.3.

No acoustic improvement, echo suppression, long-duration playback, repeated
cold boot, AUX stereo, or fault-recovery qualification is claimed.

## Next investigation

Measure per-stage progress, worst-case processing time, free heap and stack
headroom on the physical RTOS pipeline. Resolve the four-mic stall and repeat
the same liveness/playback check before acoustic comparisons or upstream PRs.
Qualify the newer library and protocol independently using the working legacy
topology, including the direct GPIO read discrepancy.

The final rollback uses the published `native-0.7.0` Satellite1 OTA application,
matching the native version observed before testing. Its SHA-256 is
`d11f0528fe3b92ac8862a0d90b79c6290b40b40df22a528783b469f809e65f48`,
verified against GitHub's release asset digest. The application contains the
unchanged production XMOS 1.1.1 payload. Local native trial builds are not the
final installed image. Published native 0.7.0 / XMOS 1.1.1 is restored; microphone
liveness, playback start/stop and continued connection were verified afterward.
