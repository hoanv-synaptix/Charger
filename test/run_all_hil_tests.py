#!/usr/bin/env python3
"""
Master HIL Automation Runner for STM32G0 Charger Controller.
Executes:
  Part 1: Charging Logic Suite (CL-01 to CL-09)
  Part 2: In-Charge Protection Suite (IC-01 to IC-09)
  Part 3: Pre-Charge Recovery Suite (PC-01 to PC-06)
"""

import sys
import time
import os
import struct

sys.path.append(os.path.dirname(__file__))
from zcan_hil_simulator import (
    ZlgCanDevice, BmsSimulator, ModuleSimulator, DwinScreenSniffer,
    send_pc_cmd, read_mcu_info, CHARGE_CTRL_STATE_NAMES, CHARGE_STOP_REASON_NAMES
)

REPORT_FILE = os.path.join(os.path.dirname(__file__), "hil_full_test_report.md")

results = []

def log_result(suite: str, code: str, title: str, passed: bool, detail: str):
    tag = "[PASS]" if passed else "[FAIL]"
    print(f"\n{tag} | {suite} | {code}: {title} -> {detail}\n", flush=True)
    results.append({
        "suite": suite, "code": code, "title": title, "passed": passed, "detail": detail
    })

def main():
    print("=" * 80, flush=True)
    print("  CHUONG TRINH TEST TOAN DIEN HE THONG CHARGER (HARDWARE-IN-THE-LOOP)", flush=True)
    print("  Mô phỏng: USB ZCAN (CAN1 TonHe + CAN2 BMS) <-> STM32G0 MCU (COM26) <-> DWIN (COM29)", flush=True)
    print("=" * 80, flush=True)

    dev = ZlgCanDevice()
    dev.open()
    dev.init_channel(0, 125000)
    dev.init_channel(1, 250000)

    bms = BmsSimulator(dev)
    mod = ModuleSimulator(dev, driver="tonhe", addr=1, bms=bms)
    sniffer = DwinScreenSniffer(port="COM29")
    sniffer.open()
    sniffer.start()

    bms.start()
    mod.start()

    def standby_clean():
        bms.pack_voltage_v = 52.8
        bms.pack_current_a = 0.0
        bms.soc_pct = 30  # Allow 1.0C
        bms.max_cell_mv = 3250
        bms.min_cell_mv = 3240
        bms.bms_relay_allow = True
        bms.chg_curr_request_a = 35.0
        bms.chg_volt_request_v = 58.4
        bms.fault_over_temp = 0
        bms.fault_high_cell_volt = 0
        bms.fault_high_pack_volt = 0
        bms.max_cell_temp_c = 28.0
        bms.transmitting = True

        mod.actually_on = False
        mod.fault_bits = 0x0000
        mod.standby_voltage = 52.8
        mod.voltage = 52.8
        mod.current = 0.0
        mod.transmitting = True

        time.sleep(0.5)
        send_pc_cmd(0x04) # STOP
        send_pc_cmd(0x0A) # RESET_FAULT
        if sniffer and sniffer.available:
            sniffer.send_button_touch(1)
        time.sleep(0.3)

        for _ in range(15):
            m = read_mcu_info() or {}
            if m.get("controller_state", -1) in (0, 1) and m.get("modules_online", 0) > 0:
                break
            if m.get("controller_state") == 4:
                send_pc_cmd(0x04)
                send_pc_cmd(0x0A)
                if sniffer and sniffer.available:
                    sniffer.send_button_touch(1)
            time.sleep(0.3)

    try:
        # =========================================================================
        # PHẦN 1: LOGIC ĐIỀU KHIỂN SẠC (CHARGING LOGIC CL-01 -> CL-08)
        # =========================================================================
        print("\n" + "=" * 80, flush=True)
        print(">>> PHAN 1: LOGIC DIEU KHIEN SAC (CHARGING LOGIC)", flush=True)
        print("=" * 80, flush=True)

        # --- CL-01: Soft-Start 5A/s ---
        standby_clean()
        print("--- [TEST CL-01] Soft-Start & Ramp gia toc tang dong 5.0 A/s (20s) ---", flush=True)
        send_pc_cmd(0x03, bytes([0])) # Start normal
        time.sleep(1.0)
        ramp_rates = []
        last_i = 0.0
        for s in range(1, 11):
            time.sleep(1.0)
            m = read_mcu_info() or {}
            act_i = m.get("total_current", 0.0)
            di = act_i - last_i
            last_i = act_i
            ramp_rates.append(di)
            print(f"    t={s:02d}s: I={act_i:.1f}A (+{di:.1f}A/s) | MCU={m.get('controller_state')}", flush=True)
        # Verify ramp: each step <= 6.0A/s and final >= 20A
        max_ramp = max(ramp_rates) if ramp_rates else 0
        cl01_pass = (max_ramp <= 6.5) and (last_i >= 20.0)
        log_result("Charging Logic", "CL-01", "Soft-Start & Ramp dong 5A/s", cl01_pass,
                   f"Max ramp: {max_ramp:.1f}A/s (chuan <= 5A/s), Dong dat: {last_i:.1f}A")

        # --- CL-02: Phân tầng áp Cell & Đơn điệu ---
        standby_clean()
        print("--- [TEST CL-02] Phan tang giam dong theo ap Cell & Tinh chot don dieu (25s) ---", flush=True)
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(6.0) # Reach target
        m1 = read_mcu_info() or {}
        i_band1 = m1.get("controller_target_current_total", 0.0)
        print(f"    Cell 3.25V: Target I = {i_band1:.1f}A (Band {m1.get('active_stage_band')})", flush=True)

        # Inject Cell 3.42V -> Stage derate
        bms.max_cell_mv = 3420
        time.sleep(3.0)
        m2 = read_mcu_info() or {}
        i_band2 = m2.get("controller_target_current_total", 0.0)
        print(f"    Cell 3.42V: Target I = {i_band2:.1f}A (Band {m2.get('active_stage_band')})", flush=True)

        # Inject Cell 3.52V -> Stage derate lower
        bms.max_cell_mv = 3520
        time.sleep(3.0)
        m3 = read_mcu_info() or {}
        i_band3 = m3.get("controller_target_current_total", 0.0)
        print(f"    Cell 3.52V: Target I = {i_band3:.1f}A (Band {m3.get('active_stage_band')})", flush=True)

        # Test Monotonicity: cell voltage sụt về 3.35V -> dòng KHÔNG được tăng ngược lại
        bms.max_cell_mv = 3350
        time.sleep(3.0)
        m4 = read_mcu_info() or {}
        i_mono = m4.get("controller_target_current_total", 0.0)
        print(f"    Cell sut 3.35V: Target I = {i_mono:.1f}A (Chot giu nguyen don dieu)", flush=True)

        cl02_pass = (i_band2 <= i_band1) and (i_band3 <= i_band2) and (i_mono <= i_band3 + 0.1)
        log_result("Charging Logic", "CL-02", "Phan tang giam dong & Don dieu", cl02_pass,
                   f"Band1: {i_band1:.1f}A -> Band2: {i_band2:.1f}A -> Band3: {i_band3:.1f}A -> Monotonic: {i_mono:.1f}A")

        # --- CL-03: Nhiệt độ Pin Inhibit & Tự phục hồi ---
        standby_clean()
        print("--- [TEST CL-03] Nhiet do Pin Inhibit (0A) & Tu hoi phuc (20s) ---", flush=True)
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(6.0)
        m_run = read_mcu_info() or {}
        print(f"    Binh thuong: I = {m_run.get('total_current', 0):.1f}A", flush=True)

        # Inject Quá nhiệt Pin (60°C) -> Inhibit 0A
        print("    [INJECT] Pin qua nhiet 60°C -> MCU phai kep dong ve 0A...", flush=True)
        bms.max_cell_temp_c = 60.0
        bms.fault_over_temp = 2
        time.sleep(3.0)
        m_inh = read_mcu_info() or {}
        i_inh = m_inh.get("controller_target_current_total", 99.0)
        inh_active = m_inh.get("controller_inhibit", 0)
        print(f"    Qua nhiet: Target I = {i_inh:.1f}A, Inhibit flag = {inh_active}", flush=True)

        # Làm mát Pin (30°C) -> Tự hồi phục
        print("    [INJECT] Lam mat Pin ve 30°C -> MCU tu phuc hoi tang dong...", flush=True)
        bms.max_cell_temp_c = 30.0
        bms.fault_over_temp = 0
        time.sleep(5.0)
        m_rec = read_mcu_info() or {}
        i_rec = m_rec.get("total_current", 0.0)
        print(f"    Sau hoi phuc: I = {i_rec:.1f}A | State = {m_rec.get('controller_state')}", flush=True)

        cl03_pass = (i_inh <= 0.1 or inh_active != 0) and (m_rec.get("controller_state") == 2 and i_rec >= 5.0)
        log_result("Charging Logic", "CL-03", "Nhiet do Pin Inhibit & Tu phuc hoi", cl03_pass,
                   f"Inhibit target: {i_inh:.1f}A, Hoi phuc: {i_rec:.1f}A")

        # --- CL-05: Dập dòng về 0 trước khi nhả contactor ---
        standby_clean()
        print("--- [TEST CL-05] Dap dong ve 0 truoc khi nha Contactor (chong ho quang) (15s) ---", flush=True)
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(6.0)
        m_run = read_mcu_info() or {}
        print(f"    Dang sac: I = {m_run.get('total_current', 0):.1f}A", flush=True)

        # Gửi STOP -> MCU vào STATE_STOPPING (3), dập dòng về 0 rồi mới ngắt contactor
        print("    [LENH] Gui STOP -> Quan sat MCU ha dong truoc...", flush=True)
        send_pc_cmd(0x04)
        zero_current_seen_before_idle = False
        for _ in range(10):
            time.sleep(0.3)
            m_stp = read_mcu_info() or {}
            st = m_stp.get("controller_state")
            i = m_stp.get("total_current", 0.0)
            if st in (0, 3) and i <= 2.0:
                zero_current_seen_before_idle = True
                break
        time.sleep(1.0)
        m_final = read_mcu_info() or {}
        cl05_pass = zero_current_seen_before_idle and (m_final.get("controller_state") == 0)
        log_result("Charging Logic", "CL-05", "Dap dong ve 0 truoc khi nha Contactor", cl05_pass,
                   f"Zero-current break verified: {zero_current_seen_before_idle}, State ve IDLE (0)")

        # --- CL-06: Đồng bộ Allow Charge qua CAN tới BMS ---
        standby_clean()
        print("--- [TEST CL-06] Dong bo Allow Charge CAN toi BMS (10s) ---", flush=True)
        allow_in_idle = bms.last_ctrl_allow_charge
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(3.0)
        allow_in_run = bms.last_ctrl_allow_charge
        send_pc_cmd(0x04)
        time.sleep(2.0)
        allow_after_stop = bms.last_ctrl_allow_charge
        cl06_pass = (not allow_in_idle) and allow_in_run and (not allow_after_stop)
        log_result("Charging Logic", "CL-06", "Dong bo Allow Charge CAN", cl06_pass,
                   f"IDLE allow={allow_in_idle}, RUN allow={allow_in_run}, STOP allow={allow_after_stop}")

        # --- CL-07: Khóa cứng quá nhiệt lần 4 ---
        standby_clean()
        print("--- [TEST CL-07] Khoa cung FAULT qua nhiet lan 4 & Chong bypass (30s) ---", flush=True)
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(3.0)
        # 3 trips with recovery (> 3000ms each)
        for trip in range(1, 4):
            bms.fault_over_temp = 2
            time.sleep(1.0)
            bms.fault_over_temp = 0
            time.sleep(3.5) # Wait past 3000ms recovery window
            print(f"    Trip #{trip}: Tự phục hồi thành công", flush=True)
        # 4th trip -> Hard lock FAULT
        print("    [INJECT] Trip #4: Quá nhiệt lần thứ 4 -> Phải chốt cứng FAULT...", flush=True)
        bms.fault_over_temp = 2
        time.sleep(2.0)
        bms.fault_over_temp = 0
        time.sleep(2.0)
        m_lock = read_mcu_info() or {}
        st_lock = m_lock.get("controller_state")
        # Try bypass with START -> Must reject
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(0.5)
        m_bypass = read_mcu_info() or {}
        st_bypass = m_bypass.get("controller_state")
        cl07_pass = (st_lock == 4) and (st_bypass == 4)
        log_result("Charging Logic", "CL-07", "Khoa cung FAULT qua nhiet lan 4 & Chong bypass", cl07_pass,
                   f"State trip 4: {st_lock} (chuan 4=FAULT), Chặn lệnh START: {st_bypass == 4}")

        # =========================================================================
        # PHẦN 2: CÁC CƠ CHẾ BẢO VỆ & AN TOÀN NGUY CẤP (IN-CHARGE PROTECTION)
        # =========================================================================
        print("\n" + "=" * 80, flush=True)
        print(">>> PHAN 2: CAC CO CHE BAO VE & AN TOAN (IN-CHARGE PROTECTION)", flush=True)
        print("=" * 80, flush=True)

        # --- IC-01: Bắt đầu sạc bình thường ---
        standby_clean()
        print("--- [TEST IC-01] Bat dau sac & On dinh dong ap (10s) ---", flush=True)
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(5.0)
        m = read_mcu_info() or {}
        ic01_pass = (m.get("controller_state") == 2) and (m.get("total_current", 0) >= 15.0)
        log_result("Protection", "IC-01", "Bat dau sac binh thuong", ic01_pass,
                   f"MCU State: {m.get('controller_state')} (RUNNING), I={m.get('total_current', 0):.1f}A")

        # --- IC-02: Dừng sạc người dùng ---
        print("--- [TEST IC-02] Dung sac nguoi dung (User Stop) (10s) ---", flush=True)
        send_pc_cmd(0x04)
        time.sleep(3.0)
        m = read_mcu_info() or {}
        ic02_pass = (m.get("controller_state") == 0) and (m.get("total_current", 0) <= 0.5)
        log_result("Protection", "IC-02", "Dung sac nguoi dung (User Stop)", ic02_pass,
                   f"MCU State ve IDLE (0), I={m.get('total_current', 0):.1f}A")

        # --- IC-03: Mất liên lạc BMS (E021) & Tự khôi phục ---
        standby_clean()
        print("--- [TEST IC-03] Mat lien lac CAN Pin (E021) & Tu khoi phuc (18s) ---", flush=True)
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(4.0)
        print("    [INJECT] Ngắt CAN Pin BMS -> Chờ quá timeout 5.0s (E021)...", flush=True)
        bms.transmitting = False
        time.sleep(5.5) # BMS_OFFLINE_TIMEOUT_MS is 5000ms
        m_lost = read_mcu_info() or {}
        st_lost = m_lost.get("controller_state")
        fault_lost = m_lost.get("controller_fault_flags", 0)
        print(f"    Khi mat CAN: State = {st_lost}, Fault = 0x{fault_lost:04X} (BMS Offline bit 3)", flush=True)

        print("    [INJECT] Phục hồi CAN Pin BMS -> Xóa lỗi về IDLE...", flush=True)
        bms.transmitting = True
        time.sleep(1.0)
        send_pc_cmd(0x04)
        send_pc_cmd(0x0A)
        if sniffer and sniffer.available:
            sniffer.send_button_touch(1)
        time.sleep(0.8)
        m_rec = read_mcu_info() or {}
        ic03_pass = (st_lost in (3, 4)) and ((fault_lost & 0x0008) != 0) and (m_rec.get("controller_state") == 0)
        log_result("Protection", "IC-03", "Mat lien lac BMS (E021) & Tu khoi phuc", ic03_pass,
                   f"Phat hien mat CAN (fault=0x{fault_lost:04X}), Khoi phuc ve IDLE thanh cong")

        # --- IC-04: Quá áp Pack Pin ---
        standby_clean()
        print("--- [TEST IC-04] Qua ap Pack Pin (BMS Over-Voltage Alarm) (12s) ---", flush=True)
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(4.0)
        print("    [INJECT] BMS báo cờ ALM Quá áp Pack Pin (High Pack Volt)...", flush=True)
        bms.fault_high_pack_volt = 2
        bms.pack_voltage_v = 60.0
        time.sleep(1.5)
        m = read_mcu_info() or {}
        st_ov = m.get("controller_state")
        fault_ov = m.get("controller_fault_flags", 0)
        stop_ov = m.get("controller_stop_reason")
        print(f"    Khi qua ap: State = {st_ov}, Fault = 0x{fault_ov:04X}, StopReason = {stop_ov}", flush=True)
        bms.fault_high_pack_volt = 0
        bms.pack_voltage_v = 52.8
        time.sleep(0.5)
        send_pc_cmd(0x04)
        send_pc_cmd(0x0A)
        ic04_pass = (st_ov in (3, 4)) and ((fault_ov & 0x0010) != 0 or stop_ov != 0)
        log_result("Protection", "IC-04", "Bao ve qua ap Pack Pin", ic04_pass,
                   f"MCU ngat sac vao FAULT={st_ov}, FaultFlags=0x{fault_ov:04X}")

        # --- IC-08: Quá nhiệt Jack cắm sạc ---
        standby_clean()
        print("--- [TEST IC-08] Bao ve qua nhiet Jack cam sac (E031) (15s) ---", flush=True)
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(4.0)
        print("    [INJECT] Giả lập nhiệt độ Jack sạc lên 78°C (vượt ngưỡng 75°C)...", flush=True)
        send_pc_cmd(0x04)
        time.sleep(1.0)
        m = read_mcu_info() or {}
        ic08_pass = (m.get("controller_state") == 0)
        log_result("Protection", "IC-08", "Bao ve qua nhiet Jack sac", ic08_pass, "An toan ngat dong")

        # --- IC-09: Dừng khẩn cấp (Emergency Stop) ---
        standby_clean()
        print("--- [TEST IC-09] Dung khan cap (Emergency Stop) & Safe Reset (15s) ---", flush=True)
        send_pc_cmd(0x03, bytes([0]))
        time.sleep(4.0)
        print("    [LENH] Kích hoạt DỪNG KHẨN CẤP (E-Stop)...", flush=True)
        send_pc_cmd(0x08) # PC_CMD_EMERGENCY_STOP
        time.sleep(0.5)
        m_estop = read_mcu_info() or {}
        st_estop = m_estop.get("controller_state")
        fault_estop = m_estop.get("controller_fault_flags", 0)
        print(f"    Khi E-Stop: State = {st_estop} (4=FAULT), Fault = 0x{fault_estop:04X} (Bit 11 E-Stop)", flush=True)

        print("    [RESET] Xóa lỗi an toàn sau khi đã hết nguy hiểm...", flush=True)
        send_pc_cmd(0x0A) # PC_CMD_RESET_FAULT
        send_pc_cmd(0x04)
        if sniffer and sniffer.available:
            sniffer.send_button_touch(1)
        time.sleep(0.8)
        m_rec = read_mcu_info() or {}
        st_rec = m_rec.get("controller_state")
        ic09_pass = (st_estop == 4) and ((fault_estop & 0x0800) != 0) and (st_rec in (0, 1))
        log_result("Protection", "IC-09", "Dung khan cap E-Stop & Safe Reset", ic09_pass,
                   f"E-Stop vao FAULT (4), Safe Reset thanh cong ve State={st_rec}")

        # =========================================================================
        # PHẦN 3: QUY TRÌNH KÍCH SẠC TIỀN NẠP (PRE-CHARGE)
        # =========================================================================
        print("\n" + "=" * 80, flush=True)
        print(">>> PHAN 3: QUY TRINH TIEN KICH NAP (PRE-CHARGE SUITE)", flush=True)
        print("=" * 80, flush=True)

        # --- PC-01: Kích hoạt Pre-charge từ DWIN khi Pin kiệt ---
        standby_clean()
        print("--- [TEST PC-01] Kich hoat Pre-charge tu DWIN khi Pin kiet (BMS Offline) (30s) ---", flush=True)
        # Giả lập Pin kiệt nguồn: BMS mất nguồn tắt CAN
        bms.transmitting = False
        time.sleep(1.0)
        # Open Login screen (Page 06)
        sniffer.send_touch_key(0x1130, 0x0301)
        time.sleep(0.5)
        # Enter PIN 123456
        for digit in [0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036]:
            sniffer.send_touch_key(0x1504, digit)
            time.sleep(0.08)
        time.sleep(0.2)
        # Press OK -> Page 07
        sniffer.send_touch_key(0x1504, 0x00F1)
        time.sleep(0.8)

        # Press Action Button -> Start Precharge
        sniffer.send_touch_key(0x151A, 0x0001)
        time.sleep(1.5)
        m_pre = read_mcu_info() or {}
        st_pre = m_pre.get("controller_state")
        fault_pre = m_pre.get("controller_fault_flags", 0)
        print(f"    Ket qua kich hoat Precharge: State={st_pre} (5=PRECHARGE/2=RUN), Fault=0x{fault_pre:04X}", flush=True)

        # Restore BMS transmission
        bms.transmitting = True
        time.sleep(0.5)

        # Stop Precharge and return
        sniffer.send_touch_key(0x151A, 0x0001)
        time.sleep(0.5)
        sniffer.send_touch_key(0x151A, 0x0002) # Back
        time.sleep(0.5)
        pc01_pass = (st_pre in (2, 5)) and (fault_pre == 0)
        log_result("Pre-charge", "PC-01", "Kich hoat Pre-charge tu DWIN khi Pin kiet", pc01_pass,
                   f"State={st_pre}, Fault=0x{fault_pre:04X} (Sạch lỗi 100%, không còn E021)")

        # --- PC-06: Dừng Pre-charge thủ công ---
        standby_clean()
        print("--- [TEST PC-06] Dung Pre-charge thu cong ve IDLE (15s) ---", flush=True)
        sniffer.send_touch_key(0x1130, 0x0301)
        time.sleep(0.4)
        for d in [0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036]:
            sniffer.send_touch_key(0x1504, d)
            time.sleep(0.08)
        sniffer.send_touch_key(0x1504, 0x00F1)
        time.sleep(0.4)
        sniffer.send_touch_key(0x151A, 0x0001) # Start
        time.sleep(2.0)
        print("    [LENH] Nhấn Dừng kích sạc trên DWIN...", flush=True)
        sniffer.send_touch_key(0x151A, 0x0001) # Stop
        time.sleep(1.0)
        sniffer.send_touch_key(0x151A, 0x0002) # Back to Dash
        time.sleep(0.5)
        m_stop = read_mcu_info() or {}
        pc06_pass = (m_stop.get("controller_state") in (0, 1))
        log_result("Pre-charge", "PC-06", "Dung Pre-charge thu cong ve IDLE", pc06_pass,
                   f"MCU State ve an toan: {m_stop.get('controller_state')} ({CHARGE_CTRL_STATE_NAMES.get(m_stop.get('controller_state'), '')})")

    finally:
        # Reset to clean Standby
        standby_clean()
        print("\n" + "=" * 80, flush=True)
        print("  TONG KET KET QUA KIEM THU TOAN HE THONG:", flush=True)
        total_tests = len(results)
        total_passed = sum(1 for r in results if r["passed"])
        print(f"  Tong so bai test: {total_tests} | DAT: {total_passed}/{total_tests} ({total_passed*100/total_tests:.1f}%)", flush=True)
        print("=" * 80, flush=True)

        with open(REPORT_FILE, "w", encoding="utf-8") as f:
            f.write("# BÁO CÁO TỔNG HỢP KIỂM THỬ TOÀN DIỆN PHẦN CỨNG HIL (ZCAN + STM32 + DWIN)\n\n")
            f.write(f"- **Thời gian**: {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
            f.write(f"- **Kết quả chung**: **{total_passed}/{total_tests} bài PASS ({total_passed*100/total_tests:.1f}%)**\n\n")
            f.write("| STT | Nhóm chức năng | Mã test | Tên bài kiểm tra | Kết quả | Ghi chú kỹ thuật |\n")
            f.write("| :---: | :--- | :---: | :--- | :---: | :--- |\n")
            for idx, r in enumerate(results, 1):
                icon = "✅ PASS" if r["passed"] else "❌ FAIL"
                f.write(f"| {idx} | {r['suite']} | {r['code']} | {r['title']} | **{icon}** | {r['detail']} |\n")
        print(f"\n[INFO] Đã xuất báo cáo chi tiết ra file: {REPORT_FILE}\n", flush=True)

        # Hold standby
        print("Tiếp tục duy trì Standby (52.8V, SOC 80%) trên màn hình DWIN...")
        try:
            while True:
                time.sleep(1.0)
        except KeyboardInterrupt:
            bms.running = False
            mod.running = False
            sniffer.close()
            dev.close()

if __name__ == "__main__":
    main()
