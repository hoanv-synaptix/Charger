"""
PC Debug Protocol — MCU ↔ App Debug communication tests (host, no hardware)
Covers: frame format, command dispatch, config load/store, TX queue, ISR safety
"""
import struct
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[1]

def read(p):
    return (ROOT / p).read_text(encoding="utf-8", errors="ignore")

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

# ─── Protocol constants (must match pc_debug_protocol.h) ───
DEBUG_CMD_ENTER          = 0x10
DEBUG_CMD_EXIT           = 0x11
DEBUG_CMD_READ_ALL       = 0x12
DEBUG_CMD_READ_ONE       = 0x13
DEBUG_CMD_READ_STATS     = 0x14
DEBUG_CMD_WRITE_REG      = 0x15
DEBUG_CMD_SEND_RAW_CAN   = 0x16
DEBUG_CMD_READ_BMS       = 0x17
DEBUG_CMD_GET_SYSTEM     = 0x18
DEBUG_CMD_GET_CHARGE_CFG = 0x19
DEBUG_CMD_SET_CHARGE_CFG = 0x1A

DEBUG_RSP_MODULE_DATA    = 0x90
DEBUG_RSP_ALL_MODULES    = 0x91
DEBUG_RSP_COMM_STATS     = 0x92
DEBUG_RSP_BMS_DATA       = 0x93
DEBUG_RSP_SYSTEM_INFO    = 0x94
DEBUG_RSP_RAW_CAN_TX     = 0x95
DEBUG_RSP_ERROR          = 0x96
DEBUG_RSP_CHARGE_CFG     = 0x97

PC_SOF1 = 0xAA
PC_SOF2 = 0x55


# ═══════════════════════════════════════════════════════
# 1. Command ID consistency between .h and .c
# ═══════════════════════════════════════════════════════

def test_debug_cmd_ids_in_header():
    h = read("App/Protocol/pc_debug_protocol.h")
    for name, val in [
        ("DEBUG_CMD_ENTER", 0x10), ("DEBUG_CMD_EXIT", 0x11),
        ("DEBUG_CMD_READ_ALL", 0x12), ("DEBUG_CMD_READ_ONE", 0x13),
        ("DEBUG_CMD_READ_STATS", 0x14), ("DEBUG_CMD_WRITE_REG", 0x15),
        ("DEBUG_CMD_SEND_RAW_CAN", 0x16), ("DEBUG_CMD_READ_BMS", 0x17),
        ("DEBUG_CMD_GET_SYSTEM", 0x18), ("DEBUG_CMD_GET_CHARGE_CFG", 0x19),
        ("DEBUG_CMD_SET_CHARGE_CFG", 0x1A),
    ]:
        assert f"#define {name}" in h, f"{name} missing from header"
        # Check hex value (case-insensitive — header may use 0x1A or 0x1a)
        assert hex(val).lower() in h.lower(), f"{name}={hex(val)} value mismatch"

def test_debug_rsp_ids_in_header():
    h = read("App/Protocol/pc_debug_protocol.h")
    for name, val in [
        ("DEBUG_RSP_MODULE_DATA", 0x90), ("DEBUG_RSP_ALL_MODULES", 0x91),
        ("DEBUG_RSP_COMM_STATS", 0x92), ("DEBUG_RSP_BMS_DATA", 0x93),
        ("DEBUG_RSP_SYSTEM_INFO", 0x94), ("DEBUG_RSP_RAW_CAN_TX", 0x95),
        ("DEBUG_RSP_ERROR", 0x96), ("DEBUG_RSP_CHARGE_CFG", 0x97),
    ]:
        assert f"#define {name}" in h, f"{name} missing from header"

def test_cmd_range_in_process_frame():
    """RX parser queues frames and main-loop processing dispatches debug commands"""
    c = read("App/Protocol/pc_protocol.c")
    assert "DEBUG_CMD_ENTER" in c
    assert "DEBUG_CMD_SET_CHARGE_CFG" in c
    assert "DebugProtocol_HandleCommand" in c
    assert "void PC_Protocol_ProcessRx" in c
    assert "enqueue_rx_frame" in c


# ═══════════════════════════════════════════════════════
# 2. Frame format validation
# ═══════════════════════════════════════════════════════

