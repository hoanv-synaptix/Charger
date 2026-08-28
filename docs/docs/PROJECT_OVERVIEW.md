# Project Documentation - Hệ Thống Điều Khiển Sạc Pin

## 1. Tổng Quan

**Tên dự án:** Firmware điều khiển sạc pin lithium
**MCU:** STM32F407 (Cortex-M4, 168MHz)
**Giao diện:** CAN1 (module sạc), CAN2 (BMS), USB/TTL (PC Debug App)
**Mục tiêu:** Hỗ trợ nhiều loại module sạc (Maxwell, Lianming, TonHe) với smart charging theo BMS

---

## 2. Kiến Trúc Phần Cứng

```
┌─────────────────────────────────────────────────────────────────────┐
│                         STM32F407VG                                  │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  CAN1 (125Kbps)         CAN2 (250Kbps)         UART/USB           │
│  ┌───────────┐           ┌───────────┐           ┌───────────┐       │
│  │ Charger   │           │    BMS    │           │ PC Debug  │       │
│  │ Modules   │           │    Pin    │           │   App    │       │
│  └─────┬─────┘           └─────┬─────┘           └─────┬─────┘       │
│        │                        │                        │             │
│        ▼                        ▼                        ▼             │
│  ┌─────────┐           ┌───────────┐           ┌───────────┐       │
│  │ Module 1 │           │ 0x02F4   │           │  Debug    │       │
│  │ Module 2 │◄──CAN───►│ 0x04F4   │◄──CAN────│ Protocol  │       │
│  │ Module N │           │ 0x05F4   │           │           │       │
│  └─────────┘           │ 0x18F0F4 │           └───────────┘       │
│                        └───────────┘                                 │
└─────────────────────────────────────────────────────────────────────┘
```

### 2.1 CAN1 - Module Sạc (125Kbps)
- **Baudrate:** 125 Kbps
- **Frame:** Extended 29-bit
- **Protocol:** Maxwell / Lianming / TonHe (selectable at runtime)
- **Địa chỉ module:** 0x01 - 0xFF

### 2.2 CAN2 - BMS Pin (250Kbps)
- **Baudrate:** 250 Kbps
- **Frame:** Standard 11-bit + Extended 29-bit
- **GPIO:** PB12 (RX), PB13 (TX)

### 2.3 UART - PC Debug
- **Baudrate:** 115200 (configurable)
- **Protocol:** Debug frame format (see `pc_debug_protocol.py`)

---

## 3. Kiến Trúc Phần Mềm

### 3.1 Layer Overview

```
┌─────────────────────────────────────────┐
│         PC Debug Application           │
│         (Python + Tkinter)             │
└────────────────┬────────────────────────┘
                 │ USB/UART
┌────────────────▼────────────────────────┐
│        Application Layer               │
│  ┌──────────────────────────────┐     │
│  │  ChargeController          │     │
│  │  (State Machine)          │     │
│  └──────────────────────────────┘     │
│  ┌──────────────────────────────┐     │
│  │  BMS Core / BMS Protocol   │     │
│  │  (Parse + State Machine)  │     │
│  └──────────────────────────────┘     │
└────────────────┬────────────────────────┘
                 │
┌────────────────▼────────────────────────┐
│     Charger Library (libchg_lib.a)    │
│  ┌──────────┐ ┌──────────┐ ┌──────┐│
│  │ Maxwell  │ │ Lianming │ │TonHe ││
│  │  Driver │ │  Driver  │ │Driver││
│  └──────────┘ └──────────┘ └──────┘│
└────────────────────────────────────────┘
```

### 3.2 Cấu Trúc Thư Mục

```
charger/
├── App/                          # Application layer (firmware)
│   ├── Inc/
│   │   ├── charge_controller.h   # Main controller API
│   │   ├── charge_cycle_config.h # Config structure
│   │   ├── bms_core.h           # BMS driver API
│   │   └── pc_debug_protocol.h  # PC communication
│   └── Src/
│       ├── charge_controller.c   # State machine + logic
│       ├── charge_cycle_config.c # Config management
│       ├── bms_core.c           # BMS driver
│       └── pc_debug_protocol.c # PC protocol handler
│
└── lib/chg_lib/                 # Static library
    ├── Inc/
    │   ├── chg_lib.h           # Main interface
    │   └── chg_lib_driver_*.h   # Driver headers
    └── Src/
        ├── chg_lib_core.c      # Driver registry
        ├── chg_lib_can_backend.c # CAN TX/RX
        ├── chg_lib_maxwell.c   # Maxwell driver
        ├── chg_lib_lianming.c  # Lianming driver
        └── chg_lib_tonhe.c     # TonHe driver

debug_app/                       # PC Debug Application
├── main.py                      # Main UI
├── protocol/
│   └── debug_protocol.py        # Protocol parser
├── services/
│   └── serial_service.py        # Serial communication
└── config_samples/
    └── LiFePO4_16S_100Ah.json # Config template
```

---

## 4. Logic Sạc (ChargeController)

### 4.1 State Machine

