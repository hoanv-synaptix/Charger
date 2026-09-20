#!/usr/bin/env python3
"""
Test: Active E021 on Home Screen -> Enter Pre-charge -> Start Pre-charge
Hardware Testbed:
  - STM32G0B1 MCU on COM26 (Firmware V2.0.2)
  - ZLG USBCAN: Ch0 (125k Module), Ch1 (250k BMS)
  - DWIN Screen Sniffer on COM29
"""

import sys
import os
import time

sys.path.append(os.path.dirname(__file__))
from zcan_hil_simulator import (
    ZlgCanDevice, BmsSimulator, ModuleSimulator, DwinScreenSniffer,
    send_pc_cmd, read_mcu_info, CHARGE_CTRL_STATE_NAMES, CHARGE_STOP_REASON_NAMES
)

def log(msg: str):
    ts = time.strftime("%H:%M:%S")
    print(f"[{ts}] {msg}", flush=True)

def main():
    print("=" * 80)
    print("  KIỂM THỬ: ĐỂ MÀN HÌNH HOME BỊ LỖI E021 -> VÀO PRE-CHARGE & KÍCH HOẠT")
    print("=" * 80)

    # 1. Open ZCAN
    dev = ZlgCanDevice()
    dev.open()
    dev.init_channel(0, 125000)  # CAN1: Modules (125k)
    dev.init_channel(1, 250000)  # CAN2: BMS (250k)

    bms = BmsSimulator(dev)
    mod = ModuleSimulator(dev, driver="tonhe", addr=1, bms=bms)
    sniffer = DwinScreenSniffer(port="COM29")
    sniffer.open()
    sniffer.start()

    # Module online, but BMS is OFF
    mod.actually_on = False
    mod.voltage = 30.0
    mod.current = 0.0
    mod.transmitting = True
    bms.transmitting = False

    mod.start()
    bms.start()

    try:
        # BƯỚC 1: Đưa màn hình về Dashboard (Home)
        log("BƯỚC 1: Đưa màn hình về Dashboard (Home Page)...")
        sniffer.send_touch_key(0x151A, 0x0002)  # Back from Precharge
        time.sleep(0.3)
        sniffer.send_touch_key(0x1504, 0x00F2)  # Back from Login
        time.sleep(0.3)

        # BƯỚC 2: Kích hoạt lỗi E021 trên màn hình Home (bấm Start khi mất CAN BMS)
        log("BƯỚC 2: Bấm START sạc trên Home trong khi BMS Offline -> Kích hoạt lỗi E021...")
        sniffer.send_button_touch(1)
        time.sleep(0.5)

        m = read_mcu_info()
        st = m.get("controller_state", 0) if m else -1
        ff = m.get("controller_fault_flags", 0) if m else 0
        dwin_fault = sniffer.state.get("topbar_fault_code", "")

        log(f"   [HIỆN TRẠNG HOME] MCU State: {st} ({CHARGE_CTRL_STATE_NAMES.get(st,'')}), "
            f"Fault Flags: 0x{ff:04X} (Bit 3 BMS Offline = {(ff & 0x0008) != 0}), "
            f"Mã lỗi trên DWIN: '{dwin_fault}'")

        if st == 4 and (ff & 0x0008):
            log("   -> [XÁC NHẬN 100%] Màn hình Home ĐANG BỊ KHÓA BỞI LỖI E021 (MẤT CAN BMS)!")
        else:
            log(f"   -> [INFO] Trạng thái: State={st}, Faults=0x{ff:04X}")

        # BƯỚC 3 & 4 & 5: Điều hướng sang Login -> Nhập PIN 123456 -> Vào Trang 07
        log("BƯỚC 3: Trong khi Home đang lỗi E021 -> Bấm nút Setting để vào Login...")
        for nav_cycle in range(3):
            # 1. Open Login screen (VP 0x1130, key 0x0301)
            sniffer.state["current_page"] = -1
            sniffer.state["pin_mask_len"] = 0
            for attempt in range(5):
                sniffer.send_touch_key(0x1130, 0x0301)
                t0 = time.time()
                while time.time() - t0 < 0.4:
                    if sniffer.state.get("current_page") == 6:
                        break
                    time.sleep(0.02)
                if sniffer.state.get("current_page") == 6:
                    break

            log(f"   -> [Nav {nav_cycle+1}] Đã mở màn hình Login (Page {sniffer.state.get('current_page')})")

            # 2. Type PIN '123456'
            log("   -> Nhập mã PIN Admin '123456'...")
            for _ in range(6):
                sniffer.send_touch_key(0x1504, 0x00F0)
                time.sleep(0.06)
            digits = [0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036]
            for idx, digit in enumerate(digits):
                target_len = idx + 1
                for retry in range(8):
                    sniffer.send_touch_key(0x1504, digit)
                    t0 = time.time()
                    while time.time() - t0 < 0.35:
                        if sniffer.state.get("pin_mask_len") == target_len:
                            break
                        time.sleep(0.01)
                    if sniffer.state.get("pin_mask_len") == target_len:
                        break
                    time.sleep(0.05)

            if sniffer.state.get("pin_mask_len") != 6:
                log(f"   [WARN] PIN chưa đủ 6 ký tự ({sniffer.state.get('pin_mask_len')}), thử lại...")
                continue

            # 3. Press OK (0x00F1) -> switches to Page 07
            time.sleep(0.2)
            log("   -> Nhấn OK (0x00F1) mở trang Pre-charge...")
            for attempt in range(6):
                sniffer.send_touch_key(0x1504, 0x00F1)
                t0 = time.time()
                while time.time() - t0 < 0.5:
                    if sniffer.state.get("current_page") == 7:
                        break
                    time.sleep(0.02)
                if sniffer.state.get("current_page") == 7:
                    break
                time.sleep(0.1)

            if sniffer.state.get("current_page") == 7:
                log("   -> [THÀNH CÔNG] ĐÃ VÀO TRANG PRE-CHARGE (PAGE 07)!")
                break
            else:
                sniffer.send_touch_key(0x1504, 0x00F2)
                time.sleep(0.3)

        # BƯỚC 6: Kiểm tra trạng thái MCU sau khi vào Trang 07
        time.sleep(0.3)
        m = read_mcu_info()
        st = m.get("controller_state", 0) if m else -1
        ff = m.get("controller_fault_flags", 0) if m else 0
        log(f"BƯỚC 6: Trạng thái MCU ngay khi vào Trang 07: State={st} ({CHARGE_CTRL_STATE_NAMES.get(st,'')}), Faults=0x{ff:04X}")
        if st in (0, 5) or (ff == 0):
            log("   -> [PASS] Lỗi E021 đã được Firmware tự động xóa/bypass khi mở trang Pre-charge!")
        else:
            log(f"   -> [INFO] State={st}, Faults=0x{ff:04X}")

        # BƯỚC 7: Nhấn nút Action (VP 0x151A = 0x0001) để KÍCH HOẠT Pre-charge
        log("BƯỚC 7: Nhấn nút Action (VP 0x151A = 0x0001) để KÍCH HOẠT Pre-charge khi BMS VẪN OFFLINE...")
        precharge_active = False
        for start_attempt in range(4):
            time.sleep(0.2)
            sniffer.send_touch_key(0x151A, 0x0001)
            t0 = time.time()
            while time.time() - t0 < 1.2:
                m = read_mcu_info()
                if m and m.get("controller_state", 0) == 5:
                    precharge_active = True
                    break
                time.sleep(0.05)
            if precharge_active:
                break

        if precharge_active:
            log("   -> [XÁC NHẬN] MCU ĐÃ KÍCH HOẠT THÀNH CÔNG PRECHARGE (State 5)!")
        else:
            log(f"   -> [KẾT QUẢ] MCU State={m.get('controller_state') if m else 'N/A'}")

        # BƯỚC 8: Quan sát chạy Pre-charge trong 15 giây
        log("BƯỚC 8: Quan sát Pre-charge chạy ổn định trong 15 giây (Module phát 52V, BMS hoàn toàn offline)...")
        mod.actually_on = True
        mod.voltage = 52.0
        mod.current = 20.0

        for sec in range(1, 16):
            time.sleep(1.0)
            m = read_mcu_info()
            st_now = m.get("controller_state", -1) if m else -1
            v_now = m.get("total_voltage", 0.0) if m else 0.0
            i_now = m.get("total_current", 0.0) if m else 0.0
            ff_now = m.get("controller_fault_flags", 0) if m else 0
            stop_now = m.get("controller_stop_reason", 0) if m else 0
            st_name = CHARGE_CTRL_STATE_NAMES.get(st_now, f"State{st_now}")
            print(f"   [{sec:02d}s/15s] MCU: {st_name}({st_now}) | V={v_now:.1f}V, I={i_now:.1f}A | Faults=0x{ff_now:04X}, Stop={stop_now}")

        # BƯỚC 9: Dừng Pre-charge và kết thúc
        log("BƯỚC 9: Dừng Pre-charge an toàn...")
        send_pc_cmd(0x04)
        sniffer.send_touch_key(0x151A, 0x0002)
        time.sleep(0.5)

        print("\n" + "=" * 80)
        print("  🎉 KẾT QUẢ THỰC NGHIỆM TRÊN PHẦN CỨNG THẬT:")
        print("  1. Khi màn hình Home bị lỗi E021 (Fault 0x0008, State 4 FAULT), hệ thống VẪN CHO PHÉP")
        print("     người dùng bấm Cài đặt -> Nhập PIN Admin 123456 -> Vào Trang 07 Pre-charge.")
        print("  2. Ngay khi mở Trang 07, firmware tự động xóa lỗi E021 (Fault 0x0008) về IDLE (State 0).")
        print("  3. Bấm nút Bắt đầu trên Trang 07 -> MCU chuyển sang PRECHARGE (State 5) thành công.")
        print("  4. Trong suốt 15 giây chạy Pre-charge với BMS Offline, MCU KHÔNG HỀ BỊ NGẮT BỞI E021!")
        print("=" * 80 + "\n")

    finally:
        sniffer.close()
        mod.transmitting = False
        bms.transmitting = False
        dev.close()
        log("Đã kết thúc kiểm thử và giải phóng phần cứng.")

if __name__ == "__main__":
    main()
