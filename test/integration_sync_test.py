#!/usr/bin/env python3
"""
Full System Integration Sync Test (E2E)
Synchronizes the 3 main interfaces of the MCU:
1. APP/PC (Serial) -> Sends target Setpoints to MCU.
2. BMS (CAN2) -> Simulates battery responses (Table-driven protocol).
3. Charger Modules (CAN1) -> Simulates rectifier modules (Maxwell/Lianming/Tonhe).

Requirements:
- pip install pyserial python-can
- PCAN / CANable adapters for CAN1 and CAN2
"""
import serial
import serial.tools.list_ports
import can
import time
import struct
import threading
import sys

# ==========================================
# CONFIGURATION
# ==========================================
CAN1_INTERFACE = 'pcan'  # Modify based on your adapter (e.g., 'slcan', 'socketcan')
CAN1_CHANNEL = 'PCAN_USBBUS1'
CAN2_INTERFACE = 'pcan'
CAN2_CHANNEL = 'PCAN_USBBUS2'
SERIAL_BAUD = 115200

# ==========================================
# THREAD 1: BMS SIMULATOR (CAN2)
# ==========================================
class BMSSimulator(threading.Thread):
    def __init__(self, bus):
        super().__init__()
        self.bus = bus
        self.running = True
        self.soc = 50
        self.voltage = 400.0  # 400V
        self.current = 10.0   # 10A
        
    def run(self):
        print("[BMS Sim] Started on CAN2 (250kbps)")
        while self.running:
            # 1. BATT_ST1 (ID: 0x02F4) - Sent every 20ms
            # Format: raw_volt (2), raw_curr (2), soc (1), soh (1), alarm_status (2)
            raw_v = int(self.voltage * 10)
            raw_c = int((self.current + 400) * 10) # Offset based on typical protocol
            data_st1 = struct.pack('<HHBBH', raw_v, raw_c, self.soc, 100, 0x0000)
            msg1 = can.Message(arbitration_id=0x02F4, data=data_st1, is_extended_id=False)
            self.bus.send(msg1)

            # 2. CELL_TEMP (ID: 0x04F4) - Sent every 500ms (we'll just send every 20ms for simplicity in test)
            data_temp = bytearray([40, 41, 40, 42, 0]) # Mock temps
            msg2 = can.Message(arbitration_id=0x04F4, data=data_temp, is_extended_id=False)
            self.bus.send(msg2)

            time.sleep(0.02) # 20ms tick

# ==========================================
# THREAD 2: CHARGER MODULE SIMULATOR (CAN1)
# ==========================================
class ModuleSimulator(threading.Thread):
    def __init__(self, bus, protocol="maxwell"):
        super().__init__()
        self.bus = bus
        self.running = True
        self.protocol = protocol
        self.state = 0 # 0=IDLE, 1=RUNNING
        
    def run(self):
        print(f"[MOD Sim] Started on CAN1 (125kbps) - Protocol: {self.protocol}")
        while self.running:
            # Listen for MCU command (Target setpoint)
            msg = self.bus.recv(timeout=0.05)
            if msg:
                # If MCU sent a command, we transition to RUNNING and broadcast summary
                self.state = 1
                
            # Periodically broadcast state to MCU (so it passes CheckOfflineTimeout)
            if self.protocol == "maxwell":
                # Maxwell summary ID: 0x02000000 + ID
                # We simulate Module ID 1
                data = bytearray([0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00])
                data[0] = 0x01 # Online/Output enabled
                data[3] = 40 # 400.0V
                data[4] = 10 # 10.0A
                out_msg = can.Message(arbitration_id=0x02000001, data=data, is_extended_id=True)
                self.bus.send(out_msg)
                
            time.sleep(0.1) # 100ms tick

# ==========================================
# THREAD 3: APP / PC SYNC (SERIAL)
# ==========================================
def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 0x80: crc = ((crc << 1) ^ 0x07) & 0xFF
            else: crc = (crc << 1) & 0xFF
    return crc

