"""
Charger Hardware Automation Test
- RS485 loopback : PC -> adapter -> MCU (echo) -> adapter -> PC  (can echo fw)
- FDCAN loopback : can thi day jumper noi CAN1 <-> CAN2 (H-H, L-L)
Luu y: firmware production KHONG con spam test frame -> bai test CAN
do counter qua GET_SYSTEM (0x18) cua debug protocol.
"""
import serial
import serial.tools.list_ports
import time
import struct
import sys
import argparse


# ============================================================
# PC Protocol helpers  [AA 55][CMD][LEN][PAYLOAD...][CRC8]
# ============================================================

def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x07) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc


def build_frame(cmd: int, payload: bytes = b'') -> bytes:
    body = bytes([cmd, len(payload)]) + payload
    return bytes([0xAA, 0x55]) + body + bytes([crc8(body)])


DEBUG_CMD_ENTER       = 0x10
DEBUG_CMD_GET_SYSTEM  = 0x18
DEBUG_RSP_SYSTEM_INFO = 0x94


def read_frames(ser: serial.Serial, timeout_s: float):
    """Doc buffer va tra ve list cac frame hop le (scan theo SOF, kiem tra CRC)."""
    end = time.time() + timeout_s
    buf = bytearray()
    frames = []
    while time.time() < end:
        chunk = ser.read(256)
        if chunk:
            buf.extend(chunk)
        i = 0
        while i + 4 <= len(buf):
            if buf[i] == 0xAA and buf[i + 1] == 0x55:
                ln = buf[i + 3]
                total = 5 + ln
                if i + total > len(buf):
                    break
                body = bytes(buf[i + 2:i + 2 + 2 + ln])
                if crc8(body) == buf[i + total - 1]:
                    frames.append((buf[i + 2], bytes(buf[i + 4:i + 4 + ln])))
                    del buf[:i + total]
                    i = 0
                    continue
                else:
                    i += 1
            else:
                i += 1
    return frames


def request_system_info(ser: serial.Serial, timeout_s=1.0):
    """Gui GET_SYSTEM, tra ve (c1tx, c1rx, c2tx, c2rx) hoac None."""
    ser.reset_input_buffer()
    ser.write(build_frame(DEBUG_CMD_GET_SYSTEM))
    for cmd, payload in read_frames(ser, timeout_s):
        if cmd == DEBUG_RSP_SYSTEM_INFO and len(payload) >= 62:
            c1tx, c1rx, c2tx, c2rx = struct.unpack('<IIII', payload[46:62])
            return c1tx, c1rx, c2tx, c2rx
    return None


def open_serial(port: str, baud=115200, timeout=1.0, retries=3, delay_s=1.0):
    """Mo COM port co thu lai."""
    last_err = None
    for i in range(1, retries + 1):
        try:
            return serial.Serial(port, baud, timeout=timeout)
        except Exception as e:
            last_err = e
            msg = str(e)
            print(f"   [!] Mo {port} lan {i} loi: {msg.splitlines()[0][:80]}")
            if 'denied' in msg.lower():
                print("       -> Port dang bi chuong trinh KHAC giu. Dong het roi thu lai.")
            elif 'not functioning' in msg.lower():
                print("       -> Thiet bi USB loi. Rut/cam lai hoac kiem tra Device Manager.")
            if i < retries:
                time.sleep(delay_s)
    raise last_err


# ============================================================
# Port detection
# ============================================================

def find_ports():
    print("\n[SCAN] Dang quet COM ports...")
    ports = serial.tools.list_ports.comports()
    usb_cdc_port = None
    rs485_port = None
    for p in ports:
        if p.vid == 0x0483:
            usb_cdc_port = p.device
            print(f"   [+] Found STM32 USB CDC: {p.device}")
        elif p.vid in [0x1a86, 0x0403, 0x10c4]:
            rs485_port = p.device
            print(f"   [+] Found USB-Serial (RS485): {p.device}")
    return rs485_port, usb_cdc_port


# ============================================================
# Test: RS485 Loopback
# ============================================================

def test_rs485(com_port):
    print(f"\n[RUN] RS485 Loopback test on {com_port}...")
    try:
        ser = open_serial(com_port, timeout=1.0)
    except Exception as e:
        print(f"[FAIL] RS485 Error: mo COM that bai: {e}")
        return False

    test_str = b"HELLO_RS485_TEST"
    passed = False
    last_rx = b''

    try:
        for attempt in range(1, 4):
            ser.reset_input_buffer()
            ser.write(test_str)

            end = time.time() + 1.5
            rx = bytearray()
            while time.time() < end:
                chunk = ser.read(64)
                if not chunk:
                    continue
                rx.extend(chunk)
                if test_str in bytes(rx):
                    break

            last_rx = bytes(rx)
            if test_str in last_rx:
                print(f"   [PASS] Lan {attempt}: echo dung '{test_str.decode()}'")
                passed = True
                break
            else:
                print(f"   [....] Lan {attempt}: khong thay echo. Nhan {len(last_rx)} bytes")

        if not passed:
            if len(last_rx) == 0:
                print("[FAIL] RS485: KHONG nhan byte nao. Kiem tra:")
                print("       - Day J801 (A->A, B->B, GND->GND), thu DAO A/B")
                print("       - Firmware co echo RS485 khong (ban debug)?")
            else:
                print(f"[FAIL] RS485: sai noi dung: {last_rx[:40]!r}")
    finally:
        ser.close()
    return passed


