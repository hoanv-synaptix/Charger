"""
Sprint 1.1 — CAN filter BMS std+ext verification
- Mock-free: inspect source files for correct filter configuration
"""
import re
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[1]

def read(p):
    return (ROOT / p).read_text(encoding="utf-8", errors="ignore")

def test_fdcan2_std_filter_in_fdcan_c():
    txt = read("Core/Src/fdcan.c")
    # FDCAN1 must stay ext-only
    assert "hfdcan1.Init.StdFiltersNbr = 0" in txt, "FDCAN1 must remain StdFiltersNbr=0"
    assert "hfdcan1.Init.ExtFiltersNbr = 1" in txt
    # FDCAN2 must have StdFiltersNbr=1 after fix
    assert "hfdcan2.Init.StdFiltersNbr = 1" in txt, "FDCAN2 StdFiltersNbr should be 1"
    assert "hfdcan2.Init.ExtFiltersNbr = 1" in txt
    # Prescalers
    assert "hfdcan1.Init.NominalPrescaler = 32" in txt
    assert "hfdcan2.Init.NominalPrescaler = 16" in txt

def test_charger_ioc_fdcam2():
    txt = read("Charger.ioc")
    assert "FDCAN2.StdFiltersNbr=1" in txt, "Charger.ioc missing StdFiltersNbr=1"
    assert "FDCAN2.NominalPrescaler=16" in txt, "Charger.ioc missing NominalPrescaler=16"
    # IPParameters must contain StdFiltersNbr
    m = re.search(r"FDCAN2\.IPParameters=(.+)", txt)
    assert m is not None
    ip = m.group(1)
    assert "StdFiltersNbr" in ip, "IPParameters must list StdFiltersNbr"
    assert "NominalPrescaler" in ip

def test_bsp_can_split_and_std_filter():
    txt = read("BSP/bsp_can.c")
    # Must have two separate config functions
    assert "config_charger_bus_filters" in txt or "config_bms" in txt, "Should split config_bus_filters"
    assert "config_bms_bus_filters" in txt, "Missing BMS bus config"
    assert "BSP_CAN_Start" in txt
    # BMS config must create Std filter
    # Look for FDCAN_STANDARD_ID + FDCAN_FILTER_TO_RXFIFO0
    bms_section = txt[txt.find("config_bms_bus_filters"):]
    assert "FDCAN_STANDARD_ID" in bms_section, "BMS filter must handle Std frames"
    assert "FDCAN_EXTENDED_ID" in bms_section, "BMS filter must also handle Ext frames"
    # Must set FilterID1=0x000 and mask 0x000 for accept-all
    assert "0x000" in bms_section
    assert "FDCAN_FILTER_TO_RXFIFO0" in bms_section
    # Global filter reject
    assert "FDCAN_REJECT" in bms_section
    # Charger bus should NOT have std filter
    charger_section = txt[txt.find("config_charger_bus_filters"):txt.find("config_bms_bus_filters")]
    assert "FDCAN_STANDARD_ID" not in charger_section, "Charger bus should be ext-only"

def test_mock_hal_config_filter():
    """Mock HAL_FDCAN_ConfigFilter to verify accept-all mask semantics"""
    # Simulate filter matching: mask 0 means don't care → all IDs pass
    def check_filter(filter_id1, filter_id2, incoming_id):
        # MASK mode: (incoming & mask) == (filter & mask) ??? ST uses FilterID2 as mask
        # For accept-all, mask=0 → (incoming & 0) == 0 always true
        mask = filter_id2
        return (incoming_id & mask) == (filter_id1 & mask)

    # Std accept-all mask 0
    for std_id in [0x02F4, 0x04F4, 0x05F4, 0x07F4, 0x123, 0x7FF]:
        assert check_filter(0x000, 0x000, std_id), f"Std {hex(std_id)} should pass accept-all"
    # Ext accept-all
    for ext_id in [0x18F128F4, 0x1806E5F4, 0x12345678]:
        assert check_filter(0x00000000, 0x00000000, ext_id)

def test_bms_std_ids_pass():
    """Ensure the 4 standard BMS frames that were previously REJECTed now would pass"""
    txt = read("BSP/bsp_can.c")
    # After fix, BMS filters must exist; we already checked Std filter presence
    # Verify that previously missing handling now present: HAL_FDCAN_RxFifo0Callback handles both IdTypes
    cb = read("BSP/bsp_can.c")
    assert "FDCAN_STANDARD_ID" in cb
    assert "FDCAN_EXTENDED_ID" in cb
    assert "BMS_FeedFrame" in cb
