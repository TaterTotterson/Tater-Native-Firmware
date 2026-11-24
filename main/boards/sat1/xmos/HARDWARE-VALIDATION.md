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
That change was reverted. The follow-up below removes this unused diagnostic
request; it does not establish compatibility of the separate direct GPIO API.

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
topology. The diagnostic cleanup and its limited hardware checks are recorded
below.

The final rollback uses the published `native-0.7.0` Satellite1 OTA application,
matching the native version observed before testing. Its SHA-256 is
`d11f0528fe3b92ac8862a0d90b79c6290b40b40df22a528783b469f809e65f48`,
verified against GitHub's release asset digest. The application contains the
unchanged production XMOS 1.1.1 payload. Local native trial builds are not the
final installed image for that initial evaluation. Published native 0.7.0 / XMOS
1.1.1 was restored; microphone
liveness, playback start/stop and continued connection were verified afterward.

## Working dev.1 follow-up and GPIO diagnostic cleanup

The original dev.1 native image was subsequently reinstalled at the user's
request. The user confirmed clear voice replies and normal operation during
additional use. The dev.2/dev.3 failures remain unresolved.

The native button reader now omits its redundant resource-211 direct GPIO
request. That request only supplied diagnostic logging; the returned button
state already came from the status register. The status-register button path,
volume/mute handling, LED controls, and XMOS audio image are unchanged.

The protocol 0x10/0x11 host checks passed. Both `sat1` and `sat1_xmos_legacy`
native builds passed. The cleanup's legacy OTA application is 2,803,888 bytes,
with SHA-256
`9809b4e82c412a97f01e5829341bab1c0f218cc5985d91f1cc26164a87571dd5`.
Its embedded XMOS compressed payload is byte-for-byte identical to the dev.1
candidate (SHA-256
`efbfcf5f3c99f5624f854fbeca8bac1a7552608c43e4a482f413493f4f31c239`).

After installing this native cleanup over OTA, a new connection and reset
uptime confirmed the reboot. XMOS reported installed/target `1.2.0-dev.1` and
skipped reflashing because that image was already installed. Three fresh status
snapshots showed the DoA frame counter advancing from 576 to 1,249, increasing
processed microphone samples with varying min/max values, a ready wake engine,
and all four microphones healthy. The subsequent frame counter was 3,620.

Device logs confirmed volume 35% → 40% → 35%, microphones muted then unmuted,
successful status-register reads, and a local wake detection. No `direct=`
diagnostic appeared after reboot. The user confirmed that volume controls,
mute lights, and the wake-and-time voice response worked normally.

The cleanup image remains installed on the test Sat1. This is a limited
functional check, not completion of the wider qualification listed above.
No release was published. Private artifact records, status snapshots, and logs
are retained in the ignored
`release_assets/sat1-xmos-integration/hardware/2026-11-24-gpio-cleanup/` directory.

## Native release target 0.7.0-r1

The standard `sat1` target now selects the same dev.1 XMOS payload. Only the
Satellite1 native version advances to `native-satellite1-0.7.0-r1`; the other
three board versions remain 0.7.0. The actual Tater version comparator was
checked against these four headers: it offers an update only to Satellite1.
The combined manifest and all eight OTA/factory artifact checksums passed.
All four standard native builds and the complete host regression set passed.

The standard Sat1 OTA application is 2,804,048 bytes, SHA-256
`d3fd81854ff1dbe5df8f4ba7455efa6a9651bc91c6259d555762c80278743818`.
Its compressed XMOS payload is identical to dev.1, and neither failed AEC4
payload is embedded. After OTA, the board reported the new native identity,
a fresh connection/uptime, and unchanged XMOS `1.2.0-dev.1`. Three fresh
snapshots showed the frame counter advancing from 571 to 1,244, changing
processed mic samples, four healthy mics, and a ready wake engine without a
coredump. The first local HTTP attempt failed to connect before the temporary
server was ready; retrying delivered the verified application successfully.

This standard release image remains installed. These checks retain the same
limits as the earlier functional checks; no additional acoustic or repeated
cold-boot qualification is claimed. Local records are in the ignored
`release_assets/sat1-r1-checks/` directory.
