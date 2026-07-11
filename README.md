# Tater Native Satellite Firmware

Native firmware for Tater-managed voice satellites.

The goal is to make satellites behave like dedicated Tater appliances instead
of configurable ESPHome nodes:

- no YAML
- no firmware editing by the user
- setup and settings managed from Tater
- official release images for supported hardware
- Tater handles the intelligence; firmware handles the hardware

## Features

### Supported Hardware

- Voice PE: `voicepe`
- Satellite1 / Sat1: `sat1` build environment, `satellite1` release key
- ReSpeaker XVF3800: `respeaker_xvf3800`
- ESP32-S3-BOX-3 Display: `s3_box`

All targets are ESP32-S3 native firmware images that connect directly to Tater
over the native satellite WebSocket protocol. ReSpeaker XVF3800 uses an 8MB
flash layout; Voice PE, Sat1, and S3 Box use the 16MB layout.

### Provisioning And Pairing

- First boot setup AP: `Tater-Setup-XXXX`
- Local setup page at `http://192.168.4.1`
- Captive DNS for phone/computer auto-popup where supported
- Wi-Fi SSID/password setup
- Tater server URL setup
- Add Satellite pairing code setup
- Device name and room setup
- Persistent device credential saved after pairing
- Unpaired native satellites rejected by Tater by default
- Wi-Fi failure fallback into setup mode on the next boot
- Setup mode can also be triggered from Tater
- Physical setup reset gesture on boards with a center/action button: 5 quick
  clicks, then hold the sixth press for 5 seconds

### Native Tater Connection

- Connects to Tater at `/api/tater/satellite/v1/ws`
- Sends board, firmware, capabilities, diagnostics, live settings, wake state,
  timer state, AEC state, XMOS status, and DoA telemetry
- Streams mic audio as 16 kHz, 16-bit, mono PCM binary WebSocket frames
- Uses an audio transmit queue and reconnect-aware send path to reduce random
  voice-session disconnects
- Supports native firmware OTA commands from Tater
- Sends logs, OTA status, playback-finished events, timer events, and trainer
  feedback events

### Voice And Audio

- Local embedded `hey_tater` microWakeWord model
- Custom microWakeWord `.json` and `.tflite` URL support with persistent cache
- Wake sensitivity, environment profile, threshold, and sliding-window settings
- Optional good-wake and close-miss raw PCM upload hooks for the trainer
- Continued-chat mic reopen
- Optional wake-word barge-in during playback
- Firmware-side adaptive AEC with live strength and delay settings
- Server-driven playback from Tater
- WAV streaming playback
- MP3 streaming decode/playback
- FLAC streaming decode/playback
- Compressed-stream jitter buffering for MP3/FLAC
- Embedded on-device wake sounds
- Custom wake-sound WAV URL support with persistent cache
- Server-driven `play.tone` support for timers and diagnostics
- Per-device volume setting

### LEDs, Buttons, And Device UI

- Tater-driven LED state machine
- Default orange-red system animations
- Setup mode animation stays white
- Listening, thinking, tool-call, replying, speaking, timer, OTA, provisioning,
  Wi-Fi, connecting, disconnected, and error states
- Per-device LED color, brightness, and animation settings
- Directional listening animation from XMOS DoA where available
- Tool-call visual hold until the final response
- Display targets render Tater state, clock/date, assistant name, volume/mute,
  and Tater-provided status/stat cards instead of LED-ring animations
- Short press stops playback or timer ringing
- Hold starts push-to-talk/intercom behavior handled by Tater
- Physical setup reset click progress, countdown, and success feedback

### Timers And Intercom Hooks

- Tater-managed timers with local satellite alarm fallback
- Timer LEDs and local alarm tone while ringing
- Timer stop from the device button
- Native intercom/push-to-talk hooks through the same satellite transport
- Broadcast/intercom routing is handled by Tater so room targeting can happen on
  the server side

### Board-Specific Hardware

Voice PE:

- 12 LED ring
- AIC3204 speaker path
- XMOS DoA telemetry
- XMOS firmware auto-update to `1.3.2` when the installed version differs
- Editable XMOS source included under `main/boards/voice_pe/xmos/source/`

Satellite1 / Sat1:

- 24 LED ring
- 48 kHz microphone capture downsampled into the 16 kHz wake/STT path
- Shared-duplex I2S speaker playback
- PCM5122/TAS2780 speaker path setup
- FUSB302B USB-C PD setup path
- XMOS DoA telemetry and firmware version/status reporting
- Line-out capability advertised to Tater

ReSpeaker XVF3800:

- 12 LED ring driven through the XVF3800 I2C control interface
- 48 kHz stereo I2S slave capture from the XVF3800, downsampled into the 16 kHz
  wake/STT path
- Shared-duplex I2S playback back into the XVF3800 speaker/line-out path
- XVF3800 DoA telemetry for directional listening LEDs
- XVF3800 firmware auto-update to the included `1.0.7` I2S host firmware when
  the installed version differs
