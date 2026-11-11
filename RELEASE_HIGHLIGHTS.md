- Releases native firmware `0.6.2` for Voice PE, Satellite1, ReSpeaker XVF3800,
  and ESP32-S3-BOX-3.
- Fixes an audio-task stack overflow reproduced on Satellite1 when activating
  a remotely downloaded wake-word model after its download and cache write
  completed successfully.
- Moves temporary model-switch and cache-load metadata into PSRAM across all
  four targets, with automatic cleanup of buffers that were not transferred
  into the active model.
- Moves Satellite1's 4,480-byte microphone capture workspace out of the audio
  stack and into PSRAM, and increases that task's stack from 8 KB to 16 KB.
- Logs minimum free stack space after custom model activation to help diagnose
  model-switching issues.
- Verified seven Satellite1 hardware checks covering cached models, fresh
  remote downloads, complete wake-boundary bundles, cached reload after
  reboot, and switching back to embedded Hey Tater. No new crashes occurred;
  measured minimum free activation stack was 13,024 bytes. All four
  firmware targets build successfully; hardware validation was on Satellite1.
