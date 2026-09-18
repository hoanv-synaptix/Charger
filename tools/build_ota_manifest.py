#!/usr/bin/env python3
"""
build_ota_manifest.py -- Calculate CRC32, size, and generate ota_manifest.json
Matches the IEEE 802.3 CRC32 polynomial (0xEDB88320) used in ota_service.c.
"""

import sys
import os
import json
import zlib
import argparse
import re

def compute_crc32(filepath):
    """Compute standard IEEE 802.3 CRC32 matching ota_service.c"""
    crc = 0
    with open(filepath, 'rb') as f:
        while chunk := f.read(65536):
            crc = zlib.crc32(chunk, crc)
    return crc & 0xFFFFFFFF

def extract_version_from_header(header_path):
    """Extract version string from app_version.h if available"""
    if not os.path.exists(header_path):
        return "1.0.0", 0x010000
    content = open(header_path, 'r', encoding='utf-8', errors='ignore').read()
    
    major = 1
    minor = 0
    patch = 0
    
    m_maj = re.search(r'#define\s+(?:APP|FW)_VERSION_MAJOR\s+(\d+)', content)
    m_min = re.search(r'#define\s+(?:APP|FW)_VERSION_MINOR\s+(\d+)', content)
    m_pat = re.search(r'#define\s+(?:APP|FW)_VERSION_PATCH\s+(\d+)', content)
    
    if m_maj: major = int(m_maj.group(1))
    if m_min: minor = int(m_min.group(1))
    if m_pat: patch = int(m_pat.group(1))
    
    ver_str = f"{major}.{minor}.{patch}"
    ver_code = (major << 16) | (minor << 8) | patch
    return ver_str, ver_code

def main():
    parser = argparse.ArgumentParser(description="Generate OTA Manifest for Charger Firmware")
    parser.add_argument("--bin", default="build/Release/Charger.bin", help="Path to Charger.bin")
    parser.add_argument("--version", default=None, help="Version string (e.g. 1.0.0 or v1.0.0)")
    parser.add_argument("--out", default="build/Release/ota_manifest.json", help="Output JSON path")
    parser.add_argument("--header", default="App/Charge/app_version.h", help="Path to app_version.h")
    args = parser.parse_args()

    if not os.path.exists(args.bin):
        print(f"[ERROR] Firmware binary not found: {args.bin}")
        sys.exit(1)

    file_size = os.path.getsize(args.bin)
    crc32_val = compute_crc32(args.bin)

    if args.version:
        v_clean = args.version.lstrip('v')
        parts = [int(p) for p in v_clean.split('.') if p.isdigit()]
        major = parts[0] if len(parts) > 0 else 1
        minor = parts[1] if len(parts) > 1 else 0
        patch = parts[2] if len(parts) > 2 else 0
        ver_str = f"{major}.{minor}.{patch}"
        ver_code = (major << 16) | (minor << 8) | patch
    else:
        ver_str, ver_code = extract_version_from_header(args.header)

    manifest = {
        "target": "STM32G0B1",
        "filename": os.path.basename(args.bin),
        "version": ver_str,
        "version_code": ver_code,
        "size": file_size,
        "crc32_hex": f"0x{crc32_val:08X}",
        "crc32_dec": crc32_val
    }

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, 'w', encoding='utf-8') as f:
        json.dump(manifest, f, indent=2)

    print("========================================")
    print("         OTA MANIFEST GENERATED         ")
    print("========================================")
    print(f"Target      : {manifest['target']}")
    print(f"File        : {manifest['filename']} ({file_size:,} bytes)")
    print(f"Version     : {ver_str} (Code: 0x{ver_code:06X})")
    print(f"CRC32       : {manifest['crc32_hex']} ({crc32_val})")
    print(f"Manifest    : {args.out}")
    print("========================================")

if __name__ == "__main__":
    main()
