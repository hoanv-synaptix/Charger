# TONHE V1.3 Protocol Analysis

## 1. Overview

| Property | Value |
|---|---|
| Standard | SAE J1939-21 based |
| Frame | 29-bit Extended CAN |
| Baudrate | 125 Kbps (isolated) |
| Format | PDU1 (PS = target address) |
| Main Controller Address | 0xA0 |
| Module Address Range | 1-240 |
| Broadcast Address | 0xFF |

## 2. CAN ID Structure

```
29-bit Extended ID:
[28:26] Priority (3 bits) - 0=highest, 7=lowest
[25]    Reserved (1 bit) - always 0
[24]    DP - Data Page (1 bit) - always 0
[23:16] PF - PDU Format (8 bits) - PGN high byte
[15:8]  PS - Specific (8 bits) - target address for PDU1
[7:0]   SA - Source Address (8 bits)
```

**CAN ID Formula:**
```c
#define TONHE_BUILD_ID(priority, pf, ps, sa) \
    (((priority) & 0x07U) << 26 | \
     ((pf) & 0xFFU) << 16 | \
     ((ps) & 0xFFU) << 8 | \
     ((sa) & 0xFFU))
```

## 3. PGN (Parameter Group Number) Table

### 3.1 Uplink (Module → Master)

| PGN | Name | Direction | Cycle | Priority |
|-----|------|-----------|-------|----------|
| 0x000100 | M_C_1: Module Status | Module→Master | 500ms + Trigger | 6 |
| 0x000200 | M_C_2: Start/Stop Confirm | Module→Master | Trigger | 2 |
| 0x000B00 | M_C_3: AC Phase Info | Module→Master | 500ms | 6 |
| 0x009100 | M_C_4: Extended Status | Module→Master | 500ms + Trigger | 7 |

### 3.2 Downlink (Master → Module)

| PGN | Name | Direction | Cycle | Priority |
|-----|------|-----------|-------|----------|
| 0x000300 | C_M_1: Broadcast Start/Stop | Master→Module | Trigger | 2 |
| 0x000400 | C_M_2: Broadcast Parameter Set | Master→Module | Trigger | 4 |
| 0x000500 | C_M_3: Timing Command | Master→Module | 5000ms | 6 |
| 0x000600 | C_M_24: Specific Module Start/Stop | Master→Module | Trigger | 2 |
| 0x000900 | C_M_23: Address Setting | Master→Module | Trigger | 6 |
| 0x009000 | C_M_12: Address Mode Selection | Master→Module | Trigger | 7 |

## 4. Message Format Details

### 4.1 M_C_1: Charging Module Status (PGN 0x000100)

**CAN ID:** `0x1801A0xx` (xx = module address)

| Byte | Length | Name | Format | Range |
|------|--------|------|--------|-------|
| 1 | 1 | Module Status | uint8 | 0x00=Normal OFF, 0x01=ON, 0x11=Fault OFF |
| 2-3 | 2 | Output Voltage | uint16 LE | 0.1V/bit, 0 offset, 0-1100V |
| 4-5 | 2 | Output Current | uint16 LE | 0.01A/bit, 0 offset, 0-200A |
| 6-7 | 2 | Fault/Warning | bitfield | See below |
| 8 | 1 | PFC Fault | bitfield | See below |

**Fault/Warning Bits (Byte 6-7):**
| Bit | Description |
|-----|-------------|
| 0 | Module input undervoltage |
| 1 | Module input phase loss |
| 2 | Module input overvoltage |
| 3 | Module output overvoltage |
| 4 | Module output overcurrent |
| 5 | Module temperature high |
| 6 | Module fan fault |
| 7 | Module hardware fault |
| 8 | Bus exception |
| 9 | SCI communication exception |
| 10 | Discharge fault |
| 11 | PFC shutdown due to exception |
| 12 | Output undervoltage warning |
| 13 | Output overvoltage warning |
| 14 | Power limit due to high temperature |
| 15 | Short circuit fault |

**PFC Fault Bits (Byte 8):**
| Bit | Description |
|-----|-------------|
| 0 | Input overcurrent fault |
| 1 | Mains frequency fault |
| 2 | Mains imbalance fault |
| 3 | DCTz fault |
| 4 | Address conflict |
| 5 | Bus bias |
| 6 | Phase exception fault |
| 7 | Bus overvoltage fault |

**Example:**
```
ID: 0x1801A001
Data: 11 00 A0 0F 10 27 00 00
→ Status: 0x11 (Fault OFF)
→ Voltage: 0x00A0 = 160 * 0.1 = 16.0V
→ Current: 0x0F27 = 3887 * 0.01 = 38.87A
→ Fault: 0x0000 (no fault)
→ PFC: 0x00 (OK)
```

---

### 4.2 M_C_2: Start/Stop Confirm (PGN 0x000200)

