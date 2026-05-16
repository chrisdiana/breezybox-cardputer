#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


PARTITION_TABLE_OFFSET = 0x8000
APP_OFFSET = 0x10000
VFS_OFFSET = 0x670000
VFS_SIZE = 0x080000
BOOTLOADER_OFFSET = 0x0
CHIP = "esp32s3"


def run(cmd: list[str]) -> None:
    subprocess.run(cmd, check=True)


def run_merge_bin(cmd_prefix: list[str], merge_args: list[str]) -> None:
    for op in ("merge-bin", "merge_bin"):
        try:
            subprocess.run(cmd_prefix + [op] + merge_args, check=True)
            return
        except subprocess.CalledProcessError as exc:
            if op == "merge_bin":
                raise exc


def parse_sdkconfig(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw_line in path.read_text().splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key] = value
    return values


def main() -> int:
    parser = argparse.ArgumentParser(description="Build a Launcher-compatible BreezyBox package")
    parser.add_argument("--build-dir", required=True, help="ESP-IDF build directory")
    parser.add_argument("--firmware-dir", required=True, help="breezybox-firmware directory")
    parser.add_argument("--out", help="Output .bin path")
    args = parser.parse_args()

    build_dir = Path(args.build_dir).resolve()
    firmware_dir = Path(args.firmware_dir).resolve()

    idf_path_env = os.environ.get("IDF_PATH")
    if not idf_path_env:
        raise SystemExit("IDF_PATH is not set. Activate ESP-IDF first.")
    idf_path = Path(idf_path_env).expanduser().resolve()

    sdkconfig = build_dir / "sdkconfig"
    app_bin = build_dir / "breezybox_cardputer.bin"
    bootloader_bin = build_dir / "bootloader" / "bootloader.bin"
    launcher_csv = firmware_dir / "partitions.launcher.8mb.csv"
    launcher_part_bin = build_dir / "launcher-partition-table.bin"
    launcher_vfs_bin = build_dir / "launcher-vfs.bin"
    fs_dir = firmware_dir / "fs_image"

    out_path = Path(args.out).resolve() if args.out else build_dir / "breezybox-launcher.bin"

    for required in (sdkconfig, app_bin, bootloader_bin, launcher_csv, fs_dir):
        if not required.exists():
            raise SystemExit(f"missing required input: {required}")

    sdk = parse_sdkconfig(sdkconfig)

    partgen = idf_path / "components" / "partition_table" / "gen_esp32part.py"
    fatfsgen = idf_path / "components" / "fatfs" / "wl_fatfsgen.py"
    for required in (partgen, fatfsgen):
        if not required.exists():
            raise SystemExit(f"missing required IDF tool: {required}")

    run([sys.executable, str(partgen), str(launcher_csv), str(launcher_part_bin)])

    fatfs_cmd = [
        sys.executable,
        str(fatfsgen),
        str(fs_dir),
        "--long_name_support",
        "--partition_size",
        str(VFS_SIZE),
        "--output_file",
        str(launcher_vfs_bin),
        "--sector_size",
        sdk.get("CONFIG_WL_SECTOR_SIZE", "4096"),
    ]
    run(fatfs_cmd)

    run_merge_bin(
        [
            sys.executable,
            "-m",
            "esptool",
            "--chip",
            CHIP,
        ],
        [
            "--output",
            str(out_path),
            hex(BOOTLOADER_OFFSET),
            str(bootloader_bin),
            hex(PARTITION_TABLE_OFFSET),
            str(launcher_part_bin),
            hex(APP_OFFSET),
            str(app_bin),
            hex(VFS_OFFSET),
            str(launcher_vfs_bin),
        ],
    )

    print(f"Launcher package written to {out_path}")
    print("Install it through Launcher. This package carries the /root FAT image in the vfs partition.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
