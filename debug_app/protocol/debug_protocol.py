"""Debug Protocol Parser for Charger Modules"""

import struct
from dataclasses import dataclass
from typing import List, Optional
from enum import IntEnum


# Commands (PC -> MCU)
class DebugCmd(IntEnum):
    ENTER = 0x10       # Enter debug mode, start streaming
    EXIT = 0x11       # Exit debug mode
    READ_ALL = 0x12    # Read all modules (one-shot)
    READ_ONE = 0x13    # Read specific module by index
    READ_STATS = 0x14  # Read communication statistics
    WRITE_REG = 0x15   # Write to module register
    SEND_RAW_CAN = 0x16  # Send raw CAN frame
    READ_BMS = 0x17    # Read BMS detailed data
    GET_SYSTEM = 0x18   # Read system information
    GET_CHARGE_CFG = 0x19
    SET_CHARGE_CFG = 0x1A


# Responses (MCU -> PC)
class DebugRsp(IntEnum):
    MODULE_DATA = 0x90    # Single module data
    ALL_MODULES = 0x91    # All modules data (streaming)
    COMM_STATS = 0x92     # Communication statistics
    BMS_DATA = 0x93       # BMS detailed data
    SYSTEM_INFO = 0x94    # System information
    RAW_CAN_TX = 0x95     # Raw CAN TX confirmation
    ERROR = 0x96          # Error response
    CHARGE_CFG = 0x97     # Charge-cycle configuration


# Standard PC Protocol Responses
class StdRsp(IntEnum):
    STATUS = 0x81
    ACK = 0x82
    NACK = 0x83
    PONG = 0x84
    READ_REG = 0x85


# Standard PC Protocol Commands
class StdCmd(IntEnum):
    SET_VOLTAGE = 0x01
    SET_CURRENT = 0x02
    START = 0x03
    STOP = 0x04
    SET_MODULE_ADDR = 0x05
    PING = 0x06
    READ_REG = 0x07
    EMERGENCY_STOP = 0x08
    SET_DRIVER = 0x09


# Driver names
DRIVER_NAMES = {
    0: "None",
    1: "Maxwell",
    2: "Lianming",
    3: "TonHe"
}

MODULE_TYPE_NAMES = {
    0: "Unknown",
    1: "EVR_10KW_100A_100V",
    2: "Maxwell",
    3: "Lianming",
    4: "TonHe",
}

CHARGE_SOURCE_MODE_NAMES = {
    0: "BMS Controlled",
    1: "Standalone (No BMS)",
}

# Module state names
STATE_NAMES = {
    0: "IDLE",
    1: "STARTING",
    2: "RUNNING",
    3: "WARNING",
    4: "OFFLINE",
    5: "FAULT",
    6: "RECOVERING",
    7: "STOPPING"
}

BMS_STATE_NAMES = {
    0: "OFFLINE",
    1: "ONLINE",
    2: "FAULT",
}

CHARGE_CTRL_STATE_NAMES = {
    0: "Idle",
    1: "Ready",
    2: "Running",
    3: "Derating",
    4: "Stopping",
    5: "Fault",
}

CHARGE_LIMIT_SOURCE_NAMES = {
    0: "None",
    1: "Cell Voltage",
    2: "Temperature",
    3: "SOC",
}

CHARGE_STAGE_BAND_NAMES = {
    0: "None",
    1: "None",
    2: "Level 1 (M1-M2)",
    3: "Level 2 (M2-M3)",
    4: "Level 3 (M3-M4)",
    5: "Level 4 (M4-M5)",
    6: "None",
}

CHARGE_STOP_REASON_NAMES = {
    0: "None",
    1: "User command",
    2: "Charge condition blocked",
    3: "BMS offline",
    4: "BMS alarm",
    5: "Protection",
    6: "Module timeout",
    7: "Module fault",
    8: "Module mismatch",
    9: "Emergency stop",
    10: "Precondition",
    11: "Charger voltage reached Vmax",
    12: "Cell voltage limit reached",
    13: "Battery full / SOC target reached",
}

