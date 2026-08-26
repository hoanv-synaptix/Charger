import os
import sys

def check_file(filename, checks):
    if not os.path.exists(filename):
        print(f"ERROR: {filename} not found.")
        return False
    with open(filename, 'r') as f:
        content = f.read()
    
    passed = True
    for check in checks:
        if check not in content:
            print(f"ERROR: Missing expected configuration in {filename}: '{check}'")
            passed = False
    return passed

def main():
    print("Running CubeMX Generator Guardrail...")
    success = True

    # Check FDCAN
    success &= check_file("Core/Src/fdcan.c", [
        "hfdcan1.Init.NominalPrescaler = 32;",
        "hfdcan2.Init.NominalPrescaler = 16;",
        "hfdcan2.Init.StdFiltersNbr = 1;"
    ])

    # Check ADC
    success &= check_file("Core/Src/adc.c", [
        "hadc1.Init.ScanConvMode = ADC_SCAN_ENABLE;",
        "hadc1.Init.NbrOfConversion = 4;"
    ])

    # Check SPI
    success &= check_file("Core/Src/spi.c", [
        "hspi1.Init.DataSize = SPI_DATASIZE_8BIT;",
        "hspi2.Init.DataSize = SPI_DATASIZE_8BIT;"
    ])

    if success:
        print("SUCCESS: All guardrail checks passed.")
        sys.exit(0)
    else:
        print("FAILED: Guardrail checks failed. Do not commit or flash.")
        sys.exit(1)

if __name__ == "__main__":
    main()
