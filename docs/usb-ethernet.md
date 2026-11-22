# Experimental USB Ethernet

This is a prototype with initial SAT1 and Voice PE hardware checks completed. It is deliberately
absent from the four production build environments and release manifests.
The first test target is SAT1 with the powered UCTRONICS U6113 adapter already
used with Echo, plus a **data-capable Micro-USB female to USB-C male converter**.
SAT1 was flashed and tested on November 21, 2026; the connected Echo was left
untouched. This is still an experimental build, based on native-0.6.7.
Voice PE has a separate `voicepe_usb_ethernet` test environment that retains
its own board identity, audio driver, and XMOS payload. Its initial USB,
Ethernet, and voice checks also passed on November 21. Validate each board
separately rather than inferring compatibility from a shared ESP32-S3.

## What the prototype does

- Runs the ESP32-S3 USB controller as a host for the adapter on the **ESP32 USB
  port**, not SAT1's XMOS USB port.
- Allows only USB VID:PID `0bda:8152` with at least two configurations, selecting
  configuration 2 for CDC ECM. Other adapters are rejected by this prototype.
- Requests a terminating USB short packet for aligned Ethernet frame lengths.
  A build-time patch to CDC 3.1.1 enables ESP-IDF's `USB_TRANSFER_FLAG_ZERO_PACK`
  only under `CONFIG_TATER_USB_ETHERNET`, preventing adjacent frames from being
  joined when their length is a multiple of the endpoint packet size.
- Uses Espressif's pinned `iot_usbh_ecm` 0.4.0 driver, DHCP, and esp-netif.
  Espressif lists RTL8152B as tested on ESP32-S3 in its
  [ECM host documentation](https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_host/usb_ecm.html).
  That listing alone does not establish compatibility with a particular
  adapter unit. Our hardware checks below prove initial bidirectional traffic
  with this unit; other adapters and sustained workloads remain unverified.
- Registers the custom USB network interface with mDNS, enabling announcements
  after DHCP and withdrawing them when its address or link is lost. The USB
  build reserves an mDNS slot for it instead of the unused standard ETH port.
  A guarded patch to mDNS 1.14.0 pairs the custom USB interface with STA for
  duplicate detection. Without this, the two interfaces see each other's
  hostname probes as conflicts and continuously rename the host on a shared LAN.
- Prefers Ethernet once it has an IPv4 address. Saved Wi-Fi remains connected
  as a fallback. Link loss or DHCP lease loss selects Wi-Fi when available;
  cable presence alone cannot displace working Wi-Fi. DNS follows the selected
  interface. A build-local patch to ESP-IDF 5.5.4 also prefers that default
  interface when both interfaces match the destination subnet. Merely setting
  the default route leaves local Tater traffic using whichever interface is
  first in lwIP's list, which was Wi-Fi in our hardware test. Explicit source
  routing, multicast interface selection, and routes to other local subnets
  retain their normal behavior. The shared ESP-IDF installation is not modified.
  A bad upstream Internet connection with a valid Ethernet lease
  does not automatically switch to Wi-Fi.
- Recreates Tater's WebSocket when the selected interface or address changes,
  serialized with pairing credential refresh. Sendspin drops old connections
  with a restart reason and remains discoverable for reconnection. Expect a
  playback interruption during handoff; seamless music migration is unproven.
- Uses the shared setup page with an optional Wi-Fi SSID in this build.
  An Ethernet-only setup still needs the Tater server and pairing code. The
  existing forget-network action clears Ethernet-only selection too.
- Reports `network.active_interface`, `wifi_ip`, `ethernet_ip`, `ethernet_link`,
  `generation`, and `usb_ethernet_state` in native status messages. This provides
  diagnostics over Wi-Fi/Tater when USB serial is unavailable.

The adapter supplies PoE-derived power itself. Firmware does not implement
PoE negotiation, drive a VBUS supply, change SAT1's USB-PD policy, or request a
higher voltage from this 5 V adapter. Adequate power at speaker volume must be
measured on hardware. ESP32-S3's USB connection is Full Speed; the adapter's
100 Mbps Ethernet rating does not make the ESP USB link run at that rate.

## Build and recover

```sh
platformio run -e sat1_usb_ethernet
platformio run -e voicepe_usb_ethernet
```

Generated prototype images (select the matching board):

