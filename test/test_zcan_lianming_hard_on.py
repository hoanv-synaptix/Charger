#!/usr/bin/env python3
"""
Test bật/tắt cứng Module Lianming trực tiếp từ ZCAN (Channel 0, 125 Kbps).
"""
import sys
import os
import time
import struct
import argparse

sys.path.insert(0, os.path.dirname(__file__))
from zcan_hil_simulator import ZlgCanDevice, send_pc_cmd

def main():
    parser = argparse.ArgumentParser(description="ZCAN Hard Control for Lianming Module")
    parser.add_argument("--addr", type=int, default=1, help="Module CAN Address (default: 1)")
    parser.add_argument("--volt", type=float, default=53.0, help="Target Voltage in Volts (default: 53.0)")
    parser.add_argument("--curr", type=float, default=3.0, help="Target Current in Amperes (default: 3.0)")
    parser.add_argument("--duration", type=int, default=10, help="Run duration in seconds before turning off (default: 10)")
    parser.add_argument("--hold-on", action="store_true", help="Keep module ON without turning off automatically")
    parser.add_argument("--periodic-cmd0", action="store_true", help="Send CMD=0 periodically in the loop")
    parser.add_argument("--off-only", action="store_true", help="Only turn OFF module")
    parser.add_argument("--status-only", action="store_true", help="Only read status")
    args = parser.parse_args()

    addr = args.addr
    can_id_tx = 0x1907C080 | (addr & 0x7F)
    can_id_rx = 0x1807C080 | (addr & 0x7F)

    # 1. Tạm thời chuyển driver của MCU sang TonHe để MCU không can thiệp CAN1
    print("[1/5] Tạm thời cô lập CAN1 của MCU (chuyển MCU sang TonHe để tránh can thiệp)...")
    send_pc_cmd(0x09, bytes([3]))  # 3 = TonHe
    time.sleep(0.3)

    # 2. Mở ZLG USBCAN Channel 0 (125k)
    print(f"[2/5] Mở ZLG USBCAN Kênh 0 (125 Kbps)...")
    dev = ZlgCanDevice()
    dev.open()
    dev.init_channel(0, 125000)
    time.sleep(0.2)

    try:
        if args.off_only:
            print(f"\n>>> [LỆNH] GỬI LỆNH TẮT (POWER OFF) TỚI MODULE {addr}...")
            stop_frame = bytes([0x02, 0, 0, 0, 0, 0, 0, 0xAA])
            dev.transmit(0, can_id_tx, stop_frame, extended=True)
            time.sleep(0.1)
            # Read status
            dev.transmit(0, can_id_tx, bytes([0x01, 0, 0, 0, 0, 0, 0, 0]), extended=True)
            time.sleep(0.1)
            frames = dev.receive(0, max_count=16, wait_ms=100)
            for r_id, ext, data in frames:
                if r_id == can_id_rx and len(data) >= 8 and data[0] == 0x01:
                    v = (data[4] << 8 | data[5]) / 10.0
                    i = (data[2] << 8 | data[3]) / 10.0
                    is_on = ((data[7] & 0x01) == 0)
                    print(f"  -> Trạng thái sau khi tắt: {'ĐANG CHẠY (ON)' if is_on else 'ĐÃ TẮT (OFF)'} | V={v:.1f}V, I={i:.1f}A")
            return

        if args.status_only:
            print(f"\n>>> [ĐỌC] ĐỌC THÔNG SỐ HIỆN TẠI TỪ MODULE {addr}...")
            dev.transmit(0, can_id_tx, bytes([0x01, 0, 0, 0, 0, 0, 0, 0]), extended=True)
            time.sleep(0.1)
            frames = dev.receive(0, max_count=16, wait_ms=100)
            for r_id, ext, data in frames:
                if r_id == can_id_rx and len(data) >= 8 and data[0] == 0x01:
                    v = (data[4] << 8 | data[5]) / 10.0
                    i = (data[2] << 8 | data[3]) / 10.0
                    is_on = ((data[7] & 0x01) == 0)
                    status_raw = (data[6] << 8) | data[7]
                    print(f"  -> Module {addr}: {'ĐANG CHẠY (ON)' if is_on else 'ĐÃ TẮT (OFF)'} | V={v:.1f}V | I={i:.1f}A | Flags=0x{status_raw:04X}")
            return

        # 3. Gửi cài đặt Điện áp và Dòng điện (CMD=0)
        v_mv = int(args.volt * 1000.0)
        i_ma = int(args.curr * 1000.0)
        set_data = bytes([
            0x00,
            (i_ma >> 16) & 0xFF, (i_ma >> 8) & 0xFF, i_ma & 0xFF,
            (v_mv >> 24) & 0xFF, (v_mv >> 16) & 0xFF, (v_mv >> 8) & 0xFF, v_mv & 0xFF,
        ])
        print(f"\n[3/5] Gửi cài đặt đầu ra (CMD=0): V={args.volt:.1f}V ({v_mv} mV), I={args.curr:.1f}A ({i_ma} mA)...")
        print(f"      CAN ID TX: 0x{can_id_tx:08X}, Data: {set_data.hex(' ').upper()}")
        dev.transmit(0, can_id_tx, set_data, extended=True)
        time.sleep(0.1)

        # Đọc phản hồi CMD=0
        frames = dev.receive(0, max_count=16, wait_ms=100)
        for r_id, ext, data in frames:
            if r_id == can_id_rx and len(data) >= 2 and data[0] == 0x00:
                result = "Thành công cả Áp và Dòng (0xFF)" if data[1] == 0xFF else f"Code=0x{data[1]:02X}"
                print(f"  -> Phản hồi từ Module: ID=0x{r_id:08X} DATA={data.hex(' ').upper()} [{result}]")

        # 4. Gửi lệnh BẬT MODULE (CMD=2, 0x55)
        print(f"\n[4/5] GỬI LỆNH BẬT NGUỒN CỨNG (CMD=2, Value=0x55) TỚI MODULE {addr}...")
        start_data = bytes([0x02, 0, 0, 0, 0, 0, 0, 0x55])
        print(f"      CAN ID TX: 0x{can_id_tx:08X}, Data: {start_data.hex(' ').upper()}")
        dev.transmit(0, can_id_tx, start_data, extended=True)
        time.sleep(0.2)

        # Đọc phản hồi CMD=2
        frames = dev.receive(0, max_count=16, wait_ms=100)
        for r_id, ext, data in frames:
            if r_id == can_id_rx and len(data) >= 8 and data[0] == 0x02:
                echo_val = data[7]
                res_str = "XÁC NHẬN BẬT THÀNH CÔNG (0x55)" if echo_val == 0x55 else f"Echo=0x{echo_val:02X}"
                print(f"  -> Phản hồi từ Module: ID=0x{r_id:08X} DATA={data.hex(' ').upper()} [{res_str}]")

        # 5. Đọc trạng thái thời gian thực
        print(f"\n[5/5] THEO DÕI ĐẦU RA THỜI GIAN THỰC (Thời gian: {args.duration}s)...")
        print("=" * 80)
        t_start = time.time()
        while time.time() - t_start < args.duration:
            elapsed = time.time() - t_start
            if args.periodic_cmd0:
                dev.transmit(0, can_id_tx, set_data, extended=True)
                time.sleep(0.05)
            # Gửi lệnh đọc trạng thái CMD=1
            dev.transmit(0, can_id_tx, bytes([0x01, 0, 0, 0, 0, 0, 0, 0]), extended=True)
            time.sleep(0.15)
            frames = dev.receive(0, max_count=16, wait_ms=100)
            for r_id, ext, data in frames:
                if r_id == can_id_rx and len(data) >= 8 and data[0] == 0x01:
                    v_out = (data[4] << 8 | data[5]) / 10.0
                    i_out = (data[2] << 8 | data[3]) / 10.0
                    is_on = ((data[7] & 0x01) == 0)
                    status_raw = (data[6] << 8) | data[7]
                    state_text = "🟢 ĐANG CHẠY (ON)" if is_on else "🔴 ĐANG TẮT (OFF)"
                    print(f"  [{elapsed:04.1f}s] Module {addr}: {state_text} | Điện áp: {v_out:5.1f} V | Dòng điện: {i_out:4.1f} A | Flags=0x{status_raw:04X} | Raw: {data.hex(' ').upper()}")
            time.sleep(0.3)

        if not args.hold_on:
            print("\n" + "=" * 80)
            print(f">>> [KẾT THÚC TEST] Gửi lệnh TẮT AN TOÀN (Power OFF, 0xAA) tới module {addr}...")
            stop_data = bytes([0x02, 0, 0, 0, 0, 0, 0, 0xAA])
            dev.transmit(0, can_id_tx, stop_data, extended=True)
            time.sleep(0.2)
            dev.transmit(0, can_id_tx, bytes([0x01, 0, 0, 0, 0, 0, 0, 0]), extended=True)
            time.sleep(0.2)
            frames = dev.receive(0, max_count=16, wait_ms=100)
            for r_id, ext, data in frames:
                if r_id == can_id_rx and len(data) >= 8 and data[0] == 0x01:
                    v_out = (data[4] << 8 | data[5]) / 10.0
                    i_out = (data[2] << 8 | data[3]) / 10.0
                    is_on = ((data[7] & 0x01) == 0)
                    print(f"  -> Trạng thái cuối: {'🟢 ON' if is_on else '🔴 OFF'} | V={v_out:.1f}V | I={i_out:.1f}A")
        else:
            print("\n[INFO] Chế độ --hold-on: Module tiếp tục duy trì trạng thái BẬT.")

    finally:
        dev.close()
        # Khôi phục MCU về Lianming driver
        print("\n[HOÀN TẤT] Khôi phục MCU về driver Lianming...")
        send_pc_cmd(0x09, bytes([2]))  # 2 = Lianming
        time.sleep(0.2)
        print("[SUCCESS] Hoàn thành kiểm tra bật cứng ZCAN.")

if __name__ == "__main__":
    main()
