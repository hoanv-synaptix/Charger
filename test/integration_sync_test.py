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

Scope as of 2026-08-28: TonHe module simulation only, matching the
currently-configured hardware driver (CHG_LIB_DRV_TONHE). Maxwell/Lianming
HIL coverage is not built yet -- host_charge_sim covers all 3 drivers'
protocol logic already; this file's job is specifically to prove the real
MCU (real HAL/FDCAN/USB CDC, not a host mock) behaves the same way, which
only needed proving once to validate the real-hardware path exists and
works. Extend TonheModuleSim's sibling classes the same way if/when Maxwell
or Lianming HIL coverage is needed.

Driver requirement (Windows): the ZLG "USBCAN 2I"-class adapter needs its
vendor driver installed (ZLG/Zhiyuan's classic ControlCAN USBCAN driver
package -- installs usbcan_x64.dll). This talks to that DLL directly via
ctypes (the pip `zlgcan` package needs a different, newer unified
zlgcan.dll this project's hardware doesn't have installed). Set
ZLG_USBCAN_DLL_PATH to override the default install path if yours differs.

Usage:
    pip install pyserial
    python test/integration_sync_test.py
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
PC_RSP_ACK = 0x82
PC_RSP_NACK = 0x83

# DebugSystemInfo_t (packed, 68 bytes) -- App/Protocol/pc_debug_protocol.h
SYSTEM_INFO_FMT = "<14B7f5I I 2B"
SYSTEM_INFO_KEYS = [
    "fw_major", "fw_minor", "fw_patch", "driver_id", "modules_total",
    "modules_online", "modules_fault", "charging", "controller_state",
    "controller_derating", "controller_inhibit", "charge_source_mode",
    "active_limit_source", "active_stage_band",
    "total_voltage", "total_current", "total_power_in", "max_temp_dcdc",
    "controller_target_voltage", "controller_target_current_total", "active_limit_current_c",
    "uptime_ticks", "can1_tx_count", "can1_rx_count", "can2_tx_count", "can2_rx_count",
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


def find_mcu_port():
    for p in serial.tools.list_ports.comports():
        if p.vid == 0x0483 and p.pid == 0x5740:
            return p.device
    return None


def get_system_info(ser):
    ser.reset_input_buffer()
    ser.write(build_frame(DEBUG_CMD_GET_SYSTEM))
    cmd, payload = read_frame(ser)
    if cmd != DEBUG_RSP_SYSTEM_INFO or len(payload) != struct.calcsize(SYSTEM_INFO_FMT):
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
# Main flow                                                        #
# ============================================================= #

def probe_module(ser) -> int:
    """Returns the configured module's addr via DEBUG_CMD_READ_ALL, or 1 as
    a fallback if none is registered yet."""
    ser.reset_input_buffer()
    ser.write(build_frame(0x12))  # DEBUG_CMD_READ_ALL
    cmd, payload = read_frame(ser)
    if cmd != 0x91 or len(payload) < 2 or payload[1] < 1:
        return 1
    # DebugModuleData_t: identity(6B) ... addr is byte offset 71 (see
    # App/Protocol/pc_debug_protocol.h field order).
    return payload[2 + 71]


def main():
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
            dev.init_channel(TonheModuleSim.CAN_CHANNEL, TonheModuleSim.CAN_BITRATE)
            dev.init_channel(BmsSim.CAN_CHANNEL, BmsSim.CAN_BITRATE)
            print(f"[INFO] CAN channels started: "
                  f"ch{TonheModuleSim.CAN_CHANNEL}={TonheModuleSim.CAN_BITRATE}bps (CAN1/modules), "
                  f"ch{BmsSim.CAN_CHANNEL}={BmsSim.CAN_BITRATE}bps (CAN2/BMS)")

            ser.dtr = True
            ser.rts = True
            time.sleep(0.3)
            ser.reset_input_buffer()

            print("[STEP] DEBUG_CMD_ENTER...")
            ser.write(build_frame(DEBUG_CMD_ENTER))
            cmd, _ = read_frame(ser)
            if cmd != PC_RSP_ACK:
                print(f"[FAIL] Expected ACK, got {cmd!r}")
                sys.exit(1)

            module_addr = probe_module(ser)
            print(f"[INFO] Configured module addr={module_addr}")

            bms = BmsSim(dev)
            module = TonheModuleSim(dev, addr=module_addr)
            bms.start()
            module.start()
            print("[INFO] BMS + TonHe module simulators running")

            print("[STEP] Letting simulators settle for 1.5s before START...")
            time.sleep(1.5)

            print("[STEP] PC_CMD_START (manual_mode=0, BMS-controlled)...")
            ser.write(build_frame(PC_CMD_START, bytes([0])))
            cmd, payload = read_frame(ser)
            if cmd == PC_RSP_NACK:
                print(f"[FAIL] START was NACKed, reason=0x{payload[0]:02X}")
                bms.running = module.running = False
                sys.exit(1)

            print("[STEP] Polling GET_SYSTEM for up to 8s, waiting for controller RUNNING...")
            reached_running = False
            deadline = time.time() + 8.0
            while time.time() < deadline:
                info = get_system_info(ser)
                if info:
                    print(f"  state={info['controller_state']} charging={info['charging']} "
                          f"modules_online={info['modules_online']} target_v={info['controller_target_voltage']:.1f} "
                          f"fault=0x{info['controller_fault_flags']:08X}")
                    if info["controller_state"] == CHARGE_CTRL_STATE_RUNNING:
                        reached_running = True
                        break
                time.sleep(0.2)

            print("[STEP] PC_CMD_STOP -- returning to a clean idle state...")
            ser.write(build_frame(PC_CMD_STOP))
            read_frame(ser)
            time.sleep(0.5)

            bms.running = False
            module.running = False

            if reached_running:
                print("[PASS] Real MCU reached RUNNING against the simulated BMS+TonHe bus.")
            else:
                print("[FAIL] Did not reach RUNNING within timeout.")
                sys.exit(1)
    except RuntimeError as e:
        print(f"[FAIL] {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
