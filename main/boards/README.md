# Board Implementations

Board folders contain the code that is specific to one physical satellite.
Shared firmware code should include `board.h` and call the common interfaces in
`audio_i2s.h`, `button.h`, and `leds.h`.

Add a new satellite by creating a folder here, then adding:

- a board header with pins, rates, IDs, and firmware version
- board-specific audio, button, LED, display, or codec implementations
- a PlatformIO environment
- a `BOARDS` entry in `scripts/build_native_firmware_manifest.py`

Keep Wi-Fi, provisioning, WebSocket protocol, OTA, native settings, wake assets,
and wake engine logic in the shared root unless a board genuinely needs a
different hardware implementation behind the same interface.