BMS_ALARM_FLAG_NAMES = {
    0: "Low pack voltage",
    1: "Low cell voltage",
    2: "High pack voltage",
    3: "High cell voltage",
    4: "Cell temp high (chg)",
    5: "Cell temp high (dchg)",
    6: "Cell temp low (chg)",
    7: "Cell temp low (dchg)",
    8: "Relay temp high",
    9: "Over charge current",
    10: "Over discharge current",
    11: "Cell voltage diff",
    12: "Low SOC",
    13: "BMS offline",
    14: "Stale data",
}

ALARM_FLAG_NAMES = {
    0: "Hardware fault",
    1: "Communication fail",
    2: "Over temperature",
    3: "Output overvoltage",
    4: "Short circuit",
    5: "AC undervoltage / phase loss",
    6: "Output overcurrent",
    16: "PFC bus overvoltage",
    17: "PFC input overcurrent",
    18: "PFC bus/phase imbalance",
    19: "Mains frequency fault",
}

TONHE_STATUS_FAULT_NAMES = {
    0: "Input undervoltage",
    1: "Input phase loss",
    2: "Input overvoltage",
    3: "Output overvoltage",
    4: "Output overcurrent",
    5: "Over temperature",
    6: "Fan fault",
    7: "Hardware fault",
    8: "Bus exception",
    9: "SCI communication fault",
    10: "Discharge fault",
    11: "PFC shutdown",
    15: "Short circuit",
}

TONHE_PFC_FAULT_NAMES = {
    0: "PFC input overcurrent",
    1: "Mains frequency fault",
    2: "Mains imbalance",
    3: "DCT fault",
    4: "Address conflict",
    5: "Bus bias",
    6: "Phase exception",
    7: "Bus overvoltage",
}

TONHE_EXT_FAULT_NAMES = {
    0: "Preceding stage wave stop",
    1: "Hot-plug fault",
    2: "CAN communication timeout",
    4: "Relay operation fault",
    6: "Internal overtemperature",
    7: "Air inlet overtemperature",
    8: "Input power limit",
    9: "Power limit due to overtemperature",
    10: "Discharge changeover exception",
    11: "Voltage/current balancing abnormal",
    12: "Heat sink differential protection",
    13: "Emergency stop",
    14: "Air inlet temperature too low",
    15: "Front-end current imbalance",
}

TONHE_STATUS_TEXT = {
    0x00: "Standby",
    0x01: "Running",
    0x11: "FAULT",
}

# Maxwell alarm bit names (from alarm_status field)
MAXWELL_ALARM_NAMES = {
    0: "Module fault",
    1: "Module protect",
    3: "SCI failure",
    4: "Input error",
    5: "Input mismatch",
    7: "DCDC overvoltage",
    8: "PFC abnormal",
    9: "AC overvoltage",
    14: "AC undervoltage",
    16: "CAN failure",
    17: "Current imbalance",
    22: "DCDC off",
    23: "Power limit",
    24: "Temperature derating",
    25: "AC power limit",
    27: "Fan failure",
    28: "Short circuit",
    30: "DCDC overtemperature",
    31: "DCDC output overvoltage",
}

# Lianming alarm bit names (from alarm_status field)
LIANMING_ALARM_NAMES = {
    1: "Module fault",
    3: "Fan fault",
    4: "Input overvoltage",
    5: "Input undervoltage",
    6: "Output overvoltage",
    7: "Output undervoltage",
    13: "Overcurrent protection",
    14: "Over temperature",
}


def _decode_bits(value: int, names: dict) -> List[str]:
    decoded = []
    for bit, name in names.items():
        if value & (1 << bit):
            decoded.append(name)
    return decoded


