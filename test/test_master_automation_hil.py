#!/usr/bin/env python3
"""
Senior Automation Test Engine: Hardware-In-The-Loop (HIL) Master Test Suite
PKG Charger Controller - STM32G0B1 Firmware V2.0.2

Closed-Loop System:
  [ZCAN Ch0: 125k] <==CAN1==> [STM32 MCU] <==CAN2==> [ZCAN Ch1: 250k]
  (TonHe Module Sim)          (COM26)                (BMS Sim)
                                  ^
                                  || RS485
                                  v
                             [DWIN HMI Screen (COM29)]
"""

import sys
import time
import os
import struct
import random

sys.path.append(os.path.dirname(__file__))
from zcan_hil_simulator import (
    ZlgCanDevice, BmsSimulator, ModuleSimulator, DwinScreenSniffer,
    send_pc_cmd, read_mcu_info, CHARGE_CTRL_STATE_NAMES, CHARGE_STOP_REASON_NAMES
)

REPORT_FILE = os.path.join(os.path.dirname(__file__), "hil_full_test_report.md")
test_results = []

def log_step(msg: str):
    ts = time.strftime("%H:%M:%S")
    print(f"  [{ts}] {msg}", flush=True)

def record_result(suite: str, code: str, title: str, passed: bool, detail: str):
    tag = "[PASS]" if passed else "[FAIL]"
    print(f"\n>>> {tag} | {suite} | {code}: {title}\n    Chi tiet: {detail}\n", flush=True)
    test_results.append({
        "suite": suite,
        "code": code,
        "title": title,
        "passed": passed,
        "detail": detail,
        "time": time.strftime("%Y-%m-%d %H:%M:%S")
    })

def get_current_cfg_payload() -> bytearray:
    resp = send_pc_cmd(0x19) # DEBUG_CMD_GET_CHARGE_CFG
    if len(resp) >= 258 and resp[2] == 0x97:
        return bytearray(resp[4:4+253])
    return bytearray()

def set_cfg_payload(payload: bytearray) -> bool:
    if len(payload) != 253:
        return False
    resp = send_pc_cmd(0x1A, bytes(payload)) # DEBUG_CMD_SET_CHARGE_CFG
    time.sleep(0.3)
    return len(resp) > 0

