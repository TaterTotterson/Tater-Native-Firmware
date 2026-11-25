# Satellite1 audio bench

This milestone adds synchronized diagnostic capture, stage telemetry and
backpressure/memory corrections. The hardware results below distinguish short
functional checks from sustained processing and playback qualification. The
default `sat1` target still embeds dev.8; bench images are excluded from release
packaging.

## Build

From the repository root, with the existing XMOS XTC 15.3.1 dependencies:

```sh
SAT1_XMOS_AUDIO_BENCH=1 SAT1_XMOS_PIPELINE=beam_ic SAT1_XMOS_RUN_SIM_TESTS=1 \
  bash main/boards/sat1/xmos/source/build_sat1_fixed_delay.sh
SAT1_XMOS_AUDIO_BENCH=1 SAT1_XMOS_PIPELINE=aec4 SAT1_XMOS_RUN_SIM_TESTS=1 \
  bash main/boards/sat1/xmos/source/build_sat1_fixed_delay.sh
SAT1_XMOS_AUDIO_BENCH=1 SAT1_XMOS_PIPELINE=aec4_beam SAT1_XMOS_RUN_SIM_TESTS=1 \
  bash main/boards/sat1/xmos/source/build_sat1_fixed_delay.sh
python3 main/boards/sat1/xmos/source/scripts/package_candidates.py \
  --bench beam_ic aec4 aec4_beam
pio run -e sat1_audio_bench_beam_ic
pio run -e sat1_audio_bench_aec4
pio run -e sat1_audio_bench_aec4_beam
```

Run firmware builds sequentially because version generation shares a source
header. Each uses its own `build_<profile>_bench` directory. Packaging verifies
the linked identity, CMake bench setting, factory MD5 and compression round trip.
Outputs and provenance go into ignored `xmos/bench_candidates/`; the matching
native host embeds that image and identity so it survives the host's boot-time
XMOS version check.

For a normal image without USB capture or stage telemetry, rebuild and package
that XMOS variant before building its native host:

```sh
SAT1_XMOS_AUDIO_BENCH=0 SAT1_XMOS_PIPELINE=aec4_beam \
  bash main/boards/sat1/xmos/source/build_sat1_fixed_delay.sh
python3 main/boards/sat1/xmos/source/scripts/package_candidates.py aec4_beam
pio run -e sat1_xmos_aec4_beam
```

The native build embeds the packaged candidate; running only `pio` does not
rebuild XMOS source. Normal packaging regenerates the selected tracked
`xmos/candidates/` assets. The default `sat1` target still embeds production
dev.8. Save the exact native image, manifest and factory payload for each
hardware comparison.

Boot normally compares version bytes, not an image hash. When rebuilding a lab
image with the same dev counter, install the normal native image between bench
installs so the version changes and page-verified XMOS programming runs again.
Save those installation logs. In-band markers verify profile/version; the
manifest's factory hash is expected provenance, not a hash read from hardware.

| Profile | Lab identity | Marker profile |
| --- | --- | --- |
| beam_ic | 1.2.0-dev.9 | 1 |
| legacy (optional) | 1.2.0-dev.12 | 2 |
| aec4 | 1.2.0-dev.10 | 3 |
| aec4_beam | 1.2.0-dev.11 | 4 |

These compare complete pipelines, not microphone count alone. `beam_ic` keeps
two independent playback references and enables IC/VNR. The four-mic variants
mix playback references to mono and bypass IC/VNR. Those differences must be
retained in any interpretation; stereo-reference four-mic AEC is later work.

These commands build only. Installing a bench image is a separate hardware
action. Retain a working native recovery/OTA image. After installation, connect
the ESP32 USB port for native logs and the XMOS USB port for audio/CDC. Play
stimuli through Tater's normal I2S route. The bench USB interface is capture
only and cannot replace the playback references with USB audio.

## Record and analyze

