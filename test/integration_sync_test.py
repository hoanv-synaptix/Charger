#!/usr/bin/env python3
"""
Real hardware-in-the-loop (HIL) smoke test.

Requires actual hardware connected -- NOT part of the host-buildable test
suite (test/host_charge_sim, test/host_protocol_sim, test/test_logic.c) that
run_all_local.bat drives automatically. Run this manually only when the
following are physically connected:
  - The MCU, flashed with the firmware under test, USB CDC connected (its
    debug protocol port -- Windows enumerates it as VID:PID 0483:5740).
  - A ZLG USBCAN-II-compatible adapter (2 CAN channels), with channel 0
    wired to the MCU's CAN1 bus (charger modules, 125 kbps) and channel 1
    wired to the MCU's CAN2 bus (BMS, 250 kbps).

What it does: simulates BOTH the BMS (CAN2) and the currently-configured
charger module (CAN1) from the PC side, using the real, verified byte
layouts this session already exercises in the host-buildable simulators
(test/host_charge_sim/sim_bms.c, sim_can_modules.c) -- not reinvented here,
copied to match. Drives the real firmware over its real USB debug protocol
(App/Protocol/pc_debug_protocol.c) and asserts it reaches RUNNING, then
returns it to a clean IDLE before exiting.

Covers all 3 drivers (Maxwell/Lianming/TonHe). Pass one or more driver names
on the command line, or "all" to run all three back-to-back:
    python test/integration_sync_test.py                  # tonhe (default)
    python test/integration_sync_test.py maxwell
    python test/integration_sync_test.py maxwell lianming
    python test/integration_sync_test.py all

Testing a driver other than the one currently configured on the unit sends
a real PC_CMD_SET_DRIVER + PC_CMD_SET_MODULE_ADDR over the wire first --
this PERSISTS to the unit's flash (see PC_CMD_SET_DRIVER's handler in
pc_protocol.c), so this script always restores whatever driver/module addr
it found configured at the start once done, best-effort.

Driver requirement (Windows): the ZLG "USBCAN 2I"-class adapter needs its
vendor driver installed (ZLG/Zhiyuan's classic ControlCAN USBCAN driver
package -- installs usbcan_x64.dll). This talks to that DLL directly via
ctypes (the pip `zlgcan` package needs a different, newer unified
zlgcan.dll this project's hardware doesn't have installed). Set
ZLG_USBCAN_DLL_PATH to override the default install path if yours differs.

Setup:
    pip install pyserial
"""
import ctypes
from ctypes import wintypes as W
import os
import struct
import sys
import threading
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("[FAIL] pyserial not installed -- run: pip install pyserial")
    sys.exit(1)

# ============================================================= #
# ZLG classic ControlCAN (usbcan_x64.dll) ctypes wrapper         #
# ============================================================= #

DEFAULT_DLL_PATH = r"C:\Program Files (x86)\ZHIYUAN USBCAN Driver\xp_win7_win8\x64\usbcan_x64.dll"
DLL_PATH = os.environ.get("ZLG_USBCAN_DLL_PATH", DEFAULT_DLL_PATH)

VCI_USBCAN2 = 4  # classic dual-channel USBCAN-II device type

# Standard SJA1000 @ 16MHz BTR0/BTR1 timing table (widely published, not
# vendor-specific -- the same values PEAK/ZLG/etc. all document).
BITRATE_TIMING = {
    125000: (0x03, 0x1C),
    250000: (0x01, 0x1C),
    500000: (0x00, 0x1C),
    1000000: (0x00, 0x14),
}


class _VciInitConfig(ctypes.Structure):
    _fields_ = [
        ("AccCode", W.DWORD), ("AccMask", W.DWORD), ("Reserved", W.DWORD),
        ("Filter", ctypes.c_ubyte), ("Timing0", ctypes.c_ubyte),
        ("Timing1", ctypes.c_ubyte), ("Mode", ctypes.c_ubyte),
    ]


class _VciCanObj(ctypes.Structure):
    _fields_ = [
        ("ID", ctypes.c_uint), ("TimeStamp", ctypes.c_uint),
        ("TimeFlag", ctypes.c_ubyte), ("SendType", ctypes.c_ubyte),
        ("RemoteFlag", ctypes.c_ubyte), ("ExternFlag", ctypes.c_ubyte),
        ("DataLen", ctypes.c_ubyte), ("Data", ctypes.c_ubyte * 8),
        ("Reserved", ctypes.c_ubyte * 3),
    ]


class _VciBoardInfo(ctypes.Structure):
    _fields_ = [
        ("hw_Version", ctypes.c_ushort), ("fw_Version", ctypes.c_ushort),
        ("dr_Version", ctypes.c_ushort), ("in_Version", ctypes.c_ushort),
        ("irq_Num", ctypes.c_ushort), ("can_Num", ctypes.c_ubyte),
        ("str_Serial_Num", ctypes.c_char * 20), ("str_hw_Type", ctypes.c_char * 40),
        ("Reserved", ctypes.c_ushort * 4),
    ]


