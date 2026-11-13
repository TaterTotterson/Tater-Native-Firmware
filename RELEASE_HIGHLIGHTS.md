- Releases native firmware `0.6.3` for Voice PE, Satellite1, ReSpeaker XVF3800,
  and ESP32-S3-BOX-3.
- Fixes startup stack overflows when expanding and installing bundled XMOS
  firmware on Voice PE, Satellite1, and ReSpeaker XVF3800. The complete update
  runs in a temporary 16 KiB PSRAM task, keeping internal RAM available for
  speech playback. Already-current XMOS firmware still skips updating.
- Shares the update worker across all three XMOS boards, with safe task and
  semaphore cleanup, allocation/error handling, and minimum free stack logging.
- Corrects Voice PE's update-required and DFU status after a successful update.
- Waits for ReSpeaker's XMOS reboot before verifying the installed firmware
  and initializing runtime controls, preventing premature verification and
  control requests during the restart.
- Hardware-validated complete same-version XMOS programming on all three
  affected boards. Voice PE also passed online speech and wake checks;
  Satellite1 and ReSpeaker passed USB-only speech and microphone checks.
  Normal firmware was restored afterward, with saved configuration preserved.
  Voice PE and ReSpeaker also passed physical power-cycle checks.
- ESP32-S3-BOX-3 has no XMOS updater; its updater behavior is unchanged.
- **Contributor credit:** Thanks to [@lloydc99](https://github.com/lloydc99)
  for reproducing the Voice PE failure, identifying the internal-RAM playback
  regression from a larger main stack, and contributing the original dedicated
  PSRAM update task in [PR #24](https://github.com/TaterTotterson/Tater-Native-Firmware/pull/24).
  Their original commit and authorship are preserved in this release.