```sh
python3 -m venv out/sat1-audio-bench-venv
out/sat1-audio-bench-venv/bin/python -m pip install \
  -r scripts/requirements-sat1-audio-bench.txt
out/sat1-audio-bench-venv/bin/python scripts/sat1_audio_bench.py devices

# Replace 3 with the SATELLITE1 Audio Bench input device index.
out/sat1-audio-bench-venv/bin/python scripts/sat1_audio_bench.py record \
  --device 3 --seconds 30 --profile beam_ic --case double_talk \
  --manifest main/boards/sat1/xmos/bench_candidates/manifest.json \
  --notes 'Record distance, direction, room, playback level and stimulus' \
  --output audio_bench_results/beam_ic/double_talk.wav

# Run alongside recording; choose the XMOS CDC port, not the ESP32 port.
out/sat1-audio-bench-venv/bin/python scripts/sat1_audio_bench.py monitor \
  --port /dev/cu.REPLACE_WITH_XMOS_CDC --seconds 40 \
  --output audio_bench_results/beam_ic/stages.jsonl

python3 scripts/sat1_audio_bench.py analyze \
  audio_bench_results/beam_ic/double_talk.wav
```

Open ESP32 logging before USB capture and wait for boot and XMOS enumeration.
Opening or closing the native ESP32 Serial/JTAG port can reset the board even
when a script does not request a reset. Keep that connection open until audio
and stage captures have finished. A reset invalidates the current USB recording;
record a fresh session after enumeration instead of joining it across resets.
Both USB connectors can power the board. Reconnecting only ESP32 USB is not a
power cycle while XMOS USB remains connected. If the native USB interface
disappears, stop captures, disconnect both cables, and reconnect ESP32 first
for recovery. After flashing the saved application, verify recovery through
Tater status before reopening serial logging.

Files are created without overwriting existing recordings. WAV is 16 kHz PCM32
with twelve channels. **Do not play the complete WAV:** its final four channels
are digital markers. Extract only audio channels for listening. Select native
format, unity capture gain and no host audio enhancements.

On macOS, enable microphone access for the host application before recording
and restart it if requested by the OS. A successfully opened CoreAudio stream
can still deliver only zeros when microphone access is denied. Check host
authorization separately from DSP liveness and USB stage counters.

The bench input endpoint uses asynchronous USB packets. Its callback follows
the DSP queue's fill level with 15, 16 or 17 samples per USB millisecond,
preserving every PCM and marker word. A full capture buffer drains rather than
being discarded. TinyUSB's second packet-size controller is disabled for this
mode; producer overflow, consumer starvation and incomplete FIFO writes count
as USB loss events. The normal audio endpoint retains its existing policy.

| Zero-based channel | Contents |
| --- | --- |
| 0–3 | Raw East/West/North/South, before calibration and beamforming |
| 4–5 | Original left/right I2S playback references, before mono mixing/AEC |
| 6 | Processed AGC audio used for wake and recognition |
| 7 | Noise-suppressed audio before AGC |
| 8 | Low 16 bits of the DSP frame sequence |
| 9–10 | Low/high 16-bit halves of the 100 MHz capture tick |
| 11 | Flags: validity, format version, profile, image dev counter |

Each marker occupies bits 16–31 of its PCM word, surviving PCM24/float32 host
paths. Sequence wraps at 65,536 frames; tick wraps at 2^32. Flags use bits 0/1
for raw/reference validity, bits 2–3 for format version 1, bits 4–6 for profile,
bit 7 reserved, and bits 8–15 for the dev counter. Markers repeat across all
240 samples of their originating processing frame.

Inputs and outputs travel in the same processing frame. Internal AEC/NS/AGC
delays still apply: sample zero is not necessarily the same acoustic instant
in every channel. Align measured impulse responses before waveform comparison.
The analyzer deliberately does not call raw-to-AGC attenuation an ERLE score.

The capture sidecar saves expected firmware, case and host overflows. In-band
identity must match the manifest. Reports reject missing/repeated frames,
malformed markers, absent data, unsupported identities, host overflows and
interrupted recordings. Initial warm-up zeros and partial edge frames are
allowed. Reports include channel levels, constant channels and capture timing;
transport validity is not a speech-quality or recognition pass.

## Diagnose a stall

Separate low-priority tasks report per-tile counters once per second. Audio
callbacks contain no CDC logging calls and USB capture uses zero-timeout
enqueue operations. Entries, completions, active status,
active age, last/max elapsed ticks and completed calls exceeding 15 ms are
reported. Times include scheduling waits, not just CPU execution. Ages wrap
after about 42.95 seconds. Heap/minimum-ever heap and USB drops are reported.