```text
.pio/build/sat1_usb_ethernet/firmware.bin
.pio/build/sat1_usb_ethernet/firmware.factory.bin
.pio/build/voicepe_usb_ethernet/firmware.bin
.pio/build/voicepe_usb_ethernet/firmware.factory.bin
```

Each environment inherits its board's identity, partitions, flash size, and
XMOS payload. `TATER_USB_ETHERNET=ON` adds the SDK configuration overlay without
changing the ordinary `sat1`, `voicepe`, `respeaker_xvf3800`, or `s3_box` builds.
The version is unchanged; identify this prototype by its environment and status
fields. It has not been published as a release.

The USB OTG controller shares its internal PHY with USB Serial/JTAG. At startup,
this build checks for a PC's USB SOF packets for up to 500 ms. A detected PC keeps
USB Serial/JTAG for the entire boot (`console_preserved`) and disables Ethernet
hosting until the next boot. This works even when no serial terminal is open.
With Ethernet-only settings, that condition also opens the setup AP so Wi-Fi
can be configured. A USB startup error opens setup if no fallback SSID exists.

When hosting the adapter, the same USB port cannot also provide normal serial
logging. To recover, disconnect the adapter, connect the ESP32 USB port directly
to the computer, and reset/cold boot. If needed, use the board's physical BOOT/reset
procedure to enter the ROM download loader and flash its ordinary production image.
No USB eFuses are changed. Plugging a PC into an already-running USB-host boot
is not a console recovery procedure.

When flashing later, explicitly identify the target board's serial port. Do not use automatic
port selection while the Echo or another satellite is connected. A regular
production OTA image removes this experimental host support; an Ethernet-only setup
will then need Wi-Fi setup or the prototype reflashed over USB.

## Software verification

```sh
# Downloads/builds the pinned components used by the boundary test.
platformio run -e sat1_usb_ethernet
bash main/scripts/test_usb_ethernet_host.sh
bash main/scripts/test_usb_ecm_boundary_host.sh
bash main/scripts/test_usb_mdns_pair_host.sh
bash main/scripts/test_usb_ipv4_route_host.sh
bash main/scripts/test_identity_host.sh
bash main/scripts/test_network_recovery_host.sh
bash main/scripts/test_sat1_pd_policy_host.sh
platformio run -e voicepe -e sat1 -e respeaker_xvf3800 -e s3_box
```

The USB host test compiles the production startup, event handling, and routing
code against a fault-injecting IDF facade with AddressSanitizer and
UndefinedBehaviorSanitizer. It exercises PC detection (including delayed
connection), adapter/configuration filtering, Wi-Fi and Ethernet address
changes, duplicate link notifications, lease loss with a cable still present,
physical link loss, reconnect without stale addresses, and every fallible
startup stage. Failure cases check that driver, interface, handler, and host
resources unwind. Identity tests cover both build modes, Ethernet-only setup
persistence, and returning to provisioning while retaining pairing credentials.
The boundary test applies the actual build patch twice to a temporary copy of
the resolved CDC source, compiles its transfer allocator in both build modes,
and models a USB device receiving 63, 64, 65, 128, 1472, and 1514 byte frames.
It checks that the prototype preserves each frame boundary while ordinary CDC
behavior remains unchanged. It needs the managed components downloaded by a
PlatformIO build. These tests do not execute the real USB controller or ECM driver.

The mDNS regression runs the patched component's real interface-pair lookup
and IPv4 conflict detector with both SAT1 addresses. It distinguishes our own
probe from a real competing host and checks unregister cleanup. The IPv4
regression runs the SDK's actual destination and source routing functions with
Wi-Fi first in the interface list, covering Ethernet preference, missing DHCP,
link loss, fallback, explicit source addresses, multicast, loopback, and the
setup AP subnet. Both patches are applied twice to verify repeatable builds;
both tests run with the experimental option enabled and disabled under ASan/UBSan.

## Initial hardware results — November 21, 2026

SAT1 with the UCTRONICS adapter and data converter passed these checks:

- Direct USB app-only flash preserved saved Wi-Fi and confirmed PC console
  protection. A private backup of the previous native-0.6.8 app was retained.
- Ethernet received `10.4.20.159`; Wi-Fi remained at `10.4.20.19`. Thirty-six
  ping requests across 22-, 86-, and 1472-byte payloads all returned.
- Testing exposed and fixed the mDNS self-conflict and shared-subnet routing
  bugs described above. Tater's actual WebSocket peer is now the Ethernet
  address, not just a reported preferred-interface field.
