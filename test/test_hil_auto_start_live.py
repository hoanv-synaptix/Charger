#!/usr/bin/env python3
"""
Comprehensive Live HIL Automation Test for Plug & Charge Auto Start (Workflow W4)
Hardware:
  - ZLG USBCAN2: CAN1 (TonHe @ 125kbps) + CAN2 (BMS @ 250kbps)
  - STM32G0 MCU: USB CDC (COM26)
"""

import sys
import os
import time

sys.path.append(os.path.dirname(__file__))
from zcan_hil_simulator import (
    ZlgCanDevice, BmsSimulator, ModuleSimulator,
    send_pc_cmd, read_mcu_info, CHARGE_CTRL_STATE_NAMES
)

def set_mcu_auto_start(enabled: int, port="COM26") -> bool:
    """Set auto_start_enabled for both profiles and save to Flash."""
    for mode in [0, 1]:
        resp = send_pc_cmd(0x19, bytes([mode]), port=port)
        if resp and len(resp) >= 259:
            cfg = bytearray(resp[4:259])
            cfg[254] = 1 if enabled else 0
            cfg[243] = mode
            send_pc_cmd(0x1A, bytes(cfg), port=port)
            time.sleep(0.15)
    resp_act = send_pc_cmd(0x19, b'', port=port)
    return (resp_act[258] == (1 if enabled else 0)) if resp_act and len(resp_act) >= 259 else False

def get_mcu_auto_start(port="COM26") -> int:
    """Read auto_start_enabled from active MCU profile."""
    resp = send_pc_cmd(0x19, b'', port=port)
    return resp[258] if resp and len(resp) >= 259 else -1

def ensure_idle_state(port="COM26"):
    for _ in range(3):
        info = read_mcu_info(port)
        st = info.get("controller_state", 0)
        if st in (1, 2, 6):
            send_pc_cmd(0x04, port=port)  # STOP
            time.sleep(0.5)
        elif st == 4:
            send_pc_cmd(0x0A, port=port)  # RESET_FAULT
            time.sleep(0.5)
        elif st == 0:
            return True
    return False

def wait_for_state(target_states, timeout=6.0, port="COM26"):
    if isinstance(target_states, int):
        target_states = [target_states]
    t0 = time.time()
    while time.time() - t0 < timeout:
        info = read_mcu_info(port)
        st = info.get("controller_state", -1)
        if st in target_states:
            return st, True, time.time() - t0
        time.sleep(0.15)
    return info.get("controller_state", -1), False, time.time() - t0