| Stage | Boundary |
| --- | --- |
| 0 | Capture callback, including allocation/input preparation |
| 1 | Speaker-reference queue wait |
| 2 | Microphone driver wait |
| 3 | Raw direction/health/beam processing |
| 4 | AEC and optional post-AEC beam |
| 5–6 | Intertile audio send/receive |
| 7–9 | IC/VNR, noise suppression, AGC |
| 10–11 | ESP32 I2S output, USB packing/enqueue |
| 12–13 | Speaker I2S input, reference enqueue |
| 14 | Fixed diagnostic frame-pool wait |
| 15 | Raw calibration/direction-publication mutex wait |
| 16 | Four-mic direction analysis outside the beam mutex |
| 17 | Post-AEC beam mutex wait |
| 18 | Post-AEC beam processing and output copy |

Compare several snapshots: normal waits may be active. An entered-but-not-ended
AEC differs from advancing AEC with blocked I2S output. CDC itself shares MCU
scheduling/intertile resources; if logs stop too, retain ESP32 logs and collect
a debugger trace. The last printed stage is not proof of a failing instruction.

Bench frames use two fixed buffers on tile 1 and three on tile 0. The input
task waits for a returned buffer when the pool is occupied. Ownership returns
to the pool after intertile transmission or final output; the generic pipeline
must not free these buffers. Compile-time checks bound each profile's frame
size. Bench-only heap budgets are 120 KiB on tile 1 and 144 KiB on tile 0;
normal builds retain their existing heap budgets and allocate DSP frames from
the heap.

Speaker passthrough uses a single generic stage. Input, reference preparation
and output run sequentially with one stereo transport frame in flight. The
former two no-op stages added a queue that could retain two more frames while
the producer and consumer each owned one: up to 23,040 bytes at 48 kHz instead
of 5,760. Reference backpressure now stops the next input allocation. The task
stack covers the larger callback plus the passthrough stage, since the input
and output callbacks do not nest. This change also applies to bench-disabled
builds; it does not change the embedded production firmware asset.

DSP frames use a dedicated audio intertile link in both normal and bench
builds. I2S RPC retains its control link; enabled CDC uses another link and
serializes each client request through receipt of its response. A blocked DSP
send on a shared link can hold
its transmit lock while tile 0 waits for an I2S reply before releasing a frame;
the reply then waits for that same transmit lock. Separate links allow RPC to
complete while the receiving pipeline applies backpressure. USB audio retains
its own context. Both tiles initialize/start the audio link independently of
USB audio or CDC settings. Capture-only bench builds do not start a USB
playback forwarding task.

Raw calibration and post-AEC processing share beam state, but direction
correlation runs outside that mutex. The capture task publishes a completed
direction snapshot under the mutex after analysis. Post-AEC beamforming reads
that snapshot, allowing it to continue during the next direction calculation
without reading a partially updated estimator result. Stage 15 covers both
short raw-side acquisitions per frame; stages 16–18 separate correlation,
post-side lock waiting and beam work from their enclosing boundaries.

Bench capture increases frame size, bandwidth, memory and scheduling load. It can
expose or change a timing fault. Reproduce a fix with instrumentation disabled
before changing a release image.

## Qualification and following milestones

Use identical stimuli, positions, route, supply and playback level per profile.
Record quiet, distant speech, music only, speech over music, moving talker, TV,
and startup/restart cases. Save both logs and Tater transcripts. Count missed
wakes, false wakes per hour, correct commands, interruptions and response time.
Silence and playback-only cases distinguish echo leakage from desired speech.

For restart tests, capture separate sessions after each boot. A USB disconnect
during reset invalidates a continuous recording as expected; assess recovery
from boot logs and a fresh capture instead.

Next: diagnose physical stalls; establish runtime memory/deadline margins;
verify repeated cold/ESP-only restarts and speaker routes; compare against
dev.8. Then evaluate stereo-reference four-mic AEC, adaptive directional
processing during playback, residual-echo suppression and dereverberation,
followed by wake/turn-taking tuning. This milestone does not establish HomePod
parity or resolve issue #129.

Host regression command: `bash main/scripts/test_sat1_audio_bench_host.sh`.