def test_frame_format_get_charge_cfg():
    """GET_CHARGE_CFG: [AA 55 19 00 CRC] — 0-byte payload"""
    frame = build_frame(DEBUG_CMD_GET_CHARGE_CFG, b'')
    assert frame[0] == 0xAA
    assert frame[1] == 0x55
    assert frame[2] == 0x19
    assert frame[3] == 0x00  # len=0
    assert len(frame) == 5   # SOF(2) + CMD + LEN + CRC
    body = frame[2:4]
    assert crc8(body) == frame[4]

def test_frame_format_set_charge_cfg():
    """SET_CHARGE_CFG: [AA 55 1A 07F ...CRC] — 207-byte payload"""
    payload = bytes(range(207))  # mock config data
    frame = build_frame(DEBUG_CMD_SET_CHARGE_CFG, payload)
    assert frame[2] == 0x1A
    assert frame[3] == 207
    assert len(frame) == 2 + 2 + 207 + 1  # 212 bytes total
    body = frame[2:2+2+207]
    assert crc8(body) == frame[2+2+207]

def test_frame_crc_detection():
    """Modified payload must fail CRC"""
    frame = build_frame(DEBUG_CMD_GET_CHARGE_CFG, b'')
    bad = bytearray(frame)
    bad[2] ^= 0xFF  # corrupt CMD
    body = bad[2:4]
    assert crc8(bytes(body)) != bad[4]


# ═══════════════════════════════════════════════════════
# 3. FeedByte FSM simulator — end-to-end parse
# ═══════════════════════════════════════════════════════

class FeedByteSim:
    """Replicates pc_protocol.c FeedByte state machine in Python"""
    ST_SOF1, ST_SOF2, ST_CMD, ST_LEN, ST_PAYLOAD, ST_CRC = range(6)

    def __init__(self):
        self.reset()
        self.parsed_frames = []

    def reset(self):
        self.state = self.ST_SOF1
        self.rx_cmd = 0
        self.rx_len = 0
        self.rx_idx = 0
        self.rx_payload = bytearray(256)

    def feed(self, byte: int) -> bool:
        """Returns True when a complete frame is parsed"""
        if self.state == self.ST_SOF1:
            if byte == PC_SOF1:
                self.state = self.ST_SOF2
        elif self.state == self.ST_SOF2:
            if byte == PC_SOF2:
                self.state = self.ST_CMD
            elif byte == PC_SOF1:
                self.state = self.ST_SOF2  # AA AA 55 re-sync
            else:
                self.state = self.ST_SOF1
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
            body = bytes([self.rx_cmd, self.rx_len]) + bytes(self.rx_payload[:self.rx_len])
            ok = crc8(body) == byte
            if ok:
                self.parsed_frames.append((
                    self.rx_cmd,
                    bytes(self.rx_payload[:self.rx_len])
                ))
            self.state = self.ST_SOF1
            return ok
        return False

def test_feedbyte_parse_get_charge_cfg():
    sim = FeedByteSim()
    frame = build_frame(DEBUG_CMD_GET_CHARGE_CFG, b'')
    for b in frame:
        sim.feed(b)
    assert len(sim.parsed_frames) == 1
    cmd, payload = sim.parsed_frames[0]
    assert cmd == 0x19
    assert len(payload) == 0

def test_feedbyte_parse_set_charge_cfg():
    sim = FeedByteSim()
    config_data = struct.pack('<H', 3) + b'\x00' * 205  # version=3 + 205 bytes
    frame = build_frame(DEBUG_CMD_SET_CHARGE_CFG, config_data)
    for b in frame:
        sim.feed(b)
    assert len(sim.parsed_frames) == 1
    cmd, payload = sim.parsed_frames[0]
    assert cmd == 0x1A
    assert len(payload) == 207

def test_feedbyte_sof_resync_aa55():
    """AA AA 55 should sync correctly (second AA treated as new SOF1)"""
    sim = FeedByteSim()
    # Send garbage then AA AA 55 19 00 CRC
    for b in [0xFF, 0xFE, 0xAA, 0xAA, 0x55, 0x19, 0x00]:
        sim.feed(b)
    frame = build_frame(DEBUG_CMD_GET_CHARGE_CFG, b'')
    for b in frame[4:]:  # remaining CRC
        result = sim.feed(b)
    assert len(sim.parsed_frames) == 1
    assert sim.parsed_frames[0][0] == 0x19

def test_feedbyte_bad_crc_rejected():
    sim = FeedByteSim()
    frame = bytearray(build_frame(DEBUG_CMD_GET_CHARGE_CFG, b''))
    frame[-1] ^= 0xFF  # corrupt CRC
    for b in frame:
        sim.feed(b)
    assert len(sim.parsed_frames) == 0

