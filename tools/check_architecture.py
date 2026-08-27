#!/usr/bin/env python3
"""
check_architecture.py -- Enforce module dependency direction (AGENTS.md secs 5, 6, 23).

What it does
------------
Scans every .c/.h file under the tracked source directories, classifies each
file into one architectural layer by its path, then checks every local
#include ("...") against a fixed table of which layers that layer is allowed
to depend on. This is a grep-based check on purpose (see AGENTS.md 2.1: no
unnecessary abstraction) -- it does not build a real translation-unit graph.

Layers (see AGENTS.md section 5):
    APP_SYSTEM    App/System   -- composition root, may depend on anything below it
    APP_CHARGE    App/Charge   -- pure charging policy, hardware-agnostic
    APP_PROTOCOL  App/Protocol -- PC/DWIN protocol handling
    MODULES       Modules/*    -- BMS, charger drivers, HMI protocol
    PLATFORM_BSP  BSP/*        -- MCU-specific hardware access
    PLATFORM_USB  USB_Device/App/* -- USB CDC transport (CubeMX user-code section)
    UTILS         Utils/*      -- cross-cutting helpers (logging, ...)
    GENERATED     Core/*, Drivers/*, Middlewares/*, USB_Device/Target/*, cmake/*
                  -- CubeMX/vendor code, must never depend upward (AGENTS.md sec 4)

Baseline
--------
This project has existing violations predating this checker (see
docs/AUDIT_Findings.md and AGENTS.md section 28 "Project Refactor Order").
Rather than block CI on debt nobody asked this change to fix, known
violations are listed in tools/arch_baseline.txt. Any violation not in the
baseline is NEW and fails the build. Fixing a violation and removing it from
the baseline is expected and encouraged; adding a new baseline entry should
be rare and requires a comment explaining why.
"""
import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BASELINE_PATH = Path(__file__).resolve().parent / "arch_baseline.txt"

# Longest-prefix-wins mapping of source directory -> layer name.
LAYER_DIRS = [
    ("App/System", "APP_SYSTEM"),
    ("App/Charge", "APP_CHARGE"),
    ("App/Protocol", "APP_PROTOCOL"),
    ("Modules", "MODULES"),
    ("BSP", "PLATFORM_BSP"),
    ("USB_Device/App", "PLATFORM_USB"),
    ("USB_Device/Target", "GENERATED"),
    ("Utils", "UTILS"),
    ("Core", "GENERATED"),
    ("Drivers", "GENERATED"),
    ("Middlewares", "GENERATED"),
    ("cmake", "GENERATED"),
]

# Exact-path overrides for files whose role does not match their directory.
# Core/Src/main.c is CubeMX's generated entry point, but by convention it is
# also the true composition root: it calls MX_*_Init() (generated) and then
# hands off to App_Init()/App_Loop() (AGENTS.md sec 4 "application hooks").
# Treat it like APP_SYSTEM rather than plain GENERATED.
EXACT_LAYER_OVERRIDES = {
    "Core/Src/main.c": "ENTRY_POINT",
}

# Directories that are scanned for header ownership + include statements.
SCAN_DIRS = ["App", "BSP", "Modules", "Utils", "Core", "Drivers", "Middlewares", "USB_Device"]

# source_layer -> set of layers it may #include "local_header.h" from.
# A layer may always include its own layer.
ALLOWED_TARGETS = {
    "APP_SYSTEM":   {"APP_CHARGE", "APP_PROTOCOL", "MODULES", "PLATFORM_BSP", "PLATFORM_USB", "UTILS", "GENERATED"},
    "APP_CHARGE":   {"MODULES", "UTILS"},
    "APP_PROTOCOL": {"APP_CHARGE", "MODULES", "UTILS", "PLATFORM_USB"},
    "MODULES":      {"PLATFORM_BSP", "UTILS"},
    "PLATFORM_BSP": {"GENERATED", "UTILS"},
    "PLATFORM_USB": {"GENERATED", "UTILS"},
    "GENERATED":    {"PLATFORM_BSP", "UTILS"},  # sanctioned hook exception, AGENTS.md sec 4
    "UTILS":        {"GENERATED"},
    # main() is the real composition root (see EXACT_LAYER_OVERRIDES above):
    # it may wire together anything below it, same as APP_SYSTEM.
    "ENTRY_POINT":  {"APP_SYSTEM", "APP_CHARGE", "APP_PROTOCOL", "MODULES",
                      "PLATFORM_BSP", "PLATFORM_USB", "UTILS", "GENERATED"},
}

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"')