## Software verification, November 25, 2026

- Twenty-two host tests pass, including the actual C snapshot/packing code,
  counter/timer wraparound, corrupt/drop/repeat detection, host overflow,
  exclusive recording files, fixed-pool ownership, audio/RPC link selection in
  both layouts and build modes, and sanitized USB partial/empty-read handling.
  The actual USB callback also preserves samples from a full buffer, reports
  starvation, and passes two thirty-second simulated clocks at ±2,000 ppm
  without a lost or changed byte.
  Actual speaker callbacks are also checked with two/four microphones and
  16/48 kHz transport under address/undefined-behavior sanitizers: allocation
  and transfer stay stereo in every case.
  A concurrent callback test pauses direction analysis midway through writing
  its result: post-AEC processing must finish using the previous coherent
  snapshot, then adopt the newly completed result. The original callbacks
  compile but fail this regression; the revised callbacks pass under sanitizers.
  A second concurrent test exercises the actual CDC client callbacks with eight
  writers, checks full request/response serialization and bounded formatting,
  and fails against the original driver. CDC wiring is also checked on both
  tiles with USB audio and CDC independently enabled/disabled.
  The real speaker and generic pipelines also run together with a blocked
  output callback, at 16 and 48 kHz, under sanitizers. Only one transport frame
  may remain live; after release, all 64 frames must arrive in order and be
  freed. The original two-stage source compiles but fails this backpressure
  regression.
  A separate concurrent regression runs the actual DSP output callback and
  SDK intertile TX implementation with a blocked audio receiver. The I2S reply
  must complete before the receiver releases its frame. Both pipeline layouts
  pass; the original shared-link normal configuration compiles but deadlocks
  and fails the bounded assertion. This reproduces the transport hazard, not
  the exact instruction that caused a physical failure.
- All eleven existing channel-contract tests pass, including four xcore cases.
  Direction/beam, native protocol, recognition routing, updater and playback
  wake-policy regressions pass.
- XMOS voice compatibility passes 24 serial AEC and eight output-VNR frames.
  Four-mic AEC passes its canary/pool/schedule checks and 32 reference/scheduled
  frames. The combined AEC/DoA/calibration/beam simulator passes 48 frames and
  three initializations, with maximum reference ticks of 919,626. These are
  synthetic checks, not physical RTOS deadline measurements.
- All three main bench XMOS profiles and matching ESP32 hosts build. Each host
  fits the existing 3 MiB app partition and embeds exactly its matching compressed
  image, excluding the production dev.8 payload. The default bench-disabled
  XMOS profile also builds. Production assets/version/release selection are
  unchanged.
- The first physical baseline test enumerated the twelve-channel audio and CDC
  interfaces, but stopped producing DSP frames after eight frames. AEC and
  output stages completed those frames; capture entered its ninth call without
  reaching the next reference wait. Tile 1 minimum heap was 5,752 bytes, below
  the roughly 11.5 KiB diagnostic frame allocation. This points to the bench's
  allocation boundary and does not establish the cause of the earlier four-mic
  hardware stall. The fixed pools above replace that per-frame bench allocation.
- A full ESP32 flash backup and the starting XMOS factory partition were saved
  before installation. Only the active ESP32 application slot is rewritten;
  settings and boot selection are retained. Capture quality and restart/stall
  qualification remain separate hardware checks.
- Fixed pools alone reached twelve completed output frames before another
  stall: tile 0 waited in I2S output with all three buffers occupied, and tile 1
  CDC reports were absent. The shared intertile transport permits the lock
  cycle described above. After moving bench DSP traffic to the separate link,
  both tiles continued advancing for over three minutes and output exceeded
  12,600 frames. Minimum-ever heaps were 26,872 bytes on tile 0 and 16,720 bytes
  on tile 1; USB drops remained zero and completed AEC calls never exceeded
  15 ms in those snapshots. This supports the bench transport fix; it does not
  qualify a release build or identify every earlier four-mic fault.
- Before the permission restart, host WAVs were invalid: all samples were zero and
  AVFoundation reported microphone authorization denied. The user enabled
  access, with an application restart required. Synchronized audio validation
  and acoustic comparisons must resume after that restart. The revised
  four-mic images build but have not yet been installed on this device.
