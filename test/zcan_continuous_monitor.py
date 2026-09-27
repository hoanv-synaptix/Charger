#!/usr/bin/env python3
"""
ZCAN & MCU Continuous Real-Time Monitor
Monitors CAN1 (125k) and CAN2 (250k) traffic continuously via ZLG USBCAN,
decoding all communication between MCU, Lianming/TonHe/Maxwell modules, and BMS.
Optionally monitors MCU internal state machine via USB CDC.
"""
import sys
import os
import time
import struct
import argparse
import threading
import serial.tools.list_ports

sys.path.insert(0, os.path.dirname(__file__))
from zcan_hil_simulator import ZlgCanDevice, read_mcu_info, CHARGE_CTRL_STATE_NAMES, CHARGE_STOP_REASON_NAMES

def find_mcu_port():
    ports = serial.tools.list_ports.comports()
    for p in ports:
        if "USB Serial Device" in p.description or "STMicroelectronics" in p.description:
            return p.device
    for p in ports:
        if p.device in ("COM12", "COM26"):
            return p.device
    return None

def decode_lianming_frame(can_id: int, data: bytes):
    addr = can_id & 0x7F
    id_base = can_id & ~0x7F

    # MCU -> Module
    if id_base == 0x1907C080:
        if len(data) >= 8 and data[0] == 0x00:
            i_ma = (data[1] << 16) | (data[2] << 8) | data[3]
            v_mv = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7]
            return f"[MCU -> LM#{addr}] SET V/I: V_set={v_mv/1000.0:.2f}V, I_lim={i_ma/1000.0:.2f}A"
        elif len(data) >= 1 and data[0] == 0x01:
            return f"[MCU -> LM#{addr}] POLL STATUS (CMD=1)"
        elif len(data) >= 8 and data[0] == 0x02:
            cmd_val = data[7]
            action = "🟢 START (0x55)" if cmd_val == 0x55 else ("🔴 STOP (0xAA)" if cmd_val == 0xAA else f"0x{cmd_val:02X}")
            return f"[MCU -> LM#{addr}] CMD START/STOP: {action}"
        return f"[MCU -> LM#{addr}] CMD=0x{data[0]:02X} Data={data.hex(' ').upper()}"

    elif id_base == 0x19008080:
        return f"[MCU -> LM#{addr}] READ AMBIENT TEMP (0x19008080)"

    elif id_base == 0x19018080:
        return f"[MCU -> LM#{addr}] READ AC VOLTAGE (0x19018080)"

    # Module -> MCU
    elif id_base == 0x1807C080:
        if len(data) >= 2 and data[0] == 0x00:
            res_str = "THÀNH CÔNG (0xFF)" if data[1] == 0xFF else f"Code=0x{data[1]:02X}"
            return f"[LM#{addr} -> MCU] ACK SET V/I: {res_str}"
        elif len(data) >= 8 and data[0] == 0x01:
            i_out = ((data[2] << 8) | data[3]) / 10.0
            v_out = ((data[4] << 8) | data[5]) / 10.0
            flags = (data[6] << 8) | data[7]
            is_on = ((data[7] & 0x01) == 0)
            state_str = "🟢 ON" if is_on else "🔴 OFF"
            return f"[LM#{addr} -> MCU] STATUS: {state_str} | V={v_out:5.1f}V | I={i_out:4.1f}A | Flags=0x{flags:04X}"
        elif len(data) >= 2 and data[0] == 0x02:
            res_str = "THÀNH CÔNG (0xFF)" if data[1] == 0xFF else f"Code=0x{data[1]:02X}"
            return f"[LM#{addr} -> MCU] ACK START/STOP: {res_str}"
        return f"[LM#{addr} -> MCU] RESP CMD=0x{data[0]:02X} Data={data.hex(' ').upper()}"

    elif id_base == 0x18008080:
        if len(data) >= 6:
            temp = ((data[4] << 8) | data[5]) / 10.0
            return f"[LM#{addr} -> MCU] AMBIENT TEMP: {temp:.1f}°C"
        return f"[LM#{addr} -> MCU] AMBIENT TEMP Data={data.hex(' ').upper()}"

    elif id_base == 0x18018080:
        if len(data) >= 8:
            vab = ((data[2] << 8) | data[3]) * 32 / 10.0
            vbc = ((data[4] << 8) | data[5]) * 32 / 10.0
            vca = ((data[6] << 8) | data[7]) * 32 / 10.0
            return f"[LM#{addr} -> MCU] AC VOLTAGE: Vab={vab:.0f}V Vbc={vbc:.0f}V Vca={vca:.0f}V"
        return f"[LM#{addr} -> MCU] AC VOLTAGE Data={data.hex(' ').upper()}"

    elif id_base == 0x18078080:
        return f"[LM#{addr} BROADCAST] Current Sharing Heartbeat: {data.hex(' ').upper()}"

    return None

