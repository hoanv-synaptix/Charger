#!/usr/bin/env python3
"""
Comprehensive HIL Automation Test for Plug & Charge Auto Start (Workflow W4)
Hardware:
  - ZLG USBCAN2: CAN1 (Channel 0 @ 125kbps) + CAN2 (Channel 1 @ 250kbps)
  - STM32G0 MCU: USB CDC (COM26)
  - DWIN RS-485 Sniffer: CH340 (COM29)
"""

import sys
import os
import time

sys.path.append(os.path.dirname(__file__))
from zcan_hil_simulator import (
    ZlgCanDevice, BmsSimulator, ModuleSimulator, DwinScreenSniffer,
    send_pc_cmd, read_mcu_info, CHARGE_CTRL_STATE_NAMES
)

def get_auto_start_enabled(port="COM26") -> int:
    """Read auto_start_enabled from MCU config (byte 258 of GET_CHARGE_CFG)."""
    resp = send_pc_cmd(0x19, b'\x00', port=port)
    if resp and len(resp) >= 259:
        return resp[258]
    return -1

def wait_for_mcu_state(target_states, timeout=5.0, port="COM26"):
    """Poll MCU until controller_state is in target_states."""
    if isinstance(target_states, int):
        target_states = [target_states]
    t0 = time.time()
    while time.time() - t0 < timeout:
        info = read_mcu_info(port=port)
        st = info.get("controller_state", -1)
        if st in target_states:
            return st, True
        time.sleep(0.1)
    return info.get("controller_state", -1), False