@dataclass
class ModuleData:
    """Module data structure matching DebugModuleData_t in C code

    Extended to include AC phases, PFC bus voltages, rated specs.
    Total: 126 bytes
    """
    module_idx: int
    driver_id: int
    enabled: bool
    online: bool
    running: bool
    state: int
    voltage: float
    current: float
    current_limit: float
    temp_dcdc: float
    temp_ambient: float
    temp_pfc: float
    ac_phase_a_voltage: float
    ac_phase_b_voltage: float
    ac_phase_c_voltage: float
    pfc_bus_pos_voltage: float
    pfc_bus_neg_voltage: float
    input_power: int
    rated_power: float
    rated_current: float
    alarm_status: int
    alarm_flags: int
    pfc_fault: int
    addr: int
    group: int
    last_rx_tick: int
    last_tx_tick: int
    tx_count: int
    rx_count: int
    error_count: int
    timeout_count: int
    recovery_count: int
    vendor_data_len: int = 0
    vendor_data: bytes = b''

    @staticmethod
    def from_bytes(data: bytes) -> 'ModuleData':
        """Unpack from C struct - must match DebugModuleData_t exactly (123 bytes)

        C struct layout (packed, little-endian, 123 bytes):
          offset   size  field
          ------   ----  -----
          0        1     module_idx
          1        1     driver_id
          2        1     enabled
          3        1     online
          4        1     running
          5        1     state
          6        12    voltage, current, current_limit
          18       12    temp_dcdc, temp_ambient, temp_pfc
          30       12    ac_phase_a/b/c_voltage
          42       8     pfc_bus_pos/neg_voltage
          50       4     input_power
          54       8     rated_power, rated_current
          62       8     alarm_status, alarm_flags
          70       1     pfc_fault
          71       1     addr
          72       1     group
          73       8     last_rx_tick, last_tx_tick
          81       20    tx_count, rx_count, error_count, timeout_count, recovery_count
          101      1     vendor_data_len
          102      21    vendor_data[21]
        Total: 123 bytes
        """
        if len(data) < 123:
            raise ValueError(f"ModuleData requires 123 bytes, got {len(data)}")

        # Use struct.unpack_from at exact byte offsets to match C packed layout
        module_idx, driver_id, enabled, online, running, state = struct.unpack_from(
            "<BBBBBB", data, 0)
        voltage, current, current_limit = struct.unpack_from(
            "<fff", data, 6)
        temp_dcdc, temp_ambient, temp_pfc = struct.unpack_from(
            "<fff", data, 18)
        ac_a, ac_b, ac_c = struct.unpack_from(
            "<fff", data, 30)
        pfc_pos, pfc_neg = struct.unpack_from(
            "<ff", data, 42)
        input_power, = struct.unpack_from(
            "<I", data, 50)
        rated_power, rated_current = struct.unpack_from(
            "<ff", data, 54)
        alarm_status, alarm_flags = struct.unpack_from(
            "<II", data, 62)
        pfc_fault, addr, group = struct.unpack_from(
            "<BBB", data, 70)
        last_rx_tick, last_tx_tick = struct.unpack_from(
            "<II", data, 73)
        tx_count, rx_count, error_count, timeout_count, recovery_count = struct.unpack_from(
            "<IIIII", data, 81)
        vendor_data_len, = struct.unpack_from(
            "<B", data, 101)
        vendor_data = data[102:123]

        return ModuleData(
            module_idx=module_idx,
            driver_id=driver_id,
            enabled=bool(enabled),
            online=bool(online),
            running=bool(running),
            state=state,
            voltage=voltage,
            current=current,
            current_limit=current_limit,
            temp_dcdc=temp_dcdc,
            temp_ambient=temp_ambient,
            temp_pfc=temp_pfc,
            ac_phase_a_voltage=ac_a,
            ac_phase_b_voltage=ac_b,
            ac_phase_c_voltage=ac_c,
            pfc_bus_pos_voltage=pfc_pos,
            pfc_bus_neg_voltage=pfc_neg,
            input_power=input_power,
            rated_power=rated_power,
            rated_current=rated_current,
            alarm_status=alarm_status,
            alarm_flags=alarm_flags,
            pfc_fault=pfc_fault,
            addr=addr,
            group=group,
            last_rx_tick=last_rx_tick,
            last_tx_tick=last_tx_tick,
            tx_count=tx_count,
            rx_count=rx_count,
            error_count=error_count,
            timeout_count=timeout_count,
            recovery_count=recovery_count,
            vendor_data_len=vendor_data_len,
            vendor_data=vendor_data,
        )

    def get_driver_name(self) -> str:
        return DRIVER_NAMES.get(self.driver_id, f"Unknown({self.driver_id})")

    def get_state_name(self) -> str:
        return STATE_NAMES.get(self.state, f"Unknown({self.state})")

    def get_alarm_flag_names(self) -> List[str]:
        return _decode_bits(self.alarm_flags, ALARM_FLAG_NAMES)

    def get_vendor_alarm_names(self) -> List[str]:
        if self.driver_id == 3:
            names = _decode_bits(self.alarm_status & 0xFFFF, TONHE_STATUS_FAULT_NAMES)
            names.extend(_decode_bits((self.alarm_status >> 16) & 0xFFFF, TONHE_EXT_FAULT_NAMES))
            names.extend(_decode_bits(self.pfc_fault, TONHE_PFC_FAULT_NAMES))
            return names
        if self.driver_id == 1:
            return _decode_bits(self.alarm_status, MAXWELL_ALARM_NAMES)
        if self.driver_id == 2:
            return _decode_bits(self.alarm_status & 0xFFFF, LIANMING_ALARM_NAMES)
        return []

    def get_alarm_summary(self) -> str:
        names = []
        for name in self.get_alarm_flag_names() + self.get_vendor_alarm_names():
            if name not in names:
                names.append(name)
        if names:
            return "; ".join(names)
        if self.alarm_status or self.alarm_flags or self.pfc_fault:
            return "Unknown alarm"
        return "None"

    def get_detailed_alarms(self) -> str:
        """Get detailed alarm breakdown for debugging"""
        if self.driver_id != 3:  # Only for TonHe
            return ""

        lines = []
        if self.state == 5:  # FAULT state
            lines.append(">>> Module in FAULT state <<<")
        elif self.state == 4:
            lines.append(">>> Module OFFLINE <<<")

        # Alarm Status (bytes 6-7 from M_C_1)
        if self.alarm_status != 0:
            lines.append(f"Fault word: 0x{self.alarm_status:04X}")
            for bit in range(16):
                if self.alarm_status & (1 << bit):
                    name = TONHE_STATUS_FAULT_NAMES.get(bit, f"Bit {bit}")
                    lines.append(f"  - {name}")

        # PFC Fault (byte 8 from M_C_1)
        if self.pfc_fault != 0:
            lines.append(f"PFC fault: 0x{self.pfc_fault:02X}")
            for bit in range(8):
                if self.pfc_fault & (1 << bit):
                    name = TONHE_PFC_FAULT_NAMES.get(bit, f"Bit {bit}")
                    lines.append(f"  - {name}")

        ext_fault = (self.alarm_status >> 16) & 0xFFFF
        if ext_fault != 0:
            lines.append(f"Extended fault: 0x{ext_fault:04X}")
            for bit in range(16):
                if ext_fault & (1 << bit):
                    name = TONHE_EXT_FAULT_NAMES.get(bit, f"Bit {bit}")
                    lines.append(f"  - {name}")

        return "\n".join(lines) if lines else ""


