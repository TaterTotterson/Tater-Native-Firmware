# Four-microphone speech tracking and cleanup

`aec4_track` is the default profile for the release `sat1` environment in
native **0.7.0-r4**. It has passed the functional checks below;
recognition accuracy and acoustic gains have not been quantified.

The normal image is dev.13 (`sat1_xmos_aec4_track`); the capture image is dev.14
(`sat1_audio_bench_aec4_track`, marker profile 5). Version changes ensure that
the native host programs the intended DSP image when switching profiles.

## Processing

Tile 1 retains raw microphone health/gain calibration and 4Y1X AEC. Playback
references are still mixed to mono for AEC. Each frame carries its raw
calibration to tile 0; cleaned microphone levels never retrain that calibration.

Tile 0's first processing stage owns VNR inference, lagged reference coherence,
post-AEC direction estimation and fractional-delay beamforming. The tracker
requires speech evidence, smooths movements according to confidence and checks
large direction changes across successive frames. During playback, a strong
echo indication prevents steering toward the speaker. A native voice-session
hint allows tracking a confirmed utterance. Low-confidence input returns to a
wide beam. VNR alone cannot distinguish a nearby person from recorded vocals.

The beam also produces a blocking reference: the first healthy aligned mic
minus the target beam, scaled by one half. An aligned target cancels from this
reference. The next stage runs IC and adapts only on confident nonspeech while
steering is settled. It consumes the current frame's speech evidence and never
runs the shared inference engine concurrently with stage 1.

The IC bypass matches the library's 600-sample primary delay plus 32 samples
of WOLA delay. During speech, the cancellation estimate is limited to one
quarter of the primary's frame energy norm. Steering changes bypass IC briefly.
High reference coherence without speech permits gradual attenuation down to
one half; speech immediately releases that attenuation. This broadband gain
alone is not an SNR or ERLE improvement. Existing NS/AGC follow, with real VNR
evidence supplied to AGC. IC adds about 39.5 ms at 16 kHz.

This does not implement dedicated dereverberation or stereo-reference 4Y2X AEC.

## Direction LEDs and response-delay investigation

Direction observations are published separately from audio steering overrides,
so forcing a wide beam does not erase the UI's direction observation. The native
renderer accepts valid fractional directions whose integer delays round to
zero, including the centre LED index. Snapshot age is calculated after copying
the protected state to avoid rejecting a concurrently updated sample.

The temporary comparison used the exact previously validated dev.3 DSP with
125 ms DoA polling and 100 ms LED updates. The native defaults retain those
bounded update rates. The display-only dev.15 trial was rolled back after the
user reported delayed/missing direction at command onset. Immediate commands
were still recognized correctly. On the restored dev.3 baseline, the user
confirmed immediate direction in quiet and later clarified that direction
still disappears with music. These observations do not establish that dev.15
alone caused the playback-specific issue.

Live baseline status during music showed FORCE_OMNI control/mode 4 and an
inactive raw direction energy gate. Raw microphone energy per sample was below
the learned music/noise floor. Dev.14 now tests direction after AEC with VNR
speech evidence rather than relying on that raw playback/noise threshold.
Backend speech timeouts have not been changed. The earlier 1–3 second
end-of-speech report has no precise follow-up timing measurement yet.

The first dev.14 hardware run passed a 30-second quiet USB capture: 1,999
complete DSP frames, no transport errors, clipping or USB drops. A 60-second
stage observation reported no processing calls over 15 ms; maxima were
7.99549 ms for AEC and 5.40415 ms for quiet voice analysis. Minimum free heaps
were 24,336 bytes on tile 0 and 22,808 bytes on tile 1. These short quiet checks
do not demonstrate an acoustic improvement.

The user subsequently reported the music-related direction fix working on
dev.14. Four wake detections and advancing DSP frames were observed with music
active and no audio transport/playback errors. Later telemetry showed lifetime
processing maxima of 8.13118 ms for AEC and 9.07566 ms for voice analysis, with
no processing calls over 15 ms. A later requested music recording contained
zero playback references and is retained as a failed stimulus, not a music
acoustic measurement or sustained-playback qualification.

Normal dev.13 was then installed and verified with a live microphone. After a
full power cycle including both USB cables, the user confirmed audible music,
immediate wake/commands from both sides and correctly pointing direction LEDs.
Status confirmed retained dev.13, a skipped DSP update, a new native boot and
three wake detections. A subsequent 120-second observation recorded 8,470
additional DSP frames with no stalls, status-read errors or audio errors.
Music was active for 70.7 seconds; the user confirmed stopping it intentionally.
This is a passed liveness check with bounded playback coverage, not a
120-second continuous-music pass. The raw observer's `incomplete_playback`
classification is retained alongside that clarification.

Exact tested normal factory SHA-256:
`2af046f6d95372f332cdc48236c74098ce4f8bcf8608efe8c04925122c75865f`.
Pre-release comparison native application SHA-256 (0.7.0-r3):
`37c5cd576c42309ad47d963c832b552315bfd8e02cbcad8d32d302583c3d5ae6`.
Private captures/status/artifacts are under
`audio_bench_results/hardware/20261125T225521Z-music-tracking/`.

These are short functional checks. Far-field accuracy, measured moving-talker
performance, ERLE, false wakes per hour and repeated cold boots remain broader
qualification work. The legacy profiles keep their original direction-state
publication; separate observations are exposed only by `aec4_track`.

## Build and qualification

Build sequentially, following [AUDIO-BENCH.md](AUDIO-BENCH.md), with
`SAT1_XMOS_PIPELINE=aec4_track`. Package using
`package_candidates.py --bench aec4_track` for the capture image, or
`package_candidates.py aec4_track` for normal operation, before the matching
native build. A native-only build does not rebuild the DSP payload.

Host and XS3A policy tests cover echo lag/gain, independent speech, DC offsets,
full-range arithmetic and speech cancellation limits. Beam tests verify target
rejection from the blocking reference and imported calibration with a failed
mic. The real-library simulator checks IC bypass alignment, repeated
initialization and the combined AEC/VNR/DoA/beam/IC/NS/AGC timing budget.

Hardware acceptance requires stage deadlines and heap margin, valid USB capture,
quiet and music wake/commands from both sides, movement, direction LEDs during
listening over music, end-of-speech comparison, and normal-image restart tests.
Preserve the exact native image, factory hash, source digest, capture and user
observations. Keep the previously validated normal image ready for rollback.
