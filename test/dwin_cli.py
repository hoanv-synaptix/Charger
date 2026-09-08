#!/usr/bin/env python3
"""
DWIN CLI -- debug the DWIN panel THROUGH the MCU, over the USB-CDC debug
protocol. The MCU relays raw bytes to/from the panel on its own RS485
transceiver, so nothing needs re-wiring.

Requires a DEBUG build of the firmware (CHG_DEBUG_DWIN -- cmake --preset Debug).
In a Release build DEBUG_CMD_DWIN_XFER is not compiled and every command
here gets a NACK.

    python test/dwin_cli.py                       # auto-port, run the self-test
    python test/dwin_cli.py --port COM12 version
    python test/dwin_cli.py --port COM12 read 0x1042
    python test/dwin_cli.py --port COM12 pattern
    python test/dwin_cli.py --port COM12 page 1
    python test/dwin_cli.py --port COM12 poll          # watch the action button
    python test/dwin_cli.py --port COM12 raw A55A0483000F01
"""
import argparse
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("[FAIL] pyserial not installed -- run: pip install pyserial")
    sys.exit(1)

# ---- USB-CDC debug protocol -------------------------------------------
SOF1, SOF2 = 0xAA, 0x55
CMD_ENTER, CMD_EXIT = 0x10, 0x11
CMD_DWIN_XFER = 0x1B
RSP_DWIN_XFER = 0x9B
RSP_ACK, RSP_NACK = 0x82, 0x83


def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


def cdc_frame(cmd: int, payload: bytes = b"") -> bytes:
    body = bytes([cmd, len(payload)]) + payload
    return bytes([SOF1, SOF2]) + body + bytes([crc8(body)])


def cdc_read(ser, want_cmd=None, timeout=0.6):
    """Read one AA55 frame; returns (cmd, payload) or (None, b'')."""
    end = time.time() + timeout
    buf = bytearray()
    while time.time() < end:
        buf += ser.read(256)
        while len(buf) >= 5:
            i = buf.find(b"\xAA\x55")
            if i < 0:
                buf.clear()
                break
            if len(buf) < i + 5:
                break
            cmd = buf[i + 2]
            ln = buf[i + 3]
            if len(buf) < i + 5 + ln:
                break
            frame = bytes(buf[i:i + 5 + ln])
            del buf[:i + 5 + ln]
            payload = frame[4:4 + ln]
            if want_cmd is None or cmd == want_cmd:
                return cmd, payload
    return None, b""


# ---- DWIN DGUS-II frames --------------------------------------------
# Must match Modules/hmi/dwin_protocol.h (DWIN_HEADER_1 / _2). Stock DGUS is
# 0x5A,0xA5; this project is currently built with them swapped.
H1, H2 = 0xA5, 0x5A


def dwin_write(vp: int, words) -> bytes:
    body = bytes([vp >> 8, vp & 0xFF])
    for w in words:
        body += bytes([(w >> 8) & 0xFF, w & 0xFF])
    p = bytes([0x82]) + body
    return bytes([H1, H2, len(p)]) + p


def dwin_write_text(vp: int, text: str, n_words: int = 4) -> bytes:
    """Write a fixed-width ASCII-compatible GBK Text Display field."""
    raw = text.encode("ascii")[: n_words * 2].ljust(n_words * 2, b"\0")
    p = bytes([0x82, vp >> 8, vp & 0xFF]) + raw
    return bytes([H1, H2, len(p)]) + p


def dwin_read(vp: int, n: int) -> bytes:
    p = bytes([0x83, vp >> 8, vp & 0xFF, n])
    return bytes([H1, H2, len(p)]) + p


# ---- transport ------------------------------------------------------
class Link:
    def __init__(self, ser):
        self.ser = ser

    def xfer(self, tx: bytes = b"", settle=0.20) -> bytes:
        """Push tx to the panel via the MCU, then collect the reply."""
        self.ser.reset_input_buffer()
        self.ser.write(cdc_frame(CMD_DWIN_XFER, tx))
        cmd, _ = cdc_read(self.ser, RSP_DWIN_XFER, 0.5)
        if cmd is None:
            print("[FAIL] no DWIN_XFER response -- Release build? (need CHG_DEBUG_DWIN)")
            return b""
        time.sleep(settle)
        self.ser.write(cdc_frame(CMD_DWIN_XFER, b""))
        cmd, payload = cdc_read(self.ser, RSP_DWIN_XFER, 0.5)
        return bytes(payload) if cmd == RSP_DWIN_XFER else b""


def pick_port():
    for p in serial.tools.list_ports.comports():
        d = (p.description or "")
        if "USB Serial" in d or "CDC" in d or "ACM" in d:
            return p.device
    ports = [p.device for p in serial.tools.list_ports.comports()]
    print(f"[FAIL] cannot auto-pick a port; pass --port. seen: {ports}")
    sys.exit(1)


