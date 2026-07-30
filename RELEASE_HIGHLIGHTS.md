- Updates Voice PE, Satellite1, ReSpeaker XVF3800, and S3 Box to firmware version 0.3.0.
- Adds synchronized media prepare/ready/commit sessions so two satellites can
  buffer independently and begin on one shared future timestamp.
- Adds local left, right, mono, and stereo routing after decode, allowing a
  paired satellite to play one channel through both of its physical outputs.
- Adds satellite clock probes, scheduled TTS overlays, one-second playhead
  telemetry, and bounded sample corrections for long-running stereo alignment.
- Preserves per-speaker volume and acoustic delay calibration while drift
  correction maintains the configured phase relationship.
- Adds versioned audio scenes with simultaneous foreground TTS and an optional
  looping WAV, MP3, or FLAC background.
- Adds configurable foreground/background volume, duck level, attack/release,
  and background fade-out with graceful foreground-only fallback.
- Advertises audio-scene, ducking, and looping-background capabilities and
  correlates completion with `scene_id`.
- Adds persistent media sessions and TTS overlays so music keeps its decoder
  position, ducks beneath unrelated speech, and returns to its prior level
  after the overlay.
- Automatically promotes legacy foreground `play.url` commands to overlays
  while a persistent media session is active.
