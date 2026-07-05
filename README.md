# Tater Native Satellite Firmware

This is the first native firmware scaffold for official Tater satellites.
The initial target is **Voice PE**. Hardware-specific code now lives under
`main/boards/<board>/`, while Wi-Fi, provisioning, WebSocket protocol, OTA,
settings, wake-word, and playback orchestration remain shared.

This first Voice PE target now covers the basic native satellite loop:

- first-boot Wi-Fi/Tater provisioning over `Tater-Setup-XXXX`
- saves provisioning data to NVS
- joins Wi-Fi from saved config
- connects to Tater at `/api/tater/satellite/v1/ws`
- sends `hello`
- starts a voice session from the Voice PE center button
- streams 16 kHz, 16-bit, mono PCM as binary WebSocket frames
- sends `voice.stop` when the button is released
- receives `state`, `voice.event`, `play.url`, and `ota.url`
- drives Voice PE LED states and directional listening from XMOS DoA
- fetches and plays Tater runtime WAV TTS URLs
- plays embedded wake sounds on-device
- runs the embedded `hey_tater` microWakeWord model locally
- auto-updates Voice PE XMOS firmware to `1.3.2` when the installed version differs
- includes the editable Voice PE XMOS source used to rebuild that update image
- loads a custom microWakeWord `.json`/`.tflite` URL into RAM
- applies live settings from Tater
- enters setup mode from Tater or a deliberate button gesture
- reopens the mic for continued chat
- uploads optional good-wake and close-miss raw PCM clips to the trainer
- performs firmware OTA from a native `ota.url` command

Still intentionally limited:

- streaming decode/playback
- persistent on-device custom wake model cache
- custom wake sound URL download/playback
- barge-in during playback
- S3 Box/display targets
- full Tater device-management UI for native OTA

## Build With PlatformIO

This is the build path currently verified from this repo:

```sh
cd /Users/ahphooey/Scripts/Tater-Native-Firmware
platformio run
```

The firmware images are generated under `.pio/build/voicepe/`.

Flash over USB from source:

```sh
platformio run -t upload --upload-port /dev/cu.usbmodem4101
platformio device monitor --port /dev/cu.usbmodem4101 --baud 115200
```

Package official updater artifacts after a successful build:

```sh
./scripts/build_native_firmware_manifest.py --skip-build
```

Use `--board voicepe` explicitly when packaging a specific board. New satellite
targets should add their own PlatformIO environment and manifest board entry.

This writes:

- `prebuilt_firmware/latest.json`
- `prebuilt_firmware/native-voicepe-x.y.z.json`
- `prebuilt_firmware/<board>/native-<board>-x.y.z/firmware.bin`
- `prebuilt_firmware/<board>/native-<board>-x.y.z/firmware.factory.bin`

Tater uses `firmware.bin` for native OTA and `firmware.factory.bin` for USB recovery.
Published builds are distributed from GitHub Releases. Tater reads:

```text
https://github.com/TaterTotterson/Tater-Native-Firmware/releases/latest/download/latest.json
```

That `latest.json` points at the release manifest, and the release manifest
points at the release asset URLs for OTA and factory binaries.

## Voice PE XMOS Source

The embedded Voice PE XMOS update image lives at:

```text
main/boards/voice_pe/xmos/ffva_v1.3.2-vod_upgrade.bin
```

The editable source used to build that image is included at:

```text
main/boards/voice_pe/xmos/source/
```

See `main/boards/voice_pe/xmos/README.md` before rebuilding or bumping the XMOS
firmware version.

## Release Tags

Firmware releases are built by GitHub Actions when a `native-*` tag is pushed.
The tag must match the firmware version in the board header.

For Voice PE:

```sh
git tag native-voicepe-0.1.20
git push origin native-voicepe-0.1.20
```

The release workflow builds `voicepe`, packages release assets, writes release
URL-backed manifests, and creates or updates the GitHub Release with:

- `latest.json`
- `native-voicepe-x.y.z-manifest.json`
- `native-voicepe-x.y.z-voicepe-ota.bin`
- `native-voicepe-x.y.z-voicepe-factory.bin`
- `RELEASE_NOTES.md`

Flash over USB without a browser:

```sh
./scripts/flash_native_satellite_usb.py /dev/cu.usbmodem4101
```

The script reads the native firmware manifest, verifies the factory image SHA-256, erases flash, writes the image, and reboots the board. Pass `--no-erase` only when you intentionally want to preserve existing flash data.

If this is a fresh device, connect to the setup Wi-Fi network printed in
serial logs, usually `Tater-Setup-XXXX`, then open:

```text
http://192.168.4.1
```

Save:

- Wi-Fi SSID
- Wi-Fi password
- Tater server URL, for example `http://192.168.1.20:8501`
- Add Satellite pairing code from Tater, or a saved device/API token
- device name
- room

When a one-time Add Satellite code is used, Tater returns a permanent device
credential during `hello.ack`. The firmware saves that credential and uses it
for future WebSocket connections.

Unpaired native satellites are rejected by default. For bench testing only,
setting `TATER_NATIVE_SATELLITE_ALLOW_UNPAIRED=1` allows open native WebSocket
connections.

To put a running Voice PE back into setup mode from the device, click the
center button 5 times quickly. The LEDs show click progress. On the sixth press,
hold the button for 5 seconds while the LED ring counts down. The device plays
the embedded `short-definite-fart` wake sound, clears saved provisioning, then
reboots into setup mode.

Do not hold the center button while plugging in or resetting the board; on Voice
PE that button is also the ESP32-S3 bootloader strap pin.

## Build With ESP-IDF

Install ESP-IDF, then:

```sh
cd /Users/ahphooey/Scripts/Tater-Native-Firmware
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
```

Set these values in `menuconfig` under `Tater Native Satellite`:

- Wi-Fi SSID
- Wi-Fi password
- Tater server URL, for example `ws://tater.local:8501`
- device name
- room name
- satellite token, if Tater API auth is enabled

Flash:

```sh
idf.py flash monitor
```

## Voice PE Hardware Map

Pulled from the existing ESPHome Voice PE config:

- ESP32-S3, 16 MB flash, PSRAM
- I2C: SDA GPIO5, SCL GPIO6
- XMOS reset: GPIO4
- mic I2S input: BCLK GPIO13, LRCLK GPIO14, DIN GPIO15
- speaker I2S output: BCLK GPIO8, LRCLK GPIO7, DOUT GPIO10
- speaker amp enable: GPIO47
- LED ring: WS2812, GPIO21, 12 LEDs
- center button: GPIO0, active low
- rotary encoder: GPIO16/GPIO18