def test_feedbyte_burst_100_frames():
    """100 back-to-back GET_CHARGE_CFG frames must all parse"""
    sim = FeedByteSim()
    for _ in range(100):
        frame = build_frame(DEBUG_CMD_GET_CHARGE_CFG, b'')
        for b in frame:
            sim.feed(b)
    assert len(sim.parsed_frames) == 100
    assert all(cmd == 0x19 for cmd, _ in sim.parsed_frames)


# ═══════════════════════════════════════════════════════
# 4. Config struct size & field validation
# ═══════════════════════════════════════════════════════

def test_charge_cycle_config_size():
    """ChargeCycleConfig_t must be exactly 243 bytes (v6)."""
    h = read("App/Charge/charge_cycle_config.h")
    assert "_Static_assert(sizeof(ChargeCycleConfig_t) == 243" in h
    assert "uint32_t admin_pin" in h

def test_debug_module_data_size():
    """DebugModuleData_t must be 123 bytes"""
    h = read("App/Protocol/pc_debug_protocol.h")
    assert "_Static_assert(sizeof(DebugModuleData_t) == 123" in h

def test_debug_system_info_size():
    """DebugSystemInfo_t must be 72 bytes"""
    h = read("App/Protocol/pc_debug_protocol.h")
    assert "_Static_assert(sizeof(DebugSystemInfo_t) == 72" in h

def test_charge_config_has_version_field():
    h = read("App/Charge/charge_cycle_config.h")
    assert "uint16_t version" in h
    assert "CHARGE_CYCLE_CONFIG_VERSION" in h

def test_charge_config_packed():
    h = read("App/Charge/charge_cycle_config.h")
    assert "__attribute__((packed))" in h


# ═══════════════════════════════════════════════════════
# 5. Response payload — GET_CHARGE_CFG response path
# ═══════════════════════════════════════════════════════

def test_build_charge_config_calls_get():
    c = read("App/Protocol/pc_debug_protocol.c")
    assert "ChargeCycleConfig_Get(&config)" in c

def test_build_charge_config_version_override():
    c = read("App/Protocol/pc_debug_protocol.c")
    assert "config.version = CHARGE_CYCLE_CONFIG_VERSION" in c

def test_build_charge_config_memcpy_size():
    c = read("App/Protocol/pc_debug_protocol.c")
    assert "memcpy(data, &config, sizeof(config))" in c

def test_get_charge_cfg_response_cmd():
    c = read("App/Protocol/pc_debug_protocol.c")
    assert "DEBUG_RSP_CHARGE_CFG" in c

def test_set_charge_cfg_validates_length():
    c = read("App/Protocol/pc_debug_protocol.c")
    # SET_CHARGE_CFG must reject wrong payload length
    assert "len != sizeof(config)" in c

def test_set_charge_cfg_saves_flash():
    c = read("App/Protocol/pc_debug_protocol.c")
    assert "ChargeCycleStorage_Save" in c


# ═══════════════════════════════════════════════════════
# 6. ISR safety — no LOG in ISR path
# ═══════════════════════════════════════════════════════

def test_no_log_in_debug_handler():
    """DebugProtocol_HandleCommand must not call LOG during protocol dispatch"""
    c = read("App/Protocol/pc_debug_protocol.c")
    # Find the function body
    start = c.find("bool DebugProtocol_HandleCommand")
    assert start >= 0
    # Find the closing brace (scan forward, count braces)
    depth = 0
    in_func = False
    end = start
    for i in range(start, len(c)):
        if c[i] == '{':
            depth += 1
            in_func = True
        elif c[i] == '}':
            depth -= 1
            if in_func and depth == 0:
                end = i
                break
    func_body = c[start:end+1]
    # Must NOT contain LOG( calls
    assert "LOG(" not in func_body, \
        f"LOG found in DebugProtocol_HandleCommand — blocks USB ISR!\n{func_body[:200]}"

def test_no_log_in_debug_enter_exit():
    """DebugProtocol_Enter/Exit must not call LOG (runs in USB ISR)"""
    c = read("App/Protocol/pc_debug_protocol.c")
    for func_name in ["DebugProtocol_Enter", "DebugProtocol_Exit"]:
        start = c.find(f"void {func_name}(void)")
        assert start >= 0, f"{func_name} not found"
        depth = 0
        in_func = False
        end = start
        for i in range(start, len(c)):
            if c[i] == '{':
                depth += 1
                in_func = True
            elif c[i] == '}':
                depth -= 1
                if in_func and depth == 0:
                    end = i
                    break
        func_body = c[start:end+1]
        assert "LOG(" not in func_body, \
            f"LOG found in {func_name} — blocks USB ISR!"

