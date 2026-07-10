# Sat1 XMOS Firmware

Sat1 embeds the XMOS factory image so native firmware can bring new devices to
the audio firmware expected by Tater.

Included image:

```text
sat1_xmos_1_0_6_factory.bin
```

Target version:

```text
1.0.6
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
cp build/satellite1_firmware_fixed_delay.factory.bin ../sat1_xmos_1_0_6_factory.bin
cp build/satellite1_firmware_fixed_delay.factory.md5 ../sat1_xmos_1_0_6_factory.md5
```

The build script expects XMOS XTC Tools. Set `XMOS_TOOL_PATH` if XTC is not
installed at the default path.
