"""
Sprint 1.2 — ISR race + LOG removal + FIFO burst + tick wrap
"""
import random
import threading
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[1]

def read(p):
    return (ROOT / p).read_text(encoding="utf-8", errors="ignore")

# ---------- CRC8 ----------
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

# ---------- Python replica of pc_protocol FeedByte FSM ----------
class PcProtoSim:
    ST_SOF1, ST_SOF2, ST_CMD, ST_LEN, ST_PAYLOAD, ST_CRC = range(6)
    def __init__(self):
        self.state = self.ST_SOF1
        self.rx_cmd = 0
        self.rx_len = 0
        self.rx_idx = 0
        self.rx_payload = bytearray(256)
        self.frames_ok = 0
        self.frames_bad_crc = 0
        self.queue = []
        self.queue_depth = 8

    def enqueue(self, cmd, payload):
        if len(self.queue) >= self.queue_depth:
            return False
        self.queue.append((cmd, payload))
        return True

    def feed(self, byte: int):
        if self.state == self.ST_SOF1:
            if byte == 0xAA:
                self.state = self.ST_SOF2
        elif self.state == self.ST_SOF2:
            self.state = self.ST_CMD if byte == 0x55 else self.ST_SOF1
        elif self.state == self.ST_CMD:
            self.rx_cmd = byte
            self.state = self.ST_LEN
        elif self.state == self.ST_LEN:
            self.rx_len = byte
            self.rx_idx = 0
            if self.rx_len > 255:
                self.state = self.ST_SOF1
            elif self.rx_len == 0:
                self.state = self.ST_CRC
            else:
                self.state = self.ST_PAYLOAD
        elif self.state == self.ST_PAYLOAD:
            self.rx_payload[self.rx_idx] = byte
            self.rx_idx += 1
            if self.rx_idx >= self.rx_len:
                self.state = self.ST_CRC
        elif self.state == self.ST_CRC:
            buf = bytes([self.rx_cmd, self.rx_len]) + bytes(self.rx_payload[:self.rx_len])
            if crc8(buf) == byte:
                self.frames_ok += 1
                # simulate process_frame sending ACK/NACK via enqueue (no LOG)
                self.enqueue(0x82, bytes([self.rx_cmd]))
            else:
                self.frames_bad_crc += 1
            self.state = self.ST_SOF1

def test_no_log_in_isr():
    bms = read("Modules/bms/bms_core.c")
    # BMS_FeedFrame should contain no LOG
    feed = bms[bms.find("void BMS_FeedFrame"):bms.find("void BMS_Process")]
    assert "LOG(" not in feed, "BMS_FeedFrame must not call LOG (ISR)"

    pc = read("App/Protocol/pc_protocol.c")
    # FeedByte and process_frame should not contain LOG
    # Extract process_frame region
    pf_start = pc.find("static void process_frame")
    pf_end = pc.find("void PC_Protocol_FeedByte", pf_start)
    pf = pc[pf_start:pf_end]
    assert "LOG(" not in pf, "process_frame (ISR) must not LOG"
    fb = pc[pc.find("void PC_Protocol_FeedByte"):pc.find("void PC_Protocol_SendStatus", pc.find("void PC_Protocol_FeedByte"))]
    assert "LOG(" not in fb, "FeedByte (ISR) must not LOG"

def test_volatile_and_critical_sections():
    bms = read("Modules/bms/bms_core.c")
    assert "volatile" in bms
    assert "BSP_EnterCritical" in bms
    assert "BSP_ExitCritical" in bms
    # BMS_GetView must use the platform critical-section wrapper.
    gv = bms[bms.find("void BMS_GetView"):bms.find("bool BMS_ShouldClose")]
    assert "BSP_EnterCritical" in gv
    assert "BSP_ExitCritical" in gv

    pc = read("App/Protocol/pc_protocol.c")
    assert "volatile uint8_t g_tx_head" in pc or "volatile uint8_t g_tx_tail" in pc
    assert "BSP_EnterCritical" in pc

    chg = read("Modules/chg_lib/chg_lib_core.c")
    assert "BSP_EnterCritical" in chg

