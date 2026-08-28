# TonHe V1.3 — Test Protocol

**CAN:** Extended 29-bit (SAE J1939), 125Kbps  
**Driver:** `chg_lib_tonhe.c`

---

## CAN ID — J1939 Format

```
| Priority(3) | R(1) | DP(1) | PF(8) | PS(8) | SA(8) |
  P=Priority   | 0    | 0     | PDU Format | Dest/Group | Src Addr
```

### ID Builder (tonhe_build_id)

```
ID = (priority << 26) | (pf << 16) | (ps << 8) | sa
```

### Module Address

```
Controller: 0xA0
Module: 0x01 – 0xF0
Broadcast: 0xFF
```

---

## Uplink — Module → Controller

| PGN | Name | ID (SA=module) | Data |
|-----|------|-----------------|------|
| 0x000100 | M_C_1 Status | 0x1801A0**SA** | Byte0=Status, Byte1-2=V(0.1V LE), Byte3-4=I(0.01A LE), Byte5-6=Fault, Byte7=PFC |
| 0x000200 | M_C_2 Confirm | 0x1802A0**SA** | Byte0=0x01 (OK) |
| 0x000B00 | M_C_3 AC Phase | 0x180BA0**SA** | Byte0-1=Va, Byte2-3=Vb, Byte4-5=Vc, Byte6-7=Temp |
| 0x009100 | M_C_4 Extended | 0x1891A0**SA** | Byte3-4=ExtFault (LE) |

### Status Values (Byte 0)

| Giá trị | Ý nghĩa |
|---------|---------|
| 0x00 | Normal Off |
| 0x01 | On (Running) |
| 0x11 | Fault Off |

---

## Downlink — Controller → Module

| PGN | Name | Priority | ID | Byte Layout |
|-----|------|----------|----|-------------|
| 0x000400 | C_M_2 Param Set | 4 | 0x1004FFA0 | B0-2=0xFF, B3=Group, B4-5=V(0.1V LE), B6-7=I(0.01A LE) |
| 0x000600 | C_M_24 Spec Start/Stop | 2 | 0x0806**SA**A0 | B0=Cmd(0xAA/0x55), B1=Mode, B2-3=V(0.1V LE), B4-5=I(0.01A LE) |

**Note:** C_M_2 broadcast param → all modules process. C_M_24 specific → only target module.

---

## Data Format

### C_M_2 Broadcast Parameter Set (0x1004FFA0)

| Byte | Nội dung | Ví dụ (500V, 100A) |
|------|----------|---------------------|
| 0-2 | Processing flag (all modules) | FF FF FF |
| 3 | Group + Multiple | 0x00 |
| 4-5 | Voltage (0.1V/bit, LE U16) | 0x88 0x13 (5000 = 500V) |
| 6-7 | Current (0.01A/bit, LE U16) | 0x10 0x27 (10000 = 100A) |

### C_M_24 Specific Start/Stop (0x0806**SA**A0)

| Byte | Nội dung | Ví dụ (Start 500V/100A) |
|------|----------|-------------------------|
| 0 | Command: 0xAA=Start, 0x55=Stop | 0xAA |
| 1 | Mode (Standby) | 0x00 |
| 2-3 | Voltage (0.1V/bit, LE U16) | 0x88 0x13 (500V) |
| 4-5 | Current (0.01A/bit, LE U16) | 0x10 0x27 (100A) |
| 6-7 | Reserved | 0x00 0x00 |

---

## Test Cases

### 1. Module Online — Status Check (M_C_1)
```
Inject: ID=0x1801A001, Data=01 88 13 10 27 00 00 00
→ Status=0x01 (running), Voltage=500V, Current=100A
→ Debug UART: [CAN RX] ID:1801A001 Data:01 88 13 10 27 00 00 00
```

### 2. Inject Status Offline
```
Inject: ID=0x1801A001, Data=00 00 00 00 00 00 00 00
→ Status=0x00 (normal off), online=false
```

