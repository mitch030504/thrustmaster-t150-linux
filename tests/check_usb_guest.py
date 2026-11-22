#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Test a capture-backed virtual T150 with a distribution kernel in QEMU."""

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

PROJECT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("release", help="Installed kernel release with headers and USB Raw Gadget")
    release = parser.parse_args().release
    kernel = Path("/usr/lib/modules") / release
    with tempfile.TemporaryDirectory(prefix="t150-usb-guest-") as scratch:
        guest = Path(scratch)
        for name in ("sys", "dev"):
            (guest / name).mkdir()
        subprocess.run(["cc", "-static", "-pthread", str(PROJECT / "tests/usb_guest.c"),
                        "-o", str(guest / "init")], check=True)
        subprocess.run(["make", "-f", "dkms_make.mak", f"KDIR={kernel}/build", "all"],
                       cwd=PROJECT, check=True)
        shutil.copyfile(PROJECT / "build/hid-t150.ko", guest / "hid-t150.ko")
        shutil.copyfile(PROJECT / "traffic/old_caps/hid_report_fw35", guest / "report")
        for name in ("udc-core", "dummy_hcd", "raw_gadget"):
            filename = subprocess.check_output(["modinfo", "-k", release, "-F", "filename", name],
                                               text=True).strip()
            with (guest / f"{name}.ko").open("wb") as output:
                subprocess.run(["zstd", "-dc", filename], stdout=output, check=True)
        names = "\n".join(path.name for path in guest.iterdir()) + "\n"
        with (guest / "initramfs").open("wb") as output:
            subprocess.run(["cpio", "--quiet", "-o", "-H", "newc"], cwd=guest,
                           input=names.encode(), stdout=output, check=True)
        result = subprocess.run([
            "qemu-system-x86_64", "-accel", "kvm", "-cpu", "host", "-m", "512",
            "-nodefaults", "-display", "none", "-serial", "stdio", "-nic", "none",
            "-no-reboot", "-kernel", str(kernel / "vmlinuz"),
            "-initrd", str(guest / "initramfs"), "-append", "console=ttyS0 rdinit=/init loglevel=5",
        ], capture_output=True, text=True, timeout=25, check=True)
        print(result.stdout, end="")
        required = ["OUT 43 ff ff", "OUT 40 11 ff ff", "OUT 03 0e 00 04",
                    "OUT 01 00 00 40 f4 01", "OUT 01 08 00 40 f4 01",
                    "OUT 41 00 41 01", "OUT 41 00 00 01", "T150_GUEST_USB_OK"]
        position = 0
        for line in required:
            found = result.stdout.find(line, position)
            if found < 0:
                raise SystemExit(f"Missing or out-of-order guest output: {line}")
            position = found + len(line)


if __name__ == "__main__":
    main()