def decode_bms_frame(can_id: int, data: bytes):
    if can_id == 0x02F4 and len(data) >= 5:
        v = (data[0] | (data[1] << 8)) / 10.0
        i = ((data[2] | (data[3] << 8)) / 10.0) - 400.0
        soc = data[4]
        return f"[BMS -> MCU 0x02F4] Batt ST1: V_pack={v:.1f}V, I_pack={i:.1f}A, SOC={soc}%"
    elif can_id == 0x03F4 and len(data) >= 5:
        v_req = (data[0] | (data[1] << 8)) / 10.0
        i_req = (data[2] | (data[3] << 8)) / 10.0
        relay = bool(data[4] & 0x01)
        return f"[BMS -> MCU 0x03F4] Batt ST2: V_req={v_req:.1f}V, I_req={i_req:.1f}A, RelayAllow={'YES' if relay else 'NO'}"
    elif can_id == 0x01F4 and len(data) >= 6:
        v_chg = (data[0] | (data[1] << 8)) / 10.0
        i_chg = (data[2] | (data[3] << 8)) / 10.0
        st = data[4]
        return f"[MCU -> BMS 0x01F4] Chg Status: V={v_chg:.1f}V, I={i_chg:.1f}A, State={st}"
    return None

def main():
    parser = argparse.ArgumentParser(description="ZCAN & MCU Real-Time Sniffer")
    parser.add_argument("--can1-only", action="store_true", help="Only monitor CAN1 (Modules)")
    parser.add_argument("--all-frames", action="store_true", help="Print all raw frames without throttling poll requests")
    parser.add_argument("--mcu-poll", action="store_true", default=True, help="Poll MCU state via USB CDC periodically")
    parser.add_argument("--port", type=str, default="", help="MCU Serial Port (default: auto-detect)")
    args = parser.parse_args()

    mcu_port = args.port or find_mcu_port() or "COM12"
    print(f"[*] Khởi tạo ZLG USBCAN Kênh 0 (CAN1 125k) {'và Kênh 1 (CAN2 250k)' if not args.can1_only else ''}...")
    print(f"[*] Cổng MCU USB CDC: {mcu_port}")

    dev = ZlgCanDevice()
    dev.open()
    dev.init_channel(0, 125000)
    if not args.can1_only:
        try:
            dev.init_channel(1, 250000)
        except Exception as e:
            print(f"[!] Kênh 1 không khởi tạo được ({e}), tiếp tục chỉ dùng Kênh 0.")

    print("\n" + "=" * 90)
    print(">>> BẮT ĐẦU CHẾ ĐỘ GIÁM SÁT THỜI GIAN THỰC (Nhấn Ctrl+C để dừng) <<<")
    print("=" * 90 + "\n")

    last_lm_status = {}
    last_lm_status_time = {}
    last_mcu_check_time = 0

    try:
        while True:
            now = time.time()
            ts_str = time.strftime("%H:%M:%S") + f".{int(now * 1000) % 1000:03d}"

            # 1. Đọc frames từ CAN1 (Channel 0)
            frames_c0 = dev.receive(0, max_count=32, wait_ms=20)
            for can_id, ext, data in frames_c0:
                dec = decode_lianming_frame(can_id, data)
                if dec:
                    # Lọc bản tin đọc trạng thái lặp lại để không spam màn hình (chỉ in mỗi 1s hoặc khi có thay đổi)
                    is_status_resp = ("STATUS:" in dec)
                    is_poll_req = ("POLL STATUS" in dec)

                    if is_poll_req and not args.all_frames:
                        continue  # Bỏ qua dòng query CMD=1 vì in dòng phản hồi là đủ

                    if is_status_resp and not args.all_frames:
                        last_s = last_lm_status.get(can_id)
                        last_t = last_lm_status_time.get(can_id, 0)
                        if last_s == dec and (now - last_t) < 1.0:
                            continue
                        last_lm_status[can_id] = dec
                        last_lm_status_time[can_id] = now

                    # Bỏ qua bản tin chia dòng nội bộ nếu không bật --all-frames
                    if "Current Sharing Heartbeat" in dec and not args.all_frames:
                        continue

                    print(f"[{ts_str} CAN1] {dec}")
                elif args.all_frames:
                    print(f"[{ts_str} CAN1] ID=0x{can_id:08X} DATA={data.hex(' ').upper()}")

            # 2. Đọc frames từ CAN2 (Channel 1) nếu có
            if not args.can1_only:
                frames_c1 = dev.receive(1, max_count=32, wait_ms=0)
                for can_id, ext, data in frames_c1:
                    dec_bms = decode_bms_frame(can_id, data)
                    if dec_bms:
                        print(f"[{ts_str} CAN2] {dec_bms}")
                    elif args.all_frames:
                        print(f"[{ts_str} CAN2] ID=0x{can_id:08X} DATA={data.hex(' ').upper()}")

            # 3. Đọc telemetry từ MCU mỗi 2 giây
            if args.mcu_poll and (now - last_mcu_check_time) >= 2.0:
                last_mcu_check_time = now
                try:
                    info = read_mcu_info(port=mcu_port, timeout=0.1)
                    if info:
                        st_name = CHARGE_CTRL_STATE_NAMES.get(info.get("controller_state"), str(info.get("controller_state")))
                        stop_reason = CHARGE_STOP_REASON_NAMES.get(info.get("controller_stop_reason"), str(info.get("controller_stop_reason")))
                        print(f"[{ts_str} MCU ] FSM: {st_name.upper():<10} | V_out: {info['total_voltage']:5.1f}V | I_out: {info['total_current']:4.1f}A | Mod: {info['modules_online']}/{info['modules_total']} (Fault:{info['modules_fault']}) | Driver={info['driver_id']} | Stop={stop_reason}")
                except Exception:
                    pass

            time.sleep(0.02)

    except KeyboardInterrupt:
        print("\n[!] Dừng giám sát theo yêu cầu người dùng.")
    finally:
        dev.close()
        print("[*] Đã đóng ZLG USBCAN an toàn.")

if __name__ == "__main__":
    main()