def show(label, resp: bytes):
    if not resp:
        print(f"  {label}: (no reply from panel)")
        return
    print(f"  {label}: {resp.hex(' ')}")
    if len(resp) >= 7 and resp[0] == H1 and resp[1] == H2 and resp[3] == 0x83:
        vp = (resp[4] << 8) | resp[5]
        words = resp[7:7 + resp[6] * 2]
        vals = [(words[i] << 8) | words[i + 1] for i in range(0, len(words) - 1, 2)]
        print(f"        -> read reply VP 0x{vp:04X} = {vals}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("cmd", nargs="?", default="pattern",
                    choices=["version", "read", "write", "page", "poll", "raw", "pattern"])
    ap.add_argument("args", nargs="*")
    a = ap.parse_args()
    port = a.port or pick_port()
    print(f"[..] {port} (MCU USB-CDC) -> relay -> DWIN RS485")
    ser = serial.Serial(port, 115200, timeout=0.3)
    time.sleep(0.3)
    ser.write(cdc_frame(CMD_ENTER))
    cdc_read(ser, timeout=0.4)          # quiet the 1s auto-stream
    lk = Link(ser)

    try:
        if a.cmd == "version":
            show("version (no CRC)", lk.xfer(bytes([H1, H2, 0x04, 0x83, 0x00, 0x0F, 0x01])))

        elif a.cmd == "read":
            vp = int(a.args[0], 0)
            n = int(a.args[1], 0) if len(a.args) > 1 else 1
            show(f"read 0x{vp:04X} x{n}", lk.xfer(dwin_read(vp, n)))

        elif a.cmd == "write":
            vp = int(a.args[0], 0)
            words = [int(x, 0) & 0xFFFF for x in a.args[1:]]
            lk.xfer(dwin_write(vp, words))
            print(f"  wrote 0x{vp:04X} <- {words}")

        elif a.cmd == "page":
            pid = int(a.args[0], 0)
            lk.xfer(dwin_write(0x0084, [0x5A01, pid]))
            print(f"  page -> {pid}")

        elif a.cmd == "raw":
            tx = bytes.fromhex(a.args[0])
            show("raw", lk.xfer(tx))

        elif a.cmd == "poll":
            print("  polling VP 0x1042 -- press the on-screen button (Ctrl+C to stop)")
            while True:
                r = lk.xfer(dwin_read(0x1042, 1), settle=0.15)
                if len(r) >= 9 and r[3] == 0x83:
                    v = (r[7] << 8) | r[8]
                    if v:
                        print(f"  [button] keycode = 0x{v:04X}")
                        lk.xfer(dwin_write(0x1042, [0]))
                time.sleep(0.2)

        elif a.cmd == "pattern":
            print("[1] version read")
            show("version", lk.xfer(bytes([H1, H2, 0x04, 0x83, 0x00, 0x0F, 0x01])))
            print("[2] dashboard test pattern")
            lk.xfer(dwin_write_text(0x1000, "52.1"))
            lk.xfer(dwin_write_text(0x1004, "10.5"))
            lk.xfer(dwin_write_text(0x1008, "3.0"))
            lk.xfer(dwin_write_text(0x1010, "52.1"))
            lk.xfer(dwin_write_text(0x1014, "3.25"))
            lk.xfer(dwin_write_text(0x1018, "20.0"))
            lk.xfer(dwin_write_text(0x1020, "231"))
            lk.xfer(dwin_write_text(0x1024, "232"))
            lk.xfer(dwin_write_text(0x1028, "230"))
            lk.xfer(dwin_write_text(0x1030, "27.0"))
            lk.xfer(dwin_write_text(0x1034, "33.0"))
            lk.xfer(dwin_write_text(0x1038, "-5.0"))
            lk.xfer(dwin_write_text(0x1048, "66%"))      # SOC Text Display
            lk.xfer(dwin_write(0x1041, [2]))              # status = CHARGING
            lk.xfer(dwin_write(0x1042, [1]))              # button = STOP
            print("[3] page -> dashboard")
            lk.xfer(dwin_write(0x0084, [0x5A01, 1]))
            print("[4] read back")
            show("0x1000", lk.xfer(dwin_read(0x1000, 3)))
            show("0x1041", lk.xfer(dwin_read(0x1041, 1)))
            print("\n>>> screen should now show DC 52.1/10.5/3.0kW, PACK 52.1, "
                  "CELL 3.25, AC 231/232/230, TEMP 27/33/-5, SOC 66, CHARGING.")
            print(">>> If read-back replies are empty but writes seem ignored on "
                  "screen: CRC is probably ON in the DGUS CFG (firmware sends none).")
    except KeyboardInterrupt:
        pass
    finally:
        ser.write(cdc_frame(CMD_EXIT))
        ser.close()


if __name__ == "__main__":
    main()
