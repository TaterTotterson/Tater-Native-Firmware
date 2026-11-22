# USB Ethernet and PoE

Native firmware **0.7.0**, release build **`native-0.7.0-rev1`**, includes USB
Ethernet in all four standard images: SAT1, Voice PE, ReSpeaker XVF3800, and
ESP32-S3-BOX-3. SAT1 and Voice PE have passed the hardware checks below.
ReSpeaker and S3 Box include the same shared driver but have not yet been
tested with the adapter; their USB wiring, power delivery, and voice behavior
remain unverified on hardware.

## Tested adapter and alternatives

Use the powered [UCTRONICS B0B9ZLVGJ4 (U6113)](https://www.amazon.com/dp/B0B9ZLVGJ4),
the same adapter tested with [Tater Echo Firmware](https://github.com/TaterTotterson/Tater-Echo-Firmware#usb-ethernet-adapters).
For these USB-C boards, use a **data-capable Micro-USB female → USB-C male converter**.
The adapter supplies both 5 V power and Ethernet data over USB. A compatible
PoE switch/injector powers it; its separate power input can also power the adapter.
Use the board's **ESP32 USB port** (the XIAO USB port on ReSpeaker), not a
separate XMOS USB audio port. Check the exact board's connector and power
requirements before trying an untested setup.

The current firmware accepts only USB VID:PID **`0bda:8152`**, selecting its
**CDC-ECM configuration 2**. Other powered RTL8152 adapters exposing that same
identity and configuration may work, but are untested. Product revisions can
change the USB chip or descriptors. USB-C, higher wattage, or an Ethernet socket
alone does not establish compatibility. Other chipsets are not enabled, even
if a computer or another ECM driver supports them. A power-only PoE splitter
can power a board but cannot provide its Ethernet connection.

Espressif lists RTL8152B among its tested ESP32-S3 ECM devices in the
[ECM host documentation](https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_host/usb_ecm.html).
That driver-level list does not qualify arbitrary adapters for this firmware.
ESP32-S3 uses USB Full Speed (12 Mbps signaling); the adapter's 100 Mbps Ethernet
rating does not make the USB link run at that rate. Actual throughput is lower.

The adapter negotiates PoE and supplies power. Firmware does not negotiate PoE,
request a higher voltage, drive host VBUS power, or change SAT1's USB-PD policy.
Choose a supply appropriate to the board and speaker volume. This 5 V setup
does not provide SAT1's higher-voltage amplifier modes.

## Connect and check

1. Install the matching 0.7.0 or newer board image using Tater OTA or the USB
   flasher. Identify the exact board when multiple satellites are connected.
   A factory flash overwrites setup data; use the app/OTA image to retain it.
2. Configure the satellite's Tater server and pairing. Saved Wi-Fi can remain
   configured as fallback. The setup page also accepts Ethernet-only setup
   with a blank Wi-Fi SSID; that path has host regression coverage but has not
   yet been checked on hardware.
3. Disconnect USB from the computer. Connect the powered adapter and data
   converter, then **cold boot through the adapter**.
4. Wait for DHCP and the Tater connection. In Tater **3.0.8 or later**, the
   satellite's diagnostics should show **Connected by USB Ethernet** on its
   normal refresh. Network status also reports `active_interface=usb_ethernet`,
   `ethernet_ip`, `wifi_ip`, `ethernet_link`, and `usb_ethernet_state`.
5. Make a spoken request and confirm the complete reply. DHCP and link LEDs
   alone do not prove working bidirectional voice traffic.

Ethernet takes priority after it has an IPv4 address. Saved Wi-Fi remains
connected and is selected if the Ethernet link or lease is lost. DNS follows
the selected interface. A bad upstream Internet connection with a valid
Ethernet lease does not automatically select Wi-Fi. A handoff recreates the
Tater WebSocket and restarts Sendspin connections; playback may be interrupted.

## USB console and recovery

The ESP32-S3 USB host and USB Serial/JTAG share a PHY. At boot, firmware checks
for a computer's USB SOF packets for up to 500 ms. A detected computer preserves
the console for that entire boot (`console_preserved`); Ethernet requires a
subsequent boot through the adapter. This works without an open serial terminal.
When no Wi-Fi is saved, console preservation or a USB startup error opens setup.

While hosting Ethernet, the same USB port cannot provide normal serial logging.
For recovery, disconnect the adapter, connect the ESP32 USB port directly to a
computer, and reset/cold boot. Use the board's BOOT/reset procedure for ROM
flashing if needed. No USB eFuses are changed. Plugging a computer into an
already-running USB-host boot does not restore the console.

## Verification

SAT1 and Voice PE were tested separately with the adapter and converter on
November 21, 2026. Both passed:

- App-only USB flashing with saved Wi-Fi and Tater pairing retained, and the
  direct-computer console guard working.
- DHCP with simultaneous Wi-Fi, plus 36 successful ping requests per board
  across 22-, 86-, and 1472-byte payloads.
- Tater's actual WebSocket using the Ethernet source address, and stable mDNS
  hostnames while both interfaces shared a LAN.
- A downloaded 16 kHz mono speech WAV played in full, followed by a normal
  spoken request and complete Tater reply. Reported audio drops and send
  failures were zero.
- Cold startup through the adapter with the USB-C connector reversed, followed
  by reconnection to Tater over Ethernet. Existing XMOS firmware was retained.

Extended voice/Sendspin music and power soak, Ethernet-only provisioning,
missing DHCP, and live failover with board power retained remain unverified on
hardware. Removing the sole PoE supply checks cold-start recovery rather than
live fallback. Passing one board does not qualify another board or adapter.

## Build and regression details

```sh
platformio run -e voicepe -e sat1 -e respeaker_xvf3800 -e s3_box
for test in main/scripts/test_*_host.sh; do bash "$test"; done
```

All four standard environments enable `TATER_USB_ETHERNET=ON` and the SDK defaults overlay.
Development aliases `sat1_usb_ethernet` and `voicepe_usb_ethernet` inherit the
same release configuration. Board identity, flash layout, audio, and XMOS
payload remain specific to each target.

Pinned dependencies include `iot_usbh_ecm` 0.4.0, CDC 3.1.1, mDNS 1.14.0,
and ESP-IDF 5.5.4. Guarded build patches fix three issues exposed in testing:

- Terminate aligned bulk OUT Ethernet frames with a USB short packet, so
  adjacent frames cannot merge at 64-byte boundaries.
- Pair the custom USB and Wi-Fi mDNS interfaces for self-probe detection,
  preventing repeated hostname conflict renaming on the same LAN.
- Prefer the usable default interface when Wi-Fi and Ethernet share a subnet;
  setting the default alone left local traffic on Wi-Fi. The SDK source is
  copied and patched per build, leaving the shared framework unchanged.

Host tests use ASan/UBSan and enabled/disabled configurations to exercise startup
failure cleanup, filtering, address and link changes, routing, persistence,
actual patched CDC allocation, mDNS conflict detection, and SDK IPv4 routing.
These checks do not execute the physical USB controller. Build first so the
pinned component sources are available; set `IDF_PATH` when using a non-default
PlatformIO core directory.