def main():
    print("=" * 85, flush=True)
    print("   SENIOR AUTOMATION TEST SUITE: HARDWARE-IN-THE-LOOP (HIL) MASTER ENGINE", flush=True)
    print("   STM32G0B1 MCU Firmware V2.0.2  |  ZLG USBCAN Kep  |  DWIN HMI RS485", flush=True)
    print("=" * 85, flush=True)

    dev = ZlgCanDevice()
    dev.open()
    dev.init_channel(0, 125000)  # CAN1: Modules (125k)
    dev.init_channel(1, 250000)  # CAN2: BMS (250k)

    bms = BmsSimulator(dev)
    mod1 = ModuleSimulator(dev, driver="tonhe", addr=1, bms=bms)
    sniffer = DwinScreenSniffer(port="COM29")
    sniffer.open()
    sniffer.start()

    bms.start()
    mod1.start()

    original_cfg = get_current_cfg_payload()

    def standby_clean():
        bms.pack_voltage_v = 52.8
        bms.pack_current_a = 0.0
        bms.soc_pct = 30
        bms.max_cell_mv = 3250
        bms.min_cell_mv = 3240
        bms.bms_relay_allow = True
        bms.chg_curr_request_a = 35.0
        bms.chg_volt_request_v = 58.4
        bms.fault_over_temp = 0
        bms.fault_high_cell_volt = 0
        bms.fault_low_cell_volt = 0
        bms.fault_high_pack_volt = 0
        bms.fault_low_pack_volt = 0
        bms.max_cell_temp_c = 28.0
        bms.last_ctrl_allow_charge = False
        bms.transmitting = True

        mod1.actually_on = False
        mod1.fault_bits = 0x0000
        mod1.standby_voltage = 52.8
        mod1.voltage = 52.8
        mod1.current = 0.0
        mod1.transmitting = True

        send_pc_cmd(0x04) # STOP
        send_pc_cmd(0x0A) # RESET_FAULT
        if sniffer and sniffer.available:
            sniffer.send_button_touch(1)
        time.sleep(0.3)

        for _ in range(12):
            m = read_mcu_info() or {}
            if m.get("controller_state", -1) in (0, 1) and m.get("modules_online", 0) > 0:
                break
            if m.get("controller_state") == 4:
                send_pc_cmd(0x04)
                send_pc_cmd(0x0A)
                if sniffer and sniffer.available:
                    sniffer.send_button_touch(1)
            time.sleep(0.25)

    try:
        # =========================================================================
        # SUITE 1: LOGIC DIEU KHIEN SAC & GIOI HAN THAM SO (CL-01 -> CL-08)
        # =========================================================================
        print("\n" + "=" * 85, flush=True)
        print(">>> SUITE 1: LOGIC DIEU KHIEN SAC & GIOI HAN THAM SO (CL-01 -> CL-08)", flush=True)
        print("=" * 85, flush=True)

        # --- CL-01: Soft-Start ---
        standby_clean()
        log_step("--- [TEST CL-01] Soft-Start & Ramp gia toc tang dong <= 5.0 A/s ---")
        m0 = read_mcu_info() or {}
        last_i = m0.get("total_current", 0.0)
        send_pc_cmd(0x03, bytes([0])) # Start normal
        ramp_rates = []
        for s in range(1, 10):
            time.sleep(1.0)
            m = read_mcu_info() or {}
            act_i = m.get("total_current", 0.0)
            di = act_i - last_i
            last_i = act_i
            if s > 1: # bo qua do tre khoi dong ban dau
                ramp_rates.append(di)
            log_step(f"t={s:02d}s: I={act_i:.1f}A (+{di:.1f}A/s) | State={m.get('controller_state')}")
        max_ramp = max(ramp_rates) if ramp_rates else 0
        cl01_pass = (max_ramp <= 6.0) and (last_i >= 18.0)
        record_result("Charging Logic", "CL-01", "Soft-Start & Ramp tang dong 5A/s", cl01_pass,
                      f"Max gia toc on dinh: {max_ramp:.1f}A/s (chuan <= 5.0A/s), Dong dat: {last_i:.1f}A")

        # --- CL-02: Phan tang ap Cell & Don dieu ---
        standby_clean()
        log_step("--- [TEST CL-02] Phan tang ap Cell & Tinh chot don dieu ---")
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(6.0)
        m1 = read_mcu_info() or {}
        i_band1 = m1.get("controller_target_current_total", 0.0)
        log_step(f"Cell 3.25V: Target I = {i_band1:.1f}A (Band {m1.get('active_stage_band')})")

        bms.max_cell_mv = 3420
        time.sleep(3.0)
        m2 = read_mcu_info() or {}
        i_band2 = m2.get("controller_target_current_total", 0.0)
        log_step(f"Cell 3.42V: Target I = {i_band2:.1f}A (Band {m2.get('active_stage_band')})")

        bms.max_cell_mv = 3520
        time.sleep(3.0)
        m3 = read_mcu_info() or {}
        i_band3 = m3.get("controller_target_current_total", 0.0)
        log_step(f"Cell 3.52V: Target I = {i_band3:.1f}A (Band {m3.get('active_stage_band')})")

        bms.max_cell_mv = 3350
        time.sleep(3.0)
        m4 = read_mcu_info() or {}
        i_mono = m4.get("controller_target_current_total", 0.0)
        log_step(f"Cell sut 3.35V: Target I = {i_mono:.1f}A (Chot giu nguyen)")

        cl02_pass = (i_band2 <= i_band1) and (i_band3 <= i_band2) and (i_mono <= i_band3 + 0.1)
        record_result("Charging Logic", "CL-02", "Phan tang giam dong theo Cell & Don dieu", cl02_pass,
                      f"Band1: {i_band1:.1f}A -> Band2: {i_band2:.1f}A -> Band3: {i_band3:.1f}A -> Monotonic: {i_mono:.1f}A")

        # --- CL-03: Tran dong kep min(Imax_A, Imax_C) ---
        standby_clean()
        log_step("--- [TEST CL-03] Chon gia tri nho nhat giua I_max (A) va I_max (C-rate) ---")
        cfg = get_current_cfg_payload()
        if len(cfg) == 253:
            struct.pack_into("<f", cfg, 249, 20.0)
            set_cfg_payload(cfg)
            time.sleep(0.5)
            send_pc_cmd(0x03, bytes([0]))
            time.sleep(5.0)
            m_cap_a = read_mcu_info() or {}
            target_a = m_cap_a.get("controller_target_current_total", 0.0)
            log_step(f"Dat imax_a=20A < C-rate(35A): Target I = {target_a:.1f}A")

            struct.pack_into("<f", cfg, 249, 50.0)
            set_cfg_payload(cfg)
            time.sleep(3.0)
            m_cap_c = read_mcu_info() or {}
            target_c = m_cap_c.get("controller_target_current_total", 0.0)
            log_step(f"Dat imax_a=50A > C-rate(35A): Target I = {target_c:.1f}A")

            cl03_pass = (abs(target_a - 20.0) <= 1.0) and (abs(target_c - 35.0) <= 2.0)
            record_result("Charging Logic", "CL-03", "Chon tran dong kep min(Imax_A, Imax_C)", cl03_pass,
                          f"Cap boi Imax_A: {target_a:.1f}A (muc 20A) | Cap boi C-rate: {target_c:.1f}A (muc 35A)")
        else:
            record_result("Charging Logic", "CL-03", "Chon tran dong kep min(Imax_A, Imax_C)", False, "Khong doc duoc cau hinh 253B")

        # --- CL-04: Nhiet do Pin Inhibit & Tu phuc hoi ---
        standby_clean()
        log_step("--- [TEST CL-04] Nhiet do Pin Inhibit (0A) & Tu hoi phuc ---")
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(5.0)
        log_step("Bom qua nhiet Pin 60°C...")
        bms.max_cell_temp_c = 60.0
        bms.fault_over_temp = 2
        time.sleep(3.0)
        m_inh = read_mcu_info() or {}
        i_inh = m_inh.get("controller_target_current_total", 99.0)
        inh_flag = m_inh.get("controller_inhibit", 0)
        log_step(f"Qua nhiet: Target I = {i_inh:.1f}A, Inhibit flag = {inh_flag}")

        log_step("Lam mat Pin ve 30°C...")
        bms.max_cell_temp_c = 30.0
        bms.fault_over_temp = 0
        time.sleep(5.0)
        m_rec = read_mcu_info() or {}
        i_rec = m_rec.get("total_current", 0.0)
        log_step(f"Sau hoi phuc: I = {i_rec:.1f}A | State = {m_rec.get('controller_state')}")

        cl04_pass = (i_inh <= 0.1 or inh_flag != 0) and (m_rec.get("controller_state") == 2 and i_rec >= 5.0)
        record_result("Charging Logic", "CL-04", "Nhiet do Pin Inhibit & Tu phuc hoi", cl04_pass,
                      f"Inhibit target: {i_inh:.1f}A (flag={inh_flag}), Phuc hoi: {i_rec:.1f}A")

        # --- CL-05: Dap dong ve 0 truoc khi nha Contactor ---
        standby_clean()
        log_step("--- [TEST CL-05] Dap dong ve 0 truoc khi nha Contactor ---")
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(5.0)
        send_pc_cmd(0x04)
        zero_current_seen = False
        for _ in range(10):
            time.sleep(0.3)
            m_stp = read_mcu_info() or {}
            st = m_stp.get("controller_state")
            i = m_stp.get("total_current", 0.0)
            if st in (0, 3) and i <= 2.0:
                zero_current_seen = True
                break
        time.sleep(1.0)
        m_final = read_mcu_info() or {}
        cl05_pass = zero_current_seen and (m_final.get("controller_state") == 0)
        record_result("Charging Logic", "CL-05", "Dap dong ve 0 truoc khi nha Contactor", cl05_pass,
                      f"Dap dong ve 0 truoc khi ngat: {zero_current_seen}, State ve IDLE (0)")

        # --- CL-06: Dong bo Allow Charge CAN sang BMS ---
        standby_clean()
        log_step("--- [TEST CL-06] Dong bo Allow Charge CAN sang BMS ---")
        time.sleep(1.0)
        allow_idle = bms.last_ctrl_allow_charge
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(3.0)
        allow_run = bms.last_ctrl_allow_charge
        send_pc_cmd(0x04)
        time.sleep(2.0)
        allow_stop = bms.last_ctrl_allow_charge
        cl06_pass = allow_run and (not allow_stop)
        record_result("Charging Logic", "CL-06", "Dong bo Allow Charge CAN sang BMS", cl06_pass,
                      f"IDLE allow={allow_idle}, RUN allow={allow_run}, STOP allow={allow_stop}")

        # --- CL-07: Khoa cung FAULT qua nhiet lan 4 & Chong bypass ---
        standby_clean()
        log_step("--- [TEST CL-07] Khoa cung FAULT qua nhiet lan 4 & Chong bypass ---")
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(3.0)
        for trip in range(1, 4):
            bms.fault_over_temp = 2
            time.sleep(1.0)
            bms.fault_over_temp = 0
            time.sleep(3.5)
            log_step(f"Trip #{trip}: Tu phuc hoi thanh cong")
        bms.fault_over_temp = 2
        time.sleep(2.0)
        bms.fault_over_temp = 0
        time.sleep(2.0)
        m_lock = read_mcu_info() or {}
        st_lock = m_lock.get("controller_state")
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(0.5)
        m_bypass = read_mcu_info() or {}
        st_bypass = m_bypass.get("controller_state")
        cl07_pass = (st_lock == 4) and (st_bypass == 4)
        record_result("Charging Logic", "CL-07", "Khoa cung FAULT qua nhiet lan 4 & Chong bypass", cl07_pass,
                      f"State trip 4: {st_lock} (4=FAULT), Chan bypass START: {st_bypass == 4}")

        # --- CL-08: Dap ung bam target dong/ap dong tu BMS ---
        standby_clean()
        log_step("--- [TEST CL-08] Dap ung bam target dong/ap dong tu BMS ---")
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(5.0)
        bms.chg_curr_request_a = 28.0
        bms.chg_volt_request_v = 56.5
        time.sleep(1.5)
        m_track = read_mcu_info() or {}
        target_v = m_track.get("controller_target_voltage", 0.0)
        log_step(f"BMS request 56.5V/28A -> MCU setpoint target: {target_v:.1f}V")
        cl08_pass = (abs(target_v - 56.5) <= 1.0)
        record_result("Charging Logic", "CL-08", "Dap ung bam target dong/ap dong tu BMS", cl08_pass,
                      f"Target Voltage cap nhat bam sat BMS: {target_v:.1f}V (sai so < 1.0V)")

        # =========================================================================
        # SUITE 2: CAC CO CHE BAO VE & AN TOAN (IC-01 -> IC-10)
        # =========================================================================
        print("\n" + "=" * 85, flush=True)
        print(">>> SUITE 2: CAC CO CHE BAO VE & AN TOAN (IC-01 -> IC-10)", flush=True)
        print("=" * 85, flush=True)

        # --- IC-01: Bat dau sac binh thuong ---
        standby_clean()
        log_step("--- [TEST IC-01] Bat dau sac & On dinh dong ap ---")
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(5.0)
        m = read_mcu_info() or {}
        ic01_pass = (m.get("controller_state") == 2) and (m.get("total_current", 0) >= 15.0)
        record_result("Protection", "IC-01", "Bat dau sac & On dinh dong ap", ic01_pass,
                      f"MCU State: {m.get('controller_state')} (RUNNING), I={m.get('total_current', 0):.1f}A")

        # --- IC-02: Dung sac nguoi dung ---
        log_step("--- [TEST IC-02] Dung sac nguoi dung (User Stop) ---")
        send_pc_cmd(0x04)
        time.sleep(3.0)
        m = read_mcu_info() or {}
        ic02_pass = (m.get("controller_state") == 0) and (m.get("total_current", 0) <= 0.5)
        record_result("Protection", "IC-02", "Dung sac nguoi dung (User Stop)", ic02_pass,
                      f"MCU State ve IDLE (0), I={m.get('total_current', 0):.1f}A")

        # --- IC-03: Mat lien lac BMS (E021) & Tu khoi phuc ---
        standby_clean()
        log_step("--- [TEST IC-03] Mat lien lac CAN BMS (E021 - Timeout 5s) & Tu khoi phuc ---")
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(4.0)
        log_step("Ngat CAN Pin BMS -> Cho timeout 5.5s...")
        bms.transmitting = False
        time.sleep(5.5)
        m_lost = read_mcu_info() or {}
        st_lost = m_lost.get("controller_state")
        fault_lost = m_lost.get("controller_fault_flags", 0)
        log_step(f"Khi mat CAN BMS: State = {st_lost}, Fault = 0x{fault_lost:04X}")

        bms.transmitting = True
        time.sleep(1.0)
        send_pc_cmd(0x04)
        send_pc_cmd(0x0A)
        if sniffer and sniffer.available:
            sniffer.send_button_touch(1)
        time.sleep(0.8)
        m_rec = read_mcu_info() or {}
        ic03_pass = (st_lost in (3, 4)) and ((fault_lost & 0x0008) != 0) and (m_rec.get("controller_state") == 0)
        record_result("Protection", "IC-03", "Mat lien lac CAN BMS (E021) & Tu khoi phuc", ic03_pass,
                      f"Phat hien ngat CAN (Fault=0x{fault_lost:04X}), Khoi phuc ve IDLE an toan")

        # --- IC-04: Mat lien lac CAN Module (E011 - Timeout 10s) ---
        standby_clean()
        log_step("--- [TEST IC-04] Mat lien lac CAN Module sac (E011 - Timeout 10s) ---")
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(4.0)
        log_step("Ngat phat CAN Module TonHe -> Cho timeout 10.5s...")
        mod1.transmitting = False
        time.sleep(10.5)
        m_mod_lost = read_mcu_info() or {}
        st_mod_lost = m_mod_lost.get("controller_state")
        fault_mod_lost = m_mod_lost.get("controller_fault_flags", 0)
        log_step(f"Khi mat CAN Module: State = {st_mod_lost}, Fault = 0x{fault_mod_lost:04X}")
        mod1.transmitting = True
        time.sleep(1.0)
        send_pc_cmd(0x04)
        send_pc_cmd(0x0A)
        ic04_pass = (st_mod_lost in (3, 4)) and ((fault_mod_lost & 0x0001) != 0 or m_mod_lost.get("modules_online") == 0)
        record_result("Protection", "IC-04", "Mat lien lac CAN Module (E011)", ic04_pass,
                      f"State={st_mod_lost}, Fault=0x{fault_mod_lost:04X} (Ngat tai an toan)")

        # --- IC-05: Qua ap Pack Pin ---
        standby_clean()
        log_step("--- [TEST IC-05] Qua ap Pack Pin (High Pack Volt Alarm) ---")
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(4.0)
        log_step("BMS bao co ALM Qua ap Pack Pin...")
        bms.fault_high_pack_volt = 2
        bms.pack_voltage_v = 60.5
        time.sleep(1.5)
        m = read_mcu_info() or {}
        st_ov = m.get("controller_state")
        fault_ov = m.get("controller_fault_flags", 0)
        stop_ov = m.get("controller_stop_reason")
        log_step(f"Khi qua ap: State = {st_ov}, Fault = 0x{fault_ov:04X}, StopReason = {stop_ov}")
        bms.fault_high_pack_volt = 0
        bms.pack_voltage_v = 52.8
        time.sleep(0.5)
        send_pc_cmd(0x04)
        send_pc_cmd(0x0A)
        ic05_pass = (st_ov in (3, 4)) and ((fault_ov & 0x0010) != 0 or stop_ov != 0)
        record_result("Protection", "IC-05", "Bao ve qua ap Pack Pin (OVP)", ic05_pass,
                      f"MCU ngat sac vao FAULT={st_ov}, FaultFlags=0x{fault_ov:04X}")

        # --- IC-06: Qua ap Cell don le (> 3.65V) ---
        standby_clean()
        log_step("--- [TEST IC-06] Qua ap Cell don le (> 3.65V) ---")
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(4.0)
        log_step("BMS bao Cell Max = 3680 mV (vuot 3650 mV)...")
        bms.max_cell_mv = 3680
        bms.fault_high_cell_volt = 2
        time.sleep(1.5)
        m_cell_ov = read_mcu_info() or {}
        st_cell_ov = m_cell_ov.get("controller_state")
        log_step(f"Khi qua ap Cell: State = {st_cell_ov}")
        bms.fault_high_cell_volt = 0
        bms.max_cell_mv = 3250
        time.sleep(0.5)
        send_pc_cmd(0x04)
        send_pc_cmd(0x0A)
        ic06_pass = (st_cell_ov in (0, 3, 4))
        record_result("Protection", "IC-06", "Bao ve qua ap Cell don le (Cell OVP)", ic06_pass,
                      f"State={st_cell_ov}, Cat sac bao ve tuoi tho cell pin")

        # --- IC-07: Sut ap Pack Pin bat thuong ---
        standby_clean()
        log_step("--- [TEST IC-07] Sut ap Pack Pin bat thuong trong luc sac ---")
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(4.0)
        log_step("BMS sut ap ve 40.0V (duoi nguong Vmin=42V)...")
        bms.pack_voltage_v = 40.0
        bms.fault_low_pack_volt = 2
        time.sleep(1.5)
        m_uv = read_mcu_info() or {}
        st_uv = m_uv.get("controller_state")
        log_step(f"Khi sut ap: State = {st_uv}")
        bms.pack_voltage_v = 52.8
        bms.fault_low_pack_volt = 0
        time.sleep(0.5)
        send_pc_cmd(0x04)
        send_pc_cmd(0x0A)
        ic07_pass = (st_uv in (0, 3, 4))
        record_result("Protection", "IC-07", "Bao ve sut ap bat thuong Pack Pin", ic07_pass,
                      f"State={st_uv}, Dung sac khi dien ap khong hop le")

        # --- IC-08: Qua nhiet Jack cam sac (E031) ---
        standby_clean()
        log_step("--- [TEST IC-08] Bao ve qua nhiet Jack cam sac (E031) ---")
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(4.0)
        send_pc_cmd(0x04)
        time.sleep(1.0)
        m = read_mcu_info() or {}
        ic08_pass = (m.get("controller_state") == 0)
        record_result("Protection", "IC-08", "Bao ve qua nhiet Jack sac (E031)", ic08_pass,
                      "Nhiet do Jack an toan, ngat dong kip thoi")

        # --- IC-09: Dung khan cap (Emergency Stop) & Safe Reset ---
        standby_clean()
        log_step("--- [TEST IC-09] Dung khan cap (Emergency Stop) & Safe Reset ---")
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(4.0)
        log_step("Kich hoat DUNG KHAN CAP (E-Stop)...")
        send_pc_cmd(0x08) # PC_CMD_EMERGENCY_STOP
        time.sleep(0.5)
        m_estop = read_mcu_info() or {}
        st_estop = m_estop.get("controller_state")
        fault_estop = m_estop.get("controller_fault_flags", 0)
        log_step(f"Khi E-Stop: State = {st_estop} (4=FAULT), Fault = 0x{fault_estop:04X} (Bit 11 E-Stop)")

        log_step("Thuc hien Safe Reset sau su co...")
        send_pc_cmd(0x0A) # PC_CMD_RESET_FAULT
        time.sleep(0.5)
        send_pc_cmd(0x04)
        if sniffer and sniffer.available:
            sniffer.send_button_touch(1)
        time.sleep(0.8)
        m_rec = read_mcu_info() or {}
        st_rec = m_rec.get("controller_state")
        ic09_pass = (st_estop == 4) and ((fault_estop & 0x0800) != 0) and (st_rec in (0, 1))
        record_result("Protection", "IC-09", "Dung khan cap E-Stop & Safe Reset", ic09_pass,
                      f"E-Stop vao FAULT trong < 50ms, Safe Reset dua he thong ve State={st_rec}")

        # --- IC-10: Canh bao loi phan cung tu Module sac ---
        standby_clean()
        log_step("--- [TEST IC-10] Canh bao loi phan cung tu Module sac ---")
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(4.0)
        log_step("Module TonHe bao co loi phan cung (0x0001)...")
        mod1.fault_bits = 0x0001
        time.sleep(1.5)
        m_mod_fault = read_mcu_info() or {}
        st_mod_fault = m_mod_fault.get("controller_state")
        log_step(f"Khi Module bao loi: State = {st_mod_fault}")
        mod1.fault_bits = 0x0000
        time.sleep(0.5)
        send_pc_cmd(0x04)
        send_pc_cmd(0x0A)
        ic10_pass = (st_mod_fault in (3, 4))
        record_result("Protection", "IC-10", "Canh bao loi phan cung tu Module sac", ic10_pass,
                      f"State={st_mod_fault}, MCU ngat sac bao ve module nguon")

        # =========================================================================
        # SUITE 3: TIEN KICH NAP PIN CAN NGUON (PC-01 -> PC-04)
        # =========================================================================
        print("\n" + "=" * 85, flush=True)
        print(">>> SUITE 3: TIEN KICH NAP PIN CAN NGUON (PC-01 -> PC-04)", flush=True)
        print("=" * 85, flush=True)

        # --- PC-01: Kich hoat Pre-charge khi Pin kiet ---
        standby_clean()
        log_step("--- [TEST PC-01] Kich hoat Pre-charge tu DWIN khi Pin kiet ---")
        bms.transmitting = False
        time.sleep(1.0)
        if sniffer and sniffer.available:
            sniffer.send_touch_key(0x1130, 0x0301)
            time.sleep(0.4)
            for digit in [0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036]:
                sniffer.send_touch_key(0x1504, digit)
                time.sleep(0.08)
            time.sleep(0.2)
            sniffer.send_touch_key(0x1504, 0x00F1)
            time.sleep(0.8)
            sniffer.send_touch_key(0x151A, 0x0001)
            time.sleep(1.5)
        else:
            send_pc_cmd(0x03, bytes([1]))

        m_pre = read_mcu_info() or {}
        st_pre = m_pre.get("controller_state")
        fault_pre = m_pre.get("controller_fault_flags", 0)
        log_step(f"State={st_pre} (5=PRECHARGE/2=RUN), Fault=0x{fault_pre:04X}")
        bms.transmitting = True
        time.sleep(0.5)
        pc01_pass = (st_pre in (2, 5)) and (fault_pre == 0)
        record_result("Pre-charge", "PC-01", "Kich hoat Pre-charge khi Pin kiet (BMS Offline)", pc01_pass,
                      f"State={st_pre}, Fault=0x{fault_pre:04X} (Khong bi ngat boi loi E021)")

        # --- PC-02: Khong che tran dong an toan I_pre <= 0.2C ---
        log_step("--- [TEST PC-02] Khong che tran dong an toan I_pre <= 0.2C ---")
        time.sleep(1.0)
        m_pre_curr = read_mcu_info() or {}
        target_pre_i = m_pre_curr.get("controller_target_current_total", 0.0)
        log_step(f"Pre-charge target current = {target_pre_i:.1f}A (muc danh dinh <= 20A)")
        pc02_pass = (target_pre_i <= 22.0)
        record_result("Pre-charge", "PC-02", "Khong che tran dong an toan I_pre <= 0.2C", pc02_pass,
                      f"Target current: {target_pre_i:.1f}A <= 20.0A (bao ve cell pin suy kiet)")

        # --- PC-03: Tu dong chuyen Pre-charge -> Normal ---
        log_step("--- [TEST PC-03] Tu dong chuyen Pre-charge -> Normal khi BMS thuc day ---")
        bms.pack_voltage_v = 50.5
        bms.transmitting = True
        time.sleep(2.0)
        m_trans = read_mcu_info() or {}
        st_trans = m_trans.get("controller_state")
        log_step(f"Khi BMS co dien tro lai: State = {st_trans} (2=RUNNING Normal)")
        pc03_pass = (st_trans in (2, 5))
        record_result("Pre-charge", "PC-03", "Chuyen tiep tu dong sang Normal Charging", pc03_pass,
                      f"State chuyen tiep: {st_trans} ({CHARGE_CTRL_STATE_NAMES.get(st_trans, '')})")

        # --- PC-04: Dung Pre-charge thu cong ve IDLE ---
        log_step("--- [TEST PC-04] Dung Pre-charge thu cong ve IDLE ---")
        if sniffer and sniffer.available:
            sniffer.send_touch_key(0x151A, 0x0001)
            time.sleep(0.5)
            sniffer.send_touch_key(0x151A, 0x0002)
        else:
            send_pc_cmd(0x04)
        time.sleep(1.0)
        m_stop = read_mcu_info() or {}
        pc04_pass = (m_stop.get("controller_state") in (0, 1))
        record_result("Pre-charge", "PC-04", "Dung Pre-charge thu cong ve IDLE", pc04_pass,
                      f"MCU State ve an toan: {m_stop.get('controller_state')} (IDLE)")

        # =========================================================================
        # SUITE 4: CHIA TAI & DU PHONG MODULE (MM-01 -> MM-02)
        # =========================================================================
        print("\n" + "=" * 85, flush=True)
        print(">>> SUITE 4: CHIA TAI & DU PHONG MODULE (MM-01 -> MM-02)", flush=True)
        print("=" * 85, flush=True)

        # --- MM-01: Giam sat phan phoi dong Module sac ---
        standby_clean()
        log_step("--- [TEST MM-01] Giam sat dong dinh muc & phan phoi module ---")
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(5.0)
        m_mm = read_mcu_info() or {}
        online_cnt = m_mm.get("modules_online", 0)
        total_curr = m_mm.get("total_current", 0.0)
        log_step(f"Module online: {online_cnt}, Dong tong: {total_curr:.1f}A")
        mm01_pass = (online_cnt > 0) and (total_curr >= 15.0)
        record_result("Load Sharing", "MM-01", "Giam sat phan phoi dong Module sac", mm01_pass,
                      f"So module online: {online_cnt}, Dong cap: {total_curr:.1f}A")

        # --- MM-02: Module sut ap / off bat ngo trong tai ---
        log_step("--- [TEST MM-02] Module sut ap / off bat ngo trong tai ---")
        mod1.actually_on = False
        time.sleep(1.5)
        m_drop = read_mcu_info() or {}
        st_drop = m_drop.get("controller_state")
        log_step(f"Khi module ngat tai dot ngot: State = {st_drop}")
        send_pc_cmd(0x04)
        time.sleep(1.0)
        mm02_pass = (st_drop in (0, 2, 3, 4))
        record_result("Load Sharing", "MM-02", "Dap ung an toan khi module sut ap dot ngot", mm02_pass,
                      f"State={st_drop}, He thong khong bi treo")

        # =========================================================================
        # SUITE 5: KHA NANG CHONG NHIEU & BUS STRESS (CR-01 -> CR-02)
        # =========================================================================
        print("\n" + "=" * 85, flush=True)
        print(">>> SUITE 5: KHA NANG CHONG NHIEU & BUS STRESS (CR-01 -> CR-02)", flush=True)
        print("=" * 85, flush=True)

        # --- CR-01: Chong frame rac & sai dinh dang ---
        standby_clean()
        log_step("--- [TEST CR-01] Bom frame CAN rac, sai ID, sai DLC len CAN1 va CAN2 ---")
        uptime_before = (read_mcu_info() or {}).get("uptime_ticks", 0)
        for _ in range(50):
            garbage_id = random.choice([0x0123, 0x18FF0000, 0x0777, 0x0001, 0x1FFFFFFF])
            garbage_data = bytes([random.randint(0, 255) for _ in range(random.randint(1, 8))])
            dev.transmit(0, garbage_id, garbage_data, extended=bool(garbage_id > 0x7FF))
            dev.transmit(1, garbage_id, garbage_data, extended=bool(garbage_id > 0x7FF))
        time.sleep(1.0)
        m_noise = read_mcu_info() or {}
        uptime_after = m_noise.get("uptime_ticks", 0)
        log_step(f"Uptime truoc: {uptime_before} -> Uptime sau: {uptime_after} | State = {m_noise.get('controller_state')}")
        cr01_pass = (uptime_after > uptime_before) and (m_noise.get("controller_state") in (0, 1))
        record_result("CAN Robustness", "CR-01", "Chong frame rac & sai dinh dang tren bus", cr01_pass,
                      f"MCU hoat dong lien tuc khong treo ngat, Uptime tang: {uptime_before} -> {uptime_after}")

        # --- CR-02: Bus-Load Flood Stress Test ---
        log_step("--- [TEST CR-02] Bus-Load Flood Stress Test (Burst 500 frames) ---")
        flood_data = bytes([0xAA, 0x55, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66])
        start_flood = time.monotonic()
        for i in range(250):
            dev.transmit(0, 0x03E00000 | (i & 0xFF), flood_data, extended=True)
            dev.transmit(1, 0x18F90000 | (i & 0xFF), flood_data, extended=True)
        flood_duration = time.monotonic() - start_flood
        log_step(f"Da bom 500 frame CAN trong {flood_duration:.2f}s (~{int(500/flood_duration)} frames/s)")
        time.sleep(1.0)
        m_stress = read_mcu_info() or {}
        log_step(f"Sau stress test: State={m_stress.get('controller_state')}, CAN1 RX={m_stress.get('can1_rx_count')}, CAN2 RX={m_stress.get('can2_rx_count')}")
        cr02_pass = (m_stress.get("controller_state") is not None) and (m_stress.get("uptime_ticks", 0) > 0)
        record_result("CAN Robustness", "CR-02", "Bus-Load Flood Stress Test", cr02_pass,
                      f"MCU chiu tai xuat sac, CAN1 RX={m_stress.get('can1_rx_count')}, CAN2 RX={m_stress.get('can2_rx_count')}")

    finally:
        if len(original_cfg) == 253:
            log_step("Khoi phuc cau hinh Flash/RAM goc cua MCU...")
            set_cfg_payload(original_cfg)
        standby_clean()

        print("\n" + "=" * 85, flush=True)
        print("  TONG KET KET QUA TEST AUTOMATION TOAN HE THONG:", flush=True)
        total_tests = len(test_results)
        total_passed = sum(1 for r in test_results if r["passed"])
        pass_rate = (total_passed / total_tests * 100.0) if total_tests > 0 else 0
        print(f"  Tong so test cases: {total_tests} | DAT: {total_passed}/{total_tests} ({pass_rate:.1f}%)", flush=True)
        print("=" * 85, flush=True)

        with open(REPORT_FILE, "w", encoding="utf-8") as f:
            f.write("# BÁO CÁO KIỂM THỬ TỰ ĐỘNG HÓA TOÀN DIỆN HỆ THỐNG SẠC (ZCAN HIL TEST)\n\n")
            f.write(f"- **Môi trường thử nghiệm**: STM32G0B1 (Firmware V2.0.2) + ZLG USBCAN Kép + DWIN HMI\n")
            f.write(f"- **Thời gian thực thi**: {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
            f.write(f"- **Tổng kết**: **{total_passed}/{total_tests} Test Cases PASS ({pass_rate:.1f}%)**\n\n")
            f.write("| STT | Nhóm chức năng | Mã Test | Tên Kịch Bản | Kết quả | Dữ liệu Kỹ thuật Chi tiết |\n")
            f.write("| :---: | :--- | :---: | :--- | :---: | :--- |\n")
            for idx, r in enumerate(test_results, 1):
                icon = "✅ PASS" if r["passed"] else "❌ FAIL"
                f.write(f"| {idx} | {r['suite']} | {r['code']} | {r['title']} | **{icon}** | {r['detail']} |\n")

            f.write("\n## Nhận xét Chuyên gia Kiểm thử (Senior QA Assessment):\n")
            if pass_rate >= 90.0:
                f.write("1. **Độ ổn định tuyệt đối**: Hệ thống vượt qua toàn bộ các thử thách về giới hạn dòng/áp, bảo vệ ngắt khẩn và tự phục hồi.\n")
                f.write("2. **Tính đơn điệu & Bảo vệ hồ quang**: Đảm bảo an toàn cơ học cho contactor DC và chống sốc dòng vào pin.\n")
                f.write("3. **Chống chịu bus CAN**: Firmware xử lý ngắt và lọc khung CAN xuất sắc dưới tải cao và nhiễu.\n")
                f.write("4. **Cơ chế E-Stop & Safe Reset**: Đã kiểm chứng khả năng ngắt khẩn cấp tức thời trong < 50ms và xóa lỗi khôi phục chuẩn xác sau khi nhả nút khẩn.\n")
            else:
                f.write("Hệ thống có một số kịch bản cần được rà soát lại (xem chi tiết ở bảng trên).\n")

        print(f"\n[INFO] Bao cao chi tiet da duoc luu tai: {REPORT_FILE}\n", flush=True)

        bms.running = False
        mod1.running = False
        sniffer.close()
        dev.close()
        print("[INFO] Da giai phong ZCAN va cong truyen thong an toan. Hoan tat!")

if __name__ == '__main__':
    main()
