# ReSpeaker XVF3800 Host Reference

This folder preserves the old Tater ReSpeaker XVF3800 host-side component for
reference while we maintain the native firmware implementation.

It includes the ESPHome component files that described:

- XVF3800 I2C DFU update flow
- firmware version reads
- mute/GPO reads and writes
- LED ring writes
- azimuth reads
- fixed-beam lock/unlock commands

The reference source came from the old Tater ESPHome firmware tree:

```text
/Users/ahphooey/Scripts/microWakeWords/respeaker_xvf3800/components/respeaker_xvf3800/
```

Its original vendored note is preserved in:

```text
respeaker_xvf3800_esphome_component/UPSTREAM_COMPONENT_README.md
```

Do not include this folder in native firmware builds. When updating native
ReSpeaker behavior, port the relevant protocol detail into:

```text
main/boards/respeaker_xvf3800/audio_respeaker_xvf3800.c
```
