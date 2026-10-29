#!/usr/bin/env python3
"""
Hardware Verification Script using ZCAN + USB RS-485 + USB CDC.
Tests and verifies the logic issues reported:
1. Verifies that DWIN Button cannot exit DWIN_STATUS_COMPLETE.
2. Demonstrates PC Command (0x0A) is the only way to recover currently.
"""

import sys
import os
import time
import serial

sys.path.append(os.path.join(os.path.dirname(__file__), "."))
from zcan_hil_simulator import ZlgCanDevice, BmsSimulator, ModuleSimulator, DwinScreenSniffer

def crc8(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x07) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc

def send_pc_cmd(com_port, cmd, payload=b""):
    try:
        s = serial.Serial(com_port, 115200, timeout=0.5)
        body = bytes([cmd, len(payload)]) + payload
        frame = bytes([0xAA, 0x55]) + body + bytes([crc8(body)])
        s.write(frame)
        time.sleep(0.1)
        resp = s.read(100)
        s.close()
        return resp
    except Exception as e:
        print(f"PC Comm error: {e}")
        return None

def main():
    print("=" * 80)
    print("  KIEM THU THUC TE TREN PHAN CUNG: ZCAN + RS-485 (COM29) + MCU (COM26)")
    print("=" * 80)

    # 1. Open RS-485 Sniffer
    sniffer = DwinScreenSniffer(port="COM29")
    sniffer.open()
    sniffer.start()
    if not sniffer.available:
        print("[FAIL] Khong the mo cong RS485 COM29!")
        return

    # 2. Open ZCAN
    zcan = ZlgCanDevice()
    zcan.open()
    zcan.init_channel(0, 125000)  # CAN1 Modules
    zcan.init_channel(1, 250000)  # CAN2 BMS
    print("[OK] ZCAN & RS-485 Da ket noi thanh cong!")

    bms = BmsSimulator(zcan)
    mod = ModuleSimulator(zcan, driver="tonhe", addr=1, bms=bms)

    # Setup normal standby
    bms.pack_voltage_v = 52.8
    bms.max_cell_mv = 3315
    bms.min_cell_mv = 3300
    bms.soc_pct = 80
    bms.bms_relay_allow = True
    bms.transmitting = True

    mod.transmitting = True
    mod.voltage = 0.0
    mod.current = 0.0
    mod.fault_bits = 0x0000

    bms.start()
    mod.start()

    time.sleep(2.0)
    print(f"\n[TRANG THAI BAN DAU]")
    print(f"  -> DWIN Status Icon: {sniffer.state['status_icon']} (0=READY, 1=STARTING, 2=CHARGING, 3=COMPLETE, 4=ERROR)")
    print(f"  -> DWIN Button Icon: {sniffer.state['button_icon']} (0=START, 1=STOP, 2=RESET)")
    print(f"  -> DWIN Topbar Code: '{sniffer.state['topbar_code']}'")

    # If in error or complete, reset via PC first to guarantee clean start
    if sniffer.state['status_icon'] in (3, 4):
        print("  -> Reset trang thai cu qua PC Protocol...")
        send_pc_cmd("COM26", 0x0A)
        time.sleep(1.0)

    # Start Charging via DWIN button
    print("\n>>> BƯỚC 1: Kich hoat bat dau sac (DWIN Button Touch: START)...")
    sniffer.send_button_touch(1)
    time.sleep(3.0)

    # Mod energize
    mod.voltage = 53.0
    mod.current = 15.0
    mod.actually_on = True
    time.sleep(3.0)

    print(f"  -> DWIN Status Icon khi dang sac: {sniffer.state['status_icon']} (Ky vong: 2=CHARGING)")
    print(f"  -> DWIN Button Icon: {sniffer.state['button_icon']} (Ky vong: 1=STOP)")

    # Trigger Charge Complete
    print("\n>>> BƯỚC 2: Bom tin hieu PIN DAY (Cell Voltage = 3550 mV > 3530 mV & SOC = 100%)...")
    bms.soc_pct = 100
    bms.max_cell_mv = 3550
    mod.current = 0.0
    mod.actually_on = False

    time.sleep(4.0)
    status_after_complete = sniffer.state['status_icon']
    btn_after_complete = sniffer.state['button_icon']
    print(f"  -> DWIN Status Icon: {status_after_complete} (Ky vong: 3=COMPLETE)")
    print(f"  -> DWIN Button Icon: {btn_after_complete} (Ky vong: 2=RESET)")

    if status_after_complete != 3:
        print("[WARNING] He thong chua chuyen sang COMPLETE, kiem tra lai nguong ngat!")
    else:
        print("[PASS] MCU da phat hien hoan tat sac va chuyen DWIN sang COMPLETE!")

    # NOW TEST DWIN BUTTON TOUCH TO RESET
    print("\n>>> BƯỚC 3: THUC HIEN TEST NGUOI DUNG BAM NUT RESET TREN DWIN RS485...")
    print("  -> Gui frame cham nut DWIN (VP 0x1042 = 1)...")
    sniffer.send_button_touch(1)
    time.sleep(2.0)

    status_after_dwin_reset = sniffer.state['status_icon']
    btn_after_dwin_reset = sniffer.state['button_icon']
    print(f"  -> Ket qua sau khi cham nut DWIN:")
    print(f"     Status Icon: {status_after_dwin_reset}")
    print(f"     Button Icon: {btn_after_dwin_reset}")

    if status_after_dwin_reset == 3:
        print("\n [XAC NHAN LOI LOGIC #1 THANH CONG!]")
        print("     Trang thai DWIN VAN BI KET O 3 (COMPLETE)! Nut Reset tren man hinh hoan toan bi liet!")
        print("     Nguyen nhan: app_main.c:app_action_button() khong co 'case DWIN_STATUS_COMPLETE'.")
    else:
        print("     Status da chuyen sang:", status_after_dwin_reset)

    # NOW RECOVER VIA PC PROTOCOL
    print("\n>>> BƯỚC 4: GUI LENH RESET QUA PC USB CDC (COM26, CMD 0x0A)...")
    send_pc_cmd("COM26", 0x0A)
    time.sleep(1.5)

    status_after_pc_reset = sniffer.state['status_icon']
    btn_after_pc_reset = sniffer.state['button_icon']
    print(f"  -> Ket qua sau khi PC gui lenh 0x0A:")
    print(f"     Status Icon: {status_after_pc_reset} (Ky vong: 0=READY)")
    print(f"     Button Icon: {btn_after_pc_reset} (Ky vong: 0=START)")

    if status_after_pc_reset == 0:
        print("\n [CHUNG MINH HOAN TAT]")
        print("     Lenh PC (0x0A) goi ChargeController_AcknowledgeCompletion() nen da dua he thong ve READY.")
        print("     Dieu nay chung minh 100% ham ChargeController_AcknowledgeCompletion() can phai duoc")
        print("     goi trong app_action_button() khi man hinh o trang thai COMPLETE!")

    # Cleanup
    bms.running = False
    mod.running = False
    zcan.close()
    if sniffer.ser and sniffer.ser.is_open:
        sniffer.ser.close()
    print("\n[FINISH] Da dong ket noi ZCAN va RS485.")

if __name__ == "__main__":
    main()
