import struct
import time
import random

def create_fuzz_frame():
    # USB CDC format: AA AA 55 [Length] [CMD] [Payload...] [CRC8]
    cmd = random.choice([0x10, 0x11, 0x12, 0x13, 0xFF])
    
    if cmd == 0x11: # SET_MANUAL_TARGET (2 floats)
        payload = struct.pack(">ff", 
            random.choice([float('nan'), float('inf'), -1.0, 0.0, 1000.0]),
            random.choice([float('nan'), float('inf'), -1.0, 0.0, 1000.0])
        )
    else:
        payload = bytes([random.randint(0, 255) for _ in range(random.randint(0, 10))])

    length = len(payload) + 1
    frame = bytearray([0xAA, 0xAA, 0x55, length, cmd]) + payload
    
    # Fake CRC for now
    frame.append(0x00)
    return frame

if __name__ == "__main__":
    print("Running PC Fuzzing script...")
    for _ in range(10):
        frame = create_fuzz_frame()
        print(f"Fuzz frame: {frame.hex()}")
        time.sleep(0.1)
    print("Fuzzing completed.")
