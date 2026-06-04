#!/usr/bin/env python3

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


PARTITION_TABLE_OFFSET = 0x8000
APP_OFFSET = 0x10000
FS_OFFSET = 0x4F0000
BOOTLOADER_OFFSET = 0x0
CHIP = "esp32s3"


def run(cmd: list[str]) -> None:
    subprocess.run(cmd, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Build a shared BreezyBox install image")
    parser.add_argument("--build-dir", required=True, help="ESP-IDF build directory")
    parser.add_argument("--firmware-dir", required=True, help="breezybox-firmware directory")
    parser.add_argument("--out", help="Output .bin path")
    args = parser.parse_args()

    build_dir = Path(args.build_dir).resolve()
    app_bin = build_dir / "breezybox_cardputer.bin"
    bootloader_bin = build_dir / "bootloader" / "bootloader.bin"
    partition_table_bin = build_dir / "partition_table" / "partition-table.bin"
    littlefs_bin = build_dir / "spiffs.bin"
    out_path = Path(args.out).resolve() if args.out else build_dir / "breezybox-universal.bin"

    for required in (app_bin, bootloader_bin, partition_table_bin, littlefs_bin):
        if not required.exists():
            raise SystemExit(f"missing required input: {required}")

    run([
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        CHIP,
        "merge_bin",
        "--output",
        str(out_path),
        hex(BOOTLOADER_OFFSET),
        str(bootloader_bin),
        hex(PARTITION_TABLE_OFFSET),
        str(partition_table_bin),
        hex(APP_OFFSET),
        str(app_bin),
        hex(FS_OFFSET),
        str(littlefs_bin),
    ])

    print(f"Shared install image written to {out_path}")
    print("Use this .bin for either Launcher installs or direct flashing at offset 0x0.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