- Mute state bridged through the XVF3800 control interface
- 8MB flash layout with two OTA app slots

ESP32-S3-BOX-3 Display:

- 320x240 LCD with Tater-themed display UI
- Tater assistant name, online state, voice state, clock, date, volume, mute,
  and configured display stats
- Display feed polling from Tater with room/profile targeting
- 48 kHz stereo I2S microphone capture downsampled into the 16 kHz wake/STT path
- Shared-duplex I2S speaker playback
- Display-backed setup reset, volume, mute, timer, OTA, provisioning, and voice
  state feedback
- 16MB flash layout with two OTA app slots

### Current Limits

- M4A/AAC and OGG/Vorbis are intentionally not included until there is a real
  need for them.
- Sat1 XMOS direct-flash recovery is newer than the Voice PE DFU path and still
  needs more real-device testing across factory XMOS versions.
- S3 Box display feed depends on Tater being reachable; the display falls back
  to local state/clock placeholders when server-fed stats are unavailable.

## How To Set Up

### Option 1: OTA From Tater

Use this when a satellite is already paired and connected.

1. Open Tater.
2. Go to Satellites.
3. Open the target satellite.
4. Choose the firmware update action.
5. Tater downloads the native OTA image from the firmware release manifest and
   sends an `ota.url` command to the satellite.
6. The satellite enters the OTA LED state, downloads the image, flashes it, and
   reboots.

Tater reads the official release index from:

```text
https://github.com/TaterTotterson/Tater-Native-Firmware/releases/latest/download/latest.json
```

### Option 2: Flash Over USB From Tater

Use this for first flash, recovery, or a satellite that cannot reach OTA.

1. Plug the satellite into USB.
2. Open Tater.
3. Go to the firmware/USB flashing UI.
4. Pick the supported native firmware target.
5. Flash the factory image.
6. After reboot, provision the device from the setup AP.

The factory image erases and writes the full flash layout. OTA images are only
for already-running native firmware.

### Option 3: Flash Over USB From The Command Line

The repo includes a browser-free USB flashing script. It reads the native
firmware manifest, verifies SHA-256, erases flash by default, writes the factory
image, and reboots the board.

Voice PE:

```sh
./scripts/flash_native_satellite_usb.py /dev/cu.usbmodem4101 --board voicepe
```

Satellite1 / Sat1:

```sh
./scripts/flash_native_satellite_usb.py /dev/cu.usbmodem4101 --board sat1
```

ReSpeaker XVF3800:

```sh
./scripts/flash_native_satellite_usb.py /dev/cu.usbmodem4101 --board respeaker_xvf3800
```

ESP32-S3-BOX-3 Display:

```sh
./scripts/flash_native_satellite_usb.py /dev/cu.usbmodem4101 --board s3_box
```

Factory images rewrite the full boot/partition/app layout and the device will
need provisioning afterward. For local development on an already-provisioned
device, flash the app image instead:

```sh
./scripts/flash_native_satellite_usb.py /dev/cu.usbmodem4101 --app-image .pio/build/sat1/firmware.bin
```

`--app-image` writes the app slots only and leaves Wi-Fi/pairing setup data in
place. `--no-erase` does not protect setup data when writing a factory image at
`0x0`.

### First Boot Provisioning

If no Wi-Fi credentials are saved, the satellite starts setup mode.

1. Connect your phone or computer to the setup Wi-Fi network:

```text
Tater-Setup-XXXX
```

2. The captive portal should open automatically. If it does not, open:

```text
http://192.168.4.1
```

3. In Tater, go to Satellites and choose Add Satellite.
4. Enter the pairing code shown by Tater into the setup page.
5. Fill in:

- Wi-Fi SSID
- Wi-Fi password
- Tater server URL, for example `http://192.168.1.20:8501`
- pairing code
- device name
- room

6. Save. The satellite reboots, joins Wi-Fi, pairs with Tater, and appears in
   the Satellites page.

When pairing succeeds, Tater returns a permanent device credential during
`hello.ack`. The firmware saves that credential and uses it for future
WebSocket connections.

### Setup Reset From The Device

Use this if the saved Wi-Fi/server settings are wrong on a board with a
center/action button.

1. Click the center/action button 5 times quickly.
2. On the sixth press, hold for 5 seconds.
3. The LED ring shows a countdown.
4. The satellite plays the embedded `short-definite-fart` wake sound.
5. Saved provisioning is cleared and the device reboots into setup mode.

Do not hold the center button while plugging in or resetting the board. On these
ESP32-S3 boards, GPIO0 is also a bootloader strap pin.

For ReSpeaker XVF3800, the mute switch can also trigger setup reset: toggle it
8 times within the reset window, then leave mute on for 5 seconds. Tater setup
reset and USB recovery remain available.

### Bench Testing Unpaired Devices

Unpaired native satellites are rejected by default. For bench testing only, set:

```sh
TATER_NATIVE_SATELLITE_ALLOW_UNPAIRED=1
```

Do not use that setting for normal installs.