- After the permission restart, the host received genuine twelve-channel data.
  Two twenty-second captures each had one zero-filled gap and skipped three
  frame identities near fifteen seconds, while DSP stages kept advancing and
  the old USB event count remained zero. This prompted the packet-sizing and
  buffer-drain changes above; those captures remain invalid for comparison.
- After those changes, the baseline passed thirty seconds of capture: all
  480,000 sample frames had valid markers, with 1,999 complete DSP frames and
  no missing/repeated identities. The mean capture interval was 15.00044 ms,
  with no interval above 30 ms. Raw mic and processed channels contained data;
  playback references were silent during this ambient case. This establishes
  capture continuity, not echo attenuation or recognition quality.
- The user reported spoken answers from both baseline positions; ESP32 logs
  contain two wake detections. Its simultaneous USB voice recording is invalid
  because opening ESP32 logging reset the device. Logging must start first.
- The first four-mic capture stopped after thirteen completed frames. All AEC
  and output calls completed; capture then waited for another reference, while
  the speaker-input counter stayed at thirteen. Tile 1 minimum heap was only
  2,880 bytes. The speaker pipeline allocated four channels from microphone
  configuration while always transferring stereo. Its frame is now explicitly
  stereo (5,760 rather than 11,520 bytes at 48 kHz), with an allocation check.
  This correction also applies when bench instrumentation is disabled.
- With the stereo-buffer correction installed, four-mic AEC passed thirty
  seconds of capture: 480,000 valid sample frames, 1,999 complete DSP frames,
  no missing/repeated identities and no interval above 30 ms. Its mean capture
  interval was 15.00024 ms. This verifies sustained bench processing after the
  initial reference starvation; longer restart and playback testing is still
  required before release qualification.
- The corrected four-mic image also passed the ninety-second manual voice
  recording: all 1,440,000 sample frames validated, including both playback
  references, with no clipped samples. The user reported answers from both
  positions. These few trials establish basic operation, not a recognition
  success rate or improvement over the baseline. The bench-disabled four-mic
  XMOS image also builds; it has not been runtime-qualified on hardware.
- Four-mic AEC plus beamforming passed thirty-second ambient and ninety-second
  voice captures with all sample markers valid and no clipping; the user also
  reported answers from both positions. Both four-mic candidates recorded zero
  USB loss events in these captures. The beam variant's AEC-plus-beam boundary
  recorded twenty completed calls above 15 ms, with a 17.7826 ms maximum, while
  plain four-mic AEC peaked at 8.10243 ms. These elapsed times include scheduling
  and mutex waits. No captured frame identities were lost, but beam timing still
  needs work; do not call this a deadline-margin or release-qualification pass.
  The raw analysis and post-AEC beam paths share a mutex across direction
  analysis, making lock scope a candidate for follow-up measurement.
- During the timing follow-up, the earlier beam image passed thirty seconds of
  music capture and ninety seconds of speech over music; the user reported
  answers from both positions. The latter boundary peaked at 16.94097 ms with
  five completed calls above 15 ms. The snapshot candidate initially passed
  thirty seconds of music capture, peaking at 14.61267 ms with post-beam lock
  waiting below 0.137 ms. It subsequently stalled, and its attempted voice
  capture contained only zeros. The user reported no wake response even in
  quiet. These are not a successful acoustic comparison or a stability pass.
- Two later diagnostic candidates also stalled. The CDC transaction mutex
  alone did not prevent the failure. With the dedicated CDC link, twenty- and
  five-second probes passed before progress stopped near 5,500 frames. The
  soak monitor failed at 96.2 seconds after tile 1 reports had been absent for
  fifteen seconds. Tile 0 remained blocked in I2S output; the last tile 1
  snapshot showed AEC/post-beam processing active. Its completed AEC-plus-beam
  maximum was 15.51785 ms. These snapshots do not identify a failing instruction
  or establish that CDC caused the remaining stall. Task/interrupt liveness,
  assertions, allocation and stack margins still need investigation.
- The original saved ESP32 application was restored after each failed
  candidate. Following the last recovery, Tater reported installed and target
  XMOS dev.8, a completed update, and varying microphone samples/features.
  Experimental changes remain off the device. No fifteen-minute candidate
  stability run has passed; another manual music comparison must wait for one.
