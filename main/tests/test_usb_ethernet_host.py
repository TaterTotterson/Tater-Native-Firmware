"""Run production startup/event code against a fault-injecting IDF facade."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

MAIN = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="tater-usb-ethernet-") as directory:
    root = Path(directory)
    shutil.copy2(MAIN / "tests/usb_ethernet_stubs/fake_idf.h", root / "fake_idf.h")
    headers = ["sdkconfig.h", "mdns.h", "esp_err.h", "esp_event.h", "esp_intr_alloc.h", "esp_log.h",
               "esp_netif.h", "esp_wifi.h", "driver/usb_serial_jtag.h", "freertos/FreeRTOS.h",
               "freertos/task.h", "iot_eth.h", "iot_eth_netif_glue.h", "iot_usbh_cdc.h",
               "iot_usbh_ecm.h", "usb/usb_host.h"]
    for header in headers:
        path = root / header
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "fake_idf.h"\n')
    executable = root / "test-usb-ethernet"
    command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(root), "-I", str(MAIN)]
    command += [str(MAIN / name) for name in ("network_state.c", "network_manager.c", "usb_ethernet.c", "tests/test_usb_ethernet_host.c")]
    subprocess.run(command + ["-o", str(executable)], check=True)
    for scenario in ("console", "late_console", "success", "host", "cdc", "new_ecm", "eth_install",
                     "netif", "glue", "attach", "mdns_init", "mdns_register", "eth_events", "ip_events", "eth_start", "task"):
        subprocess.run([str(executable), scenario], check=True)
