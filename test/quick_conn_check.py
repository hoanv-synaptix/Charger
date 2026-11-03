import sys
import os
import serial

sys.path.append(os.path.dirname(__file__))

ser = serial.Serial("COM44", 115200, timeout=0.5)
f = bytearray([0xAA, 0x55, 0x0F, 0x00])
crc = 0
for b in f[2:]:
    crc ^= b
    for _ in range(8):
        crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
f.append(crc)
print("Sending:", f.hex())
ser.write(f)
import time; time.sleep(0.1)
resp = ser.read(ser.in_waiting or 64)
print("Received:", resp.hex() if resp else "None")
ser.close()
