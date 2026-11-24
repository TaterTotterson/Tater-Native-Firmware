# Sat1 XMOS Firmware

The standard native `sat1` environment embeds the released
`sat1_xmos_1_1_1_factory.bin.zlib` and automatically restores XMOS **1.1.1** when
another version is installed. Its uncompressed factory image and MD5 remain
alongside it for reference and rollback.

This branch also contains newer integration source and three separate test
images. See [INTEGRATION.md](INTEGRATION.md) for provenance, build commands,
version identities, native test environments and the pending hardware matrix.
The source wrapper builds integration candidates; do not rename its output as
1.1.1 or overwrite the production payload.

The lab-only `sat1_xmos_1_1_0_raw4_usb_factory.bin` exposes the physical
microphones independently and remains outside ESP firmware payloads. Rebuilding
that target uses the source wrapper with
`SAT1_XMOS_TARGET=satellite1_usb_firmware_raw4` and the `legacy` configuration.
Its USB target has independent version-generation settings; it is a lab tool,
not one of the native test images.
