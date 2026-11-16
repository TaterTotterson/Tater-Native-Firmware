- Releases native firmware `0.6.5` for Voice PE, Satellite1, ReSpeaker XVF3800,
  and ESP32-S3-BOX-3.
- Addresses internal-memory exhaustion and reboot risks with Send Good Wakes
  and Close Misses enabled, reported in
  [issue #25](https://github.com/TaterTotterson/Tater-Native-Firmware/issues/25).
- Keeps wake captures immediate, but holds one bounded PSRAM snapshot and waits
  until the full voice turn, reply playback, and other audio activity finish
  before uploading it to the trainer. New voice activity preempts an upload;
  disabled or stale pending captures are discarded safely.
- Places optional capture requests and audio snapshots explicitly in PSRAM,
  sizes HTTP buffers to prefer PSRAM, and avoids internal-stack upload fallback.
  Task-creation failures defer optional uploads rather than exhausting voice RAM.
- Reserves a reusable cleanup worker at startup. Playback, wake verification,
  wake-boundary processing, wake downloads, and Sendspin startup cleanup no
  longer allocate a temporary self-deletion task when internal RAM is low.
- Adds host regressions for upload deferral, foreground preemption, bounded
  capture ownership, allocation failures, and cleanup with allocations disabled.
