- Releases native firmware `0.6.0` for Voice PE, Satellite1, ReSpeaker XVF3800,
  and ESP32-S3-BOX-3.
- Adds detector-independent wake-boundary support to every native satellite.
  After the main wake-word detector accepts a wake, a small boundary model
  replays the buffered audio to locate the acoustic end of the wake phrase.
  This keeps “Hey Tater” out of speech recognition while preserving a command
  spoken immediately afterward. The boundary model does not run continuously.
- Adds complete schema-v4 V9 bundle download, validation, caching, and runtime
  support. Boundary-capable devices advertise their capability so Tater and the
  Home Assistant bridge can send the complete bundle while older firmware keeps
  receiving its compatible MWW-only manifest.
- Updates the built-in Hey Tater detector to the newer model used by Tater Echo
  Firmware and includes its matching boundary model.
- Adds the ordered audio-boundary marker used by Tater and Home Assistant to
  separate wake audio from command audio without clipping fast speech.
- Compresses the embedded XMOS update payloads for Voice PE, Satellite1, and
  ReSpeaker, then expands one into PSRAM only when an XMOS update is needed.
  This recovers application-flash space without changing the XMOS image bytes.