**CAN ID:** `0x0802A0xx` (xx = module address)

| Byte | Length | Name | Format |
|------|--------|------|--------|
| 1 | 1 | Command Received | 0x00=Not received, 0x01=Received |
| 2-8 | 7 | Reserved | 0x00 |

**Example:**
```
ID: 0x0802A001
Data: 01 00 00 00 00 00 00 00
→ Command received successfully
```

---

### 4.3 M_C_3: AC Phase Information (PGN 0x000B00)

**CAN ID:** `0x18CBA0xx` (xx = module address)

| Byte | Length | Name | Format | Range |
|------|--------|------|--------|-------|
| 1-2 | 2 | A-phase Voltage | uint16 LE | 0.1V/bit, 0-750V |
| 3-4 | 2 | B-phase Voltage | uint16 LE | 0.1V/bit, 0-750V |
| 5-6 | 2 | C-phase Voltage | uint16 LE | 0.1V/bit, 0-750V |
| 7-8 | 2 | Ambient Temperature | uint16 LE | 1°C/bit |

**Example:**
```
ID: 0x18CBA001
Data: E5 08 E9 08 D7 08 18 00
→ A-phase: 0x08E5 = 227.7V
→ B-phase: 0x08E9 = 228.1V
→ C-phase: 0x08D7 = 226.3V
→ Ambient: 0x0018 = 24°C
```

---

### 4.4 M_C_4: Extended Status/Fault (PGN 0x009100)

**CAN ID:** `0x1991A0xx` (xx = module address)

| Byte | Length | Name | Format |
|------|--------|------|--------|
| 1-2 | 2 | Module Status | bitfield |
| 3-4 | 2 | Fault/Warning | bitfield |
| 5-8 | 4 | Reserved | 0x00 |

**Module Status Bits (Byte 1-2):**
| Bit | Description |
|-----|-------------|
| 0 | Current equalization |
| 1 | Mute |
| 2 | E2 Fault overflow |
| 3 | 0=DC input, 1=AC input |
| 4 | 0=E2 fault enabled, 1=E2 fault disabled |
| 5 | 0=Hot-plug disabled, 1=Hot-plug enabled |

**Extended Fault/Warning Bits (Byte 3-4):**
| Bit | Description |
|-----|-------------|
| 0 | Preceding stage wave stop |
| 1 | Hot-plug fault |
| 2 | CAN communication timeout |
| 4 | Relay operation fault |
| 6 | Internal element overtemperature |
| 7 | Air inlet overtemperature |
| 8 | Input power limit |
| 9 | Power limit due to overtemperature |
| 10 | Discharge changeover exception |
| 11 | Abnormal voltage and current balancing |
| 12 | Heat sink temperature differential protection |
| 13 | Emergency stop |
| 14 | Air inlet temperature too low |
| 15 | Uneven current (double vienna parallel) |

---

### 4.5 C_M_1: Broadcast Start/Stop (PGN 0x000300)

**CAN ID:** `0x0C03FFA0` (broadcast to all)

| Byte | Length | Name | Format |
|------|--------|------|--------|
| 1-3 | 3 | Module Processing Flag | bitfield (modules 1-24) |
| 4 | 1 | Start/Stop | 0x55=Stop, 0xAA=Start |
| 5 | 1 | Group + Multiple | Upper4=Group, Lower4=Multiple |
| 6-8 | 3 | Reserved | 0x00 |

**Module Processing Flag:**
- Bit N = 1: Module address N+1 processes this message
- Supports modules 1-24 per frame

**Example:**
```
ID: 0x0C03FFA0
Data: FF FF FF AA 00 00 00 00
→ All modules 1-24 process
→ Start (0xAA)
→ Group 0 (default), Multiple 0
```

---

### 4.6 C_M_2: Broadcast Parameter Set (PGN 0x000400)

**CAN ID:** `0x1004FFA0`

| Byte | Length | Name | Format | Range |
|------|--------|------|--------|-------|
| 1-3 | 3 | Module Processing Flag | bitfield | |
| 4 | 1 | Group + Multiple | uint8 | |
| 5-6 | 2 | Charging Voltage | uint16 LE | 0.1V/bit, 0-750V |
| 7-8 | 2 | Charging Current | uint16 LE | 0.01A/bit, 0-500A |

**Example:**
```
ID: 0x1004FFA0
Data: FF FF FF 00 88 13 04 10
→ Modules 1-24 process
→ Group 0, Multiple 0
→ Voltage: 0x1388 = 5000 * 0.1 = 500V
→ Current: 0x1040 = 4160 * 0.01 = 41.6A
```

---

### 4.7 C_M_24: Specific Module Start/Stop (PGN 0x000600)

**CAN ID:** `0x0806A0xx` (xx = target module address)