def test_no_log_in_enqueue_frame():
    """enqueue_frame must not call LOG (may be called from ISR)"""
    c = read("App/Protocol/pc_protocol.c")
    start = c.find("static bool enqueue_frame")
    assert start >= 0
    depth = 0
    in_func = False
    end = start
    for i in range(start, len(c)):
        if c[i] == '{':
            depth += 1
            in_func = True
        elif c[i] == '}':
            depth -= 1
            if in_func and depth == 0:
                end = i
                break
    func_body = c[start:end+1]
    assert "LOG(" not in func_body, \
        f"LOG found in enqueue_frame — blocks USB ISR!"


# ═══════════════════════════════════════════════════════
# 7. TX queue management
# ═══════════════════════════════════════════════════════

def test_tx_queue_depth():
    c = read("App/Protocol/pc_protocol.c")
    assert "PC_TX_QUEUE_DEPTH" in c

def test_enqueue_drops_when_in_flight():
    """enqueue_frame must not drop frame if g_tx_in_flight is true"""
    c = read("App/Protocol/pc_protocol.c")
    # Find enqueue_frame and check for in_flight guard
    start = c.find("static bool enqueue_frame")
    assert start >= 0
    func_body = c[start:start+800]
    assert "g_tx_in_flight" in func_body
    assert "return false" in func_body

def test_process_tx_has_timeout():
    """ProcessTx must have a timeout to recover from stuck TX"""
    c = read("App/Protocol/pc_protocol.c")
    assert "last_tx_start" in c
    assert "timeout_ms" in c or "Timeout" in c

def test_notify_tx_complete_advances_head():
    c = read("App/Protocol/pc_protocol.c")
    start = c.find("void PC_Protocol_NotifyTxComplete")
    assert start >= 0
    func_body = c[start:start+400]
    assert "g_tx_head" in func_body
    assert "g_tx_count--" in func_body
    assert "g_tx_in_flight = 0U" in func_body

def test_process_tx_large_frame_timeout():
    """Timeout must scale with frame size for 207-byte config responses"""
    c = read("App/Protocol/pc_protocol.c")
    assert "frame_len" in c or "len / 8" in c or "len/8" in c


# ═══════════════════════════════════════════════════════
# 8. USB CDC TX path
# ═══════════════════════════════════════════════════════

def test_cdc_transmit_fs_exists():
    c = read("USB_Device/App/usbd_cdc_if.c")
    assert "CDC_Transmit_FS" in c
    assert "USBD_CDC_TransmitPacket" in c

def test_cdc_transmit_checks_tx_state():
    c = read("USB_Device/App/usbd_cdc_if.c")
    assert "TxState" in c
    assert "USBD_BUSY" in c

def test_cdc_transmitcplt_calls_notify():
    c = read("USB_Device/App/usbd_cdc_if.c")
    assert "PC_Protocol_NotifyTxComplete" in c

def test_cdc_receive_calls_feedbyte():
    c = read("USB_Device/App/usbd_cdc_if.c")
    assert "PC_Protocol_FeedByte" in c


# ═══════════════════════════════════════════════════════
# 9. Simulated MCU response — end-to-end protocol test
# ═══════════════════════════════════════════════════════

