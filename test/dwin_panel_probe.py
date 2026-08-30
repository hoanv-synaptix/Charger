#!/usr/bin/env python3
"""
DWIN panel probe -- talks to a DGUS-II screen DIRECTLY from the PC over a
USB<->RS485 adapter, bypassing the MCU. Use it to prove that the panel,
the DGUS 13/14-bin project and the wiring are all correct, independent of
the charger firmware.

WIRING: move the screen's RS485 A/B pair from the MCU transceiver onto a
USB-RS485 adapter (A->A, B->B, GND common). The screen must be powered.

    python test/dwin_panel_probe.py --port COM12
    python test/dwin_panel_probe.py            # auto-pick a USB serial port

What it does, in order:
 1. Read the DGUS version (5A A5 04 83 000F 01). A correct reply proves the
    link + baud (115200-8N1) + that frame CRC is OFF in the panel CFG.
    Then it retries WITH a CRC-16 to tell you if the panel wants CRC on.
 2. Write a full dashboard test pattern to VP 0x1000..0x1043.
 3. Switch to the dashboard page (VP 0x0084 <- 0x5A01 0001).
 4. Read a couple of VPs back to confirm the writes landed.
 5. Poll VP 0x1042 for ~5 s -- press the on-screen action button and it
    should print the returned key code.

This mirrors exactly the frames Modules/hmi/dwin_protocol.c emits.
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

H1, H2 = 0x5A, 0xA5
CMD_WRITE, CMD_READ = 0x82, 0x83


def crc16(data: bytes) -> bytes:
    """DGUS CRC-16 (poly 0xA001, reflected), low byte first on the wire."""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if (crc & 1) else (crc >> 1)
    return bytes([crc & 0xFF, (crc >> 8) & 0xFF])


def frame(cmd: int, body: bytes, with_crc: bool = False) -> bytes:
    payload = bytes([cmd]) + body
    if with_crc:
        payload += crc16(payload)
    return bytes([H1, H2, len(payload)]) + payload


def w_words(vp: int, words, with_crc=False) -> bytes:
    body = bytes([(vp >> 8) & 0xFF, vp & 0xFF])
    for w in words:
        body += bytes([(w >> 8) & 0xFF, w & 0xFF])
    return frame(CMD_WRITE, body, with_crc)


def r_words(vp: int, n: int, with_crc=False) -> bytes:
    return frame(CMD_READ, bytes([(vp >> 8) & 0xFF, vp & 0xFF, n]), with_crc)


def pick_port() -> str:
    for p in serial.tools.list_ports.comports():
        if "USB" in (p.description or "") or "Serial" in (p.description or ""):
            return p.device
    print("[FAIL] no USB serial port found; pass --port COMxx")
    sys.exit(1)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()
    port = args.port or pick_port()
    print(f"[..] opening {port} @ {args.baud} 8N1")
    s = serial.Serial(port, args.baud, timeout=0.4)
    time.sleep(0.3)
    s.reset_input_buffer()

    # ---- 1. version read, CRC off then CRC on -------------------------
    crc_mode = None
    for label, use_crc in (("CRC OFF", False), ("CRC ON", True)):
        s.reset_input_buffer()
        s.write(r_words(0x000F, 1, with_crc=use_crc))
        time.sleep(0.3)
        resp = s.read(64)
        ok = len(resp) >= 6 and resp[0] == H1 and resp[1] == H2 and resp[3] == CMD_READ
        print(f"[{'OK' if ok else '--'}] version read ({label}): {resp.hex(' ') or '(no reply)'}")
        if ok and crc_mode is None:
            crc_mode = use_crc
    if crc_mode is None:
        print("\n[FAIL] no reply either way. Check: A/B swapped, GND, baud, "
              "screen power, adapter direction. Nothing else below will work.")
        return 1
    print(f"\n[==] panel is talking with {'CRC ON' if crc_mode else 'CRC OFF'}.")
    if crc_mode:
        print("     >>> firmware sends NO CRC -- turn CRC OFF in the DGUS CFG "
              "(config byte 0x05 bit 7 = 0) or the screen will ignore the MCU.")

    def send(fr):
        s.write(fr); time.sleep(0.05)

    # ---- 2. dashboard test pattern ----------------------------------
    print("\n[..] writing dashboard test pattern")
    send(w_words(0x1000, [521, 105, 3000], crc_mode))   # DC 52.1V 10.5A 3000W
    send(w_words(0x1010, [521, 325], crc_mode))         # pack 52.1V, cell 3.25V
    send(w_words(0x1012, [0x0000, 0x00C8], crc_mode))   # charged 20.0 Ah (u32)
    send(w_words(0x1020, [231, 232, 230], crc_mode))    # AC L1/L2/L3
    send(w_words(0x1030, [27, 33, 0xFFFB], crc_mode))   # temp 27 / 33 / -5 (i16)
    send(w_words(0x1040, [66, 2], crc_mode))            # SOC 66%, status=CHARGING
    send(w_words(0x1043, [1], crc_mode))                # button mode = STOP

    def w_str(vp, text, n_words=8):
        raw = text.encode("ascii")[: n_words * 2].ljust(n_words * 2, b"\0")
        body = bytes([(vp >> 8) & 0xFF, vp & 0xFF]) + raw
        send(frame(CMD_WRITE, body, crc_mode))
    w_str(0x1100, "HW V1.0")
    w_str(0x1108, "FW V2.0.0")
    w_str(0x1110, "PKG-0001")
    send(w_words(0x1118, [0x0000, 0x0E10], crc_mode))   # uptime 3600 s

    # ---- 3. page switch --------------------------------------------
    print("[..] switching to dashboard page (0x0084 <- 5A01 0001)")
    send(w_words(0x0084, [0x5A01, 0x0001], crc_mode))

    # ---- 4. read-back ---------------------------------------------
    time.sleep(0.2)
    for vp in (0x1000, 0x1041):
        s.reset_input_buffer()
        s.write(r_words(vp, 1, crc_mode))
        time.sleep(0.25)
        resp = s.read(32)
        print(f"[<-] read 0x{vp:04X}: {resp.hex(' ') or '(no reply)'}")

    print("\n>>> LOOK AT THE SCREEN NOW. Expect: DC 52.1/10.5/3000, PACK 52.1, "
          "CELL 3.25, AC 231/232/230, TEMP 27/33/-5, SOC 66, CHARGING state, "
          "STOP button, HW/FW/ID strings on the Setting page.")

    # ---- 5. action-button poll ----------------------------------
    print("\n[..] polling VP 0x1042 for 6 s -- press the on-screen button")
    end = time.time() + 6.0
    while time.time() < end:
        s.reset_input_buffer()
        s.write(r_words(0x1042, 1, crc_mode))
        time.sleep(0.4)
        resp = s.read(32)
        if len(resp) >= 9 and resp[3] == CMD_READ:
            val = (resp[7] << 8) | resp[8]
            if val != 0:
                print(f"[OK] action button keycode = 0x{val:04X}")
                send(w_words(0x1042, [0], crc_mode))  # clear, like the firmware does
    print("[done]")
    s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