### 3. Inject Fault
```
Inject: ID=0x1801A001, Data=11 00 00 00 04 00 00 00
→ Status=0x11 (fault off), alarm bit4=1 (overcurrent)
```

### 4. Confirm Message (M_C_2)
```
Inject: ID=0x1802A001, Data=01 00 00 00 00 00 00 00
→ Byte0=0x01: command accepted
```

### 5. AC Phase (M_C_3)
```
Inject: ID=0x180BA001, Data=88 13 70 17 58 1B 00 00
→ Va=0x1388/10=500V, Vb=0x1770/10=600V, Vc=0x1B58/10=700V
```

### 6. Extended Fault (M_C_4)
```
Inject: ID=0x1891A001, Data=00 00 00 04 40 00 00 00
→ Byte3-4 = 0x0400 (bit10=Discharge fault)
```

### 7. Driver TX: Set Param + Start (driver sends)
```
Driver TX: ID=0x1004FFA0, Data=FF FF FF 00 88 13 10 27
→ Broadcast param set: 500V, 100A

Driver TX: ID=0x080601A0, Data=AA 00 88 13 10 27 00 00
→ Start module 1: 500V, 100A

Expect: Module respond M_C_2 (0x1802A001) Data:01...
Expect: Module respond M_C_1 (0x1801A001) with running status
```

### 8. Driver TX: Stop
```
Driver TX: ID=0x080601A0, Data=55 00 00 00 00 00 00 00
→ Stop: cmd=0x55
Expect: Module respond M_C_1 with status=0x00 (normal off)
```

---

## Common Values

| Voltage | Raw (×10) | LE Bytes |
|---------|-----------|----------|
| 50V | 500 | 0xF4 0x01 |
| 100V | 1000 | 0xE8 0x03 |
| 200V | 2000 | 0xD0 0x07 |
| 500V | 5000 | 0x88 0x13 |
| 700V | 7000 | 0x58 0x1B |

| Current | Raw (×100) | LE Bytes |
|---------|-----------|----------|
| 10A | 1000 | 0xE8 0x03 |
| 20A | 2000 | 0xD0 0x07 |
| 50A | 5000 | 0x88 0x13 |
| 100A | 10000 | 0x10 0x27 |

---

## Fault Bit Reference

**Byte 5-6 (Fault/Warning):**

| Bit | Ý nghĩa |
|-----|---------|
| 0 | Input undervoltage |
| 1 | Input phase loss |
| 2 | Input overvoltage |
| 3 | Output overvoltage |
| 4 | Output overcurrent |
| 5 | Temperature high |
| 6 | Fan fault |
| 7 | Hardware fault |
| 8 | Bus exception |
| 9 | SCI comm fault |
| 10 | Discharge fault |
| 11 | PFC shutdown |
| 15 | Short circuit |

**Byte 7 (PFC Fault):**

| Bit | Ý nghĩa |
|-----|---------|
| 0 | Input overcurrent |
| 1 | Mains frequency fault |
| 2 | Mains imbalance |
| 7 | Bus overvoltage |

**Byte 3-4 Extended (SPN 2817):**

| Bit | Ý nghĩa |
|-----|---------|
| 2 | CAN timeout |
| 6 | Internal overtemperature |
| 7 | Air inlet overtemperature |
| 13 | Emergency stop |

---

## Checklist

- [ ] M_C_1 (0x1801A0**SA**) → parse voltage/current LE
- [ ] M_C_2 (0x1802A0**SA**) → Byte0=0x01 confirm
- [ ] M_C_3 (0x180BA0**SA**) → AC phase parse
- [ ] M_C_4 (0x1891A0**SA**) → extended fault at Byte3-4
- [ ] C_M_2 broadcast param → all modules update
- [ ] C_M_24 specific → only target module respond
- [ ] Start (0xAA) → module running
- [ ] Stop (0x55) → module idle
- [ ] Fault status → alarm flags set
- [ ] Timeout 2s → module offline