@dataclass
class BMSData:
    """BMS data structure"""
    state: int
    online: bool
    charge_relay_closed: bool
    discharge_relay_closed: bool
    batt_voltage: float
    batt_current: float
    cap_remain: float
    rate_cap: float
    soc: int
    soh: int
    max_cell_volt: int
    min_cell_volt: int
    max_cell_temp: float
    min_cell_temp: float
    chg_volt_request: float
    chg_curr_request: float
    alarm_flags: int
    last_rx_tick: int

    def get_state_name(self) -> str:
        return BMS_STATE_NAMES.get(self.state, f"UNKNOWN({self.state})")

    def get_alarm_names(self) -> List[str]:
        return _decode_bits(self.alarm_flags, BMS_ALARM_FLAG_NAMES)


@dataclass
class SystemInfo:
    """System information structure"""
    fw_major: int
    fw_minor: int
    fw_patch: int
    driver_id: int
    modules_total: int
    modules_online: int
    modules_fault: int
    charging: bool
    controller_state: int
    controller_derating: bool
    controller_inhibit: bool
    charge_source_mode: int
    active_limit_source: int
    active_stage_band: int
    total_voltage: float
    total_current: float
    total_power_in: float
    max_temp_dcdc: float
    controller_target_voltage: float
    controller_target_current_total: float
    active_limit_current_c: float
    uptime_ticks: int
    controller_fault_flags: int
    controller_stop_reason: int
    bms_stale: bool

    def get_controller_state_name(self) -> str:
        return CHARGE_CTRL_STATE_NAMES.get(self.controller_state, f"UNKNOWN({self.controller_state})")

    def get_charge_source_mode_name(self) -> str:
        return CHARGE_SOURCE_MODE_NAMES.get(self.charge_source_mode, f"Unknown({self.charge_source_mode})")

    def get_active_limit_source_name(self) -> str:
        return CHARGE_LIMIT_SOURCE_NAMES.get(self.active_limit_source, f"Unknown({self.active_limit_source})")

    def get_active_logic_name(self) -> str:
        """Return the user-facing control source, including standalone mode."""
        if self.charge_source_mode == 1:
            return "No BMS"
        return self.get_active_limit_source_name()

    def get_active_stage_band_name(self) -> str:
        return CHARGE_STAGE_BAND_NAMES.get(self.active_stage_band, f"Unknown({self.active_stage_band})")

    def get_charge_status_name(self) -> str:
        """Return the user-facing charge status derived from controller flags."""
        if not self.charging:
            return "Stopped"
        if self.controller_inhibit:
            return "Blocked"
        if self.controller_derating:
            return "Derating"
        return "Normal"

    def get_charge_level_name(self) -> str:
        """Return a friendly level name; stage boundary states remain None."""
        if self.charge_source_mode == 1:
            return "None"
        return CHARGE_STAGE_BAND_NAMES.get(self.active_stage_band, "None")

    def get_charge_block_reason_name(self) -> str:
        """Explain why current is blocked without exposing generic stage jargon."""
        if not self.controller_inhibit:
            return "None"

        reasons = {
            1: ("Cell voltage below charge range", "Cell voltage limit reached"),
            2: ("Temperature below charge range", "Temperature too high"),
            3: ("SOC below charge threshold", "Battery full / SOC target reached"),
        }
        below, above = reasons.get(self.active_limit_source, ("Charge condition blocked", "Charge condition blocked"))
        if self.active_stage_band == 1:
            return below
        if self.active_stage_band == 6:
            return above
        return "Charge condition blocked"

    def get_stop_reason_name(self) -> str:
        return CHARGE_STOP_REASON_NAMES.get(
            self.controller_stop_reason,
            f"Unknown({self.controller_stop_reason})",
        )