- A downloaded 16 kHz mono WAV was fetched from the Ethernet address and
  played completely. A fresh spoken request and full Tater reply also passed,
  with zero reported audio drops, overruns, or send failures.
- Cold boot through the adapter with the USB-C connector reversed reconnected
  to Tater from the Ethernet address. Discovery retained `sat1-da9a34.local`.
- The prototype and ordinary SAT1 build passed after these fixes. The other
  three ordinary board builds passed during prototype preparation. Voice PE's
  separate USB Ethernet checks are below; ReSpeaker and S3 Box remain untested
  with the adapter.

The tested app is `sat1-usb-ethernet-routing-fix.bin`, SHA-256
`b7bf65f4868410359810f97046299a5c774bbba65fcc55f2964cb1772a88a69e`.
Local test records are under `out/usb-ethernet-hardware/`. Boot/setup backups
in that directory contain private configuration and must never be published.
The connected Echo was not opened, reset, or flashed.

### Voice PE results — November 21, 2026

Voice PE `voicepe-9e70ec` was backed up and flashed with its own app image;
the saved Wi-Fi and Tater pairing remained intact. The PC console guard
reported `console_preserved`, and XMOS stayed on the installed 1.3.2 firmware.

Through the same powered UCTRONICS adapter and data converter:

- Tater's actual connection source was Ethernet `10.4.20.159`, while fallback
  Wi-Fi retained `10.4.20.88`.
- All 36 ping requests across 22-, 86-, and 1472-byte payloads returned.
- Discovery retained `voicepe-9e70ec.local` with both local interface addresses.
- A 16 kHz mono speech WAV was fetched from the Ethernet address and heard
  completely. A normal voice request and complete Tater reply also passed.
- After the conversation, audio drops, overruns, timeouts, and send failures
  were zero. No coredump was present; XMOS reported 1.3.2 without an update.
- Cold start with the USB-C connector reversed reconnected to Tater from the
  Ethernet address with a power-on reset and no coredump.

The app is `voicepe-usb-ethernet-experimental.bin`, SHA-256
`96fcf5664cb156ff71458f7a569903578cb4fd31ee414765d6d5e7276433db1e`.
Private backups and local test records are under
`out/voicepe-usb-ethernet-hardware/`; generated `out/` content is Git-ignored.

Remaining acceptance for both prototypes includes a sustained voice/music and power soak,
Ethernet-only provisioning, missing DHCP, and live failover with board power
retained. A PoE power cycle proves cold-start recovery rather than live
failover. This prototype has not been released.

## Hardware acceptance checklist

1. Keep Echo connected and untouched. Use the existing USB flasher's
   `--app-image` mode with an explicitly identified SAT1 serial port to preserve
   setup data. Factory images overwrite setup data even with `--no-erase`.
   Flash while directly connected to the computer, then confirm the console
   remains accessible and the saved Wi-Fi connection still works.
2. Cold boot SAT1 through the powered adapter and data converter. Verify USB
   enumeration, ECM configuration, DHCP, a nonzero `ethernet_ip`, and
   `active_interface=usb_ethernet`. Check both USB-C plug orientations.
3. Prove **transmit and receive**: pair/connect to Tater, trigger a wake word,
   complete several spoken requests, download/play a remote WAV or MP3, and
   stream Sendspin music. DHCP success or link LEDs alone are insufficient.
4. Check Ethernet-only setup with no SSID, mDNS resolution (`tater.local`),
   long playback, speaker volume, supply stability, free internal heap,
   and audio underrun/transport error counters.
5. Test missing DHCP and Ethernet link loss with valid fallback Wi-Fi. Keep the
   board powered independently through the adapter's supported auxiliary power
   input or a fixture that preserves PoE while interrupting data. Removing the
   only PoE cable cuts power and tests cold-start recovery, not live fallback.
   Confirm Tater and Sendspin reconnect on Wi-Fi and again when Ethernet returns.
6. Test repeated cold starts, adapter reconnect, PC console recovery, and rollback
   to ordinary SAT1 firmware. Complete an extended voice/music soak before
   enabling this feature in any release.

Shared ESP32-S3 networking code can be reused on the other native boards, but
USB-C wiring, power roles, and audio performance must be checked separately.
Passing one board does not certify another board or a different PoE adapter.