## Advanced Build Info

### Repository Layout

```text
main/
  app_main.c                  Shared app startup
  tater_protocol.c            Native WebSocket protocol
  playback.c                  WAV/MP3/FLAC playback and tones
  wake_engine.c               microWakeWord integration
  native_settings.c           Live settings from Tater
  audio_aec.c                 Firmware-side adaptive AEC
  provisioning.c              Setup AP, captive DNS, setup web UI
  boards/
    voice_pe/                 Voice PE board implementation
    sat1/                     Satellite1 board implementation
    respeaker_xvf3800/        ReSpeaker XVF3800 board implementation
    s3_box/                   ESP32-S3-BOX-3 display board implementation
scripts/
  build_native_firmware_manifest.py
  flash_native_satellite_usb.py
  render_release_notes.py
```

Board folders hold only physical hardware differences. Shared behavior such as
Wi-Fi, provisioning, WebSocket protocol, OTA, settings, wake assets, wake engine,
playback, AEC, timers, and logs stays in the shared root.

### Build With PlatformIO

Build the default target:

```sh
cd /Users/ahphooey/Scripts/Tater-Native-Firmware
platformio run
```

Build all supported targets:

```sh
platformio run -e voicepe -e sat1 -e respeaker_xvf3800 -e s3_box
```

Build outputs:

```text
.pio/build/voicepe/firmware.bin
.pio/build/voicepe/firmware.factory.bin
.pio/build/sat1/firmware.bin
.pio/build/sat1/firmware.factory.bin
.pio/build/respeaker_xvf3800/firmware.bin
.pio/build/respeaker_xvf3800/firmware.factory.bin
.pio/build/s3_box/firmware.bin
.pio/build/s3_box/firmware.factory.bin
```

Flash from source with PlatformIO:

```sh
platformio run -e voicepe -t upload --upload-port /dev/cu.usbmodem4101
platformio device monitor --port /dev/cu.usbmodem4101 --baud 115200
```

For Sat1, ReSpeaker XVF3800, or S3 Box, use `-e sat1`,
`-e respeaker_xvf3800`, or `-e s3_box`.

### Package Local Release Assets

After a successful build:

```sh
./scripts/build_native_firmware_manifest.py --board all --skip-build
```

Use `--board voicepe`, `--board satellite1`, `--board respeaker_xvf3800`, or
`--board s3_box` to package a single board.

This writes local release-style assets under `release_assets/<version>/`:

- `latest.json`
- `native-x.y.z-manifest.json`
- `native-<board>-x.y.z-<board>-ota.bin`
- `native-<board>-x.y.z-<board>-factory.bin`

Tater OTA uses the GitHub Release `ota` artifacts. USB recovery and first flash
use the GitHub Release `factory` artifacts, or a local factory image passed
directly to the USB flash script.

### Release Tags

Firmware releases are built by GitHub Actions when a `native-*` tag is pushed.
For combined releases, all board headers must use the same numeric version and
the tag uses the shared `native-x.y.z` form.

Example:

```sh
git tag native-0.1.33
git push origin native-0.1.33
```

The release workflow builds `voicepe`, `sat1`, `respeaker_xvf3800`, and
`s3_box`, packages release assets, writes URL-backed manifests, and creates or
updates the GitHub Release with:

- `latest.json`
- `native-x.y.z-manifest.json`
- `native-voicepe-x.y.z-voicepe-ota.bin`
- `native-voicepe-x.y.z-voicepe-factory.bin`
- `native-satellite1-x.y.z-satellite1-ota.bin`
- `native-satellite1-x.y.z-satellite1-factory.bin`
- `native-respeaker-xvf3800-x.y.z-respeaker_xvf3800-ota.bin`
- `native-respeaker-xvf3800-x.y.z-respeaker_xvf3800-factory.bin`
- `native-s3-box-x.y.z-s3_box-ota.bin`
- `native-s3-box-x.y.z-s3_box-factory.bin`
- `RELEASE_NOTES.md`

The release title is:

```text
Tater Firmware x.y.z
```

### Voice PE XMOS Firmware

The embedded Voice PE XMOS update image lives at:

```text
main/boards/voice_pe/xmos/ffva_v1.3.2-vod_upgrade.bin
```

The editable source used to build that image is included at:

```text
main/boards/voice_pe/xmos/source/
```

Read `main/boards/voice_pe/xmos/README.md` before rebuilding or bumping the
XMOS firmware version.

### Add A New Satellite Board

1. Create `main/boards/<board>/`.
2. Add a board header with pins, sample rates, IDs, capabilities, and firmware
   version.
3. Add board-specific audio, LED, button, display, codec, or power-management
   implementations behind the shared interfaces.
4. Add a PlatformIO environment.
5. Add the board to `scripts/build_native_firmware_manifest.py`.
6. Add it to the release workflow when it is ready for official builds.
7. Test USB flashing, setup mode, pairing, wake word, mic streaming, playback,
   LEDs, OTA, and reconnect behavior before publishing.
