"""
Tool đồng bộ RTC cho mạch điều khiển sạc qua cổng USB Serial.
Sử dụng: py sync_rtc.py [COM_PORT]
"""
import sys
import time
import struct
import datetime
import serial
import serial.tools.list_ports

try:
    if sys.stdout and hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
except Exception:
    pass

def crc8(data: bytes) -> int:
    crc = 0x00
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x07) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc

def find_stm32_port():
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        desc = (p.description or "").lower()
        hwid = (p.hwid or "").lower()
        if "stm" in desc or "stlink" in desc or "vcp" in desc or "0483" in hwid:
            return p.device
    if ports:
        return ports[0].device
    return None

def main():
    port = sys.argv[1] if len(sys.argv) > 1 else find_stm32_port()
    if not port:
        print("[LỖI] Không tìm thấy cổng COM nào. Vui lòng cắm cáp USB vào máy tính.")
        return 1

    print(f"[*] Đang kết nối tới cổng {port}...")
    try:
        ser = serial.Serial(port, baudrate=115200, timeout=1.0)
    except Exception as e:
        print(f"[LỖI] Không thể mở cổng {port}: {e}")
        return 1

    now = datetime.datetime.now()
    epoch_utc = int(time.time())
    print(f"[*] Giờ máy tính hiện tại (Local): {now.strftime('%Y-%m-%d %H:%M:%S')}")
    print(f"[*] Timestamp Unix Epoch (UTC): {epoch_utc}")

    # Lệnh DEBUG_CMD_SET_RTC = 0x1D, độ dài 4 byte (epoch_utc LE)
    payload = struct.pack("<I", epoch_utc)
    cmd = 0x1D
    header = bytes([0xAA, 0x55, cmd, len(payload)])
    crc_payload = bytes([cmd, len(payload)]) + payload
    crc_val = crc8(crc_payload)
    frame = header + payload + bytes([crc_val])

    print(f"[*] Gửi lệnh SET_RTC ({len(frame)} bytes): {frame.hex().upper()}")
    ser.reset_input_buffer()
    ser.write(frame)
    ser.flush()

    time.sleep(0.1)
    resp = ser.read(64)
    if not resp:
        print("[CẢNH BÁO] Không nhận được phản hồi từ MCU. Thử đọc lại...")
        time.sleep(0.5)
        resp = ser.read(64)

    if resp:
        print(f"[*] Phản hồi từ MCU: {resp.hex().upper()}")
        # Check response: 0xAA 0x55 0x9D 0x0D ...
        if len(resp) >= 18 and resp[0] == 0xAA and resp[1] == 0x55 and resp[2] == 0x9D:
            data = resp[4:4+13]
            epoch, year, month, day, hour, minute, second, weekday, is_valid = struct.unpack("<IHBBBBBBB", data)
            print("[THÀNH CÔNG] Đã đồng bộ RTC thành công!")
            print(f"    - Thời gian lưu trên RTC: {year:04d}-{month:02d}-{day:02d} {hour:02d}:{minute:02d}:{second:02d}")
            print(f"    - Thứ: {weekday} (1=T2 .. 7=CN)")
            print(f"    - Trạng thái valid: {'HỢP LỆ' if is_valid else 'CHƯA HỢP LỆ'}")
        else:
            print("[THÔNG BÁO] Đã gửi lệnh, MCU nhận được phản hồi.")
    else:
        print("[CẢNH BÁO] Không nhận được phản hồi từ MCU (kiểm tra baudrate hoặc cổng COM).")

    ser.close()
    return 0

if __name__ == "__main__":
    sys.exit(main())
