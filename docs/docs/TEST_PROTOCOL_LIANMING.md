# Lianming V2.0 - Test Protocol

**CAN:** Extended 29-bit, 125 kbit/s  
**Driver:** `charger/lib/chg_lib/Src/chg_lib_lianming.c`

Nguồn kiểm tra: `docs/lianming.txt` trích từ PDF Lianming V2.0.

---

## CAN ID

```text
TX common: 0x1907C080 | addr
RX common: 0x1807C080 | addr

AC read TX:   0x1907A080 | addr
AC read RX:   0x1807A080 | addr

Temp read TX: 0x19008080 | addr
Temp read RX: 0x18008080 | addr
```

`addr = 1..60`. Address `0` là broadcast command và không có response.

Ví dụ:

```text
addr 1: TX common = 0x1907C081, RX common = 0x1807C081
addr 3: TX common = 0x1907C083, RX common = 0x1807C083
```

---

## Command Frames

### Set Output, CMD=0x00

```text
Byte0    = 0x00
Byte1-3  = Current mA, big-endian 24-bit
Byte4-7  = Voltage mV, big-endian 32-bit
```

Ví dụ set 100 V, 90 A:

```text
TX ID: 0x1907C083
DATA:  00 01 5F 90 00 01 86 A0
```

### Read Status, CMD=0x01

```text
TX ID: 0x1907C081
DATA:  01 00 00 00 00 00 00 00
```

### Power On/Off, CMD=0x02

Theo PDF, byte điều khiển nằm ở **Byte7**.

```text
Power ON:
TX ID: 0x1907C081
DATA:  02 00 00 00 00 00 00 55

Power OFF:
TX ID: 0x1907C081
DATA:  02 00 00 00 00 00 00 AA
```

---

## Status Response

Ví dụ PDF cho module 2:

```text
RX ID: 0x1807C082
DATA:  01 00 01 17 03 E8 00 00
```

Format:

```text
Byte0    = CMD echo, 0x01
Byte1    = reserved trong ví dụ read-status
Byte2-3  = output current, A * 10, big-endian
Byte4-5  = output voltage, V * 10, big-endian
Byte6-7  = status flags, big-endian
```

Ví dụ trên:

```text
current = 0x0117 / 10 = 27.9 A
voltage = 0x03E8 / 10 = 100.0 V
flags   = 0x0000
```

### Status Flags

`raw = (Byte6 << 8) | Byte7`.

Byte7:

```text
bit7 output undervoltage
bit6 output overvoltage
bit5 input undervoltage
bit4 input overvoltage
bit3 fan fault
bit2 constant current mode
bit1 module fault
bit0 module off, 0 = running
```

Byte6:

```text
bit7 set shutdown
bit6 overtemperature protection
bit5 overcurrent protection
```

---

## Diagnostic Reads

### AC Input Voltage

```text
TX ID: 0x1907A081
DATA:  31 00 00 00 00 00 00 00

RX ID: 0x1807A081
DATA:  31 xx VabH VabL VbcH VbcL VcaH VcaL
```

Scale:

```text
Vab = raw / 32
Vbc = raw / 32
Vca = raw / 32
```

### Environment Temperature

```text
TX ID: 0x19008081
DATA:  00 00 00 00 00 00 00 00

RX ID: 0x18008081
DATA:  xx xx xx xx TempH TempL xx xx
```

Scale:

```text
temperature = raw / 10 deg C
```

---

## Checklist

- [ ] Start/stop dùng Byte7, không dùng Byte6.
- [ ] RX common ID đúng `0x1807C080 | addr`.
- [ ] Read-status không yêu cầu Byte1 phải là `0xFF`.
- [ ] Current parse từ Byte2-3, chia 10.
- [ ] Voltage parse từ Byte4-5, chia 10.
- [ ] Running khi Byte7 bit0 = 0.
- [ ] Stopped khi Byte7 bit0 = 1.
- [ ] Temp parse từ Byte4-5, chia 10.
- [ ] Timeout nội bộ firmware hiện dùng 2 s để phát hiện sớm; PDF ghi 2 phút mất monitoring command.