```
        ┌──────────┐
        │   IDLE   │ (Chưa khởi động)
        └────┬─────┘
             │ Start()
             ▼
        ┌──────────┐
        │  READY   │ (Kiểm tra điều kiện)
        └────┬─────┘
             │ Preconditions OK
             ▼
    ┌───────┴───────┐
    ▼               ▼
┌────────┐    ┌──────────┐
│RUNNING │    │ DERATING │ (Giảm dòng do stage limit)
└────┬───┘    └────┬─────┘
     │ Stop()      │ Derating end
     ▼             ▼
┌──────────┐    ┌────────┐
│ STOPPING │    │RUNNING │
└────┬─────┘    └────────┘
     │ Complete
     ▼
┌──────────┐
│   FAULT  │ (Lỗi)
└──────────┘
```

### 4.2 Quy Trình Tính Dòng Sạc

**Nguyên tắc:** System chọn **giá trị nhỏ nhất** từ các nguồn giới hạn:

```
Dòng sạc = MIN( BMS request, Stage limits )

Trong đó Stage limits = MIN( Cell Voltage Stage, Temperature Stage, SOC Stage )
```

### 4.3 Stage Evaluation Logic

Mỗi stage có thể enable/disable và có 5 bands:

**Cell Voltage Stage:**
- `cell_volt_enabled`: Bật/tắt
- `cell_volt_1_v` - `cell_volt_5_v`: Ngưỡng cell voltage (V)
- `cell_curr_1_c` - `cell_curr_4_c`: Dòng giới hạn theo C-rate

**Temperature Stage:**
- `temp_enabled`: Bật/tắt
- `temp_1_c` - `temp_5_c`: Ngưỡng nhiệt độ (°C)
- `temp_curr_1_c` - `temp_curr_4_c`: Dòng giới hạn theo C-rate

**SOC Stage:**
- `soc_enabled`: Bật/tắt
- `soc_1_pct` - `soc_5_pct`: Ngưỡng SOC (%)
- `soc_curr_1_c` - `soc_curr_4_c`: Dòng giới hạn theo C-rate

### 4.4 Ví Dụ Tính Dòng

```
BMS Request: 20A
Cell Voltage: 3.45V → Band 3 (cell_curr_3_c = 0.5C = 50A)
Temperature: 30°C → Band 2 (temp_curr_2_c = 1.0C = 100A)
SOC: 85% → Band 2 (soc_curr_2_c = 0.8C = 80A)

Stage Limits:
- Cell: 50A
- Temp: 100A
- SOC: 80A

→ MIN(50, 100, 80) = 50A
→ MIN(20, 50) = 20A

Kết quả: Dòng sạc = 20A
```

### 4.5 Inhibit (Block Charging)

Khi **bất kỳ** stage nào ở trạng thái:
- `BELOW_MIN`: Cell/Temp/SOC thấp hơn ngưỡng tối thiểu
- `ABOVE_MAX`: Cell/Temp/SOC cao hơn ngưỡng tối đa

→ System set `inhibit = 1` → **Dừng sạc ngay lập tức**

---

## 5. BMS Integration

### 5.1 BMS Messages (CAN2)

| ID | Frame Type | Tên | Cycle | Mô Tả |
|----|------------|-----|-------|--------|
| 0x02F4 | Standard | BATT_ST1 | 20ms | Voltage, Current, SOC |
| 0x04F4 | Standard | CELL_VOLT | 100ms | Max/Min cell voltage |
| 0x05F4 | Standard | CELL_TEMP | 500ms | Max/Min/Avg temperature |
| 0x07F4 | Standard | ALM_INFO | Event | Alarm flags |
| 0x18F128F4 | Extended | BATT_ST2 | 100ms | Capacity, SOH |
| 0x1806E5F4 | Extended | ChgRequest | 1000ms | **BMS yêu cầu V/I** |
| 0x18F528F4 | Extended | BmsSwSta | 500ms | Relay status |

### 5.2 BMS Control (Ctrl_INFO)

Firmware gửi lệnh điều khiển đến BMS:
- **ID:** 0x18F0F428
- **Content:** Mask + Control bits (charge on/off, discharge on/off)
- **Cycle:** 500ms (keep-alive)

---

## 6. PC Debug Application

### 6.1 Giao Diện