# ============================================================
# Test: FDCAN Loopback (CAN1 <-> CAN2 qua day jumper)
# ============================================================

def test_can_loopback(com_port):
    print(f"\n[RUN] FDCAN Loopback test via {com_port}...")
    print("   [!] Yeu cau: da noi jumper CAN1<->CAN2 (H-H, L-L) TRUOC khi reset board")
    try:
        ser = open_serial(com_port, timeout=1.0)
    except Exception as e:
        print(f"[FAIL] USB CDC error: {e}")
        return False

    try:
        ser.reset_input_buffer()
        ser.write(build_frame(DEBUG_CMD_ENTER))
        time.sleep(0.1)
        ser.reset_input_buffer()

        s1 = request_system_info(ser)
        if not s1:
            print("[FAIL] Khong doc duoc System Info (lan 1).")
            return False
        print(f"   [+] Ban dau    -> C1_TX:{s1[0]}  C1_RX:{s1[1]}  C2_TX:{s1[2]}  C2_RX:{s1[3]}")

        wait_s = 4.0
        print(f"   [WAIT] Cho {wait_s:.0f}s (Ctrl_INFO BMS 500ms tren CAN2)...")
        time.sleep(wait_s)

        s2 = request_system_info(ser)
        if not s2:
            print("[FAIL] Khong doc duoc System Info (lan 2).")
            return False
        print(f"   [+] Sau {wait_s:.0f}s -> C1_TX:{s2[0]}  C1_RX:{s2[1]}  C2_TX:{s2[2]}  C2_RX:{s2[3]}")

        d_c1rx = s2[1] - s1[1]
        d_c2tx = s2[2] - s1[2]

        print(f"   => CAN2 gui {d_c2tx} frame | CAN1 nhan {d_c1rx} frame")

        if d_c2tx == 0:
            print("[FAIL] CAN2 khong gui duoc Ctrl_INFO nao -> FW/CAN2 loi")
            return False
        print("   [+] CAN2 hoat dong (dang phat Ctrl_INFO 500ms)")

        if d_c1rx >= d_c2tx * 0.75:
            print("[PASS] CAN1 nhan du frame tu CAN2 (bus vat ly OK)")
            return True

        print("[FAIL] CAN1 KHONG nhan duoc frame cua CAN2 ->")
        print("       1. Jumper CAN1<->CAN2 da noi chua? (H-H, L-L)")
        print("       2. Da RESET board SAU khi noi jumper chua?")
        print("          (neu de board chay mot minh lau ngay -> bus-off)")
        return False
    finally:
        ser.close()


# ============================================================
# Main
# ============================================================

def main():
    parser = argparse.ArgumentParser(description='Charger Hardware Automation Test')
    parser.add_argument('--rs485', type=str, help='RS485 COM port (e.g. COM7)')
    parser.add_argument('--usb', type=str, help='USB CDC COM port (e.g. COM16)')
    parser.add_argument('--skip-rs485', action='store_true')
    parser.add_argument('--skip-can', action='store_true')
    args = parser.parse_args()

    rs485_port = args.rs485
    usb_port = args.usb

    if not rs485_port and not usb_port:
        r, u = find_ports()
        rs485_port = rs485_port or r
        usb_port = usb_port or u

    rs_res = None
    can_res = None

    if rs485_port and not args.skip_rs485:
        rs_res = test_rs485(rs485_port)
    elif not args.skip_rs485:
        print("\n[SKIP] Khong tim thay RS485 adapter")

    if usb_port and not args.skip_can:
        can_res = test_can_loopback(usb_port)
    elif not args.skip_can:
        print("\n[SKIP] Khong tim thay STM32 USB CDC")

    print("\n================ SUMMARY ================")
    if rs_res is None:
        print("RS485 Loopback Test: >> SKIPPED <<")
    else:
        print(f"RS485 Loopback Test: {'PASS' if rs_res else 'FAIL'}")
    if can_res is None:
        print("FDCAN Loopback Test: >> SKIPPED <<")
    else:
        print(f"FDCAN Loopback Test: {'PASS' if can_res else 'FAIL'}")
    print("=========================================")

    failed = (rs_res is False) or (can_res is False)
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
