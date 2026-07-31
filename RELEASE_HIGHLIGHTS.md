- Updates Voice PE, Satellite1, ReSpeaker XVF3800, and S3 Box to firmware
  version 0.3.2.
- Uses native media-session content and visual metadata to show the speaking or
  tool-call state while TTS is playing.
- Completes the transient visual state when native TTS succeeds, fails, or is
  stopped, preventing satellites from remaining stuck in a speaking animation.
- Restores an active tool-call visual after its TTS finishes and otherwise
  returns the satellite to idle, or disconnected when the server is unavailable.
- Leaves persistent music sessions independent from transient TTS visual-state
  completion.
