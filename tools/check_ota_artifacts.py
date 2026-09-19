#!/usr/bin/env python3
"""Validate production OTA artifacts and manifest consistency."""

import argparse
import hashlib
import json
import os
import sys
import zlib


def digest(path):
    sha = hashlib.sha256()
    crc = 0
    size = 0
    with open(path, "rb") as stream:
        while chunk := stream.read(65536):
            size += len(chunk)
            sha.update(chunk)
            crc = zlib.crc32(chunk, crc)
    return size, crc & 0xFFFFFFFF, sha.hexdigest()


def fail(message):
    print(f"[ERROR] {message}")
    raise SystemExit(1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", required=True)
    parser.add_argument("--bootloader", required=True)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--max-app", type=int, default=120 * 1024)
    parser.add_argument("--max-bootloader", type=int, default=7 * 1024)
    args = parser.parse_args()

    for path in (args.app, args.bootloader, args.manifest):
        if not os.path.isfile(path):
            fail(f"missing artifact: {path}")

    app_size, app_crc, app_sha = digest(args.app)
    boot_size = os.path.getsize(args.bootloader)
    if app_size > args.max_app:
        fail(f"application is {app_size} bytes, limit is {args.max_app}")
    if boot_size > args.max_bootloader:
        fail(f"bootloader is {boot_size} bytes, limit is {args.max_bootloader}")

    with open(args.manifest, "r", encoding="utf-8") as stream:
        manifest = json.load(stream)

    if manifest.get("target_mcu", manifest.get("target")) != "STM32G0B1":
        fail("manifest target_mcu is not STM32G0B1")
    if manifest.get("filename") != os.path.basename(args.app):
        fail("manifest filename does not match application artifact")
    if manifest.get("size") != app_size:
        fail("manifest size does not match application artifact")
    if manifest.get("sha256", "").lower() != app_sha:
        fail("manifest SHA-256 does not match application artifact")

    crc_text = manifest.get("crc32", manifest.get("crc32_hex", ""))
    try:
        manifest_crc = int(crc_text, 16)
    except (TypeError, ValueError):
        fail("manifest CRC32 is invalid")
    if manifest_crc != app_crc:
        fail("manifest CRC32 does not match application artifact")

    print(f"[OK] app={app_size} bytes crc32=0x{app_crc:08X} sha256={app_sha}")
    print(f"[OK] bootloader={boot_size} bytes")


if __name__ == "__main__":
    main()