def test_simulated_get_charge_cfg_response():
    """
    Simulate full PC→MCU→PC exchange:
    1. PC builds GET_CHARGE_CFG frame
    2. FeedByteSim parses it
    3. MCU builds response (207-byte config + version)
    4. FeedByteSim parses response
    """
    # Step 1: PC sends GET_CHARGE_CFG
    pc_frame = build_frame(DEBUG_CMD_GET_CHARGE_CFG, b'')

    # Step 2: MCU parses
    mcu_sim = FeedByteSim()
    for b in pc_frame:
        mcu_sim.feed(b)
    assert len(mcu_sim.parsed_frames) == 1
    assert mcu_sim.parsed_frames[0][0] == DEBUG_CMD_GET_CHARGE_CFG

    # Step 3: MCU builds 207-byte config response
    mock_config = struct.pack('<H', 3)  # version=3
    mock_config += struct.pack('<f', 100.0)  # battery_capacity_ah
    mock_config += b'\x00' * (207 - len(mock_config))  # pad to 207
    assert len(mock_config) == 207

    mcu_response = build_frame(DEBUG_RSP_CHARGE_CFG, mock_config)
    assert len(mcu_response) == 2 + 2 + 207 + 1  # 212 bytes

    # Step 4: PC parses response
    pc_sim = FeedByteSim()
    for b in mcu_response:
        pc_sim.feed(b)
    assert len(pc_sim.parsed_frames) == 1
    cmd, payload = pc_sim.parsed_frames[0]
    assert cmd == DEBUG_RSP_CHARGE_CFG
    assert len(payload) == 207
    # Verify version field
    version = struct.unpack('<H', payload[0:2])[0]
    assert version == 3

def test_simulated_set_charge_cfg_roundtrip():
    """
    PC sends SET_CHARGE_CFG → MCU validates → MCU responds with saved config
    """
    # Build 207-byte config payload
    config_data = struct.pack('<H', 3)  # version
    config_data += struct.pack('<f', 200.0)  # battery_capacity_ah
    config_data += struct.pack('<f', 10.0)   # imin_c
    config_data += struct.pack('<f', 50.0)   # imax_c
    config_data += b'\x00' * (207 - len(config_data))
    assert len(config_data) == 207

    # PC sends
    pc_frame = build_frame(DEBUG_CMD_SET_CHARGE_CFG, config_data)
    assert len(pc_frame) == 212

    # MCU parses
    mcu_sim = FeedByteSim()
    for b in pc_frame:
        mcu_sim.feed(b)
    assert len(mcu_sim.parsed_frames) == 1
    cmd, payload = mcu_sim.parsed_frames[0]
    assert cmd == DEBUG_CMD_SET_CHARGE_CFG
    assert len(payload) == 207

    # MCU responds with same config (echo after save)
    mcu_response = build_frame(DEBUG_RSP_CHARGE_CFG, payload)
    pc_sim = FeedByteSim()
    for b in mcu_response:
        pc_sim.feed(b)
    assert len(pc_sim.parsed_frames) == 1
    assert pc_sim.parsed_frames[0][0] == DEBUG_RSP_CHARGE_CFG
    assert len(pc_sim.parsed_frames[0][1]) == 207

def test_simulated_error_response():
    """MCU sends NACK/ERROR → PC parses error"""
    error_payload = bytes([0x01, 0x03])  # cmd=GET_CHARGE_CFG, err=BUFFER
    mcu_response = build_frame(DEBUG_RSP_ERROR, error_payload)
    pc_sim = FeedByteSim()
    for b in mcu_response:
        pc_sim.feed(b)
    assert len(pc_sim.parsed_frames) == 1
    cmd, payload = pc_sim.parsed_frames[0]
    assert cmd == DEBUG_RSP_ERROR
    assert payload[0] == 0x01  # BAD_PARAM


# ═══════════════════════════════════════════════════════
# 10. Multi-command burst stress test
# ═══════════════════════════════════════════════════════

def test_mixed_command_burst():
    """Parse mixed debug commands without losing frames"""
    sim = FeedByteSim()
    commands = [
        DEBUG_CMD_ENTER,
        DEBUG_CMD_READ_ALL,
        DEBUG_CMD_GET_SYSTEM,
        DEBUG_CMD_GET_CHARGE_CFG,
        DEBUG_CMD_READ_BMS,
        DEBUG_CMD_EXIT,
    ]
    for cmd in commands:
        frame = build_frame(cmd, b'')
        for b in frame:
            sim.feed(b)
    assert len(sim.parsed_frames) == len(commands)
    for i, (cmd, _) in enumerate(sim.parsed_frames):
        assert cmd == commands[i]

def test_large_payload_burst():
    """Parse SET_CHARGE_CFG (207B) x5 without losing frames"""
    sim = FeedByteSim()
    config_data = b'\x00' * 207
    for _ in range(5):
        frame = build_frame(DEBUG_CMD_SET_CHARGE_CFG, config_data)
        for b in frame:
            sim.feed(b)
    assert len(sim.parsed_frames) == 5
    assert all(cmd == DEBUG_CMD_SET_CHARGE_CFG for cmd, _ in sim.parsed_frames)
    assert all(len(p) == 207 for _, p in sim.parsed_frames)
