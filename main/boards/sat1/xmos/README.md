# Sat1 XMOS Firmware

Sat1 embeds the XMOS factory image so native firmware can bring new devices to
the audio firmware expected by Tater.

Included image:

```text
sat1_xmos_1_1_1_factory.bin
```

Lab-only raw capture image (not embedded in the ESP firmware):

```text
sat1_xmos_1_1_0_raw4_usb_factory.bin
```

Target version:

```text
1.1.1
```

Source for rebuilding the image is kept in:

```text
source/
```

To rebuild, materialize the XMOS dependencies and run:

```bash
cd main/boards/sat1/xmos/source
bash scripts/fetch_dependencies.sh
./build_sat1_fixed_delay.sh
cp build/satellite1_firmware_fixed_delay.factory.bin ../sat1_xmos_1_1_1_factory.bin
cp build/satellite1_firmware_fixed_delay.factory.md5 ../sat1_xmos_1_1_1_factory.md5
```

The included four-channel USB capture image remains at 1.1.0 because it is a
lab-only testing and calibration tool, not an OTA payload. To rebuild it after
the production build has configured the workspace:

```bash
XMOS_CLEAN_BUILD=0 SAT1_XMOS_TARGET=satellite1_usb_firmware_raw4 ./build_sat1_fixed_delay.sh
cp build/satellite1_usb_firmware_raw4.factory.bin ../sat1_xmos_1_1_1_raw4_usb_factory.bin
cp build/satellite1_usb_firmware_raw4.factory.md5 ../sat1_xmos_1_1_1_raw4_usb_factory.md5
```

The build script expects XMOS XTC Tools. Set `XMOS_TOOL_PATH` if XTC is not
installed at the default path.