@dataclass
class ChargeCycleConfig:
    version: int
    battery_capacity_ah: float
    imin_c: float
    imax_c: float
    ipre_c: float
    ilow_c: float
    vmin_v: float
    vmax_v: float
    vpre_v: float
    vlow_v: float
    temp_limit_c: float
    cell_volt_enabled: bool
    cell_volt_delta_v: float
    cell_volt_1_v: float
    cell_volt_2_v: float
    cell_volt_3_v: float
    cell_volt_4_v: float
    cell_volt_5_v: float
    cell_curr_1_c: float
    cell_curr_2_c: float
    cell_curr_3_c: float
    cell_curr_4_c: float
    temp_enabled: bool
    temp_delta_c: float
    temp_1_c: float
    temp_2_c: float
    temp_3_c: float
    temp_4_c: float
    temp_5_c: float
    temp_curr_1_c: float
    temp_curr_2_c: float
    temp_curr_3_c: float
    temp_curr_4_c: float
    soc_enabled: bool
    soc_delta_pct: float
    soc_1_pct: float
    soc_2_pct: float
    soc_3_pct: float
    soc_4_pct: float
    soc_5_pct: float
    soc_curr_1_c: float
    soc_curr_2_c: float
    soc_curr_3_c: float
    soc_curr_4_c: float
    protect_jack_charge_enabled: bool
    protect_jack_charge_delta_v: float
    protect_jack_charge_delay_s: int
    protect_jack_temp_enabled: bool
    protect_jack_temp_delay_s: int
    protect_jack_temp_threshold_c: float
    protect_jack_temp_delta_c: float
    protect_jack_temp_power_limit_pct: float
    charge_source_mode: int
    can_battery_id: int
    source_module_count: int
    module_type: int
    module_u_min_v: float
    module_u_max_v: float
    module_i_min_a: float
    module_i_max_a: float

    FORMAT = "<H" + ("f" * 10) + "B" + ("f" * 10) + "B" + ("f" * 10) + "B" + ("f" * 10) + "BfH" + "BHfff" + "BBBBffff"
    SIZE = struct.calcsize(FORMAT)

    @classmethod
    def from_bytes(cls, data: bytes) -> "ChargeCycleConfig":
        if len(data) < cls.SIZE:
            raise ValueError(f"ChargeCycleConfig requires {cls.SIZE} bytes, got {len(data)}")

        values = struct.unpack_from(cls.FORMAT, data)
        return cls(
            version=values[0],
            battery_capacity_ah=values[1],
            imin_c=values[2],
            imax_c=values[3],
            ipre_c=values[4],
            ilow_c=values[5],
            vmin_v=values[6],
            vmax_v=values[7],
            vpre_v=values[8],
            vlow_v=values[9],
            temp_limit_c=values[10],
            cell_volt_enabled=bool(values[11]),
            cell_volt_delta_v=values[12],
            cell_volt_1_v=values[13],
            cell_volt_2_v=values[14],
            cell_volt_3_v=values[15],
            cell_volt_4_v=values[16],
            cell_volt_5_v=values[17],
            cell_curr_1_c=values[18],
            cell_curr_2_c=values[19],
            cell_curr_3_c=values[20],
            cell_curr_4_c=values[21],
            temp_enabled=bool(values[22]),
            temp_delta_c=values[23],
            temp_1_c=values[24],
            temp_2_c=values[25],
            temp_3_c=values[26],
            temp_4_c=values[27],
            temp_5_c=values[28],
            temp_curr_1_c=values[29],
            temp_curr_2_c=values[30],
            temp_curr_3_c=values[31],
            temp_curr_4_c=values[32],
            soc_enabled=bool(values[33]),
            soc_delta_pct=values[34],
            soc_1_pct=values[35],
            soc_2_pct=values[36],
            soc_3_pct=values[37],
            soc_4_pct=values[38],
            soc_5_pct=values[39],
            soc_curr_1_c=values[40],
            soc_curr_2_c=values[41],
            soc_curr_3_c=values[42],
            soc_curr_4_c=values[43],
            protect_jack_charge_enabled=bool(values[44]),
            protect_jack_charge_delta_v=values[45],
            protect_jack_charge_delay_s=values[46],
            protect_jack_temp_enabled=bool(values[47]),
            protect_jack_temp_delay_s=values[48],
            protect_jack_temp_threshold_c=values[49],
            protect_jack_temp_delta_c=values[50],
            protect_jack_temp_power_limit_pct=values[51],
            charge_source_mode=values[52],
            can_battery_id=values[53],
            source_module_count=values[54],
            module_type=values[55],
            module_u_min_v=values[56],
            module_u_max_v=values[57],
            module_i_min_a=values[58],
            module_i_max_a=values[59],
        )

    def to_bytes(self) -> bytes:
        return struct.pack(
            self.FORMAT,
            self.version,
            self.battery_capacity_ah,
            self.imin_c,
            self.imax_c,
            self.ipre_c,
            self.ilow_c,
            self.vmin_v,
            self.vmax_v,
            self.vpre_v,
            self.vlow_v,
            self.temp_limit_c,
            int(self.cell_volt_enabled),
            self.cell_volt_delta_v,
            self.cell_volt_1_v,
            self.cell_volt_2_v,
            self.cell_volt_3_v,
            self.cell_volt_4_v,
            self.cell_volt_5_v,
            self.cell_curr_1_c,
            self.cell_curr_2_c,
            self.cell_curr_3_c,
            self.cell_curr_4_c,
            int(self.temp_enabled),
            self.temp_delta_c,
            self.temp_1_c,
            self.temp_2_c,
            self.temp_3_c,
            self.temp_4_c,
            self.temp_5_c,
            self.temp_curr_1_c,
            self.temp_curr_2_c,
            self.temp_curr_3_c,
            self.temp_curr_4_c,
            int(self.soc_enabled),
            self.soc_delta_pct,
            self.soc_1_pct,
            self.soc_2_pct,
            self.soc_3_pct,
            self.soc_4_pct,
            self.soc_5_pct,
            self.soc_curr_1_c,
            self.soc_curr_2_c,
            self.soc_curr_3_c,
            self.soc_curr_4_c,
            int(self.protect_jack_charge_enabled),
            self.protect_jack_charge_delta_v,
            self.protect_jack_charge_delay_s,
            int(self.protect_jack_temp_enabled),
            self.protect_jack_temp_delay_s,
            self.protect_jack_temp_threshold_c,
            self.protect_jack_temp_delta_c,
            self.protect_jack_temp_power_limit_pct,
            self.charge_source_mode,
            self.can_battery_id,
            self.source_module_count,
            self.module_type,
            self.module_u_min_v,
            self.module_u_max_v,
            self.module_i_min_a,
            self.module_i_max_a,
        )

    def get_module_type_name(self) -> str:
        return MODULE_TYPE_NAMES.get(self.module_type, f"Unknown({self.module_type})")

    def get_charge_source_mode_name(self) -> str:
        return CHARGE_SOURCE_MODE_NAMES.get(self.charge_source_mode, f"Unknown({self.charge_source_mode})")


