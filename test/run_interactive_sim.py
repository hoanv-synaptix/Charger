#!/usr/bin/env python3
"""
Interactive Background Simulator for Manual Testing on DWIN Screen.
Simulates:
  - CAN1: TonHe Charger Module (Address 1 @ 125kbps)
  - CAN2: BMS (Standard & Extended frames @ 250kbps)
Allows the operator to freely touch and test the DWIN screen:
  - START / STOP / RESET buttons
  - AUTO / MANUAL toggle switch (Page 02 <-> Page 12)
  - SETTING footer button (Page 01 -> Page 02 / 12)
  - Time & Mode configuration
"""

import sys
import os
import time

sys.path.append(os.path.dirname(__file__))
from zcan_hil_simulator import (
    ZlgCanDevice, BmsSimulator, ModuleSimulator, DwinScreenSniffer,
    read_mcu_info, send_pc_cmd, CHARGE_CTRL_STATE_NAMES
)

def main():
    print("=" * 80)
    print("  KHOI TAO LUONG GIA LAP TU DONG (INTERACTIVE BENCH SIMULATOR)")
    print("  Cho phep nguoi dung tu do thao tac truc tiep tren man hinh DWIN")
    print("=" * 80)

    # 1. Open RS-485 Sniffer passively (COM29)
    sniffer = DwinScreenSniffer(port="COM29")
    sniffer.open()
    sniffer.start()
    if sniffer.available:
        print("[OK] RS-485 Sniffer tren COM29 da san sang (theo doi trang thai man hinh).")
    else:
        print("[WARN] Khong mo duoc COM29, bo qua sniffer RS-485.")

    # 2. Open ZLG USBCAN
    dev = ZlgCanDevice()
    dev.open()
    dev.init_channel(0, 125000)  # CAN1 TonHe Modules
    dev.init_channel(1, 250000)  # CAN2 BMS
    print("[OK] ZLG CAN (CAN1 125k TonHe, CAN2 250k BMS) da ket noi va bat dau phat.")

    # 3. Create & configure Simulators
    bms = BmsSimulator(dev)
    mod = ModuleSimulator(dev, driver="tonhe", addr=1, bms=bms)

    # Initial Healthy Battery Telemetry
    bms.pack_voltage_v = 52.8
    bms.pack_current_a = 0.0
    bms.soc_pct = 75
    bms.max_cell_mv = 3310
    bms.min_cell_mv = 3300
    bms.max_cell_temp_c = 28.0
    bms.min_cell_temp_c = 27.0
    bms.avg_cell_temp_c = 27.5
    bms.bms_relay_allow = True
    bms.chg_curr_request_a = 35.0
    bms.chg_volt_request_v = 58.4

    # Initial Module Telemetry
    mod.standby_voltage = 52.8
    mod.voltage = 52.8
    mod.current = 0.0
    mod.actually_on = False
    mod.ac_phase_a = 220.0
    mod.ac_phase_b = 221.0
    mod.ac_phase_c = 219.0
    mod.temp_dcdc = 32.0
    mod.temp_ambient = 26.0

    bms.transmitting = True
    mod.transmitting = True
    bms.start()
    mod.start()

    # Clear any residual faults on MCU to ensure clean IDLE state
    time.sleep(1.0)
    send_pc_cmd(0x0A, port="COM26") # RESET_FAULT
    time.sleep(0.5)

    print("\n" + "=" * 80)
    print("  HE THONG GIA LAP DANG CHAY NGAM THANH CONG!")
    print("  -> Thong so Pin: 52.8V, SOC 75%, Cell 3.31V, Nhiet do 27.5 C, Yeu cau 35A/58.4V")
    print("  -> Thong so Module: TonHe Address #1, AC 220V, San sang dap ung lenh dong/ngat")
    print("  -> Ban co the cham vao man hinh DWIN de test cac nut:")
    print("     + Nut START / STOP / RESET tren Dashboard")
    print("     + Nut gạt chuyen MANUAL <-> AUTO (Page 02 <-> Page 12)")
    print("     + Nut SETTING o Footer de chuyen man hinh")
    print("=" * 80 + "\n")

    last_print = 0.0
    try:
        while True:
            now = time.time()
            if now - last_print >= 2.5:
                last_print = now
                m = read_mcu_info("COM26") or {}
                st = m.get("controller_state", -1)
                st_name = CHARGE_CTRL_STATE_NAMES.get(st, f"State {st}")
                v = m.get("total_voltage", 0.0)
                i = m.get("total_current", 0.0)
                p = m.get("total_power_in", 0.0)
                icon = sniffer.state.get("status_icon", -1) if sniffer.available else -1
                btn = sniffer.state.get("button_icon", -1) if sniffer.available else -1
                page = sniffer.state.get("current_page", -1) if sniffer.available else -1

                icon_names = {0: "READY", 1: "STARTING", 2: "CHARGING", 3: "COMPLETE", 4: "ERROR"}
                btn_names = {0: "START", 1: "STOP", 2: "RESET"}
                icon_str = icon_names.get(icon, str(icon))
                btn_str = btn_names.get(btn, str(btn))

                print(f"[LIVE] MCU: {st_name:<10} | V={v:5.1f}V | I={i:5.1f}A | P={p:4.1f}kW | "
                      f"DWIN: Page {page} | Status={icon_str} | Button={btn_str}", flush=True)

            time.sleep(0.1)

    except KeyboardInterrupt:
        print("\n[INFO] Nhan Ctrl+C, dung luong gia lap an toan...")
    finally:
        bms.running = False
        mod.running = False
        sniffer.close()
        dev.close()
        print("[INFO] Da tat ZCAN va giai phong cong COM. Hoan tat.")

if __name__ == "__main__":
    main()
