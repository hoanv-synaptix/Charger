"""USB CDC Serial Service for Debug App"""

import serial
import serial.tools.list_ports
import threading
import time
from typing import Optional, Callable, List

SOF1 = 0xAA
SOF2 = 0x55
CRC_POLY = 0x07
MAX_PAYLOAD = 255


def crc8(data: bytes) -> int:
    """Calculate CRC8 checksum"""
    crc = 0x00
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ CRC_POLY) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc


def build_frame(cmd: int, payload: bytes = b"") -> bytes:
    """Build a protocol frame"""
    header = bytes([SOF1, SOF2, cmd, len(payload)])
    crc_data = bytes([cmd, len(payload)]) + payload
    crc = crc8(crc_data)
    return header + payload + bytes([crc])


class SerialService:
    """USB CDC Serial Service with protocol framing"""

    def __init__(self):
        self.port: Optional[serial.Serial] = None
        self.running = False
        self.rx_thread: Optional[threading.Thread] = None
        self._callbacks = []
        self._log_callback = None

    def list_ports(self) -> List[str]:
        """List available COM ports"""
        return [p.device for p in serial.tools.list_ports.comports()]

    def connect(self, port: str, baudrate: int = 115200) -> bool:
        """Connect to serial port"""
        try:
            if self.port and self.port.is_open:
                self.port.close()

            self.port = serial.Serial(port, baudrate, timeout=0.1)
            self.port.dtr = True
            self.port.rts = True
            self.running = True
            self.rx_thread = threading.Thread(target=self._rx_loop, daemon=True)
            self.rx_thread.start()
            return True
        except Exception as e:
            print(f"Connect error: {e}")
            return False

    def disconnect(self):
        """Disconnect from serial port"""
        self.running = False
        if self.rx_thread:
            self.rx_thread.join(timeout=1)
        if self.port and self.port.is_open:
            self.port.close()
        self.port = None

    def is_connected(self) -> bool:
        """Check if connected"""
        return self.port is not None and self.port.is_open

    def send(self, cmd: int, payload: bytes = b"") -> bool:
        """Send a protocol frame"""
        if not self.port or not self.port.is_open:
            return False
        try:
            frame = build_frame(cmd, payload)
            self.port.write(frame)
            self._log(f"TX: cmd=0x{cmd:02X} len={len(payload)} data={payload.hex()}")
            return True
        except Exception as e:
            self._log(f"TX Error: {e}")
            return False

    def on_frame(self, callback: Callable[[int, bytes], None]):
        """Register frame callback"""
        self._callbacks.append(callback)

    def on_log(self, callback: Callable[[str], None]):
        """Register log callback"""
        self._log_callback = callback

    def _rx_loop(self):
        """Receive loop - runs in background thread"""
        buf = bytearray()
        while self.running:
            try:
                waiting = self.port.in_waiting
                data = self.port.read(waiting if waiting > 0 else 1)
                if data:
                    buf.extend(data)
                    self._process_buffer(buf)
            except Exception as e:
                self._log(f"RX Error: {e}")
                break

    def _process_buffer(self, buf: bytearray):
        """Process receive buffer, extract frames"""
        while len(buf) >= 5:
            # Find SOF
            idx = buf.find(b'\xAA\x55')

            if idx < 0:
                # No SOF found, clear buffer
                buf.clear()
                return

            if idx > 0:
                # Discard bytes before SOF
                del buf[:idx]

            if len(buf) < 4:
                return

            cmd = buf[2]
            plen = buf[3]

            if plen > MAX_PAYLOAD:
                # Invalid length, skip SOF1 and try again
                del buf[0]
                continue

            total = 4 + plen + 1  # header + payload + crc

            if len(buf) < total:
                return  # Need more bytes

            # Extract frame
            frame = bytes(buf[:total])
            del buf[:total]

            # Verify CRC
            crc_data = frame[2:4 + plen]
            if crc8(crc_data) != frame[-1]:
                self._log(f"RX Bad CRC: expected 0x{crc8(crc_data):02X}, got 0x{frame[-1]:02X}")
                continue

            payload = frame[4:4 + plen]
            self._log(f"RX: cmd=0x{cmd:02X} len={len(payload)}")

            # Notify callbacks
            for cb in self._callbacks:
                try:
                    cb(cmd, payload)
                except Exception as e:
                    print(f"Callback error: {e}")

    def _log(self, msg: str):
        """Log message"""
        timestamp = time.strftime("%H:%M:%S")
        full_msg = f"[{timestamp}] {msg}"
        if self._log_callback:
            self._log_callback(full_msg)