- A subsequent comparison reinstalled the exact saved earlier beam image,
  before the timing/CDC changes. No ESP32 serial logger was opened and no music
  was playing. Its first twenty-second probe passed, but the next five-second
  probe contained 80,000 zero sample frames. The run failed at 67.3 seconds;
  output stopped near 6,380 DSP frames including boot time. Tater's microphone
  input was flat at -130, tile 0 waited in I2S output, and tile 1 reports stopped.
  Tile 1 minimum-ever heap fell from 10,512 to 2,792 bytes near the failure;
  AEC-plus-beam peaked at 17.1473 ms. This reproduces the failure without the
  latest changes. It motivates the single-frame speaker path above but does
  not prove an allocation failure. The original dev.8 recovery was verified
  before installing unchanged plain four-mic AEC for the next comparison.
- Unchanged plain four-mic AEC then passed a 900.7-second quiet processing run
  with fifteen periodic USB probes (twenty seconds initially, then five seconds
  each minute). All probes validated. Both tiles kept advancing, no USB drops
  were reported, and tile 1 minimum-ever heap remained 10,512 bytes. AEC peaked
  at 8.00615 ms with no completed calls above 15 ms. This is a quiet stability
  result, not a continuous fifteen-minute waveform or a music/recognition score.
- The revised beam image with single-frame speaker transport also passed
  900.7 seconds and all fifteen periodic probes, with both tiles advancing and
  zero reported USB drops. Tile 1 minimum-ever heap stayed at 23,664 bytes;
  tile 0 minimum was 31,056. AEC-plus-beam peaked at 15.3737 ms, with forty
  completed calls above 15 ms; post-beam work peaked at 7.37192 ms and its mutex
  wait at 0.15242 ms. This supports the backpressure change's stability and
  memory benefit in this run. It does not prove which instruction caused the
  earlier stalls, or establish a full real-time deadline margin. Normal-build
  playback and restart qualification are separate checks.
- The matching bench-disabled beam image then passed a 900-second idle run.
  Its XMOS direction-processing counter advanced by 59,851 frames across
  171 backend status samples, with no observed counter/uptime reset, missing
  progress, or status read errors. Native playback write/render/starvation
  counters stayed zero. Sendspin was inactive throughout: this is not a music
  playback result. Backend snapshots are cached, so the observed frame rate
  includes status-sampling latency rather than measuring each DSP deadline.
- An ESP32-controlled restart using a read-only MAC query plus hardware reset
  retained the same normal beam image. Native uptime reset from 944 to 23
  seconds and the XMOS direction counter restarted; the boot-time version check
  skipped reflashing. A fresh sixty-second observation then passed with 4,038
  additional direction-processing frames, varying microphone/features and no
  observed progress loss or status-read errors. This does not test a cold
  power cycle. Music was not started during this session; sustained playback
  and manual wake/answer checks on the revised normal image remain pending.

- The following music test first stopped after 481.4 seconds because one
  backend status omitted the direction-counter object. Microphone samples
  remained variable and subsequent direction counters advanced normally,
  with Sendspin active and no playback-driver errors. This was an observer
  failure, not demonstrated DSP failure. The observer now preserves the last
  valid counter across omissions and retains its fifteen-second progress
  timeout. The original attempt remains saved as an incomplete observation.
- A fresh music run then failed after 400.2 seconds. The direction counter
  froze at 118,075, microphone samples became constant +23 and features were
  all -128. It then restarted at 169 while ESP32 uptime continued increasing;
  one playback write failure was reported and the stream ended. This is a
  physical processing failure/reset, despite the preceding quiet passes.
  The original dev.8 application was restored and live microphone data verified.
  The subsequent dedicated-audio-link change above removes the known
  normal-build RPC/audio lock cycle. Its later results are recorded below.
- With the dedicated link installed in the normal image, two consecutive
  900-second observation windows passed on the same uninterrupted music
  session. Across 339 status samples, the direction-processing counter advanced
  by 119,857 frames with no observed reset, missing counter sample, status-read
  error, or playback write/render/starvation error. Every observation reported
  music active; the inter-window sample spacing was 2.97 seconds. No wake
  detections occurred during this thirty-minute playback check. These are
  processing/playback stability observations, not per-frame timing or acoustic
  suppression measurements.
