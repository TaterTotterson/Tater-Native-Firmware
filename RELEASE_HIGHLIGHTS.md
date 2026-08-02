- Updates Voice PE, Satellite1, ReSpeaker XVF3800, and S3 Box to firmware
  version 0.3.4.
- Adds live volume changes for active music sessions so Tater's Music Core
  player can adjust satellite and stereo-pair playback without restarting the
  track.
- Adds media-session start positions for draggable player seeking and
  15-second rewind or forward controls across supported audio formats.
- Extends seek prebuffering for later positions in MP3 and FLAC tracks while
  keeping synchronized stereo members aligned at the same playback offset.
- Reports playhead telemetry for both individual and grouped media sessions so
  Tater can keep player progress current.
- Keeps unrelated TTS overlays, music ducking, and post-announcement playback
  restoration working with live volume and seek controls.