def main():
    print("=" * 80)
    print("  CHUONG TRINH TEST AUTOMATION QUA ZCAN CHO TINH NANG AUTO START (W4)")
    print("  Hardware: ZLG USBCAN + MCU (COM26) + DWIN Sniffer (COM29)")
    print("=" * 80)

    # 1. Open RS-485 Sniffer
    sniffer = DwinScreenSniffer(port="COM29")
    sniffer.open()
    sniffer.start()
    if not sniffer.available:
        print("[FAIL] Khong the mo RS-485 tren COM29!")
        return

    # 2. Open ZLG CAN
    zcan = ZlgCanDevice()
    zcan.open()
    zcan.init_channel(0, 125000)  # CAN1 Modules
    zcan.init_channel(1, 250000)  # CAN2 BMS
    print("[OK] ZLG CAN (CAN1 @ 125k, CAN2 @ 250k) va RS-485 (COM29) san sang!")

    # 3. Setup Simulators
    bms = BmsSimulator(zcan)
    mod = ModuleSimulator(zcan, driver="tonhe", addr=1, bms=bms)

    bms.pack_voltage_v = 52.8
    bms.max_cell_mv = 3300
    bms.min_cell_mv = 3290
    bms.soc_pct = 50
    bms.bms_relay_allow = True
    bms.chg_curr_request_a = 30.0
    bms.chg_volt_request_v = 58.4
    bms.transmitting = False  # Start with unplugged cable!

    mod.transmitting = True
    mod.voltage = 0.0
    mod.current = 0.0
    mod.fault_bits = 0x0000

    bms.start()
    mod.start()

    time.sleep(1.0)
    # Ensure system is in IDLE
    info = read_mcu_info("COM26")
    if info.get("controller_state") != 0:
        print("  -> Dua MCU ve IDLE (Stop / Reset Fault)...")
        send_pc_cmd(0x0A, port="COM26")  # RESET_FAULT
        time.sleep(0.5)

    test_results = []

    # =========================================================================
    # TEST 1: MANUAL MODE (Default) - CAM GIAC XE KHONG TU DONG SAC
    # =========================================================================
    print("\n" + "-" * 70)
    print(">>> TEST 1: Kiem tra Che do MANUAL (Default) - Cam sac khong tu dong chay")
    print("-" * 70)

    # Ensure Manual Mode first (send key 2 to VP 0x1132)
    sniffer.send_touch_key(0x1132, 2)
    time.sleep(0.5)
    en = get_auto_start_enabled("COM26")
    print(f"  [CONF] auto_start_enabled hien tai: {en} (Ky vong: 0 = MANUAL)")

    print("  -> Mo phong cam giac sac (BMS CAN bat dau truyen)...")
    bms.transmitting = True
    time.sleep(2.5)  # Wait longer than 1.5s latch timer

    st, _ = wait_for_mcu_state(0, timeout=1.0)
    print(f"  [CHECK] MCU State: {st} ({CHARGE_CTRL_STATE_NAMES.get(st, 'Unknown')})")
    p1 = (en == 0) and (st == 0)
    test_results.append(("Test 1: Manual Mode - Cam giac khong tu chay (an toan IDLE)", p1))
    print(f"  ==> KET QUA TEST 1: {'[PASS]' if p1 else '[FAIL]'}")

    # Unplug BMS CAN
    bms.transmitting = False
    time.sleep(5.5)  # Wait for CAN timeout

    # =========================================================================
    # TEST 2: DWIN TOGGLE SWITCH - CHUYEN SANG AUTO (VP 0x1132, Key 1)
    # =========================================================================
    print("\n" + "-" * 70)
    print(">>> TEST 2: Chuyen sang AUTO tren DWIN (VP 0x1132, Key 1)")
    print("-" * 70)

    sniffer.state["current_page"] = -1
    print("  -> Gui phim bat Auto: VP 0x1132, Key = 0x0001...")
    sniffer.send_touch_key(0x1132, 1)
    time.sleep(0.8)

    en = get_auto_start_enabled("COM26")
    page = sniffer.state.get("current_page")
    print(f"  [CONF] auto_start_enabled sau khi bat: {en} (Ky vong: 1 = AUTO)")
    print(f"  [DWIN] Trang hien tai tren man hinh: {page} (Ky vong: 12 = SETTING_AUTO)")

    p2 = (en == 1) and (page == 12)
    test_results.append(("Test 2: Bat Auto qua DWIN (VP 0x1132 -> Page 12, Flash=1)", p2))
    print(f"  ==> KET QUA TEST 2: {'[PASS]' if p2 else '[FAIL]'}")

    # =========================================================================
    # TEST 3: PLUG & CHARGE AUTO START - CAM GIAC XE -> TU DONG SAC SAU 1.5s
    # =========================================================================
    print("\n" + "-" * 70)
    print(">>> TEST 3: Plug & Charge Auto Start (Cam giac -> Tu dong sac)")
    print("-" * 70)

    bms.pack_voltage_v = 52.8
    bms.max_cell_mv = 3300
    bms.min_cell_mv = 3290
    bms.soc_pct = 50
    bms.bms_relay_allow = True
    bms.chg_curr_request_a = 30.0

    print("  -> Mo phong cam giac sac (BMS CAN ket noi tro lai)...")
    t_plug = time.time()
    bms.transmitting = True

    # Wait for MCU to enter STARTING (1) or CHARGING (2)
    st, ok = wait_for_mcu_state([1, 2], timeout=4.0)
    elapsed = time.time() - t_plug
    print(f"  [CHECK] MCU State chuyen thanh: {st} ({CHARGE_CTRL_STATE_NAMES.get(st, 'Unknown')}) sau {elapsed:.2f}s (Ky vong >= 1.5s)")

    if ok:
        # Simulate module energize
        mod.actually_on = True
        mod.voltage = 53.0
        mod.current = 15.0
        time.sleep(2.0)
        st_chg, _ = wait_for_mcu_state(2, timeout=2.0)
        print(f"  [CHECK] Module phan hoi -> MCU State: {st_chg} (2 = CHARGING)")

    p3 = ok and (elapsed >= 1.3)
    test_results.append(("Test 3: Auto Start sau 1.5s nhan CAN tu xe", p3))
    print(f"  ==> KET QUA TEST 3: {'[PASS]' if p3 else '[FAIL]'}")

    # =========================================================================
    # TEST 4: RUT GIAC SAC KHI DANG SAC (UNPLUG -> AUTO STOP & RESET SESSION)
    # =========================================================================
    print("\n" + "-" * 70)
    print(">>> TEST 4: Rut giac sac khi dang chay (CAN offline 5s -> Dung & Reset)")
    print("-" * 70)

    print("  -> Mo phong rut giac sac (BMS ngung gui CAN)...")
    bms.transmitting = False
    mod.current = 0.0
    mod.actually_on = False

    t_unplug = time.time()
    st, ok = wait_for_mcu_state(0, timeout=8.0)
    elapsed = time.time() - t_unplug
    print(f"  [CHECK] MCU ngat sac ve IDLE: {st} sau {elapsed:.2f}s (Ky vong 5..6s do CAN timeout)")

    p4 = ok and (st == 0)
    test_results.append(("Test 4: Rut giac sac -> Tu ngat an toan ve IDLE sau 5s", p4))
    print(f"  ==> KET QUA TEST 4: {'[PASS]' if p4 else '[FAIL]'}")

    # =========================================================================
    # TEST 5: PIN DA DAY (CELL >= 3550 mV) -> CHUYEN SANG COMPLETE, KHONG SAC
    # =========================================================================
    print("\n" + "-" * 70)
    print(">>> TEST 5: Pin da day san khi cam giac -> Khong sac, hien COMPLETE")
    print("-" * 70)

    # Pin day: cell 3580mV (> 3550mV cutoff)
    bms.max_cell_mv = 3580
    bms.min_cell_mv = 3570
    bms.soc_pct = 100
    bms.pack_voltage_v = 57.6

    print("  -> Mo phong cam giac voi pin da day (Cell = 3580 mV, SOC = 100%)...")
    bms.transmitting = True
    time.sleep(3.0)

    info = read_mcu_info("COM26")
    st = info.get("controller_state", -1)
    status_icon = sniffer.state.get("status_icon")
    print(f"  [CHECK] MCU State: {st} (Ky vong: 0 = IDLE / Safe)")
    print(f"  [DWIN] Status Icon: {status_icon} (Ky vong: 3 = COMPLETE)")

    p5 = (st == 0) and (status_icon == 3)
    test_results.append(("Test 5: Pin day san -> Khong kich hoat sac, hien COMPLETE", p5))
    print(f"  ==> KET QUA TEST 5: {'[PASS]' if p5 else '[FAIL]'}")

    # Reset complete via PC
    bms.transmitting = False
    send_pc_cmd(0x0A, port="COM26")
    time.sleep(2.0)

    # =========================================================================
    # TEST 6: DWIN FOOTER SETTING BUTTON (VP 0x1060)
    # =========================================================================
    print("\n" + "-" * 70)
    print(">>> TEST 6: Kiem tra nut SETTING Footer tren Man 01 (VP 0x1060)")
    print("-" * 70)

    # Đảm bảo đang ở Màn 01 (Home Dashboard) trước khi bấm nút Setting ở Footer
    sniffer.send_touch_key(0x151A, 2)  # Return to Home if in precharge/other
    time.sleep(0.5)

    # Case 6A: Khi đang ở chế độ AUTO (auto_start_enabled == 1)
    sniffer.send_touch_key(0x1132, 1)  # Bật Auto
    time.sleep(0.5)
    en_auto = get_auto_start_enabled("COM26")
    print(f"  [6A] Khi dang o che do AUTO (Flash={en_auto}):")
    # Reset tracking và bấm nút Footer Setting (VP 0x1060)
    sniffer.state["current_page"] = -1
    print("       -> Cham nut SETTING Footer (VP 0x1060)...")
    sniffer.send_touch_key(0x1060, 1)
    time.sleep(0.8)
    page_auto = sniffer.state.get("current_page")
    print(f"       -> Trang mo ra tren man hinh: {page_auto} (Ky vong: 12 = SETTING_AUTO)")

    # Case 6B: Khi đang ở chế độ MANUAL (auto_start_enabled == 0)
    sniffer.send_touch_key(0x1132, 2)  # Về Manual
    time.sleep(0.5)
    en_man = get_auto_start_enabled("COM26")
    print(f"  [6B] Khi dang o che do MANUAL (Flash={en_man}):")
    sniffer.state["current_page"] = -1
    print("       -> Cham nut SETTING Footer (VP 0x1060)...")
    sniffer.send_touch_key(0x1060, 1)
    time.sleep(0.8)
    page_man = sniffer.state.get("current_page")
    print(f"       -> Trang mo ra tren man hinh: {page_man} (Ky vong: 2 = SETTING_MANUAL)")

    p6 = (page_auto == 12) and (page_man == 2)
    test_results.append(("Test 6: Nut Footer VP 0x1060 tu dong mo dung trang (12 khi Auto, 2 khi Manual)", p6))
    print(f"  ==> KET QUA TEST 6: {'[PASS]' if p6 else '[FAIL]'}")

    # =========================================================================
    # SUMMARY
    # =========================================================================
    print("\n" + "=" * 80)
    print("  TONG KET KET QUA KIEM THU TU DONG (HIL AUTOMATION REPORT)")
    print("=" * 80)
    all_pass = True
    for name, ok in test_results:
        tag = "[PASS]" if ok else "[FAIL]"
        print(f"  {tag} {name}")
        if not ok:
            all_pass = False

    print("=" * 80)
    if all_pass:
        print("  >>> XAC NHAN: TAT CA CAC TEST CASE HARDWARE QUA ZCAN DEU DAT 100%! <<<")
    else:
        print("  >>> CO TEST CASE CHUA DAT, KIEM TRA LOG CHI TIET O TUNG MUC! <<<")
    print("=" * 80)

    # Clean up
    bms.running = False
    mod.running = False
    sniffer.close()
    zcan.close()

if __name__ == "__main__":
    main()
