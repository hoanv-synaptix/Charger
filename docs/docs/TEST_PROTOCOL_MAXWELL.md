# Maxwell MXR — Test Protocol

**CAN:** Extended 29-bit, 125Kbps  
**Driver:** `chg_lib_maxwell.c`

---

## CAN ID

```
TX (Controller → Module): 0x0C0060F0 | DST[11:18] | SRC[3:10] | GRP[0:2]
  DST = module address (1-63)
  SRC = 0xF0 (controller)
  GRP = group (0-7)

Example: addr=1, group=0 → ID = 0x0C0060F0

RX (Module → Controller): src_addr trong ID = module addr
  → Extract: src = (ID >> 3) & 0xFF
  → Match với module đã add
```

---

## Data Format (8 bytes)

| Byte | Nội dung | Ví dụ |
|------|----------|-------|
| 0 | Func Code: 0x03=Write, 0x10=Read | 0x03 |
| 1 | Reserved | 0x00 |
| 2-3 | Register address (BE) | 0x00 0x21 |
| 4-7 | Data (IEEE 754 BE float / U32 BE) | 0x44 0x2F 0x00 0x00 |

---

## Key Registers

| Reg | Tên | TX Data (Float→BE) | Mô tả |
|-----|-----|---------------------|--------|
| 0x0021 | SET_VOLTAGE | Float V | Điện áp đặt |
| 0x0022 | SET_CURR_LIMIT | Float ratio | Dòng giới hạn (0.0-1.0) |
| 0x0030 | ON_OFF | U32: 0=Start, 1=Stop | Bật/tắt |
| 0x0001 | OUTPUT_VOLTAGE | — | Đọc V ra |
| 0x0002 | OUTPUT_CURRENT | — | Đọc A ra |
| 0x0004 | TEMP_DCDC | — | Nhiệt độ DCDC |
| 0x000B | TEMP_AMBIENT | — | Nhiệt độ môi trường |
| 0x0040 | ALARM_STATUS | — | Trạng thái lỗi |

---

## Response Format (Module → Controller)

| Byte | Nội dung |
|------|----------|
| 0 | Func Code echo: 0x41=Float, 0x42=U32 |
| 1 | Error: 0xF0=OK, 0xF2=Fail |
| 2-3 | Register (BE) |
| 4-7 | Value (IEEE 754 BE float hoặc U32 BE) |

---

## Test Cases

### 1. Module Online Check
```
TX: ID=0x0C0060F0, Data=10 00 00 01 00 00 00 00  (Read reg 0x0001)
Expect RX: ID=0x0C006001, Data=41 F0 00 01 XX XX XX XX  (Float value)
```

### 2. Set Voltage 700V
```
TX: ID=0x0C0060F0, Data=03 00 00 21 44 2F 00 00  (700.0f = 0x442F0000)
Expect RX: ID=0x0C006001, Data=41 F0 00 21 44 2F 00 00
```

### 3. Set Current Limit 20A (ratio=1.0)
```
TX: ID=0x0C0060F0, Data=03 00 00 22 3F 80 00 00  (1.0f = 0x3F800000)
Expect RX: ID=0x0C006001, Data=41 F0 00 22 3F 80 00 00
```

### 4. Start Module
```
TX: ID=0x0C0060F0, Data=03 00 00 30 00 00 00 00  (ON_OFF=0 = Start)
Expect RX: ID=0x0C006001, Data=41 F0 00 30 00 00 00 00
→ Sau đó đọc reg 0x0002 xem current output
TX: ID=0x0C0060F0, Data=10 00 00 02 00 00 00 00
```

### 5. Stop Module
```
TX: ID=0x0C0060F0, Data=03 00 00 30 00 00 00 01  (ON_OFF=1 = Stop)
Expect RX: ID=0x0C006001, Data=41 F0 00 30 00 00 00 01
```

### 6. Read Alarm Status
```
TX: ID=0x0C0060F0, Data=10 00 00 40 00 00 00 00
Expect RX: ID=0x0C006001, Data=42 F0 00 40 XX XX XX XX  (U32 alarm bits)
```

---

## Debug UART Expected Output

```
[CAN RX] ID:0C006001 Data:41 F0 00 01 44 7A 00 00  (700V response)
[CAN RX] ID:0C006001 Data:41 F0 00 30 00 00 00 00  (Start confirm)
```

---

## IEEE 754 Float Reference

| Giá trị | Hex (BE) |
|---------|----------|
| 0.0 | 00 00 00 00 |
| 1.0 | 00 00 80 3F |
| 10.0 | 00 00 20 41 |
| 20.0 | 00 00 A0 41 |
| 50.0 | 00 00 48 42 |
| 100.0 | 00 00 C8 42 |
| 200.0 | 00 00 48 43 |
| 500.0 | 00 00 FA 43 |
| 700.0 | 00 00 2F 44 |
| 750.0 | 00 00 BC 44 |

---

## Checklist

- [ ] Module respond ID đúng (src_addr extract)
- [ ] Set voltage → module echo đúng giá trị IEEE 754 BE
- [ ] Set current → ratio được apply
- [ ] Start → ON_OFF=0 module confirm
- [ ] Stop → ON_OFF=1 module confirm
- [ ] Read alarm → U32 bits đúng
- [ ] Timeout 1.5s → module offline