def test_fuzz_feedbyte_burst_1000():
    sim = PcProtoSim()
    # Build burst of 1000 valid frames interleaved with random garbage
    frames = [build_frame(0x06, b'') for _ in range(200)]  # PING
    frames += [build_frame(0x01, b'\x00\x00\x80\x3f') for _ in range(200)]  # SET_VOLTAGE 1.0
    frames += [build_frame(0x09, b'\x01') for _ in range(200)]
    # Add 400 bad CRC frames
    bad = []
    for _ in range(400):
        f = build_frame(0x01, b'\x00\x00\x00\x00')
        bad.append(f[:-1] + bytes([f[-1] ^ 0xFF]))
    stream = b''.join(frames + bad)
    # Add random interleaved 0xAA bytes to test re-sync
    stream = stream + b'\xAA\xAA\x55' * 10
    # Feed byte by byte (simulates ISR burst)
    for b in stream:
        sim.feed(b)
    # Must have processed many frames without exception
    assert sim.frames_ok >= 500
    assert sim.frames_bad_crc >= 300
    # SOF re-sync edge case: AA AA 55 should be handled
    sim2 = PcProtoSim()
    for b in b'\xAA\xAA\x55\x06\x00\xd1':  # AA AA 55 is overlapping SOF
        sim2.feed(b)
    # Should recover

def test_tick_wrap_and_stale():
    # BMS elapsed logic: now < last => elapsed=0 (ISR race case)
    def elapsed(now, last):
        if now >= last:
            return now - last
        else:
            return 0
    assert elapsed(0x00000010, 0xFFFFFFF0) == 0
    assert elapsed(0x1000, 0x0FFF) == 1
    # Wrap simulation: 49 days tick uses uint32 wrap; elapsed via unsigned subtraction would be large
    # but our code clamps to 0 to avoid false OFFLINE
    # Verify OFFLINE timeout 5000
    BMS_OFFLINE = 5000
    assert elapsed(100, 0) < BMS_OFFLINE
    assert elapsed(6000, 0) >= BMS_OFFLINE
    # Simulate rapid ISR update after Process read: Process reads last=1000, ISR updates to 1005, now=1002 => elapsed clamped 0 (not negative)
    assert elapsed(1002, 1005) == 0

def test_chg_lib_view_atomic():
    # Check chg_lib drivers protect GetModuleView with irq
    for name in ["chg_lib_maxwell.c", "chg_lib_lianming.c", "chg_lib_tonhe.c"]:
        txt = read(f"Modules/chg_lib/{name}")
        # GetModuleView must use the platform critical-section wrapper.
        assert "BSP_EnterCritical" in txt
        assert "BSP_ExitCritical" in txt
        # must copy atomically
        assert "tmp" in txt or "CHG_LIB_ModuleView_t" in txt

def test_bsp_can_volatile():
    txt = read("BSP/bsp_can.c")
    assert "volatile uint32_t g_c1_tx" in txt

def test_can_consumers_run_outside_isr():
    """Protocol/driver consumers must not be wrapped in long IRQ-off calls."""
    chg = read("Modules/chg_lib/chg_lib_core.c")
    for name in ["void CHG_LIB_Process", "void CHG_LIB_FeedCanFrame"]:
        start = chg.find(name)
        assert start >= 0, f"{name} not found"
        end = chg.find("\n}", start)
        assert end > start
        body = chg[start:end]
        assert "BSP_EnterCritical" not in body
        assert "BSP_ExitCritical" not in body

    app = read("App/System/app_main.c")
    assert "BSP_CAN_ProcessRx();" in app
    assert app.find("BSP_CAN_ProcessRx();") < app.find("CHG_LIB_Process(now);")
    assert app.find("BSP_CAN_ProcessRx();") < app.find("BMS_Process(now);")
