"""Exercise the pinned CDC allocator and model ECM USB packet boundaries."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

PROJECT = Path(__file__).resolve().parents[2]
component = PROJECT / "managed_components/espressif__iot_usbh_cdc/iot_usbh_cdc.c"
if not component.exists():
    raise SystemExit("Build sat1_usb_ethernet first to download the pinned CDC component")
with tempfile.TemporaryDirectory(prefix="tater-ecm-boundary-") as directory:
    root = Path(directory)
    driver = root / "driver"
    driver.mkdir()
    shutil.copy2(component, driver / component.name)
    cmake = os.environ.get("CMAKE", "cmake")
    patch_command = [cmake, f"-DUSB_ETH_PROJECT_DIR={root}", f"-DUSB_ETH_COMPONENT_DIR={driver}",
                     f"-DUSB_ETH_PATCH={PROJECT / 'patches/iot-usbh-cdc-3.1.1-ecm-zlp.patch'}",
                     f"-DUSB_ETH_PATCH_STAMP={root / 'patch.stamp'}", "-P",
                     str(PROJECT / "cmake/ApplyUsbEthernetPatch.cmake")]
    # Test clean application and repeated application, even if the build already patched its copy.
    reverse = subprocess.run(["git", "apply", "--reverse", "--check", "--directory=driver",
                              str(PROJECT / "patches/iot-usbh-cdc-3.1.1-ecm-zlp.patch")],
                             cwd=root, capture_output=True)
    if reverse.returncode == 0:
        subprocess.run(["git", "apply", "--reverse", "--directory=driver",
                        str(PROJECT / "patches/iot-usbh-cdc-3.1.1-ecm-zlp.patch")], cwd=root, check=True)
    for _ in range(2):
        subprocess.run(patch_command, check=True, capture_output=True)
    source = (driver / component.name).read_text()
    start = source.index("static esp_err_t _cdc_transfers_allocate(")
    function = source[start:source.index("\n}\n", start) + 3]
    harness = (PROJECT / "main/tests/test_usb_ecm_boundary_host.c").read_text()
    (root / "test.c").write_text(harness.replace("/* ACTUAL_CDC_ALLOCATOR */", function))
    executable = root / "test-boundary"
    for usb_enabled in (0, 1):
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", f"-DCONFIG_TATER_USB_ETHERNET={usb_enabled}",
                        str(root / "test.c"), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