def app_sync_test(port):
    print("\n[APP Sync] Sending target SETPOINT down to MCU...")
    
    # Send PC_CMD_SET_VOLTAGE (0x01)
    payload_v = struct.pack('<f', 400.0)
    frame_v = bytearray([0xAA, 0x55, 0x01, len(payload_v)])
    frame_v.extend(payload_v)
    frame_v.append(crc8(frame_v[2:]))
    port.write(frame_v)
    time.sleep(0.1)

    # Send PC_CMD_SET_CURRENT (0x02)
    payload_i = struct.pack('<f', 20.0)
    frame_i = bytearray([0xAA, 0x55, 0x02, len(payload_i)])
    frame_i.extend(payload_i)
    frame_i.append(crc8(frame_i[2:]))
    port.write(frame_i)
    time.sleep(0.1)
    
    # Read MCU State (CMD_READ_REG or wait for STATUS 0x81)
    print("[APP Sync] Querying MCU State to verify propagation...")
    frame = bytearray([0xAA, 0x55, 0x13, 0x01, 0x00])
    frame.append(crc8(frame[2:]))
    port.write(frame)
    
    # Wait for MCU response
    port.timeout = 0.1
    start = time.time()
    while time.time() - start < 2.0:
        if port.in_waiting > 0:
            b = port.read(1)
            if b == b'\xaa':
                b2 = port.read(1)
                if b2 == b'\x55':
                    cmd_len = port.read(2)
                    if len(cmd_len) == 2:
                        cmd, length = cmd_len[0], cmd_len[1]
                        payload = port.read(length) if length > 0 else b""
                        crc_byte = port.read(1)
                        if cmd == 0x93 or cmd == 0x81: # STATUS
                            print(f"[APP Sync] Success! MCU processed the flow. State length: {len(payload)}")
                            return True
                        elif cmd == 0x83: # NACK
                            print(f"[APP Sync] Received NACK! Reason: {payload[0] if length > 0 else 'unknown'}")
                            return False
                        elif cmd == 0x96: # ERROR
                            print(f"[APP Sync] Received ERROR (0x96)! Reason: {payload[0] if length > 0 else 'unknown'}")
                            return False
                        elif cmd == 0x82: # ACK
                            print(f"[APP Sync] Received ACK!")
                            # Keep waiting for status
                            continue
                        else:
                            print(f"[APP Sync] Received other CMD: {hex(cmd)}")
                            continue
                else:
                    # Print raw char if it's not our header
                    try: print(b.decode('ascii') + b2.decode('ascii'), end='', flush=True)
                    except: pass
            else:
                try: print(b.decode('ascii'), end='', flush=True)
                except: pass
    
    print("\n[APP Sync] Failed to get response from MCU.")
    return False


if __name__ == '__main__':
    print("=== Full E2E Synchronization Test ===")
    
    try:
        can1_bus = can.interface.Bus(interface=CAN1_INTERFACE, channel=CAN1_CHANNEL, bitrate=125000)
        can2_bus = can.interface.Bus(interface=CAN2_INTERFACE, channel=CAN2_CHANNEL, bitrate=250000)
    except Exception as e:
        print(f"CAN Interface error (Are adapters plugged in?): {e}")
        print("Note: Install python-can and drivers to run full HIL test.")
        # Proceeding with mock buses if physical not found (for demonstration)
        can1_bus = can.interface.Bus(interface='virtual', channel='vcan0')
        can2_bus = can.interface.Bus(interface='virtual', channel='vcan1')
        print("Using VIRTUAL CAN buses for demonstration.")

    # Find MCU serial port
    ports = list(serial.tools.list_ports.comports())
    st_port = None
    # 1. Prefer "USB Serial Device" or "STLink"
    for p in ports:
        if "USB Serial Device" in p.description or "STMicroelectronics" in p.description or "STLink" in p.description:
            st_port = p.device
            break
    
    if st_port is None:
        print("[ERROR] MCU USB CDC Port not found!")
        sys.exit(1)

    # Start Simulators
    bms_sim = BMSSimulator(can2_bus)
    mod_sim = ModuleSimulator(can1_bus, protocol="maxwell")
    
    bms_sim.start()
    mod_sim.start()
    
    print(f"[APP Sync] Using Serial Port: {st_port}")
    try:
        with serial.Serial(st_port, SERIAL_BAUD) as ser:
            ser.dtr = True
            ser.rts = True
            ser.reset_input_buffer()
            # Run the E2E flow
            app_sync_test(ser)
            
            print("\nTest complete. Monitoring buses for 5 seconds...")
            time.sleep(5)
            
    finally:
        print("Shutting down simulators...")
        bms_sim.running = False
        mod_sim.running = False
        bms_sim.join()
        mod_sim.join()
        can1_bus.shutdown()
        can2_bus.shutdown()
        print("Done.")