def main():
    print("=" * 80)
    print("  CHUONG TRINH TEST AUTOMATION TOAN DIEN QUA ZCAN CHO TINH NANG AUTO START")
    print("  Hardware Target: STM32G0 Controller (COM26) <-> ZLG USBCAN2 (CAN1 & CAN2)")
    print("=" * 80)

    # 1. Initialize ZLG CAN
    dev = ZlgCanDevice()
    dev.open()
    dev.init_channel(0, 125000)  # CAN1 TonHe Modules
    dev.init_channel(1, 250000)  # CAN2 BMS
    print("[1/2] Da ket noi va khoi tao ZLG CAN thanh cong (CAN1 125k, CAN2 250k).")

    bms = BmsSimulator(dev)
    mod = ModuleSimulator(dev, driver="tonhe", addr=1, bms=bms)

    bms.pack_voltage_v = 52.8
    bms.max_cell_mv = 3300
    bms.min_cell_mv = 3290
    bms.soc_pct = 50
    bms.bms_relay_allow = True
    bms.chg_curr_request_a = 30.0

    bms.transmitting = True
    mod.transmitting = True
    bms.start()
    mod.start()

    time.sleep(1.5)
    send_pc_cmd(0x0A, port="COM26") # RESET_FAULT
    time.sleep(0.5)

    info = read_mcu_info("COM26")
    print(f"[2/2] MCU Telemetry: State={info.get('controller_state')} ({CHARGE_CTRL_STATE_NAMES.get(info.get('controller_state'))}) | "
          f"Modules Online={info.get('modules_online')}/1 | CAN1 RX={info.get('can1_rx_count')} | CAN2 RX={info.get('can2_rx_count')}")

    test_results = []

    # =========================================================================
    # TEST 1: MANUAL MODE - CAM GIAC XE KHONG TU DONG SAC (AN TOAN)
    # =========================================================================
    print("\n" + "-" * 70)
    print(">>> [TEST 1/5] Che do MANUAL - Cam giac sac giu nguyen IDLE (Khong tu chay)")
    print("-" * 70)
    set_mcu_auto_start(0)
    ensure_idle_state()
    time.sleep(0.3)
    en = get_mcu_auto_start()
    print(f"  [1] Xac nhan config Flash: auto_start_enabled = {en} (MANUAL)")

    print("  [2] Phat song song CAN1 (Module) va CAN2 (BMS) trong 3.0s...")
    time.sleep(3.0)

    info = read_mcu_info("COM26")
    st = info.get("controller_state", -1)
    print(f"  [3] Kiem tra trang thai MCU: State = {st} ({CHARGE_CTRL_STATE_NAMES.get(st)})")
    p1 = (en == 0) and (st == 0)
    test_results.append(("Test 1: Manual Mode giu nguyen IDLE an toan khi cam giac", p1))
    print(f"  ==> KET QUA TEST 1: {'[PASS]' if p1 else '[FAIL]'}")

    # =========================================================================
    # TEST 2: LUU CAU HINH AUTO START VAO FLASH
    # =========================================================================
    print("\n" + "-" * 70)
    print(">>> [TEST 2/5] Luu cau hinh AUTO START vao Flash MCU")
    print("-" * 70)
    set_mcu_auto_start(1)
    time.sleep(0.3)
    en = get_mcu_auto_start()
    print(f"  [1] Doc lai Flash MCU: auto_start_enabled = {en} (AUTO)")
    p2 = (en == 1)
    test_results.append(("Test 2: Ghi nhan va luu Flash auto_start_enabled = 1", p2))
    print(f"  ==> KET QUA TEST 2: {'[PASS]' if p2 else '[FAIL]'}")

    # =========================================================================
    # TEST 3: AUTO START - CAM GIAC XE -> TU DONG KICH HOAT SAC SAU 1.5s
    # =========================================================================
    print("\n" + "-" * 70)
    print(">>> [TEST 3/5] Plug & Charge Auto Start - Cam giac -> Tu dong sac sau 1.5s")
    print("-" * 70)
    # Ensure idle state before starting test
    bms.transmitting = True
    time.sleep(1.0)
    ensure_idle_state("COM26")
    time.sleep(0.5)

    # Simulate unplugging first to reset session
    bms.transmitting = False
    print("  [1] Mo phong chua cam giac (ngat CAN 5.5s de reset session)...")
    time.sleep(5.5)

    print("  [2] Cam giac sac tro lai (BMS bat dau truyen goi tin CAN)...")
    bms.transmitting = True
    t_start = time.time()

    st, ok, elapsed = wait_for_state([1, 2], timeout=6.0)
    print(f"  [3] Phat hien MCU chuyen sang: {st} ({CHARGE_CTRL_STATE_NAMES.get(st)}) sau {elapsed:.2f}s (Chuan: >= 1.5s)")

    if ok:
        time.sleep(1.0)
        info = read_mcu_info("COM26")
        v_mod = info.get("total_voltage", 0.0)
        i_mod = info.get("total_current", 0.0)
        print(f"  [4] Module dang cap nguon: V={v_mod:.1f}V, I={i_mod:.1f}A")

    p3 = ok and (st in (1, 2)) and (elapsed >= 1.4)
    test_results.append(("Test 3: Tu dong kich hoat sac sau 1.5s chong rung tin hieu", p3))
    print(f"  ==> KET QUA TEST 3: {'[PASS]' if p3 else '[FAIL]'}")

    # =========================================================================
    # TEST 4: RUT GIAC KHI DANG SAC -> TU DONG NGAT SAC VA RESET
    # =========================================================================
    print("\n" + "-" * 70)
    print(">>> [TEST 4/5] Rut giac khi dang sac -> Tu dong ngat va Reset session an toan")
    print("-" * 70)
    print("  [1] Mo phong rut giac sac (BMS CAN offline)...")
    bms.transmitting = False
    t_unplug = time.time()

    # The MCU will detect BMS offline after 5.0s and stop charging safely
    time.sleep(5.5)
    info = read_mcu_info("COM26")
    st = info.get("controller_state", -1)
    print(f"  [2] Trang thai MCU sau khi rut giac 5.5s: State = {st} ({CHARGE_CTRL_STATE_NAMES.get(st)})")
    p4 = (st in (0, 4)) # IDLE or FAULT due to BMS disconnect, ready for next session
    test_results.append(("Test 4: Rut giac sac ngat dong an toan", p4))
    print(f"  ==> KET QUA TEST 4: {'[PASS]' if p4 else '[FAIL]'}")

    # Reset fault to clean state
    bms.transmitting = True
    time.sleep(1.0)
    send_pc_cmd(0x0A, port="COM26")
    time.sleep(0.5)

    # =========================================================================
    # TEST 5: PIN DA DAY SAN KHI CAM GIAC -> KHONG SAC, BAO COMPLETE
    # =========================================================================
    print("\n" + "-" * 70)
    print(">>> [TEST 5/5] Pin da day san khi cam giac (Cell = 3.65V > 3.60V) -> Khong sac")
    print("-" * 70)
    # Simulate unplug first
    bms.transmitting = False
    time.sleep(5.5)

    # Re-plug with full battery
    print("  [1] Mo phong cam giac voi pin da day (Cell = 3650 mV > 3600 mV)...")
    bms.max_cell_mv = 3650
    bms.min_cell_mv = 3640
    bms.soc_pct = 100
    bms.pack_voltage_v = 58.4
    bms.transmitting = True
    time.sleep(0.5)
    ensure_idle_state()

    time.sleep(3.0)
    info = read_mcu_info("COM26")
    st = info.get("controller_state", -1)
    print(f"  [2] Kiem tra MCU State: State = {st} ({CHARGE_CTRL_STATE_NAMES.get(st)}) (Ky vong: 0 = IDLE, khong kich hoat sac)")
    p5 = (st == 0)
    test_results.append(("Test 5: Pin day san khong kich hoat sac (khoa an toan Complete)", p5))
    print(f"  ==> KET QUA TEST 5: {'[PASS]' if p5 else '[FAIL]'}")

    # =========================================================================
    # TONG KET
    # =========================================================================
    print("\n" + "=" * 80)
    print("  BAO CAO TONG HOP TEST AUTOMATION QUA ZCAN (PLUG & CHARGE AUTO START)")
    print("=" * 80)
    all_pass = True
    for name, ok in test_results:
        tag = "[PASS]" if ok else "[FAIL]"
        print(f"  {tag} | {name}")
        if not ok:
            all_pass = False

    print("=" * 80)
    if all_pass:
        print("  >>> XAC NHAN: TAT CA CAC TEST CASE HARDWARE QUA ZCAN DEU DAT 100%! <<<")
    else:
        print("  >>> CO TEST CASE CHUA DAT! <<<")
    print("=" * 80)

    # Clean up
    bms.running = False
    mod.running = False
    dev.close()

if __name__ == "__main__":
    main()
