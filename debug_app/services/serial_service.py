"""USB CDC Serial Service for Debug App"""

import os
import serial
import serial.tools.list_ports
import threading
import time
from typing import Optional, Callable, List

SOF1 = 0xAA
SOF2 = 0x55
CRC_POLY = 0x07
MAX_PAYLOAD = 255
DEBUG_LOG_FILE = "serial_debug.log"
DEBUG_LOG_MAX_BYTES = 5 * 1024 * 1024  # 5MB — debug aid, not an audit log; truncate past this

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
        # Raw serial debug log to DEBUG_LOG_FILE — off by default (was a
        # hardcoded-True module constant that opened+wrote+closed the file
        # on every single TX/RX frame, including the MCU's automatic ~1s
        # telemetry stream, synchronously on the RX thread; a slow disk op
        # there delays draining the OS serial buffer, which reads back to
        # the user as app lag). Now an explicit opt-in toggle
        # (set_debug_enabled(), wired to a UI checkbox) that keeps one file
        # handle open instead of reopening per call.
        self.debug_enabled = False
        self._debug_fh = None

    def set_debug_enabled(self, enabled: bool):
        """Toggle raw serial debug logging to DEBUG_LOG_FILE at runtime."""
        if enabled == self.debug_enabled:
            return
        self.debug_enabled = enabled
        if enabled:
            try:
                if os.path.exists(DEBUG_LOG_FILE) and os.path.getsize(DEBUG_LOG_FILE) > DEBUG_LOG_MAX_BYTES:
                    # Debug aid, not an audit log -- truncate rather than
                    # rotate/archive, so a long-forgotten enabled session
                    # doesn't grow this file forever.
                    open(DEBUG_LOG_FILE, "w").close()
                self._debug_fh = open(DEBUG_LOG_FILE, "a")
            except Exception:
                self._debug_fh = None
                self.debug_enabled = False
        else:
            if self._debug_fh:
                try:
                    self._debug_fh.close()
                except Exception:
                    pass
                self._debug_fh = None

    def _debug_log(self, msg: str):
        if self.debug_enabled and self._debug_fh:
            try:
                self._debug_fh.write(f"[{time.strftime('%H:%M:%S')}] {msg}\n")
                self._debug_fh.flush()
            except Exception:
                pass

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
        # Don't leak the debug-log handle across reconnects.
        self.set_debug_enabled(False)

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
            self._debug_log(f"[SERIAL TX] cmd=0x{cmd:02X} len={len(payload)} frame={frame.hex().upper()}")
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
                self._debug_log(f"[SERIAL RX] No SOF in {len(buf)} bytes, clearing: {buf[:16].hex().upper()}...")
                buf.clear()
                return

            if idx > 0:
                # Discard bytes before SOF
                self._debug_log(f"[SERIAL RX] Skipping {idx} bytes before SOF")
                del buf[:idx]

            if len(buf) < 4:
                return

            cmd = buf[2]
            plen = buf[3]

            if len(buf) == 4:
                self._debug_log(f"[SERIAL RX] Header: cmd=0x{cmd:02X} plen={plen}, waiting for {4+plen+1} total bytes")

            if plen > MAX_PAYLOAD:
                # Invalid length, skip SOF1 and try again
                self._debug_log(f"[SERIAL RX] Invalid plen={plen}, skipping SOF")
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
            expected_crc = crc8(crc_data)
            if expected_crc != frame[-1]:
                self._debug_log(f"[SERIAL RX] CRC FAIL cmd=0x{cmd:02X} plen={plen} expected=0x{expected_crc:02X} got=0x{frame[-1]:02X}")
                self._log(f"RX Bad CRC: expected 0x{expected_crc:02X}, got 0x{frame[-1]:02X}")
                continue

            payload = frame[4:4 + plen]
            self._debug_log(f"[SERIAL RX] Frame OK cmd=0x{cmd:02X} plen={plen} payload={payload[:8].hex().upper()}{'...' if plen > 8 else ''}")
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