```
┌─────────────────────────────────────────────────────────────────────┐
│  [Connect] [Baud:115200▼] [COM29]           Charger Debug v1.0    │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌───────────────┐  ┌──────────────────────────────────────────┐ │
│  │ Module Manager │  │ Module Details / Monitor                    │ │
│  │               │  │                                            │ │
│  │ Driver: [▼]   │  │ Voltage: 54.6V  Current: 20.0A          │ │
│  │ Addr: [0x01] │  │ State: Running  Temp: 45°C              │ │
│  │ [Add] [Remove]│  │                                            │ │
│  │               │  │                                            │ │
│  │ Modules:      │  │                                            │ │
│  │ [✓] Max 0x01 │  │                                            │ │
│  │     Ton 0x02  │  │                                            │ │
│  └───────────────┘  └──────────────────────────────────────────┘ │
│                                                                     │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │ Charge Configuration (212 bytes)                            │   │
│  │                                                              │   │
│  │ General:  Battery: [100.0] Ah  I_min: [0.1]  I_max: [1.0]│   │
│  │ Cell Stage: [✓] Enabled                                     │   │
│  │   V1: [3.2]V I1: [1.0]C  V2: [3.3]V I2: [0.8]C        │   │
│  │   V3: [3.4]V I3: [0.5]C  V4: [3.5]V I4: [0.3]C        │   │
│  │                                                              │   │
│  │ [Import] [Export] [Read MCU] [Write MCU] [Defaults]         │   │
│  └──────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

### 6.2 Commands

| CMD | Name | Description |
|-----|------|-------------|
| 0x01 | READ_ALL | Request all modules data |
| 0x02 | READ_BMS | Request BMS snapshot |
| 0x10 | SET_DRIVER | Select module driver |
| 0x11 | SET_MODULE_ADDR | Add module |
| 0x12 | SET_VOLTAGE | Set output voltage |
| 0x13 | SET_CURRENT | Set current limit |
| 0x14 | START | Start charging |
| 0x15 | STOP | Stop charging |
| 0x19 | GET_CHARGE_CFG | Read config from MCU |
| 0x1A | SET_CHARGE_CFG | Write config to MCU |

### 6.3 Config Management

**Config Structure (212 bytes):**
- Battery: capacity, voltage/current limits
- Cell Voltage Stage: 5 thresholds + 4 current limits
- Temperature Stage: 5 thresholds + 4 current limits
- SOC Stage: 5 thresholds + 4 current limits
- Protection: cell, jack voltage, jack temp
- Module: type, count, min/max V/I

**Storage:**
- RAM: Active config (used by firmware)
- Flash: Persistent config (sector 7, 0x08060000)

---

## 7. Cấu Hình Mẫu

### 7.1 LiFePO4 16S 100Ah

```json
{
  "version": 2,
  "battery_capacity_ah": 100.0,
  "imax_c": 1.0,
  "cell_volt_enabled": true,
  "cell_volt_1_v": 3.2,
  "cell_volt_2_v": 3.3,
  "cell_volt_3_v": 3.4,
  "cell_volt_4_v": 3.5,
  "cell_volt_5_v": 3.6,
  "cell_curr_1_c": 1.0,
  "cell_curr_2_c": 0.8,
  "cell_curr_3_c": 0.5,
  "cell_curr_4_c": 0.3,
  "temp_enabled": true,
  "temp_1_c": 10.0,
  "temp_2_c": 25.0,
  "temp_3_c": 40.0,
  "temp_4_c": 50.0,
  "temp_5_c": 55.0,
  "temp_curr_1_c": 1.0,
  "temp_curr_2_c": 1.0,
  "temp_curr_3_c": 0.8,
  "temp_curr_4_c": 0.3,
  "soc_enabled": true,
  "soc_1_pct": 10.0,
  "soc_2_pct": 90.0,
  "soc_3_pct": 95.0,
  "soc_4_pct": 98.0,
  "soc_5_pct": 100.0,
  "soc_curr_1_c": 1.0,
  "soc_curr_2_c": 0.8,
  "soc_curr_3_c": 0.5,
  "soc_curr_4_c": 0.2,
  "module_type": 4,
  "source_module_count": 1
}
```

---

## 8. Troubleshooting

### 8.1 Không Sạc Dù Đã Start

| Check | Action |
|-------|--------|
| Module online? | Kiểm tra CAN1 wiring, address |
| BMS online? | Kiểm tra CAN2 wiring |
| Stage inhibit? | Xem log "inhibit=X" |
| Current = 0? | Kiểm tra `imax_c`, `battery_capacity_ah` |
| Config loaded? | Check "Charge config loaded from MCU" |

### 8.2 Log Indicators

```
CC: Start src=1 act=1          # Module counts
CC: State 0->1                  # IDLE → READY
CC: State 1->2                  # READY → RUNNING
CC: Start V=54.6 I=20.0A/mod   # Charging started
CC: Stop inhibit=1 derating=0  # Stopped - check inhibit
CC: BMS offline                 # BMS not responding
```

### 8.3 Common Causes

1. **"inhibit=1"**: Stage đang block - kiểm tra SOC/cell/temp thresholds
2. **"BMS offline"**: CAN2 wiring hoặc BMS không gửi data
3. **"Module count changed"**: Module offline trong khi chạy

---

## 9. Build & Flash

### 9.1 Firmware

```bash
# Build
cd charger/build2
cmake -G Ninja ..
ninja

# Output: charger.elf
```

### 9.2 Debug App

```bash
cd debug_app
python main.py
```

---

## 10. File Reference

| File | Purpose |
|------|---------|
| `charger/App/Src/charge_controller.c` | Main charging logic |
| `charger/App/Src/bms_core.c` | BMS driver |
| `charger/App/Src/charge_cycle_config.c` | Config management |
| `charger/lib/chg_lib/Src/chg_lib_*.c` | Module drivers |
| `debug_app/main.py` | PC App UI |
| `debug_app/protocol/debug_protocol.py` | Protocol parser |
| `debug_app/config_samples/*.json` | Config templates |

---

*Last updated: 2026-07-11*