class DebugProtocolParser:
    """Parser for debug protocol frames"""

    # Size of DebugModuleData_t structure (123 bytes, verified by C compiler)
    MODULE_SIZE = 123
    CHARGE_CFG_SIZE = ChargeCycleConfig.SIZE

    def __init__(self):
        self.callbacks = {}
        self._frame_callback = None

    def on_frame(self, callback):
        """Register callback for all frames"""
        self._frame_callback = callback

    def parse_frame(self, cmd: int, payload: bytes):
        """Parse a received frame and call appropriate handler"""
        # First call the general frame callback
        if self._frame_callback:
            self._frame_callback(cmd, payload)

        # Then parse based on command
        if cmd == DebugRsp.ALL_MODULES:
            return self._parse_all_modules(payload)
        elif cmd == DebugRsp.MODULE_DATA:
            return self._parse_module(payload)
        elif cmd == DebugRsp.BMS_DATA:
            return self._parse_bms(payload)
        elif cmd == DebugRsp.SYSTEM_INFO:
            return self._parse_system_info(payload)
        elif cmd == DebugRsp.COMM_STATS:
            return self._parse_comm_stats(payload)
        elif cmd == DebugRsp.CHARGE_CFG:
            return self._parse_charge_config(payload)
        elif cmd == DebugRsp.ERROR:
            return self._parse_error(payload)
        return None

    def _parse_all_modules(self, data: bytes) -> List[ModuleData]:
        """Parse ALL_MODULES response (streaming data)"""
        if len(data) < 2:
            return []

        sequence = data[0]
        module_count = data[1]
        modules = []
        offset = 2

        for i in range(module_count):
            if offset + self.MODULE_SIZE > len(data):
                break
            mod_data = data[offset:offset + self.MODULE_SIZE]
            try:
                module = ModuleData.from_bytes(mod_data)
                modules.append(module)
            except Exception as e:
                print(f"Error parsing module {i}: {e}")
            offset += self.MODULE_SIZE

        return modules

    def _parse_module(self, data: bytes) -> Optional[ModuleData]:
        """Parse single MODULE_DATA response"""
        if len(data) < self.MODULE_SIZE:
            return None
        try:
            return ModuleData.from_bytes(data[:self.MODULE_SIZE])
        except Exception as e:
            print(f"Error parsing module: {e}")
            return None

    def _parse_bms(self, data: bytes) -> Optional[BMSData]:
        """Parse BMS_DATA response (46 bytes, matches DebugProtocol_BuildBMSData)

        Layout (packed, little-endian, 50 bytes):
          uint8  state, online, charge_relay_closed, discharge_relay_closed
          float  batt_voltage, batt_current, cap_remain, rate_cap
          uint8  soc, soh
          uint16 max_cell_volt, min_cell_volt
          float  max_cell_temp, min_cell_temp
          float  chg_volt_request, chg_curr_request
          uint32 alarm_flags, last_rx_tick
        """
        fmt = "<BBBBffffBBHHffffII"
        if len(data) < 50:
            return None

        unpacked = struct.unpack_from(fmt, data)
        return BMSData(
            state=unpacked[0],
            online=bool(unpacked[1]),
            charge_relay_closed=bool(unpacked[2]),
            discharge_relay_closed=bool(unpacked[3]),
            batt_voltage=unpacked[4],
            batt_current=unpacked[5],
            cap_remain=unpacked[6],
            rate_cap=unpacked[7],
            soc=unpacked[8],
            soh=unpacked[9],
            max_cell_volt=unpacked[10],
            min_cell_volt=unpacked[11],
            max_cell_temp=unpacked[12],
            min_cell_temp=unpacked[13],
            chg_volt_request=unpacked[14],
            chg_curr_request=unpacked[15],
            alarm_flags=unpacked[16],
            last_rx_tick=unpacked[17]
        )

    def _parse_system_info(self, data: bytes) -> Optional[SystemInfo]:
        """Parse SYSTEM_INFO response (68 bytes, matches DebugSystemInfo_t)

        Layout (packed, little-endian):
          uint8  fw_major, fw_minor, fw_patch, driver_id (4)
          uint8  modules_total, modules_online, modules_fault, charging (4)
          uint8  controller_state, controller_derating, controller_inhibit, charge_source_mode (4)
          uint8  active_limit_source, active_stage_band (2)
          float  total_voltage, total_current, total_power_in, max_temp_dcdc (16)
          float  controller_target_voltage, controller_target_current_total, active_limit_current_c (12)
          uint32 uptime_ticks, can1_tx_count, can1_rx_count, can2_tx_count, can2_rx_count (20)
        Appended diagnostics: controller fault flags, stop reason, BMS stale.
        Total: 68 bytes
        """
        fmt = "<BBBBBBBBBBBBBBfffffffIIIIIIBB"
        legacy_fmt = "<BBBBBBBBBBBBBBfffffffIIIII"
        has_diagnostics = len(data) >= struct.calcsize(fmt)
        if not has_diagnostics:
            fmt = legacy_fmt
        if len(data) < struct.calcsize(fmt):
            return None

        unpacked = struct.unpack_from(fmt, data)
        return SystemInfo(
            fw_major=unpacked[0],
            fw_minor=unpacked[1],
            fw_patch=unpacked[2],
            driver_id=unpacked[3],
            modules_total=unpacked[4],
            modules_online=unpacked[5],
            modules_fault=unpacked[6],
            charging=bool(unpacked[7]),
            controller_state=unpacked[8],
            controller_derating=bool(unpacked[9]),
            controller_inhibit=bool(unpacked[10]),
            charge_source_mode=unpacked[11],
            active_limit_source=unpacked[12],
            active_stage_band=unpacked[13],
            total_voltage=unpacked[14],
            total_current=unpacked[15],
            total_power_in=unpacked[16],
            max_temp_dcdc=unpacked[17],
            controller_target_voltage=unpacked[18],
            controller_target_current_total=unpacked[19],
            active_limit_current_c=unpacked[20],
            uptime_ticks=unpacked[21],
            controller_fault_flags=unpacked[26] if has_diagnostics else 0,
            controller_stop_reason=unpacked[27] if has_diagnostics else 0,
            bms_stale=bool(unpacked[28]) if has_diagnostics else False
        )

    def _parse_comm_stats(self, data: bytes) -> Optional[dict]:
        """Parse COMM_STATS response"""
        # B = 1 byte (module_idx)
        # I I I I I = 20 bytes (5 uint32 stats)
        # Total = 21 bytes
        fmt = "<BIIIII"

        if len(data) < struct.calcsize(fmt):
            return None

        unpacked = struct.unpack_from(fmt, data)
        return {
            'module_idx': unpacked[0],
            'tx_count': unpacked[1],
            'rx_count': unpacked[2],
            'error_count': unpacked[3],
            'timeout_count': unpacked[4],
            'recovery_count': unpacked[5]
        }

    def _parse_error(self, data: bytes) -> dict:
        """Parse ERROR response"""
        error_codes = {
            0x00: "NONE",
            0x01: "BAD_PARAM",
            0x02: "MODULE_OFFLINE",
            0x03: "NOT_SUPPORTED",
            0x04: "CAN_TX_FAIL",
            0x05: "BUFFER_FULL"
        }
        code = data[0] if len(data) > 0 else 0
        return {
            'error_code': code,
            'error_name': error_codes.get(code, f"UNKNOWN(0x{code:02X})")
        }

    def _parse_charge_config(self, data: bytes) -> Optional[ChargeCycleConfig]:
        if len(data) < self.CHARGE_CFG_SIZE:
            return None
        try:
            return ChargeCycleConfig.from_bytes(data[:self.CHARGE_CFG_SIZE])
        except Exception as e:
            print(f"Error parsing charge config: {e}")
            return None
