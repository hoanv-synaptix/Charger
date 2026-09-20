import serial
import time

def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc

def build_frame(cmd: int, payload: bytes = b'') -> bytes:
    body = bytes([cmd, len(payload)]) + payload
    return bytes([0xAA, 0x55]) + body + bytes([crc8(body)])

try:
    s = serial.Serial('COM26', 115200, timeout=1)
    frame = build_frame(0x24) # PC_CMD_GET_4G_STATUS (0x24)
    s.write(frame)
    time.sleep(0.1)
    rsp = s.read_all()
    print('Raw response len:', len(rsp), 'Hex:', rsp.hex())
    if len(rsp) >= 54 and rsp[2] == 0x89:
        state = rsp[4]
        powered = rsp[5]
        sim_ready = rsp[6]
        net_reg = rsp[7]
        pdp = rsp[8]
        csq = rsp[9]
        ip = rsp[10:30].decode('ascii', errors='ignore').rstrip('\x00')
        model = rsp[30:54].decode('ascii', errors='ignore').rstrip('\x00')
        print(f'4G Status: State={state}, Powered={powered}, SIM={sim_ready}, Reg={net_reg}, PDP={pdp}, CSQ={csq}')
        print(f'IP: "{ip}"')
        print(f'Model: "{model}"')
    else:
        print('Unexpected response or length')
    s.close()
except Exception as e:
    print('Error:', e)
