"""Exercise actual patched mDNS pairing and A-record conflict detection."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

PROJECT = Path(__file__).resolve().parents[2]
component = PROJECT / "managed_components/espressif__mdns"
if not component.exists():
    raise SystemExit("Build sat1_usb_ethernet first to download the pinned mDNS component")
patch = PROJECT / "patches/mdns-1.14.0-usb-ethernet-pair.patch"
with tempfile.TemporaryDirectory(prefix="tater-usb-mdns-") as directory:
    root = Path(directory)
    driver = root / "driver"
    driver.mkdir()
    shutil.copy2(component / "mdns_netif.c", driver / "mdns_netif.c")
    reverse = ["git", "apply", "--reverse", "--directory=driver", str(patch)]
    if subprocess.run(reverse[:3] + ["--check"] + reverse[3:], cwd=root, capture_output=True).returncode == 0:
        subprocess.run(reverse, cwd=root, check=True)
    command = [os.environ.get("CMAKE", "cmake"), f"-DUSB_ETH_PROJECT_DIR={root}",
               f"-DUSB_ETH_COMPONENT_DIR={driver}", f"-DUSB_ETH_PATCH={patch}",
               f"-DUSB_ETH_PATCH_STAMP={root / 'patch.stamp'}", "-P",
               str(PROJECT / "cmake/ApplyUsbEthernetPatch.cmake")]
    for _ in range(2):
        subprocess.run(command, check=True, capture_output=True)

    def function(source, signature):
        start = source.index(signature)
        return source[start:source.index("\n}\n", start) + 3]

    lookup = function((driver / "mdns_netif.c").read_text(),
                      "mdns_if_t mdns_priv_netif_get_other_interface(")
    collision = function((component / "mdns_receive.c").read_text(),
                         "static int check_a_collision(")
    harness = (PROJECT / "main/tests/test_usb_mdns_pair_host.c").read_text()
    harness = harness.replace("/* ACTUAL_PAIR_LOOKUP */", lookup)
    harness = harness.replace("/* ACTUAL_COLLISION_CHECK */", collision)
    (root / "test.c").write_text(harness)
    for enabled in (0, 1):
        executable = root / f"test-mdns-{enabled}"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", "-fsanitize=address,undefined",
                        f"-DCONFIG_TATER_USB_ETHERNET={enabled}", str(root / "test.c"),
                        "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
