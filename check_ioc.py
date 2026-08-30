#!/usr/bin/env python3
"""
check_ioc.py -- Guard against silent CubeMX regeneration drift.

Charger.ioc is hand-tuned in places (FDCAN bit timing, USART3 pin swap, SPI
frame size, ADC channel count, IWDG). A CubeMX "Generate Code" can silently
reset any of these to a default the moment a peripheral is touched in the
.ioc editor. AUDIT_Findings.md documented several drift incidents that
caused real field bugs (BMS frames silently dropped, wrong CAN bitrate).

This script does not parse Charger.ioc itself -- the checked-in generated
sources (Core/Src/*.c) are the ground truth for what actually gets flashed,
so we assert directly on those. Run it after every "Generate Code" and in
CI before anything else builds.
"""
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent


def read(path):
    p = REPO_ROOT / path
    if not p.exists():
        return None
    return p.read_text(errors="replace")


def check(label, text, pattern, filename):
    if text is None:
        return False, f"{label}: FILE MISSING ({filename})"
    if re.search(pattern, text):
        return True, f"{label}: OK"
    return False, f"{label}: FAILED -- pattern not found in {filename}: {pattern}"


def main():
    results = []

    fdcan = read("Core/Src/fdcan.c")
    results.append(check(
        "FDCAN1 @ 125 kbps (NominalPrescaler=32)", fdcan,
        r"hfdcan1\.Init\.NominalPrescaler\s*=\s*32\s*;", "Core/Src/fdcan.c"))
    results.append(check(
        "FDCAN2 @ 250 kbps (NominalPrescaler=16)", fdcan,
        r"hfdcan2\.Init\.NominalPrescaler\s*=\s*16\s*;", "Core/Src/fdcan.c"))
    results.append(check(
        "FDCAN2 accepts Standard-ID frames (StdFiltersNbr>=1) -- BMS depends on this",
        fdcan, r"hfdcan2\.Init\.StdFiltersNbr\s*=\s*[1-9]\d*\s*;", "Core/Src/fdcan.c"))
    results.append(check(
        "FDCAN1 AutoRetransmission enabled", fdcan,
        r"hfdcan1\.Init\.AutoRetransmission\s*=\s*ENABLE\s*;", "Core/Src/fdcan.c"))
    results.append(check(
        "FDCAN2 AutoRetransmission enabled", fdcan,
        r"hfdcan2\.Init\.AutoRetransmission\s*=\s*ENABLE\s*;", "Core/Src/fdcan.c"))

    usart = read("Core/Src/usart.c")
    results.append(check(
        "USART3 RX/TX pin swap enabled (DWIN RS485 wiring depends on this)",
        usart, r"huart3\.AdvancedInit\.AdvFeatureInit\s*=\s*UART_ADVFEATURE_SWAP_INIT\s*;",
        "Core/Src/usart.c"))

    spi = read("Core/Src/spi.c")
    for inst in ("hspi1", "hspi2"):
        results.append(check(
            f"{inst} DataSize = 8 bit", spi,
            rf"{inst}\.Init\.DataSize\s*=\s*SPI_DATASIZE_8BIT\s*;", "Core/Src/spi.c"))

    adc = read("Core/Src/adc.c")
    results.append(check(
        "ADC1 scans all 4 NTC channels (NbrOfConversion=4)", adc,
        r"hadc1\.Init\.NbrOfConversion\s*=\s*4\s*;", "Core/Src/adc.c"))
    results.append(check(
        "ADC1 scan mode enabled", adc,
        r"hadc1\.Init\.ScanConvMode\s*=\s*ADC_SCAN_ENABLE\s*;", "Core/Src/adc.c"))
    # BSP/bsp_adc.c's BSP_ADC_Process() re-arms the scan on a 200ms timer and
    # assumes it is single-shot. If a regen switches either of these to
    # free-running (ContinuousConvMode=ENABLE / DMA_CIRCULAR) the re-arm becomes
    # a redundant Stop/Start of a running conversion -- update bsp_adc.c to just
    # read the buffer and note it here, don't only delete the check.
    results.append(check(
        "ADC1 single-shot scan (ContinuousConvMode=DISABLE) -- BSP_ADC_Process re-arms it",
        adc, r"hadc1\.Init\.ContinuousConvMode\s*=\s*DISABLE\s*;", "Core/Src/adc.c"))
    results.append(check(
        "ADC1 DMA is one-shot (hdma_adc1 Mode=DMA_NORMAL) -- paired with the single-shot scan",
        adc, r"hdma_adc1\.Init\.Mode\s*=\s*DMA_NORMAL\s*;", "Core/Src/adc.c"))
    results.append(check(
        "ADC1 DMA request kept enabled for the full 4-channel scan (DMAContinuousRequests=ENABLE)",
        adc, r"hadc1\.Init\.DMAContinuousRequests\s*=\s*ENABLE\s*;", "Core/Src/adc.c"))

    hal_conf = read("Core/Inc/stm32g0xx_hal_conf.h")
    results.append(check(
        "Independent watchdog (IWDG) compiled in -- no watchdog is a safety regression",
        hal_conf, r"(?m)^#define\s+HAL_IWDG_MODULE_ENABLED\b", "Core/Inc/stm32g0xx_hal_conf.h"))

    print("CubeMX drift check")
    print("=" * 60)
    failed = 0
    for ok, msg in results:
        print(("[OK]  " if ok else "[FAIL]") + " " + msg)
        if not ok:
            failed += 1

    print("=" * 60)
    if failed:
        print(f"{failed}/{len(results)} check(s) failed. If this is an intentional "
              f"hardware/config change, update this script in the same commit "
              f"with a note explaining why; do not just delete the failing check.")
        return 1
    print(f"All {len(results)} checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