def classify(rel_path: str) -> str:
    if rel_path in EXACT_LAYER_OVERRIDES:
        return EXACT_LAYER_OVERRIDES[rel_path]
    best = None
    for prefix, layer in LAYER_DIRS:
        if rel_path == prefix or rel_path.startswith(prefix + "/"):
            if best is None or len(prefix) > len(best[0]):
                best = (prefix, layer)
    return best[1] if best else "UNKNOWN"


def build_header_index():
    """Map header basename -> layer, by scanning every .h file once."""
    index = {}
    for d in SCAN_DIRS:
        root = REPO_ROOT / d
        if not root.exists():
            continue
        for h in root.rglob("*.h"):
            rel = h.relative_to(REPO_ROOT).as_posix()
            index.setdefault(h.name, classify(rel))
    return index


def load_baseline():
    if not BASELINE_PATH.exists():
        return set()
    entries = set()
    for line in BASELINE_PATH.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        entries.add(line)
    return entries


def find_violations(header_index):
    violations = []  # (baseline_key, file, line_no, included_header, src_layer, tgt_layer)
    for d in SCAN_DIRS:
        root = REPO_ROOT / d
        if not root.exists():
            continue
        for src in list(root.rglob("*.c")) + list(root.rglob("*.h")):
            rel = src.relative_to(REPO_ROOT).as_posix()
            src_layer = classify(rel)
            if src_layer == "UNKNOWN":
                continue
            allowed = ALLOWED_TARGETS.get(src_layer, set()) | {src_layer}
            for lineno, line in enumerate(src.read_text(errors="replace").splitlines(), 1):
                m = INCLUDE_RE.match(line)
                if not m:
                    continue
                header = Path(m.group(1)).name  # strip any "priv/" style subdir
                tgt_layer = header_index.get(header)
                if tgt_layer is None or tgt_layer == "UNKNOWN":
                    continue  # stdlib-ish or not part of our tree; not this check's job
                if tgt_layer not in allowed:
                    key = f"{rel}:{header}"
                    violations.append((key, rel, lineno, header, src_layer, tgt_layer))
    return violations


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--update-baseline", action="store_true",
                     help="Rewrite tools/arch_baseline.txt with all current violations. "
                          "Only use this after deliberately reviewing what you're grandfathering in.")
    args = ap.parse_args()

    header_index = build_header_index()
    violations = find_violations(header_index)
    baseline = load_baseline()

    if args.update_baseline:
        keys = sorted({v[0] for v in violations})
        with open(BASELINE_PATH, "w") as f:
            f.write("# Known pre-existing architecture violations (see AGENTS.md sec 28).\n")
            f.write("# Format: <file>:<included_header>. Remove a line once it's fixed.\n")
            for k in keys:
                f.write(k + "\n")
        print(f"Baseline updated: {len(keys)} entries written to {BASELINE_PATH}")
        return 0

    new_violations = [v for v in violations if v[0] not in baseline]
    known_count = len(violations) - len(new_violations)

    if new_violations:
        print("Architecture check FAILED - new forbidden dependency edges introduced:\n")
        for key, rel, lineno, header, src_layer, tgt_layer in new_violations:
            print(f"  {rel}:{lineno}: [{src_layer}] includes \"{header}\" [{tgt_layer}] -- not allowed")
        print(f"\n{len(new_violations)} new violation(s). See AGENTS.md sections 5, 6, 23.")
        print("If this dependency is genuinely required, redesign the boundary; "
              "do not silently add it to the baseline.")
        return 1

    print(f"Architecture check passed. {known_count} pre-existing violation(s) tracked in "
          f"{BASELINE_PATH.name} (see AGENTS.md section 28 refactor order); no new ones introduced.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
