import serial
import time
import sys

port = sys.argv[1] if len(sys.argv) > 1 else 'COM12'
try:
    s = serial.Serial(port, 115200, timeout=1)
    s.dtr = True
    s.rts = True
    print(f"Opened {port}, waiting for data...")
    while True:
        data = s.read(100)
        if data:
            print(f"RX: {data.hex().upper()}", flush=True)
except Exception as e:
    print(f"Error: {e}", flush=True)
