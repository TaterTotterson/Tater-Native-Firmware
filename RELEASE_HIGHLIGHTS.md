- Releases native firmware `0.6.1` for Voice PE, Satellite1, ReSpeaker XVF3800,
  and ESP32-S3-BOX-3.
- Runs wake-boundary inference in a dedicated task so live microphone capture
  continues while the boundary model examines the buffered wake phrase. This
  fixes commands being missed or closed as empty when spoken immediately after
  “Hey Tater.”
- Takes a fresh audio snapshot after boundary inference, preserving speech that
  arrived while the model was running, and adds a small command guard before
  the detected boundary to avoid clipping the first sound of a fast command.
- Reduces boundary replay to the model's required 1.5-second window and sends
  the ordered wake/command marker sooner, improving wake-to-listening latency
  without changing wake-word detection or STT behavior.
- Synchronizes the shared capture ring used by live audio and boundary replay
  so every supported native satellite receives a consistent pre-roll snapshot.