- The user then confirmed wake and spoken answers from the nearby and opposite
  positions over audible music. The 92.5-second status observation recorded
  detections increasing from zero to two, continued direction-frame progress,
  and no status-read errors. This establishes basic functionality for the
  requested trials, not a recognition success rate.
- On the dedicated-link image, an ESP32-controlled restart retained XMOS
  dev.3 and skipped reflashing. A sixty-second observation passed with 4,062
  additional direction frames, no missing/read errors or reported driver
  failures. Music was active at every observation after reconnecting. A full
  cold-power-cycle and post-boot playback/wake check were then performed.
- The user disconnected all power, including both USB cables, for five seconds
  and confirmed audible music plus wake/spoken answers after reconnecting.
  Native status reported reset reason `poweron`, retained dev.3, and skipped
  reflashing. The post-cycle observation recorded live microphone/features and
  two wake detections. A separate sixty-second music-required check passed
  with 4,062 additional direction frames and zero missing/read/driver errors
  or observed playback gaps. This is one cold cycle and one ESP32-controlled
  restart of this image, not repeated-reboot reliability qualification.

These results support the fixes in this tested fork. They do not establish
that the exact cause of the upstream FPH issue is identical, quantify an
acoustic improvement, or establish full real-time deadline margin. Earlier
bench measurements still include calls above 15 ms; further timing, movement,
route and repeated-restart qualification remain work before release promotion.

The normal dedicated-link factory SHA-256 is
`c0623bf46d9519f4842822d3aa0ce089739755c60b7dec3c7a023def4ff45192`;
its matching native application SHA-256 is
`80bc3d16be31bf594e819a839ba94c5b5549df45008096cda7b00e746d06eb20`.
This dev.3 image differs from the earlier images with the same version string.
The changes and tests are committed as `9336ac8`; default production assets
and release selection remain unchanged.

| Basic hardware check | beam_ic | aec4 | aec4_beam |
| --- | --- | --- | --- |
| Thirty-second capture continuity | Pass | Pass | Pass |
| Answers from nearby/opposite positions (user report) | Both | Both | Both |
| Ninety-second voice capture continuity | Invalid: ESP logging reset | Pass | Pass |
| Fifteen-minute quiet processing / periodic captures | Not run | Pass | Revised image passed; earlier image failed |
| Fifteen-minute normal build idle processing | Not run | Not run | Revised image passed |
| Thirty-minute normal build music processing | Not run | Not run | Dedicated-link image passed |
| Nearby/opposite wake and answer over music | Not run | Not run | User confirmed; two detections recorded |
| ESP32-controlled restart and sixty-second recovery | Not run | Not run | Dedicated-link image passed |
| Full power cycle, playback/wake and post-boot minute | Not run | Not run | Dedicated-link image passed once |
| Comparative acoustic quality / release qualification | Pending | Pending | Pending |

## Fractional steering timing follow-up — November 25

The fractional-delay filter now calculates its Q8 polynomial coefficients in
32 bits, retains the single 64-bit rational accumulation, and uses a bounded
XS3A `ldivu` operation for the final divide. Saturation, truncation, half-way
rounding, integer delays, steering transitions, microphone gains and health
selection retain the previous sample values. The portable host path retains
the C division. This reduces computation; it does not change the acoustic
filter or add a new enhancement stage.

The differential test retains the original rational filter as a reference.
Host AddressSanitizer/UndefinedBehaviorSanitizer checks passed 10,519 divides,
30,986 interpolation cases and 512 complete frames. XS3A simulation passed
919 divides, 16,650 interpolation cases and 256 complete frames, including
the actual divide instruction. Both exercise all 256 fractional positions,
all microphone masks, full-scale signed samples, saturation, history edges,
short frames, changing directions and direction hold/release. The host uses
more random repetitions to bound cycle-accurate target simulation time.

| Single-thread simulator beam workload | Original arithmetic (ms) | Optimized (ms) |
| --- | --- | --- |
| Integer delay | 0.876 | 0.915 |
| Half-sample steering | 8.451 | 1.775 |
| Quarter-sample steering | 8.548 | 1.779 |
| Changing fractional direction | 8.530 | 1.776 |

