- Releases native firmware `0.6.6` for Voice PE, Satellite1, ReSpeaker XVF3800,
  and ESP32-S3-BOX-3.
- Change the satellite name and room live from Tater 3.0.4, Home Assistant bridge
  1.4.18, or Home Assistant's device editor through that bridge. No factory reset,
  reboot, or re-pairing is required; an empty room clears the assignment.
- Validate and persist both values together before confirming a change. Failed
  saves retain the previous name and room, while reconnects report the saved
  identity. Preserve Wi-Fi, pairing credentials, stable IDs, and other settings.
- Advertise live identity support so older controllers remain compatible and
  updated controllers only offer editing when the satellite supports it.
- Add host regressions using the production persistence code, including storage
  failures, room clearing, restart restoration, and credential preservation.