| Byte | Length | Name | Format | Range |
|------|--------|------|--------|-------|
| 1 | 1 | Start/Stop | 0x55=Stop, 0xAA=Start | |
| 2 | 1 | Charging Mode | 0x00=Standby | |
| 3-4 | 2 | Charging Voltage | uint16 LE | 0.1V/bit, 0-1000V |
| 5-6 | 2 | Charging Current | uint16 LE | 0.01A/bit, 0-500A |
| 7-8 | 2 | Reserved | 0x00 | |

**Example:**
```
ID: 0x080601A0
Data: AA 00 88 13 04 10 00 00
→ Start (0xAA)
→ Standby mode (0x00)
→ Voltage: 500V
→ Current: 41.6A
```

---

### 4.8 C_M_23: Module Address Setting (PGN 0x000900)

**CAN ID:** `0x0C09FFA0`

| Byte | Length | Name | Format | Range |
|------|--------|------|--------|-------|
| 1 | 1 | New Address | uint8 | 1-240 |
| 2-8 | 7 | Reserved | 0x00 |

**Example:**
```
ID: 0x0C09FFA0
Data: 02 00 00 00 00 00 00 00
→ Set all modules to address 2
```

---

### 4.9 C_M_12: Address Mode Selection (PGN 0x009000)

**CAN ID:** `0x1C90FFA0`

| Byte | Length | Name | Format |
|------|--------|------|--------|
| 1 | 1 | Mode | 0=Auto, 1=Manual |
| 2-8 | 7 | Reserved | 0x00 |

**Example:**
```
ID: 0x1C90FFA0
Data: 01 00 00 00 00 00 00 00
→ Manual address mode
```

---

## 5. State Machine

### 5.1 Module States

```
+------------------+
|     IDLE         | ← No communication, no output
+--------+---------+
         |
    Start Command
         v
+--------+---------+
|   STARTING        | ← Applying voltage/current
+--------+---------+
         |
    Confirmation
         v
+--------+---------+
|    RUNNING        | ← Active charging
+--------+---------+
         |
    Fault/Stop
         v
+--------+---------+
|    FAULT          | ← Error state
+--------+---------+
         |
    Recovery
         v
+--------+---------+
|    IDLE           |
+------------------+
```

### 5.2 Communication States

- **Online**: Received valid message within timeout (2s)
- **Offline**: No valid message within timeout
- **Recovery**: Attempting to reconnect

## 6. Constants

```c
#define TONHE_ADDR_CONTROLLER    0xA0
#define TONHE_ADDR_BROADCAST     0xFF
#define TONHE_MAX_VOLTAGE_V     750.0f    // V
#define TONHE_MAX_CURRENT_A      500.0f    // A
#define TONHE_VOLTAGE_SCALE     0.1f      // V per bit
#define TONHE_CURRENT_SCALE     0.01f     // A per bit
#define TONHE_OFFLINE_TIMEOUT_MS 2000
#define TONHE_POLL_INTERVAL_MS  20
```

## 7. Alarm Mapping

### TONHE → CHG_ALARM_*

| TONHE Bit | CHG_ALARM Flag |
|-----------|----------------|
| Input undervoltage (byte6 bit0) | CHG_ALARM_AC_UNDER_VOLT |
| Input overvoltage (byte6 bit2) | CHG_ALARM_OVER_VOLTAGE_OUT |
| Output overvoltage (byte6 bit3) | CHG_ALARM_OVER_VOLTAGE_OUT |
| Output overcurrent (byte6 bit4) | CHG_ALARM_HW_FAULT |
| Temperature high (byte6 bit5) | CHG_ALARM_OVER_TEMP |
| Fan fault (byte6 bit6) | CHG_ALARM_HW_FAULT |
| Hardware fault (byte6 bit7) | CHG_ALARM_HW_FAULT |
| Short circuit (byte6 bit15) | CHG_ALARM_SHORT_CIRCUIT |
| SCI comm exception (byte6 bit9) | CHG_ALARM_COMM_FAIL |
| CAN timeout (M_C_4 byte3 bit2) | CHG_ALARM_COMM_FAIL |
| Emergency stop (M_C_4 byte3 bit13) | CHG_ALARM_HW_FAULT |

## 8. Implementation Notes

1. **No Request/Response**: Unlike Maxwell, TONHE uses event-driven messaging
   - Modules send status periodically (500ms) + on change
   - Master sends commands, modules confirm with M_C_2

2. **Broadcast vs Specific**:
   - C_M_1, C_M_2: Broadcast to multiple modules via bitmask
   - C_M_24: Specific module address

3. **Address Assignment**:
   - Auto: Module gets address on power-on (1, 2, 3...)
   - Manual: Set via C_M_23, stored in EEPROM

4. **Parameter Limits**:
   - If Setpoint > Module Max: Module outputs max
   - If Setpoint < Module Min: Module outputs min
