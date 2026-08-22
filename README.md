<div align="center">
  <a href="https://taterassistant.com">
    <img src="images/tater-native-firmware-logo.png" alt="Tater Native Firmware" width="460"/>
  </a>
</div>
<p align="center">
  <a href="https://taterassistant.com">
    <img alt="Visit Tater Assistant" src="https://img.shields.io/badge/Tater%20Assistant-Visit%20Website-F28C28?style=for-the-badge&logo=googlechrome&logoColor=white" />
  </a>
  <a href="https://discord.gg/w52namKyXT">
    <img alt="Join the Tater Assistant Discord" src="https://img.shields.io/badge/Discord-Join%20the%20Community-5865F2?style=for-the-badge&logo=discord&logoColor=white" />
  </a>
</p>

# Tater Native Satellite Firmware

Native firmware for Tater-managed voice satellites.

The goal is to make satellites behave like dedicated Tater appliances instead
of configurable ESPHome nodes:

- no YAML
- no firmware editing by the user
- setup and settings managed from Tater
- official release images for supported hardware
- Tater handles the intelligence; firmware handles the hardware

Wake-word models now live in the
[Tater Wake Words](https://github.com/TaterTotterson/Tater-Wake-Words) repo.
Use that repo to browse shared microWakeWord packages or request/build new
custom wake words for Tater Native satellites.

## Home Assistant

Home Assistant users can connect this same native firmware directly to Home
Assistant with the
[Tater Satellite integration](https://github.com/TaterTotterson/Tater-Home-Assistant-Satellites).
The integration provides satellite pairing, voice settings, wake-word changes,
trainer linking, firmware/OTA updates, and Home Assistant Assist pipeline
support without requiring a separate firmware build.

For the complete Tater Assistant experience, connect the satellites to
[Tater](https://github.com/TaterTotterson/Tater) instead.

## Features

### Supported Hardware

- Voice PE: `voicepe`
- Satellite1 / Sat1: `sat1` build environment, `satellite1` release key
- Satellite1 Public Batch #1 / Beta.1 rev4.1: `sat1_beta_rev41` build
  environment, `satellite1_beta_rev41` release key
- ReSpeaker XVF3800: `respeaker_xvf3800`
- ESP32-S3-BOX-3 Display: `s3_box`

All targets are ESP32-S3 native firmware images that connect directly to Tater
over the native satellite WebSocket protocol. ReSpeaker XVF3800 uses an 8MB
flash layout; Voice PE, both Sat1 targets, and S3 Box use the 16MB layout.

The two Sat1 targets are intentionally separate. Select `satellite1` for Public
Batch #2 and later hardware, or `satellite1_beta_rev41` for Public Batch #1 /
Beta.1 HAT and Core rev4.1. The hardware revision is not reliably detectable in
software, so the initial USB flash must use the correct target. After that,
their distinct board IDs and OTA-family markers keep updates on the matching
firmware channel.

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
- Saved Wi-Fi credentials are preserved through temporary outages
- Wi-Fi reconnects indefinitely with capped exponential backoff and jitter
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
- Uses native WebSocket auto-reconnect plus a lifecycle watchdog that can
  recover a stuck client without deadlocking an in-flight hello or audio send
- Supports native firmware OTA commands from Tater
- Sends logs, OTA status, playback-finished events, timer events, and trainer
  feedback events

### Voice And Audio

- Local embedded `hey_tater` microWakeWord model
- Embedded alarm-only `stop` model that is active only while a timer rings
- Custom microWakeWord `.json` and `.tflite` URL support with persistent cache
- Wake sensitivity that adjusts the effective detection threshold, plus
  environment profile, threshold, and sliding-window settings
- Optional low-latency Tater STT wake verification in observe or enforce mode,
  with fail-open handling if verification is unavailable or times out
- A `tv_nearby` profile that uses a more permissive first-stage candidate and
  automatically verifies it before opening a voice session
- Optional good-wake and close-miss raw PCM upload hooks for the trainer
- Continued-chat mic reopen
- On Voice PE, Satellite1, and ReSpeaker XVF3800, wake-word listening remains
  active during playback through their hardware-AEC microphone paths; barge-in
  only controls whether a confirmed wake stops the playing audio
- On S3 Box, playback-time wake remains opt-in through barge-in until its
  firmware AEC path is ready for production use
- Firmware-side adaptive AEC with live strength and delay settings
- Server-driven playback from Tater
- WAV streaming playback
- MP3 streaming decode/playback
- FLAC streaming decode/playback
- Compressed-stream jitter buffering for MP3/FLAC
- Versioned audio scenes with a foreground stream, optional looping background,
  configurable source volumes, duck level, attack/release, and fade-out
- Persistent media sessions that keep their decoder position while unrelated
  TTS is mixed over the media, ducked, and restored
- Embedded on-device wake sounds
- Custom wake-sound WAV URL support with persistent cache
- Server-driven `play.tone` support for diagnostics
- Per-device volume setting
- Two-way hardware volume synchronization for Satellite1 buttons and the Voice
  PE encoder

When a physical volume control changes the shared device volume, the satellite
emits a compact event so the server can persist and display the new value:

```json
{
  "type": "settings.changed",
  "payload": {
    "settings": {"volume_percent": 65},
    "source": "device"
  }
}
```

`audio.scene.start` is backward-compatible with the existing `play.url` path
and is advertised as audio scene capability version 1. The foreground is
required; the background is optional and may loop:

```json
{
  "type": "audio.scene.start",
  "payload": {
    "scene_id": "morning-weather",
    "foreground": {
      "url": "https://example.test/weather.mp3",
      "kind": "tts",
      "volume_percent": 100
    },
    "background": {
      "url": "https://example.test/morning.flac",
      "loop": true,
      "volume_percent": 80
    },
    "ducking": {
      "target_percent": 35,
      "attack_ms": 150,
      "release_ms": 350
    },
    "finish": {
      "fade_ms": 500
    }
  }
}
```

The satellite emits `audio.scene.finished` with `scene_id` and `ok`, followed
by the legacy `playback.finished` event. `audio.scene.stop` stops the active
scene.

For music that must continue across unrelated announcements, Tater starts a
persistent media session:

```json
{
  "type": "media.session.start",
  "payload": {
    "session_id": "kitchen-song",
    "media": {
      "url": "https://example.test/song.mp3",
      "volume_percent": 100,
      "start_position_ms": 0,
      "loop": false
    }
  }
}
```

`start_position_ms` optionally begins decoding at a requested track position.
Tater uses it when the Music Core progress bar is moved or its rewind/forward
controls are pressed. Active media-session volume can be changed without
restarting the track:

```json
{
  "type": "media.session.volume",
  "payload": {
    "session_id": "kitchen-song",
    "volume_percent": 42
  }
}
```

While that decoder remains active, `audio.overlay.start` mixes foreground TTS
without restarting or seeking the media:

```json
{
  "type": "audio.overlay.start",
  "payload": {
    "overlay_id": "door-alert",
    "foreground": {
      "url": "https://example.test/door-alert.wav",
      "kind": "tts",
      "volume_percent": 100
    },
    "ducking": {
      "target_percent": 20,
      "attack_ms": 150,
      "release_ms": 350
    }
  }
}
```

The satellite emits `media.session.started`/`media.session.finished` and
`audio.overlay.started`/`audio.overlay.finished`. A normal `play.url` received
during an active media session is automatically promoted to an overlay for
backward-compatible TTS callers. `media.session.stop` stops the media and any
active overlay.

Transient media sessions can identify speech with `media.content_type` and set
`visual_mode` to `speaking` or `tool_call`. The satellite shows that visual
state for the lifetime of the TTS session, then returns to the held tool-call
state when applicable or to its normal idle/disconnected state. Persistent
music sessions do not complete or replace the satellite's conversational
visual state.

For stereo pairs, Tater first measures each satellite's monotonic clock, then
sends `media.session.prepare` to both members with the same URL and group id.
Each satellite decodes into its local buffer, selects `left` or `right`, and
returns `media.session.prepare.result` only after its speaker and buffer are
ready. Tater then sends an individualized `media.session.commit` containing the
same future start expressed in that satellite's clock:

```json
{
  "type": "media.session.prepare",
  "payload": {
    "session_id": "bedroom-song",
    "group_id": "bedroom-stereo",
    "media": {
      "url": "https://example.test/song.mp3",
      "volume_percent": 100,
      "loop": false
    },
    "routing": {
      "channel": "left"
    }
  }
}
```

While grouped playback is active, each member emits
`media.session.playhead` once per second. Tater projects both source positions
onto its monotonic clock and sends a bounded `media.session.adjust` when phase
error grows. The satellite distributes each correction over time and resamples
small source-rate differences into fixed-size hardware output blocks, avoiding
an abrupt dropped or repeated frame. If a stream underruns, the satellite
rebuilds its buffer, skips forward to the shared wall-clock timeline, and fades
back in while reporting rebuffer, underrun, and rejoin telemetry. Corrections
are deferred during a TTS overlay, so speech mixing and ducking stay intact.
Scheduled `audio.overlay.start` commands use the same clock mapping to duck and
center TTS on grouped members together.

### LEDs, Buttons, And Device UI

- Tater-driven LED state machine
- Default orange-red system animations
- Setup mode animation stays white
- Listening, thinking, tool-call, replying, speaking, timer, OTA, provisioning,
  Wi-Fi, connecting, disconnected, and error states
- Per-device LED color, true 0-100% brightness, and animation settings
- Stable, low-glow disconnected state without random idle LED flashes
- Directional listening animation from XMOS DoA where available, with adaptive
  speech/noise gating, confidence filtering, a brief direction hold, and a
  neutral listening state when no active talker is detected
- Sound-reactive voice-ring replies aim toward the direction observed most
  often during the preceding listening turn
- Tool-call visual hold until the final response
- Display targets render Tater state, clock/date, assistant name, volume/mute,
  and Tater-provided status/stat cards instead of LED-ring animations
- Short press stops playback or timer ringing
- Hold starts push-to-talk/intercom behavior handled by Tater
- Physical setup reset click progress, countdown, and success feedback

### Timers And Intercom Hooks

- Up to eight concurrent, optionally named timers owned entirely by the
  satellite
- Volatile monotonic countdowns that continue through Tater and Wi-Fi
  disconnects and intentionally clear when the satellite reboots
- Original embedded zen chime with a quiet speech-detection gap
- Local `stop` wake word while ringing, without an STT or network round trip
- Saying `stop` or pressing the device button dismisses every timer currently
  ringing while later timers continue counting
- Live start, list, cancel, and snooze commands; Tater does not persist or
  restore satellite timer state
- Timer LEDs only while ringing, with a 15-minute automatic alarm cutoff
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
- PCM5122/TAS2780 speaker path setup using the default 5 V USB-C supply and
  fixed TAS2780 power mode 0
- Four-microphone XMOS DoA estimation with noise calibration, confidence
  filtering, continuous talker tracking, playback omni fallback,
  fractional-delay beamforming,
  per-microphone gain calibration, unhealthy-microphone fallback, and expanded
  firmware/diagnostic reporting
- XMOS firmware auto-update to `1.1.1` when the installed version differs
- Lab-only four-channel raw USB microphone capture image for independent mic
  measurement; this diagnostic image is not embedded in normal Sat1 firmware
- Line-out capability advertised to Tater

ReSpeaker XVF3800:

- 12 LED ring driven through the XVF3800 I2C control interface
- 48 kHz stereo I2S slave capture from the XVF3800, downsampled into the 16 kHz
  wake/STT path
- Shared-duplex I2S playback back into the XVF3800 speaker/line-out path
- XVF3800 DoA and speech-detector telemetry for stable directional listening
  LEDs that ignore silent/noise-only direction updates
- XVF3800 firmware auto-update to the included `1.0.7` I2S host firmware when
  the installed version differs
- Mute state bridged through the XVF3800 control interface
- 8MB flash layout with two OTA app slots

ESP32-S3-BOX-3 Display:

- 320x240 LCD with Tater-themed display UI
- Tater assistant name, online state, voice state, clock, date, volume, mute,
  and configured display stats
- Per-device PWM screen brightness plus optional local-time night dimming with
  separate start, restore, and night-brightness settings
- Display feed polling from Tater with room/profile targeting
- 48 kHz stereo I2S microphone capture downsampled into the 16 kHz wake/STT path
- Shared-duplex I2S speaker playback
- Display-backed setup reset, volume, mute, timer, OTA, provisioning, and voice
  state feedback
- 16MB flash layout with two OTA app slots

### Current Limits

- M4A/AAC and OGG/Vorbis are intentionally not included until there is a real
  need for them.
- A Sat1 boot that must install or recover XMOS `1.1.1` can take about 20
  seconds before the satellite connects. Later boots verify the version and
  skip reflashing.
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

The server field also accepts a bare hostname/IP address, `ws://` or `wss://`,
and the full `/api/tater/satellite/v1/ws` endpoint. The firmware normalizes
these forms automatically.

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
  wake_engine.cc              microWakeWord integration
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
platformio run -e voicepe -e sat1 -e sat1_beta_rev41 -e respeaker_xvf3800 -e s3_box
```

Build outputs:

```text
.pio/build/voicepe/firmware.bin
.pio/build/voicepe/firmware.factory.bin
.pio/build/sat1/firmware.bin
.pio/build/sat1/firmware.factory.bin
.pio/build/sat1_beta_rev41/firmware.bin
.pio/build/sat1_beta_rev41/firmware.factory.bin
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

For production Sat1, legacy Sat1 Beta.1/rev4.1, ReSpeaker XVF3800, or S3 Box,
use `-e sat1`, `-e sat1_beta_rev41`, `-e respeaker_xvf3800`, or `-e s3_box`.

### Package Local Release Assets

After a successful build:

```sh
./scripts/build_native_firmware_manifest.py --board all --skip-build
```

Use `--board voicepe`, `--board satellite1`, `--board
satellite1_beta_rev41`, `--board respeaker_xvf3800`, or `--board s3_box` to
package a single board.

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
Board headers normally share the same numeric version. A board-only update can
append `-revN` (for example, `native-satellite1-0.3.3-rev1`) without bumping
unaffected boards. The combined release tag uses the newest board version, so
that example is released as `native-0.3.3-rev1`. Tater compares both the shared
three-part version and the board revision.

When publishing a follow-up package without changing any embedded board
versions, append a descriptive suffix to the existing release version, such as
`native-0.3.11-sat1beta`. This creates a new immutable release tag while every
board and the combined manifest continue to report firmware version `0.3.11`.

Example:

```sh
git tag native-0.1.33
git push origin native-0.1.33
```

For a board-only revision:

```sh
git tag native-0.3.3-rev1
git push origin native-0.3.3-rev1
```

For a same-version follow-up package:

```sh
git tag native-0.3.11-sat1beta
git push origin native-0.3.11-sat1beta
```

The release workflow builds `voicepe`, `sat1`, `sat1_beta_rev41`,
`respeaker_xvf3800`, and `s3_box`, packages release assets, writes URL-backed
manifests, and creates or updates the GitHub Release with:

- `latest.json`
- `native-x.y.z-manifest.json`
- `native-voicepe-x.y.z-voicepe-ota.bin`
- `native-voicepe-x.y.z-voicepe-factory.bin`
- `native-satellite1-x.y.z-satellite1-ota.bin`
- `native-satellite1-x.y.z-satellite1-factory.bin`
- `native-satellite1-beta-rev41-x.y.z-satellite1_beta_rev41-ota.bin`
- `native-satellite1-beta-rev41-x.y.z-satellite1_beta_rev41-factory.bin`
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
