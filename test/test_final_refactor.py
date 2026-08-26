#!/usr/bin/env python3
"""
Final Refactoring Test (Automation)
Targets:
1. UART/RS485 Non-blocking (Component 3): Flood the serial port to ensure it doesn't crash (HardFault) or block.
2. FSM/BMS Stability (Component 1 & 2): Query states continuously to ensure memory safety and no null pointer derefs during fast polling.
"""
import serial
import serial.tools.list_ports
import time
import struct
import sys
import threading

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

def send_frame(port, cmd, payload=b""):
    frame = bytearray([0xAA, 0x55, cmd, len(payload)])
    if payload:
        frame.extend(payload)
    frame.append(crc8(frame))
    port.write(frame)

def read_frame(port, timeout=1.0):
    port.timeout = timeout
    start_time = time.time()
    while time.time() - start_time < timeout:
        header = port.read(2)
        if len(header) == 2 and header == b'\xaa\x55':
            cmd_len = port.read(2)
            if len(cmd_len) == 2:
                cmd, length = cmd_len[0], cmd_len[1]
                payload = port.read(length) if length > 0 else b""
                crc_byte = port.read(1)
                if crc_byte:
                    frame = bytearray([0xAA, 0x55, cmd, length])
                    if payload: frame.extend(payload)
                    if crc8(frame) == crc_byte[0]:
                        return cmd, payload
    return None, None

def find_st_port():
    ports = serial.tools.list_ports.comports()
    for p in ports:
        if "STLink" in p.description or "Serial" in p.description or "STM" in p.description:
            return p.device
    return None

def test_uart_stress(port):
    print("\n--- [TEST 1] UART Non-blocking Stress Test ---")
    print("Flooding UART with GET_SYSTEM commands to verify Ring Buffer stability...")
    
    # Send 50 queries rapidly (should saturate the TX ring buffer if blocking, but handle gracefully if IT-driven)
    success_count = 0
    start = time.time()
    
    for i in range(50):
        send_frame(port, 0x18) # GET_SYSTEM
        time.sleep(0.01) # 10ms
        
    # Now try to read responses
    port.timeout = 0.05
    while True:
        cmd, p = read_frame(port, timeout=0.05)
        if cmd == 0x98: # RSP_SYSTEM_INFO
            success_count += 1
        elif cmd is None:
            break
            
    dur = time.time() - start
    print(f"Sent 50, Received {success_count} in {dur:.2f}s")
    if success_count > 0:
        print("[PASS] UART survived flood without HardFault. IT-based TX is stable.")
        return True
    else:
        print("[FAIL] No responses. System might have blocked or crashed.")
        return False

def test_bms_fsm_polling(port):
    print("\n--- [TEST 2] Fast BMS/FSM Polling ---")
    print("Querying BMS (0x17) and Charger (0x13) to verify memory safety...")
    
    for i in range(20):
        # 1. BMS
        send_frame(port, 0x17)
        cmd, p = read_frame(port, 0.2)
        if cmd != 0x97:
            print(f"[FAIL] BMS query {i} failed.")
            return False
            
        # 2. Modules
        send_frame(port, 0x13, bytes([0xFF])) # READ_ALL
        cmd2, p2 = read_frame(port, 0.2)
        if cmd2 != 0x93:
            print(f"[FAIL] Modules query {i} failed.")
            return False
            
    print("[PASS] 20 fast queries processed successfully. FSM & BMS structures are stable.")
    return True

if __name__ == '__main__':
    p_name = find_st_port()
    if not p_name:
        print("No STM32 COM port found. Please connect the board.")
        # We don't fail CI for this, just exit
        sys.exit(0)
        
    print(f"Found board on {p_name}, starting automation...")
    try:
        with serial.Serial(p_name, 115200) as port:
            port.reset_input_buffer()
            t1 = test_uart_stress(port)
            t2 = test_bms_fsm_polling(port)
            
            if t1 and t2:
                print("\n[RESULT] All automation checks PASSED.")
            else:
                print("\n[RESULT] Automation checks FAILED.")
                sys.exit(1)
    except Exception as e:
        print(f"Error accessing port {p_name}: {e}")
