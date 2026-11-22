"""Exercise the actual SDK routing functions with Wi-Fi first in netif_list."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

PROJECT = Path(__file__).resolve().parents[2]
idf = Path(os.environ.get("IDF_PATH", Path.home() / ".platformio/packages/framework-espidf"))
component = idf / "components/lwip"
source_path = component / "lwip/src/core/ipv4/ip4.c"
if not source_path.exists():
    raise SystemExit("Install/build the pinned ESP-IDF framework first, or set IDF_PATH")
with tempfile.TemporaryDirectory(prefix="tater-usb-ipv4-") as directory:
    root = Path(directory)
    driver = root / "driver"
    driver.mkdir()
    shutil.copy2(source_path, driver / "ip4.c")
    command = [os.environ.get("CMAKE", "cmake"), f"-DUSB_ETH_PROJECT_DIR={root}",
               f"-DUSB_ETH_COMPONENT_DIR={driver}",
               f"-DUSB_ETH_PATCH={PROJECT / 'patches/esp-idf-5.5.4-usb-ethernet-route.patch'}",
               f"-DUSB_ETH_PATCH_STAMP={root / 'patch.stamp'}", "-P",
               str(PROJECT / "cmake/ApplyUsbEthernetPatch.cmake")]
    for _ in range(2):
        subprocess.run(command, check=True, capture_output=True)

    def function(source, name):
        start = source.index("\n" + name + "(") + 1
        return "struct netif *\n" + source[start:source.index("\n}\n", start) + 3]

    source = (driver / "ip4.c").read_text()
    harness = (PROJECT / "main/tests/test_usb_ipv4_route_host.c").read_text()
    harness = harness.replace("/* ACTUAL_IPV4_ROUTE */", function(source, "ip4_route"))
    harness = harness.replace("/* ACTUAL_SOURCE_ROUTE */", function(source, "ip4_route_src"))
    harness = harness.replace("/* ACTUAL_SOURCE_HOOK */", function(
        (component / "port/hooks/lwip_default_hooks.c").read_text(), "ip4_route_src_hook"))
    (root / "test.c").write_text(harness)
    for enabled in (0, 1):
        executable = root / f"test-ipv4-{enabled}"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-fsanitize=address,undefined",
                        f"-DCONFIG_TATER_USB_ETHERNET={enabled}", str(root / "test.c"),
                        "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