class ZlgVci:
    """Minimal wrapper: open device, init+start a channel, transmit/receive.
    Covers only what this test needs -- not a general CAN library."""

    def __init__(self, device_index: int = 0, device_type: int = VCI_USBCAN2):
        if not os.path.isfile(DLL_PATH):
            raise FileNotFoundError(
                f"ZLG USBCAN driver DLL not found at {DLL_PATH!r}. "
                "Install the vendor driver, or set ZLG_USBCAN_DLL_PATH to its "
                "usbcan_x64.dll (must be the 64-bit build -- some vendor zips "
                "ship a 32-bit usbcan.dll even in an 'x64' folder; verify the "
                "PE header if VCI_OpenDevice mysteriously fails)."
            )
        self.dll = ctypes.WinDLL(DLL_PATH)
        self.dll.VCI_Receive.restype = ctypes.c_ulong
        self.device_type = device_type
        self.device_index = device_index
        self._opened = False

    def open(self):
        ret = self.dll.VCI_OpenDevice(self.device_type, self.device_index, 0)
        if ret != 1:
            raise RuntimeError(f"VCI_OpenDevice failed (ret={ret}) -- is the adapter plugged in?")
        self._opened = True

    def board_info(self) -> _VciBoardInfo:
        info = _VciBoardInfo()
        self.dll.VCI_ReadBoardInfo(self.device_type, self.device_index, ctypes.byref(info))
        return info

    def init_channel(self, can_index: int, bitrate: int, listen_only: bool = False):
        if bitrate not in BITRATE_TIMING:
            raise ValueError(f"unsupported bitrate {bitrate}")
        t0, t1 = BITRATE_TIMING[bitrate]
        cfg = _VciInitConfig(AccCode=0, AccMask=0xFFFFFFFF, Reserved=0, Filter=1,
                              Timing0=t0, Timing1=t1, Mode=1 if listen_only else 0)
        if self.dll.VCI_InitCAN(self.device_type, self.device_index, can_index, ctypes.byref(cfg)) != 1:
            raise RuntimeError(f"VCI_InitCAN(channel={can_index}) failed")
        if self.dll.VCI_StartCAN(self.device_type, self.device_index, can_index) != 1:
            raise RuntimeError(f"VCI_StartCAN(channel={can_index}) failed")

    def transmit(self, can_index: int, can_id: int, data: bytes, extended: bool = False) -> bool:
        obj = _VciCanObj()
        obj.ID = can_id
        obj.ExternFlag = 1 if extended else 0
        obj.DataLen = len(data)
        for i, b in enumerate(data):
            obj.Data[i] = b
        return self.dll.VCI_Transmit(self.device_type, self.device_index, can_index, ctypes.byref(obj), 1) == 1

    def receive(self, can_index: int, max_count: int = 16, wait_ms: int = 0):
        buf = (_VciCanObj * max_count)()
        n = self.dll.VCI_Receive(self.device_type, self.device_index, can_index,
                                  ctypes.byref(buf), max_count, wait_ms)
        return [(buf[i].ID, bool(buf[i].ExternFlag), bytes(buf[i].Data[:buf[i].DataLen])) for i in range(n)]

    def close(self):
        if self._opened:
            self.dll.VCI_CloseDevice(self.device_type, self.device_index)
            self._opened = False

    def __enter__(self):
        self.open()
        return self

    def __exit__(self, *exc):
        self.close()


# ============================================================= #
# PC debug protocol framing -- App/Protocol/pc_debug_protocol.h  #
# ============================================================= #

PC_SOF1, PC_SOF2 = 0xAA, 0x55
DEBUG_CMD_ENTER = 0x10
DEBUG_CMD_GET_SYSTEM = 0x18
DEBUG_RSP_SYSTEM_INFO = 0x94
PC_CMD_START = 0x03
PC_CMD_STOP = 0x04
PC_CMD_SET_MODULE_ADDR = 0x05
PC_CMD_SET_DRIVER = 0x09
PC_RSP_ACK = 0x82
PC_RSP_NACK = 0x83

# DebugSystemInfo_t (packed, 72 bytes) -- App/Protocol/pc_debug_protocol.h
SYSTEM_INFO_FMT = "<14B7f6I I 2B"
SYSTEM_INFO_KEYS = [
    "fw_major", "fw_minor", "fw_patch", "driver_id", "modules_total",
    "modules_online", "modules_fault", "charging", "controller_state",
    "controller_derating", "controller_inhibit", "charge_source_mode",
    "active_limit_source", "active_stage_band",
    "total_voltage", "total_current", "total_power_in", "max_temp_dcdc",
    "controller_target_voltage", "controller_target_current_total", "active_limit_current_c",
    "uptime_ticks", "can1_tx_count", "can1_rx_count", "can2_tx_count", "can2_rx_count",
    "can_reserved_or_err",
    "controller_fault_flags", "controller_stop_reason", "bms_stale",
]
CHARGE_CTRL_STATE_RUNNING = 2
CHARGE_CTRL_STATE_IDLE = 0


def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


def build_frame(cmd: int, payload: bytes = b"") -> bytes:
    frame = bytearray([PC_SOF1, PC_SOF2, cmd, len(payload)])
    frame.extend(payload)
    frame.append(crc8(frame[2:]))
    return bytes(frame)