These full-range vectors produce identical output samples. The test enforces
a 2.5 ms beam budget and at least a twofold fractional-path speedup. The real
4Y1X AEC + raw direction/calibration + residual beam simulator also passed
48 frames and three initializations, with twelve explicit fractional/transition
frames and a 9.9345 ms maximum against its 12 ms simulator budget. These
single-thread limits do not replace a whole-system hardware deadline check.

The first diagnostic image reached flash progress 100% but failed subsequent
XMOS version/startup verification. Its factory SHA-256 starts `a61f8071983ad9e6`.
The previously validated normal image was restored, and live microphone data
and increasing DSP counters were verified before another attempt.

Diagnostic startup was then changed to omit its unused USB intertile link.
Capture-only USB writes a local stream buffer and does not use that link;
DSP, I2S/control and CDC retain separate links. The SDK's actual streaming
channel establishment was also checked on a two-tile simulator: three
persistent links allow an additional control roundtrip, while four establish
successfully but leave the additional transfer blocked until the cycle limit.
A regression covers both outcomes and the bench initialization layout.
The revised diagnostic image booted and verified successfully on the board.

The revised dev.11 factory SHA-256 is
`724456e6c151f764b0a8a56269eeebd655a40a89b375e0f147c7a9b02e436bf9`;
its native application SHA-256 is
`5abae3ad30cd05f98a7ae557dc4f323842d292a3924c438aba222dd99d178731`.
Over approximately four minutes of stage observations, AEC plus beam completed
16,057 calls with a 9.66906 ms maximum and zero completed calls above 15 ms.
Post-AEC beam processing reached 1.65761 ms maximum, versus 7.37192 ms in the
earlier run; those hardware runs used different live inputs and are not a
controlled acoustic A/B comparison. Beam lock wait peaked at 0.04004 ms.
Minimum heap was 23,760 bytes on tile 1 and 31,064 on tile 0, with zero reported
USB drops. A ninety-second capture passed continuity checks with 5,999 complete
DSP frames. Backend telemetry included valid non-integer steering delays
`[458, 566, 516, 508]`, and a separate 180-second observation recorded 12,111
additional DSP frames with no read errors, missing counters or driver errors.
No music was active and no wake detections were recorded in this window;
the prompted user voice/movement trial has not been confirmed.

The matching normal dev.3 factory SHA-256 is
`bf9271f8ca279c7433db89912422b1681e9c7c4fecee9a9be3764f0491af225a`;
the prepared native application SHA-256 is
`22aa2c5b6dc715b1b3e187b1e4005bd2c370c6838f765a270b934f29995b4607`.
This normal image was installed through the dev.11-to-dev.3 version transition;
native status confirmed completed installation, live microphones and a ready
wake engine. A sixty-second observation passed with 4,038 additional direction
frames, twelve status samples and zero missing/read/driver errors. An ESP32
restart then retained dev.3 and skipped reflashing; a 14.06-second follow-up
recorded 1,009 additional direction frames, changing microphone data and a
ready wake engine. The user then confirmed audible music and wake/spoken
answers from the nearby and opposite positions on this normal image; status
recorded three wake detections since the restart. A subsequent 180-second
music-required observation passed with 34 status samples and 11,500 additional
direction frames. Every sampled status reported music active and changing
microphone samples, with zero read errors, missing counters, observed resets,
or playback write/render/starvation errors. Wake detections stayed at three
during this follow-up. This is a short playback and basic voice check on one
board. The previous thirty-minute music and cold cycle results apply to the
earlier `c0623bf4` image. Default production assets remain unchanged; these
observations do not establish a worst-case guarantee, recognition accuracy,
moving-talker quality, or release qualification.

To reproduce the arithmetic, link-budget and channel-contract checks from the
repository root with pytest and the XMOS tool environment available:

```sh
python -m pytest -q \
  main/boards/sat1/xmos/source/tests/test_aec_4y/test_beamformer_math.py \
  main/boards/sat1/xmos/source/tests/test_aec_4y/test_pipeline_contract.py
```

All fifteen checks passed, as did the 22 audio-bench host checks, the existing
DoA/beamformer host tests, both XMOS builds and both native builds.
