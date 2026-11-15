- Releases native firmware `0.6.4` for Voice PE, Satellite1, ReSpeaker XVF3800,
  and ESP32-S3-BOX-3.
- Fixes STT Wake Check enforcement opening a listening session immediately with
  `timeout_task_fail_open` when internal RAM is too limited to create its timeout
  task. A later rejection from Home Assistant or Tater could previously arrive
  after that premature fail-open and be ignored.
- Replaces the per-wake timeout task with a reusable deadline checked by the
  existing application service loop. Waiting for verification no longer needs
  another task, stack, or timeout-request allocation.
- Preserves accepted and rejected wake handling, Observe and Off modes, TV-profile
  verification, the configured timeout, and fail-open behavior when verification
  is unavailable or actually times out. Duplicate and late replies cannot start
  another session or complete a newer request.
- Adds host regression coverage for prompt rejection, acceptance, genuine and
  configured timeouts, repeated requests, duplicate/stale replies, responses
  during upload setup, unavailable uploads, and verification modes.
- Addresses the premature fail-open reported in
  [issue #22](https://github.com/TaterTotterson/Tater-Native-Firmware/issues/22).