def read_frame(ser: "serial.Serial", timeout_s: float = 2.0):
    """Resyncs on stray bytes (e.g. LOG() banner text) the same way a real
    PC app's parser must -- matches PC_Protocol_FeedByte()'s own SOF
    resync behavior (see test_pc_protocol_e2e.c's send_pc_frame helpers)."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        b = ser.read(1)
        if not b or b[0] != PC_SOF1:
            continue
        b2 = ser.read(1)
        if not b2 or b2[0] != PC_SOF2:
            continue
        hdr = ser.read(2)
        if len(hdr) < 2:
            continue
        cmd, length = hdr[0], hdr[1]
        payload = ser.read(length) if length > 0 else b""
        crc = ser.read(1)
        if len(payload) < length or len(crc) < 1:
            continue
        return cmd, payload
    return None, None


# Once DEBUG_CMD_ENTER is sent, the firmware's DebugProtocol_SendStream()
# starts pushing DEBUG_RSP_ALL_MODULES/SYSTEM_INFO/BMS_DATA unsolicited
# every DEBUG_STREAM_INTERVAL_MS (1000ms) -- see App/Protocol/
# pc_debug_protocol.c. A plain read_frame() after sending e.g. PC_CMD_START
# can pick up one of THESE instead of the real ACK/NACK for the command
# just sent, especially once a scenario has been running for a few
# seconds. read_reply() skips exactly that class of frame and keeps
# reading until it gets one of the caller's expected response codes (or
# times out) -- required for every command/ACK exchange in this file,
# not just GET_SYSTEM (which tolerates it because any SYSTEM_INFO frame,
# streamed or requested, carries the same up-to-date fields).
def read_reply(ser: "serial.Serial", expected_cmds, timeout_s: float = 2.0):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        remaining = max(0.0, deadline - time.time())
        cmd, payload = read_frame(ser, timeout_s=remaining)
        if cmd is None:
            return None, None
        if cmd in expected_cmds:
            return cmd, payload
        # Not what we asked for -- most commonly an unsolicited debug
        # stream frame, but keep waiting either way rather than returning
        # a mismatched frame as if it were the reply.
    return None, None


def find_mcu_port():
    for p in serial.tools.list_ports.comports():
        if p.vid == 0x0483 and p.pid == 0x5740:
            return p.device
    return None


def get_system_info(ser):
    ser.reset_input_buffer()
    ser.write(build_frame(DEBUG_CMD_GET_SYSTEM))
    cmd, payload = read_reply(ser, {DEBUG_RSP_SYSTEM_INFO})
    if cmd is None or len(payload) != struct.calcsize(SYSTEM_INFO_FMT):
        return None
    return dict(zip(SYSTEM_INFO_KEYS, struct.unpack(SYSTEM_INFO_FMT, payload)))


# ============================================================= #
# BMS simulator (CAN2, real byte layouts -- copied from            #
# test/host_charge_sim/sim_bms.c, ground truth: Modules/bms/       #
# bms_protocol.c)                                                  #
# ============================================================= #

class BmsSim(threading.Thread):
    CAN_CHANNEL = 1
    CAN_BITRATE = 250000

    def __init__(self, dev: ZlgVci):
        super().__init__(daemon=True)
        self.dev = dev
        self.running = True
        self.pack_voltage_v = 400.0
        self.pack_current_a = 0.0
        self.soc_pct = 50
        self.max_cell_mv = 3000
        self.max_cv_no = 1
        self.min_cell_mv = 2980
        self.min_cv_no = 2
        self.max_cell_temp_c = 25.0
        self.min_cell_temp_c = 24.0
        self.avg_cell_temp_c = 24.5
        self.max_ct_no = 1
        self.min_ct_no = 2
        self.cap_remain_x0_1ah = 500
        self.rate_cap_x0_1ah = 1000
        self.cycle_count = 10
        self.soh_pct = 100
        self.chg_volt_request_v = 500.0
        self.chg_curr_request_a = 50.0
        self.bms_relay_allow = True

    @staticmethod
    def _u16le(v):
        return struct.pack("<H", v & 0xFFFF)

    @staticmethod
    def _u16be(v):
        return struct.pack(">H", v & 0xFFFF)

    def send_batt_st1(self):
        raw_v = int(self.pack_voltage_v * 10.0)
        raw_c = int((self.pack_current_a + 400.0) * 10.0)
        d = self._u16le(raw_v) + self._u16le(raw_c) + bytes([self.soc_pct, 0, 0, 0])
        self.dev.transmit(self.CAN_CHANNEL, 0x02F4, d, extended=False)

    def send_cell_volt(self):
        d = (self._u16le(self.max_cell_mv) + bytes([self.max_cv_no]) +
             self._u16le(self.min_cell_mv) + bytes([self.min_cv_no, 0, 0]))
        self.dev.transmit(self.CAN_CHANNEL, 0x04F4, d, extended=False)

    def send_cell_temp(self):
        d = bytes([
            int(self.max_cell_temp_c + 50.0) & 0xFF, self.max_ct_no,
            int(self.min_cell_temp_c + 50.0) & 0xFF, self.min_ct_no,
            int(self.avg_cell_temp_c + 50.0) & 0xFF, 0, 0, 0,
        ])
        self.dev.transmit(self.CAN_CHANNEL, 0x05F4, d, extended=False)

    def send_alm_info(self):
        self.dev.transmit(self.CAN_CHANNEL, 0x07F4, bytes(8), extended=False)  # all clear

    def send_batt_st2(self):
        d = (self._u16le(self.cap_remain_x0_1ah) + self._u16le(self.rate_cap_x0_1ah) +
             self._u16le(self.cycle_count) + bytes([self.soh_pct, 0]))
        self.dev.transmit(self.CAN_CHANNEL, 0x18F128F4, d, extended=True)

    def send_chg_request(self):
        volt_req = int(self.chg_volt_request_v * 10.0)
        curr_req = int(self.chg_curr_request_a * 10.0)
        d = self._u16be(volt_req) + self._u16be(curr_req) + bytes(4)
        self.dev.transmit(self.CAN_CHANNEL, 0x1806E5F4, d, extended=True)

    def send_bms_sw_sta(self):
        b0 = (1 << 2) if self.bms_relay_allow else 0
        self.dev.transmit(self.CAN_CHANNEL, 0x18F528F4, bytes([b0, 0, 0, 0, 0, 0, 0, 0]), extended=True)

    def run(self):
        last = {"st1": 0, "cv": 0, "ct": 0, "alm": 0, "st2": 0, "chg": 0, "sw": 0}
        while self.running:
            now = time.monotonic() * 1000.0
            if now - last["st1"] >= 20:
                self.send_batt_st1(); last["st1"] = now
            if now - last["cv"] >= 100:
                self.send_cell_volt(); last["cv"] = now
            if now - last["ct"] >= 500:
                self.send_cell_temp(); last["ct"] = now
            if now - last["alm"] >= 500:
                self.send_alm_info(); last["alm"] = now
            if now - last["st2"] >= 100:
                self.send_batt_st2(); last["st2"] = now
            if now - last["chg"] >= 1000:
                self.send_chg_request(); last["chg"] = now
            if now - last["sw"] >= 500:
                self.send_bms_sw_sta(); last["sw"] = now
            time.sleep(0.01)


# ============================================================= #
# TonHe module simulator (CAN1, real byte layouts -- copied from   #
# test/host_charge_sim/sim_can_modules.c, ground truth:            #
# Modules/chg_lib/chg_lib_tonhe.c)                                 #
# ============================================================= #

class TonheModuleSim(threading.Thread):
    CAN_CHANNEL = 0
    CAN_BITRATE = 125000

    ADDR_CONTROLLER = 0xA0
    PRIORITY_STATUS = 6
    STATUS_NORMAL_OFF = 0x00
    STATUS_ON = 0x01
    CMD_STOP = 0x55
    CMD_START = 0xAA
    VOLTAGE_SCALE = 0.1
    CURRENT_SCALE = 0.01

    def __init__(self, dev: ZlgVci, addr: int, rated_current: float = 100.0):
        super().__init__(daemon=True)
        self.dev = dev
        self.addr = addr
        self.rated_current = rated_current
        self.running = True
        self.actually_on = False
        self.voltage = 0.0
        self.current = 0.0

    @staticmethod
    def _tonhe_id(pf, ps, sa, priority):
        return ((priority & 0x07) << 26) | ((pf & 0xFF) << 16) | ((ps & 0xFF) << 8) | (sa & 0xFF)

    def _handle_rx(self, can_id, data):
        pf = (can_id >> 16) & 0xFF
        ps = (can_id >> 8) & 0xFF
        if pf == 0x06 and ps == self.addr:
            # C_M_24: specific module start/stop
            self.actually_on = (data[0] == self.CMD_START)
            if not self.actually_on:
                self.voltage = 0.0
                self.current = 0.0
            elif self.voltage <= 0.0:
                self.voltage = 1.0
        elif pf == 0x03 and ps in (self.addr, 0xFF):
            # C_M_1: broadcast start/stop
            self.actually_on = (data[3] == self.CMD_START)
        # pf==0x04 (broadcast parameter set) / 0x05 (timing) -- not
        # simulated, matching sim_can_modules.c's own scope (the module
        # doesn't need to react to these for a RUNNING-transition smoke
        # test).

    def _broadcast_status(self):
        self.current = self.rated_current * 0.5 if self.actually_on else 0.0
        status = self.STATUS_ON if self.actually_on else self.STATUS_NORMAL_OFF
        voltage_raw = int(self.voltage / self.VOLTAGE_SCALE) & 0xFFFF
        current_raw = int(self.current / self.CURRENT_SCALE) & 0xFFFF
        data = bytes([
            status,
            voltage_raw & 0xFF, (voltage_raw >> 8) & 0xFF,
            current_raw & 0xFF, (current_raw >> 8) & 0xFF,
            0, 0,  # fault_bits = 0 (healthy)
            0,     # pfc_bits = 0
        ])
        can_id = self._tonhe_id(0x01, self.ADDR_CONTROLLER, self.addr, self.PRIORITY_STATUS)
        self.dev.transmit(self.CAN_CHANNEL, can_id, data, extended=True)

    def run(self):
        last_broadcast = 0
        while self.running:
            now = time.monotonic() * 1000.0
            for can_id, ext, data in self.dev.receive(self.CAN_CHANNEL, wait_ms=0):
                if ext and len(data) >= 4:
                    self._handle_rx(can_id, data)
            if now - last_broadcast >= 100:
                self._broadcast_status()
                last_broadcast = now
            time.sleep(0.01)


# ============================================================= #
# Maxwell module simulator (CAN1, real byte layouts -- copied from #
# test/host_charge_sim/sim_can_modules.c, ground truth:            #
# Modules/chg_lib/chg_lib_maxwell.c, register map in               #
# Modules/chg_lib/priv/chg_lib_protocol.h)                         #
# ============================================================= #

class MaxwellModuleSim(threading.Thread):
    CAN_CHANNEL = 0
    CAN_BITRATE = 125000

    PROTNO = 0x060
    PTP_POINT = 1
    ADDR_CONTROLLER = 0xF0
    FUNC_SET = 0x03
    FUNC_READ = 0x10
    RESP_FLOAT = 0x41
    RESP_INT = 0x42
    RESP_OK = 0xF0
    CMD_STOP_U32 = 0x00010000

    REG_VOLTAGE = 0x0001
    REG_CURRENT = 0x0002
    REG_CURR_LIMIT = 0x0003
    REG_TEMP_DCDC = 0x0004
    REG_PFC0_VOLTAGE = 0x0008
    REG_PFC1_VOLTAGE = 0x000A
    REG_TEMP_AMBIENT = 0x000B
    REG_AC_PHASE_A = 0x000C
    REG_AC_PHASE_B = 0x000D
    REG_AC_PHASE_C = 0x000E
    REG_TEMP_PFC = 0x0010
    REG_RATED_POWER = 0x0011
    REG_RATED_CURRENT = 0x0012
    REG_SET_VOLTAGE = 0x0021
    REG_SET_CURR_LIMIT = 0x0022
    REG_SET_OVP = 0x0023
    REG_ON_OFF = 0x0030
    REG_ALARM_STATUS = 0x0040
    REG_SHORT_RESET = 0x0044
    REG_INPUT_MODE_SET = 0x0046
    REG_INPUT_POWER = 0x0048
    # write-ack-only registers (reply value unused by apply_response())
    WRITE_ACK_REGS = {REG_SET_VOLTAGE, REG_SET_CURR_LIMIT, REG_SET_OVP, REG_ON_OFF,
                       REG_SHORT_RESET, REG_INPUT_MODE_SET}

    def __init__(self, dev: ZlgVci, addr: int, group: int = 0,
                 rated_current: float = 100.0, rated_power: float = 30000.0):
        super().__init__(daemon=True)
        self.dev = dev
        self.addr = addr
        self.group = group
        self.rated_current = rated_current
        self.rated_power = rated_power
        self.running = True
        self.actually_on = False
        self.voltage = 0.0
        self.current = 0.0
        self.pending = False
        self.pending_reg = 0

    @staticmethod
    def _mxr_id(dst_addr, src_addr, group):
        return ((MaxwellModuleSim.PROTNO & 0x1FF) << 20 | (MaxwellModuleSim.PTP_POINT & 1) << 19 |
                 (dst_addr & 0xFF) << 11 | (src_addr & 0xFF) << 3 | (group & 0x07))

    def _handle_rx(self, can_id, data):
        dst_addr = (can_id >> 11) & 0xFF
        if dst_addr != self.addr:
            return
        func = data[0]
        reg = (data[2] << 8) | data[3]
        if func == self.FUNC_SET:
            u = struct.unpack(">I", data[4:8])[0]
            f = struct.unpack(">f", data[4:8])[0]
            if reg == self.REG_SET_VOLTAGE:
                self.voltage = f
            elif reg == self.REG_SET_CURR_LIMIT:
                pass  # ratio captured off the wire only if a test needs it
            elif reg == self.REG_ON_OFF:
                self.actually_on = (u != self.CMD_STOP_U32)
                if not self.actually_on:
                    self.voltage = 0.0
                    self.current = 0.0
                elif self.voltage <= 0.0:
                    self.voltage = 1.0
        self.pending = True
        self.pending_reg = reg

    def _tick(self):
        if not self.pending:
            return
        self.pending = False
        resp = bytearray(8)
        resp[0] = self.RESP_FLOAT
        resp[1] = self.RESP_OK
        resp[2] = (self.pending_reg >> 8) & 0xFF
        resp[3] = self.pending_reg & 0xFF

        if self.actually_on:
            self.current = self.rated_current * 0.5

        reg = self.pending_reg
        if reg == self.REG_VOLTAGE:
            resp[4:8] = struct.pack(">f", self.voltage)
        elif reg == self.REG_CURRENT:
            resp[4:8] = struct.pack(">f", self.current)
        elif reg == self.REG_CURR_LIMIT:
            resp[4:8] = struct.pack(">f", 1.0)
        elif reg == self.REG_TEMP_DCDC:
            resp[4:8] = struct.pack(">f", 35.0)
        elif reg == self.REG_TEMP_AMBIENT:
            resp[4:8] = struct.pack(">f", 28.0)
        elif reg == self.REG_TEMP_PFC:
            resp[4:8] = struct.pack(">f", 40.0)
        elif reg == self.REG_PFC0_VOLTAGE:
            resp[4:8] = struct.pack(">f", 400.0)
        elif reg == self.REG_PFC1_VOLTAGE:
            resp[4:8] = struct.pack(">f", -400.0)
        elif reg == self.REG_RATED_POWER:
            resp[4:8] = struct.pack(">f", self.rated_power)
        elif reg == self.REG_RATED_CURRENT:
            resp[4:8] = struct.pack(">f", self.rated_current)
        elif reg in (self.REG_AC_PHASE_A, self.REG_AC_PHASE_B, self.REG_AC_PHASE_C):
            resp[4:8] = struct.pack(">f", 230.0)
        elif reg == self.REG_ALARM_STATUS:
            resp[0] = self.RESP_INT
            resp[4:8] = struct.pack(">I", 0)  # healthy
        elif reg == self.REG_INPUT_POWER:
            resp[0] = self.RESP_INT
            resp[4:8] = struct.pack(">I", int(self.voltage * self.current) & 0xFFFFFFFF)
        elif reg in self.WRITE_ACK_REGS:
            pass  # resp[4:8] already zeroed
        else:
            return  # unknown register: no reply, matches a real module ignoring it

        resp_id = self._mxr_id(self.ADDR_CONTROLLER, self.addr, self.group)
        self.dev.transmit(self.CAN_CHANNEL, resp_id, bytes(resp), extended=True)

    def run(self):
        while self.running:
            for can_id, ext, data in self.dev.receive(self.CAN_CHANNEL, wait_ms=0):
                if ext and len(data) >= 8:
                    self._handle_rx(can_id, data)
            self._tick()
            time.sleep(0.01)


# ============================================================= #
# Lianming module simulator (CAN1, real byte layouts -- copied     #
# from test/host_charge_sim/sim_can_modules.c, ground truth:       #
# Modules/chg_lib/chg_lib_lianming.c)                              #
# ============================================================= #

class LianmingModuleSim(threading.Thread):
    CAN_CHANNEL = 0
    CAN_BITRATE = 125000

    CMD_BASE = 0x1907C080
    RESP_BASE = 0x1807C080
    TEMP_CMD_BASE = 0x19008080
    TEMP_RESP_BASE = 0x18008080
    AC_CMD_BASE = 0x1907A080
    AC_RESP_BASE = 0x1807A080
    ADDR_MASK = 0x7F
    CMD_SET_OUTPUT = 0x00
    CMD_READ_INFO = 0x01
    CMD_START_STOP = 0x02
    START_VALUE = 0x55
    STOP_VALUE = 0xAA

    def __init__(self, dev: ZlgVci, addr: int, rated_current: float = 100.0):
        super().__init__(daemon=True)
        self.dev = dev
        self.addr = addr
        self.rated_current = rated_current
        self.running = True
        self.actually_on = False
        self.voltage = 0.0
        self.current = 0.0
        self.temp = 32.0
        self.status_raw = 0
        self.pending = False
        self.pending_func = 0

    def _handle_rx(self, can_id, data):
        id_base = can_id & ~self.ADDR_MASK
        addr = can_id & self.ADDR_MASK
        if addr != self.addr:
            return

        if id_base == self.CMD_BASE:
            if len(data) == 0:
                return
            cmd = data[0]
            if cmd == self.CMD_START_STOP:
                self.actually_on = (data[7] == self.START_VALUE) if len(data) >= 8 else False
                if not self.actually_on:
                    self.voltage = 0.0
                    self.current = 0.0
                elif self.voltage <= 0.0:
                    self.voltage = 1.0
            # CMD_SET_OUTPUT: byte1-3 current(mA)/4-7 voltage(mV)
            self.pending = True
            self.pending_func = cmd
        elif id_base == self.TEMP_CMD_BASE:
            t_raw = int(self.temp * 10.0) & 0xFFFF
            resp = bytes([0, 0, 0, 0, (t_raw >> 8) & 0xFF, t_raw & 0xFF, 0, 0])
            self.dev.transmit(self.CAN_CHANNEL, self.TEMP_RESP_BASE | self.addr, resp, extended=True)
        elif id_base == self.AC_CMD_BASE:
            v_raw = int(220.0 * 32.0) & 0xFFFF
            resp = bytes([
                0x31, 0x00,
                (v_raw >> 8) & 0xFF, v_raw & 0xFF,
                (v_raw >> 8) & 0xFF, v_raw & 0xFF,
                (v_raw >> 8) & 0xFF, v_raw & 0xFF,
            ])
            self.dev.transmit(self.CAN_CHANNEL, self.AC_RESP_BASE | self.addr, resp, extended=True)

    def _tick(self):
        if not self.pending:
            return
        self.pending = False
        resp_id = self.RESP_BASE | self.addr

        if self.pending_func in (self.CMD_START_STOP, self.CMD_SET_OUTPUT):
            self.dev.transmit(self.CAN_CHANNEL, resp_id, bytes([self.pending_func, 0x01, 0, 0, 0, 0, 0, 0]), extended=True)
            return

        # CMD_READ_INFO: 1=temperature (degC), 2-3=current(0.1A/bit BE), 4-5=voltage(0.1V/bit BE),
        # 6-7=status_flags (bit0=0 means running).
        if self.actually_on:
            self.current = self.rated_current * 0.5
        curr_raw = int(self.current * 10.0) & 0xFFFF
        volt_raw = int(self.voltage * 10.0) & 0xFFFF
        status = self.status_raw
        status = (status & ~0x01) if self.actually_on else (status | 0x01)
        resp = bytes([
            self.CMD_READ_INFO, int(self.temp) & 0xFF,
            (curr_raw >> 8) & 0xFF, curr_raw & 0xFF,
            (volt_raw >> 8) & 0xFF, volt_raw & 0xFF,
            (status >> 8) & 0xFF, status & 0xFF,
        ])
        self.dev.transmit(self.CAN_CHANNEL, resp_id, resp, extended=True)

    def run(self):
        while self.running:
            for can_id, ext, data in self.dev.receive(self.CAN_CHANNEL, wait_ms=0):
                if ext:
                    self._handle_rx(can_id, data)
            self._tick()
            time.sleep(0.01)


# ============================================================= #
# Driver registry -- CHG_LIB_DriverId_t values from chg_lib.h    #
# ============================================================= #

DRIVERS = {
    "maxwell": (1, MaxwellModuleSim),
    "lianming": (2, LianmingModuleSim),
    "tonhe": (3, TonheModuleSim),
}


# ============================================================= #
# Main flow                                                        #
# ============================================================= #

def probe_module(ser) -> int:
    """Returns the configured module's addr via DEBUG_CMD_READ_ALL, or 1 as
    a fallback if none is registered yet."""
    ser.reset_input_buffer()
    ser.write(build_frame(0x12))  # DEBUG_CMD_READ_ALL
    cmd, payload = read_reply(ser, {0x91})  # DEBUG_RSP_ALL_MODULES
    if cmd is None or len(payload) < 2 or payload[1] < 1:
        return 1
    # DebugModuleData_t: identity(6B) ... addr is byte offset 71 (see
    # App/Protocol/pc_debug_protocol.h field order).
    return payload[2 + 71]


def switch_driver(ser, driver_id: int, module_addr: int = 1, module_group: int = 0) -> bool:
    """PC_CMD_SET_DRIVER then PC_CMD_SET_MODULE_ADDR, matching the real PC
    app's flow (App/Protocol/pc_protocol.c's PC_CMD_SET_DRIVER handler
    clears any module registered by ChargeCycleConfig_Set()'s side effect
    specifically so this explicit SET_MODULE_ADDR is the source of truth)."""
    ser.reset_input_buffer()
    ser.write(build_frame(PC_CMD_SET_DRIVER, bytes([driver_id])))
    cmd, payload = read_reply(ser, {PC_RSP_ACK, PC_RSP_NACK})
    if cmd != PC_RSP_ACK:
        print(f"[FAIL] SET_DRIVER({driver_id}) failed: cmd={cmd!r}")
        return False
    ser.write(build_frame(PC_CMD_SET_MODULE_ADDR, bytes([module_addr, module_group])))
    cmd, payload = read_reply(ser, {PC_RSP_ACK, PC_RSP_NACK})
    if cmd != PC_RSP_ACK:
        print(f"[FAIL] SET_MODULE_ADDR({module_addr},{module_group}) failed: cmd={cmd!r}")
        return False
    return True


def run_scenario(dev: ZlgVci, ser, driver_name: str, module_addr: int) -> bool:
    """Runs one BMS+module simulation against the real MCU (already
    switched to `driver_name` and holding `module_addr`) and returns
    whether it reached RUNNING. Leaves the controller in IDLE either way."""
    driver_id, sim_cls = DRIVERS[driver_name]
    print(f"\n=== Scenario: {driver_name} (driver_id={driver_id}, module addr={module_addr}) ===")

    bms = BmsSim(dev)
    module = sim_cls(dev, addr=module_addr)
    bms.start()
    module.start()
    print(f"[INFO] BMS + {driver_name} module simulators running")

    try:
        print("[STEP] Letting simulators settle for up to 12s (module may go through "
              "OFFLINE->RECOVERING first if MCU uptime already exceeds the offline "
              "timeout when registered -- see B-11) or until online...")
        settle_deadline = time.time() + 12.0
        pre = None
        while time.time() < settle_deadline:
            pre = get_system_info(ser)
            if pre:
                print(f"  modules_online={pre['modules_online']} state={pre['controller_state']}")
                if pre['modules_online'] > 0:
                    break
            time.sleep(0.5)
        if pre:
            print(f"[INFO] Pre-start: modules_total={pre['modules_total']} online={pre['modules_online']} "
                  f"driver_id={pre['driver_id']} bms_stale={pre['bms_stale']} fault=0x{pre['controller_fault_flags']:08X}")

        print("[STEP] PC_CMD_START (manual_mode=0, BMS-controlled)...")
        ser.write(build_frame(PC_CMD_START, bytes([0])))
        cmd, payload = read_reply(ser, {PC_RSP_ACK, PC_RSP_NACK})
        if cmd == PC_RSP_NACK:
            # send_nack(cmd, err) payload is [cmd_echo, err] -- pc_protocol.c:246-249.
            reason = payload[1] if len(payload) > 1 else -1
            print(f"[FAIL] START was NACKed, reason=0x{reason:02X}")
            return False
        if cmd != PC_RSP_ACK:
            print(f"[FAIL] START got no valid reply (cmd={cmd!r})")
            return False

        print("[STEP] Polling GET_SYSTEM for up to 8s, waiting for controller RUNNING...")
        reached_running = False
        deadline = time.time() + 8.0
        while time.time() < deadline:
            info = get_system_info(ser)
            if info:
                print(f"  state={info['controller_state']} charging={info['charging']} "
                      f"modules_online={info['modules_online']} target_v={info['controller_target_voltage']:.1f} "
                      f"temp={info['max_temp_dcdc']:.1f}C fault=0x{info['controller_fault_flags']:08X}")
                if info["controller_state"] == CHARGE_CTRL_STATE_RUNNING:
                    reached_running = True
                    # Hold in RUNNING for 2 seconds to verify AC & Temperature telemetry update
                    for _ in range(10):
                        time.sleep(0.2)
                        info = get_system_info(ser)
                        if info:
                            print(f"  state={info['controller_state']} charging={info['charging']} "
                                  f"modules_online={info['modules_online']} target_v={info['controller_target_voltage']:.1f} "
                                  f"temp={info['max_temp_dcdc']:.1f}C fault=0x{info['controller_fault_flags']:08X}")
                    break
            time.sleep(0.2)

        print("[STEP] PC_CMD_STOP -- returning to a clean idle state...")
        ser.write(build_frame(PC_CMD_STOP))
        read_reply(ser, {PC_RSP_ACK, PC_RSP_NACK})
        time.sleep(0.5)

        if reached_running:
            print(f"[PASS] Real MCU reached RUNNING against the simulated BMS+{driver_name} bus.")
        else:
            print(f"[FAIL] {driver_name}: did not reach RUNNING within timeout.")
        return reached_running
    finally:
        bms.running = False
        module.running = False
        time.sleep(0.1)


def main():
    requested = sys.argv[1:] or ["tonhe"]
    if requested == ["all"]:
        requested = ["maxwell", "lianming", "tonhe"]
    unknown = [d for d in requested if d not in DRIVERS]
    if unknown:
        print(f"[FAIL] Unknown driver(s) {unknown}; choose from {list(DRIVERS)} or 'all'")
        sys.exit(1)

    port = find_mcu_port()
    if not port:
        print("[FAIL] MCU USB CDC port not found (looking for VID:PID 0483:5740).")
        sys.exit(1)
    print(f"[INFO] MCU serial port: {port}")

    try:
        dev = ZlgVci()
    except Exception as e:
        print(f"[FAIL] Could not initialize ZLG USBCAN adapter: {e}")
        sys.exit(1)

    # ZlgVci.__enter__ opens the device; do not call dev.open() separately
    # (VCI_OpenDevice on an already-open handle fails with ret=0, which
    # looks exactly like "adapter not plugged in" and is easy to
    # misdiagnose -- learned the hard way writing this file).
    try:
        with dev, serial.Serial(port, 115200, timeout=0.2) as ser:
            info = dev.board_info()
            print(f"[INFO] CAN adapter: {info.str_hw_Type.decode(errors='replace')} "
                  f"(serial {info.str_Serial_Num.decode(errors='replace')}, {info.can_Num} channel(s))")
            dev.init_channel(0, 125000)  # CAN1 -- charger modules, all 3 drivers
            dev.init_channel(1, 250000)  # CAN2 -- BMS
            print("[INFO] CAN channels started: ch0=125000bps (CAN1/modules), ch1=250000bps (CAN2/BMS)")

            ser.dtr = True
            ser.rts = True
            time.sleep(0.3)
            ser.reset_input_buffer()

            print("[STEP] DEBUG_CMD_ENTER...")
            ser.write(build_frame(DEBUG_CMD_ENTER))
            cmd, _ = read_reply(ser, {PC_RSP_ACK, PC_RSP_NACK})
            if cmd != PC_RSP_ACK:
                print(f"[FAIL] Expected ACK, got {cmd!r}")
                sys.exit(1)

            # Capture the driver/module config as found, to restore it after
            # testing -- SET_DRIVER persists to flash (see its handler in
            # pc_protocol.c), so switching drivers here has a real,
            # persistent side effect on this unit if not undone.
            original_info = get_system_info(ser)
            original_driver_id = original_info["driver_id"] if original_info else None
            original_addr = probe_module(ser)
            print(f"[INFO] Original config: driver_id={original_driver_id} module_addr={original_addr}")

            results = {}
            switched_any = False
            for driver_name in requested:
                driver_id, _ = DRIVERS[driver_name]
                current_info = get_system_info(ser)
                if current_info is None or current_info["driver_id"] != driver_id:
                    print(f"[STEP] Switching to driver_id={driver_id} ({driver_name})...")
                    if not switch_driver(ser, driver_id, module_addr=1, module_group=0):
                        results[driver_name] = False
                        continue
                    switched_any = True
                    module_addr = 1
                else:
                    module_addr = probe_module(ser)
                results[driver_name] = run_scenario(dev, ser, driver_name, module_addr)

            if switched_any and original_driver_id is not None:
                print(f"\n[STEP] Restoring original driver_id={original_driver_id} module_addr={original_addr}...")
                switch_driver(ser, original_driver_id, module_addr=original_addr, module_group=0)

            print("\n=== Summary ===")
            all_pass = True
            for name, ok in results.items():
                print(f"  {name}: {'PASS' if ok else 'FAIL'}")
                all_pass = all_pass and ok
            if not all_pass:
                sys.exit(1)
    except RuntimeError as e:
        print(f"[FAIL] {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
