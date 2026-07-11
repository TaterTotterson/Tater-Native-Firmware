# ReSpeaker XVF3800 XMOS Firmware

The ReSpeaker XVF3800 target embeds the Seeed/XMOS companion firmware image so
native Tater firmware can bring devices to the audio/control firmware expected
by Tater.

Current embedded image:

```text
xvf3800_i2s_1_0_7.bin
```

Target version:

```text
1.0.7
```

The embedded image was copied from the old Tater ESPHome firmware artifact:

```text
shared/assets/firmware/respeaker_xvf3800/application_xvf3800_inthost-lr48-sqr-i2c-v1.0.7-release.bin
```

It matches that artifact with:

```text
sha256 d1132b7e779818923072396315304a5dd2324aa56969b4111b30678f968a7c6a
md5    043a848f544ff2c7265ac19685daf5de
```

## Source Status

Unlike Voice PE and Sat1, the old ReSpeaker tree did not contain a full editable
XMOS firmware source checkout. It only contained the prebuilt XVF3800 binary and
an ESPHome host component that talks to the XVF firmware over I2C.

The host component has been preserved as protocol reference in:

```text
host_reference/respeaker_xvf3800_esphome_component/
```

That code is not built into Tater native firmware. The active native
implementation lives in:

```text
main/boards/respeaker_xvf3800/audio_respeaker_xvf3800.c
```

If we later need to modify the XVF3800 XMOS application itself, add the editable
XMOS source tree under this directory as:

```text
source/
```

and keep generated build outputs ignored, matching the Voice PE and Sat1 layout.
