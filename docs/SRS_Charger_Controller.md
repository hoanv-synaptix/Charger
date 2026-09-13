# Software Requirements Specification
# Bộ Điều Khiển Trạm Sạc — Charger Controller

| | |
|---|---|
| **Mã tài liệu** | SRS-CHG-CTRL-001 |
| **Phiên bản** | 1.0 |
| **Ngày phát hành** | 2026-08-26 |
| **Trạng thái** | Released |
| **Chuẩn áp dụng** | IEEE 830-1998, ISO/IEC/IEEE 29148:2018 |
| **Firmware tương ứng** | v2.0.0 |
| **Hardware tương ứng** | CHARGER_CTRL Ver1.0 (Schematic 2026-08-23) |

## Lịch sử thay đổi

| Ver | Ngày | Người thực hiện | Mô tả |
|-----|------|-----------------|-------|
| 1.0 | 2026-08-26 | — | Phát hành lần đầu, đồng bộ FW v2.0.0 |

---

## Mục lục

1. [Giới thiệu](#1-giới-thiệu-introduction)
2. [Mô tả tổng quan](#2-mô-tả-tổng-quan-overall-description)
3. [Yêu cầu chức năng](#3-yêu-cầu-chức-năng-functional-requirements)
4. [Yêu cầu giao diện ngoài](#4-yêu-cầu-giao-diện-ngoài-external-interface-requirements)
5. [Yêu cầu phi chức năng](#5-yêu-cầu-phi-chức-năng-non-functional-requirements)
6. [Ràng buộc thiết kế và đặc tả dữ liệu](#6-ràng-buộc-thiết-kế-và-đặc-tả-dữ-liệu)
7. [Phụ lục](#7-phụ-lục)

---

## 1. Giới thiệu (Introduction)

### 1.1 Mục đích (Purpose)

Tài liệu này đặc tả đầy đủ yêu cầu phần mềm (firmware) của **Bộ Điều Khiển Trạm Sạc (Charger Controller)** — hệ thống nhúng điều khiển các module sạc công suất qua bus CAN, giao tiếp với BMS (Battery Management System) để quản lý chu trình sạc pin, đồng thời cung cấp giao diện điều khiển/giám sát cho ứng dụng PC và màn HMI.

Tài liệu dành cho: kỹ sư firmware, kỹ sư phần cứng, kỹ sư test, và đội phát triển ứng dụng PC/HMI.

### 1.2 Phạm vi (Scope)

**Trong phạm vi:**
- Firmware chạy trên MCU STM32G0B1CBT6 của board CHARGER_CTRL Ver1.0
- Điều khiển tối đa 8 module sạc DC qua **CAN1 (125 kbps)** với 3 họ module: Maxwell, Lianming, TonHe
- Thu thập và xử lý dữ liệu BMS qua **CAN2 (250 kbps)**
- Máy trạng thái điều phối chu trình sạc (3 chế độ: Manual, Standalone No-BMS, BMS-Controlled)
- Giao thức điều khiển nhị phân qua USB CDC với ứng dụng PC
- Lưu trữ cấu hình chu kỳ sạc trong flash nội bộ
- Giao diện HMI DWIN qua RS485 (phần giao thức, tích hợp ứng dụng đang phát triển)

**Ngoài phạm vi:**
- Ứng dụng PC (chỉ đặc tả giao thức giao tiếp)
- Thiết kế phần cứng (tham khảo schematic riêng)
- Thiết kế cơ khí, nguồn công suất
- Kết nối cloud/IoT (dự phòng UART5 cho LTE, chưa triển khai)

### 1.3 Định nghĩa và từ viết tắt

| Thuật ngữ | Ý nghĩa |
|---|---|
| **BMS** | Battery Management System — hệ thống quản lý pin |
| **CAN/FDCAN** | Controller Area Network / Flexible Data-rate CAN |
| **CDC** | Communications Device Class — lớp USB ảo COM |
| **C-rate** | Tốc độ sạc tương đối dòng định mức pin (1C = sạc đầy trong 1h) |
| **DCDC / PFC** | Giai đoạn biến áp DC / Power Factor Correction của module sạc |
| **DE** | Driver Enable — chân điều hướng truyền/nhận RS485 |
| **FSM** | Finite State Machine — máy trạng thái hữu hạn |
| **HMI** | Human-Machine Interface — màn hình điều khiển (DWIN) |
| **LEC** | Last Error Code — mã lỗi protocol CAN gần nhất |
| **OVP / OCP** | Over-Voltage / Over-Current Protection |
| **SOC / SOH** | State of Charge / State of Health |
| **SWAP** | Bit hoán đổi TX↔RX nội bộ peripheral USART (USART_CR2.SWAP) |
| **Bus-off** | Trạng thái lỗi CAN: TEC ≥ 256, controller ngừng truyền |

### 1.4 Tài liệu tham khảo

| Mã | Tài liệu |
|---|---|
| [R1] | Schematic `CHARGER_CTRL_Ver1.0_Schematic_2026-08-23.PDF` |
| [R2] | Maxwell — CAN Communication Protocol V1.50 |
| [R3] | Lianming Power Digital Power Module CAN Communication Protocol V2.0 |
| [R4] | TonHe — CAN communication between charging module and monitor V1.3 (J1939) |
| [R5] | CAN BMS_BB_PKG V1.0 (Jikong BMS protocol) |
| [R6] | ST — RM0444: STM32G0 Series Reference Manual |
| [R7] | ST — STM32Cube FW_G0 V1.6.3 HAL |

### 1.5 Tổng quan tài liệu

Mục 2 mô tả bối cảnh và kiến trúc tổng thể. Mục 3 đặc tả yêu cầu chức năng theo từng phân hệ với mã định danh. Mục 4 đặc tả chi tiết giao diện giao thức. Mục 5 đặc tả yêu cầu phi chức năng. Mục 6 đặc tả cấu trúc dữ liệu và ràng buộc thiết kế. Mục 7 là phụ lục.

---

## 2. Mô tả tổng quan (Overall Description)

### 2.1 Bối cảnh sản phẩm

Hệ thống là **trung tâm điều khiển** của một trạm sạc pin (xe điện/thiết bị di động):

```
┌──────────┐  USB CDC   ┌───────────────────────────────────┐  CAN1 125K  ┌───────────────┐
│  PC App  │◄──────────►│         STM32G0B1CBT6             │◄───────────►│  1–8 module   │
└──────────┘  2 chiều  │                                   │  ext-frame  │  sạc DC       │
┌──────────┐  RS485     │  PC Protocol ──► Charge           │             │ Maxwell /     │
│ DWIN HMI │◄──────────►│  Debug Protocol    Controller     │             │ Lianming /    │
└──────────┘            │                     ▲    │        │             │ TonHe         │
┌──────────┐  CAN2 250K │  BMS core ──────────┘    ▼        │             └───────────────┘
│   BMS    │◄──────────►│  (telemetry)     chg_lib ─────────│
└──────────┘            │  Flash cfg | Buttons | LED        │
                        └───────────────────────────────────┘
```

### 2.2 Nền tảng phần cứng

| Thành phần | Đặc tả |
|---|---|
| MCU | STM32G0B1CBT6, LQFP48, Cortex-M0+ @ 64 MHz (HSI+PLL) |
| RAM / Flash | 144 KB RAM / 128 KB Flash |
| FDCAN1 | PD0=RX, PD1=TX, **125 kbps** — bus module sạc (cô lập CA-IS3050CG U301) |
| FDCAN2 | PB12=RX, PB13=TX, **250 kbps** — bus BMS (cô lập CA-IS3050CG U302) |
| USART3 | PB10=TX, PB11=RX, 115200-8N1, **SWAP=ENABLE** (phần cứng đảo RO/DI) — RS485 qua CS485M (U300), DE=PB1 |
| USART1 | PA9=TX — debug log 115200 (header J6) |
| USART5 | PB3/PB4 — dự phòng LTE |
| USB | FS Device, CDC class (PA11/PA12) |
| ADC1 | PA0–PA3 — 4 kênh NTC nhiệt độ |
| GPIO | RELAY_1/2/3 (PB14, PB15, PA8), LED_RUN (PC6), LED_FAULT (PC7), LED_STT (PC13), BUTTON_1 (PA15), BUTTON_2 (PD2), POWER_EN (PA4) |
| Nguồn cô lập | B1205S-1WR2 ×2 → +5V_ISO1/2 cho transceiver CAN; PA4=POWER_EN cấp nguồn cụm RS485/HMI |

> ⚠️ **Ghi chú thiết kế bắt buộc**: USART3 của board này có đường RO/DI **đảo ngược** so với mapping silicon mặc định (PB10=TX, PB11=RX). Firmware **phải** bật `USART_CR2.SWAP` (TX phát qua PB11 vào DI, RX nhận từ PB10 qua RO). Mất cấu hình này RS485 sẽ không hoạt động.

### 2.3 Chức năng chính

| Mã | Chức năng |
|---|---|
| F1 | Quản lý module sạc đa giao thức (đăng ký/chọn driver runtime, tối đa 8 module) |
| F2 | Điều phối chu trình sạc theo máy trạng thái, 3 chế độ nguồn điều khiển |
| F3 | Đọc dữ liệu BMS, giám sát kết nối, xử lý alarm |
| F4 | Điều khiển dòng sạc động theo band cell-voltage / nhiệt độ / SOC |
| F5 | Bảo vệ cứng (jack voltage, emergency stop, mismatch module) |
| F6 | Giao tiếp PC qua USB CDC (điều khiển, giám sát, cấu hình, debug) |
| F7 | Lưu/khôi phục cấu hình chu kỳ sạc trong flash (CRC32, wear-leveling) |
| F8 | Giao diện HMI DWIN qua RS485 (giao thức sẵn sàng) |
| F9 | Tự phục hồi CAN bus-off, tự re-arm UART sau lỗi framing |

### 2.4 Đặc điểm người dùng

| Người dùng | Tương tác |
|---|---|
| Ứng dụng PC | Điều khiển toàn diện qua USB CDC: chọn driver, thêm module, set điểm, start/stop, cấu hình, đọc telemetry |
| Vận hành tại chỗ | Nút START/STOP vật lý, quan sát LED |
| Màn HMI DWIN | Hiển thị + nút lệnh qua RS485 |

### 2.5 Ràng buộc (Constraints)

| Mã | Ràng buộc |
|---|---|
| C-01 | Chạy bare-metal, không RTOS; vòng lặp chính non-blocking |
| C-02 | Flash 128 KB, RAM 144 KB |
| C-03 | CAN1 bắt buộc 125 kbps (theo spec 3 họ module sạc); CAN2 bắt buộc 250 kbps (theo spec BMS) |
| C-04 | Chỉ dùng CAN 2.0 Classic frame (không FD, không BRS) |
| C-05 | USART3 phải giữ SWAP=ENABLE do đặc thù phần cứng (mục 2.2) |
| C-06 | Cấu hình flash chỉ ghi trang 63 (0x0801F800, 2 KB), ghi theo double-word |
| C-07 | Mọi thay đổi qua CubeMX regeneration phải bảo toàn: SWAP USART3, Prescaler CAN1=32, ExtFiltersNbr=1, AR=ENABLE |

### 2.6 Giả định và phụ thuộc

- Module sạc và BMS đã được cấu hình địa chỉ/định dạng đúng theo [R2]–[R5]
- Nguồn +12V của board phải có mặt để các rail cô lập +5V_ISO hoạt động
- PC app chịu trách nhiệm retry khi nhận NACK

---

## 3. Yêu cầu chức năng (Functional Requirements)

Quy ước độ ưu tiên: **M** = Must, **S** = Should, **C** = Could.

### 3.1 Giao tiếp PC (USB CDC) — FR-USB

| ID | Yêu cầu | ƯP |
|---|---|---|
| FR-USB-01 | Cung cấp kênh USB CDC ảo COM trao đổi khung nhị phân theo mục 4.1 | M |
| FR-USB-02 | Parser RX là FSM byte-by-byte, đồng bộ lại ở SOF khi lỗi; kiểm tra CRC8 trước khi xử lý | M |
| FR-USB-03 | Lệnh sai CRC / sai độ dài / không nhận diện → NACK kèm mã lỗi | M |
| FR-USB-04 | Response qua hàng đợi TX tối đa 8 frame, gửi tuần tự khi CDC rảnh, không chặn main loop | M |
| FR-USB-05 | PING → PONG kèm version FW | M |
| FR-USB-06 | START kiểm tra điều kiện tiền tố trước; thất bại → NACK | M |
| FR-USB-07 | SET_VOLTAGE/SET_CURRENT bị từ chối khi chu trình auto đang chạy (không phải manual) | M |
| FR-USB-08 | SET_DRIVER chỉ chấp nhận ID đã đăng ký; phải SET_MODULE_ADDR sau khi chọn driver | M |
| FR-USB-09 | READ_REG đọc field telemetry module theo bảng mục 4.1.4 | S |
| FR-USB-10 | Debug protocol 0x10–0x1A (mục 4.1.5) | S |

### 3.2 Quản lý module sạc (chg_lib) — FR-CHG

| ID | Yêu cầu | ƯP |
|---|---|---|
| FR-CHG-01 | Lớp trừu tượng `CHG_LIB_DriverOps_t`; driver đăng ký runtime, PC chọn qua SET_DRIVER | M |
| FR-CHG-02 | 3 driver: Maxwell (id 1), Lianming (id 2), TonHe (id 3); tối đa 8 module/driver | M |
| FR-CHG-03 | FSM mỗi module: IDLE → STARTING → RUNNING → STOPPING; kèm WARNING/OFFLINE/FAULT/RECOVERING | M |
| FR-CHG-04 | Trình tự START: set V → set I-limit → ON → RUNNING chỉ khi có áp output > 0; quá 3 lần → FAULT | M |
| FR-CHG-05 | RUNNING: poll 15 thanh ghi telemetry round-robin | M |
| FR-CHG-06 | Watchdog: mất RX 2s → WARNING; 10s → OFFLINE; OFFLINE 3s → RECOVERING (cần ≥5 RX) | M |
| FR-CHG-07 | Alarm critical (short-circuit, DCDC OV/OT, hw fault…) → FAULT + STOP ngay | M |
| FR-CHG-08 | I-limit chuẩn hóa theo rated current (fallback 20 A khi chưa cấu hình) | M |
| FR-CHG-09 | EmergencyStop: STOP toàn bộ module tức thì | M |
| FR-CHG-10 | Telemetry chuẩn hóa `CHG_LIB_ModuleView_t`; alarm raw + flag chuẩn hóa | M |
| FR-CHG-11 | CAN backend tách lớp cho phép mock khi test | C |

### 3.3 BMS — FR-BMS

| ID | Yêu cầu | ƯP |
|---|---|---|
| FR-BMS-01 | Parse toàn bộ frame BMS (mục 4.3), cache `BMS_Data_t`, quy đổi raw → vật lý | M |
| FR-BMS-02 | Frame lạ / sai DLC không được refresh watchdog kết nối | M |
| FR-BMS-03 | ≥5s không có frame hợp lệ → OFFLINE + xóa cache | M |
| FR-BMS-04 | Data-quality được tính theo timestamp từng frame định kỳ; `ALM_INFO` là event-triggered và không làm stale khi vắng mặt. `BMS_IsDataStale()` vẫn là cảnh báo mềm, không phải alarm/DWIN và không ngắt kết nối | S |
| FR-BMS-05 | Alarm critical → FAULT; tự hồi phục khi alarm xóa + dữ liệu tươi | M |
| FR-BMS-06 | Gửi Ctrl_INFO 500ms (mask charge/discharge) | M |
| FR-BMS-07 | Snapshot `BMS_View_t` (copy struct, đọc mọi lúc) | M |
| FR-BMS-08 | Xử lý tick underflow an toàn | S |

#### BMS/CAN RX execution contract

FDCAN RX ISR chỉ đọc frame từ hardware FIFO, copy vào queue cố định và tăng
counter transport. ISR không được parse BMS, parse charger, chuyển đổi float,
cập nhật state/alarm hoặc ghi log. `BSP_CAN_ProcessRx()` chuyển tối đa một batch
giới hạn frame mỗi bus sang BMS/charger từ main loop. Frame unknown hoặc sai DLC
không refresh communication watchdog. CAN traffic tổng không được coi là BMS
online; chỉ frame BMS hợp lệ mới refresh watchdog. Queue overflow/FIFO lost phải
được ghi nhận bằng diagnostic counter.

### 3.4 Điều phối chu trình sạc (Charge Controller) — FR-CTRL

| ID | Yêu cầu | ƯP |
|---|---|---|
| FR-CTRL-01 | FSM normal: IDLE → READY → RUNNING → STOPPING → IDLE; thêm PRECHARGE cho battery-recovery; FAULT từ mọi state đang cấp sạc; DERATING là cờ trong RUNNING | M |
| FR-CTRL-20 | PRECHARGE dùng `vlow_v` + `ilow_c × battery_capacity_ah`, chia đều module active. Cho phép BMS offline lúc Start; relay chỉ latch khi mọi module active/online có V nằm trong `Vlow ±1.0V`. Khi `BATT_ST1` và `CELL_VOLT` đều fresh cùng module condition trong 60 s thì controlled stop; chỉ sau khi current-settle/relay-open hoàn tất mới về IDLE và Home. Mất một điều kiện reset hold; không có BMS-wake timeout. Low-voltage BMS alarm là INFO; critical BMS alarm sau recovery, lỗi module, jack protection và E-stop vẫn stop/fault theo đường chung. | M |
| FR-CTRL-02 | START qua tiền tố: driver đã chọn, module active > 0 và == `source_module_count`, config version hợp lệ | M |
| FR-CTRL-03 | Ghi nhận owner (PC/DWIN) mỗi chu kỳ | S |
| FR-CTRL-04 | **Manual**: target V/I từ PC, clamp `module_u_max_v`/`module_i_max_a` | M |
| FR-CTRL-05 | **Standalone No-BMS**: V=`vmax_v`, I=`imax_c × capacity`; hoàn tất khi 1 module online đo được ≥ target trong 1000ms (telemetry < 2s tuổi) | M |
| FR-CTRL-06 | **BMS-Controlled**: V=`vmax_v` (clamp); I = min(`imax_c×capacity`, giới hạn stage) / số module; dùng `rate_cap` BMS nếu có | M |
| FR-CTRL-07 | Stage cell-voltage: 6 band theo `max_cell_volt`; band chỉ tăng (high-watermark latch); dưới min → inhibit | M |
| FR-CTRL-08 | Stage temperature: 6 band, hysteresis `temp_delta_c`; ngoài vùng → inhibit | M |
| FR-CTRL-09 | Stage SOC: 6 band, latch; đạt max → kết thúc chu kỳ (SOC_REACHED) | M |
| FR-CTRL-10 | Cell-voltage đạt band max → latch → kết thúc (CELL_VOLTAGE_REACHED) | M |
| FR-CTRL-11 | Inhibit → I=0 nhưng không kết thúc chu kỳ (recoverable) | M |
| FR-CTRL-12 | Hard protection jack-V: chênh áp > delta kéo dài delay_s → FAULT + stop | M |
| FR-CTRL-13 | Jack temp: derating % (soft), hysteresis + delay; ADC nhiệt thực tế tích hợp sau | S |
| FR-CTRL-14 | Module mismatch kéo dài 10s khi chạy → FAULT | M |
| FR-CTRL-15 | STOP có kiểm; STOP khi FAULT → xóa fault về IDLE | M |
| FR-CTRL-16 | EMERGENCY_STOP → EmergencyStop driver ngay + FAULT | M |
| FR-CTRL-17 | Chỉ gửi Start/Stop khi should_run thay đổi (chống spam bus) | S |
| FR-CTRL-18 | BMS stale → giữ target cũ, cảnh báo 1 lần | S |
| FR-CTRL-19 | Ramp-up setpoint (chỉ giới hạn chiều **tăng**; giảm/derating/clamp tức thì; EMERGENCY/FAULT không ramp; bước 100ms; cả 3 mode): **Dòng** 0 → target ở `CHARGE_CTRL_CURRENT_RAMP_A_PER_S` (5 A/s). **Áp** ramp từ 0 — 2 tốc độ: pre-relay-close `CHARGE_CTRL_VOLTAGE_PRECLOSE_RAMP_V_PER_S` (10 V/s, đưa module lên áp pack nhanh; relay arm khi module ≥ `CHARGE_CTRL_RELAY_ARM_VOLT_PCT` (95%) của ref → delay ≈ 0.95·pack_V / 10), post-close `CHARGE_CTRL_VOLTAGE_RAMP_V_PER_S` (2 V/s, đoạn Stage-1 → vmax). *Tốc độ là giá trị khởi điểm, chờ đo scope trên DC bus.* | S |

### 3.5 Cấu hình (Config/Storage) — FR-CFG

| ID | Yêu cầu | ƯP |
|---|---|---|
| FR-CFG-01 | `ChargeCycleConfig_t` 243 byte, version 6, packed/static-assert. v6 append `uint32_t admin_pin` (6 chữ số, 100000..999999, default 123456) sau prefix v5 239 byte; Flash migration v5→v6 giữ mọi field cũ, gán PIN default và ghi lại record v6 khi có thể. | M |
| FR-CFG-02 | Validate: float không NaN/âm, ngưỡng tăng dần, imax≥imin, module 1–8, enum trong phạm vi | M |
| FR-CFG-03 | Flash record {magic, version, length, CRC32, payload} align 8; append; trang đầy mới erase | M |
| FR-CFG-04 | Boot: nạp record hợp lệ mới nhất; không có → default | M |
| FR-CFG-05 | Ghi xong đọc-back verify | M |

### 3.6 HMI DWIN (RS485) — FR-HMI

| ID | Yêu cầu | ƯP |
|---|---|---|
| FR-HMI-01 | Giao thức DGUS-II (không CRC), header `A5 5A` (⚠ chuẩn DGUS là `5A A5` — dự án đổi theo yêu cầu 2026-08-30, xem `DWIN_HEADER_1/2`): `DWIN_SendWords()` ghi N word big-endian tới VP liên tiếp; `DWIN_SendString()` ghi field cố định pad 0x00; Page Home giữ callback `DWIN_OnActionButton()`, còn Return Key Login/Pre-Charge dispatch qua `DWIN_OnKeyEvent(vp,key)`. | S |
| FR-HMI-08 | Pre-Charge flow do firmware điều hướng page: Setting `0x1130` → Login page 6 → Pre-Charge page 7. PIN mask tối đa 6 digit, sai PIN xóa buffer/ở Login và không log PIN; session hết hạn khi Back, Stop hoặc complete. Khi Start fail hoặc runtime fault, giữ Page 07 để hiển thị ERROR và mã lỗi hiện có; fault session chỉ kết thúc khi người dùng Reset/Back. | M |
| FR-HMI-09 | Contract Page 06/07: `VP_LOGIN_PIN_TEXT=0x1500` Text 8B; `VP_LOGIN_KEY=0x1504`; `VP_PRECHARGE_VOLTAGE_TEXT=0x1510` Text 8B; `VP_PRECHARGE_CURRENT_TEXT=0x1514` Text 8B; `VP_PRECHARGE_STATUS_ICON=0x1518`; `VP_PRECHARGE_BTN_ICON=0x1519`; `VP_PRECHARGE_ACTION_KEY=0x151A`. Digit key `0x0030..0x0039`, DEL/OK/Back `0x00F0/0x00F1/0x00F2`, Action/Back `0x0001/0x0002`. Page 07 uses `CHG_LIB_SystemSummary.voltage/total_current`; invalid/offline is `---`; mã lỗi dùng lại `VP_TOPBAR_FAULT_CODE=0x1044`, không thêm VP mới. | M |
| FR-HMI-02 | RX ring-buffer ISR, drain `BSP_RS485_Read()`; re-arm sau lỗi UART; `DWIN_ParseRX()` state-machine byte-wise có resync | M |
| FR-HMI-03 | Update dữ liệu HMI trong main loop 50ms: scatter 8 nhóm field (DC/battery/AC/temp/SOC+status/btn/uptime), diff-suppressed; chuỗi định danh + trang DASH gửi 1 lần sau khi panel boot | M |
| FR-HMI-04 | Nút DWIN: nhấn upload keycode cố định ở `0x1043` (panel→MCU); nhãn nút (VAR Icon) ở `0x1042` (MCU→panel). `app_action_button(dwin_status)` (dùng chung với nút PA15): READY→Start, STARTING/CHARGING→Stop, ERROR→`Alarm_Acknowledge()` + `ChargeController_ResetFaultIfSafe()` (chỉ reset khi nguyên nhân đã hết và output an toàn; nếu chưa đạt vẫn ERROR + RESET), **COMPLETE→`ChargeController_AcknowledgeCompletion()`** (về READY, không sạc lại), OFFLINE→bỏ qua. Sau khi xử lý, MCU ghi ngay nhãn mới vào `0x1042` | S |
| FR-HMI-05 | `VP_SYS_STATUS_ICON 0x1041` (0..5) + nhãn nút `VP_SYS_BTN_ICON 0x1042` (0..3) do `dwin_status_from_state()` / `dwin_btn_mode_from_status()` dẫn xuất. Nút chạm upload ở `VP_SYS_BTN_KEY 0x1043` (MCU không ghi VP này) | S |
| FR-HMI-06 | Bảng Alarm (VP `0x1200+`) — Phase 2, cần module event-log | C (chưa làm) |
| FR-HMI-07 | RTC (`VP_SYS_RTC_SET 0x009C`): panel tự giữ giờ; `DWIN_SetRTC()` có sẵn nhưng chưa gọi (chờ `BSP_RTC`) | C (chưa làm) |

### 3.6.1 Pre-Charge HMI fault handling

The existing alarm-code contract is reused; no new alarm code, fault flag or
DWIN VP is introduced. If a Pre-Charge start request fails, the controller
remains safe and Page 07 remains visible with status `ERROR`, the existing
`VP_TOPBAR_FAULT_CODE` value, and the existing `RESET` action. A runtime
STOP/ESTOP/protection fault stops the output and opens the relay through the
normal controller path, then remains visible on Page 07 until the operator
chooses `RESET` or `BACK`. `RESET` clears the presentation only when the root
condition and output-safety checks have passed; `BACK` returns to Home only
after the output is safe. Normal pre-charge completion still returns to Home.
Alarm-level `INFO` conditions, including expected low-voltage BMS alarms,
must not force the Page 07 `ERROR` state.

### 3.7 Vận hành & an toàn — FR-OPS

| ID | Yêu cầu | ƯP |
|---|---|---|
| FR-OPS-01 | POWER_EN (PA4) kéo HIGH đầu init | M |
| FR-OPS-02 | LED_RUN khi đang sạc; LED_FAULT khi critical/fault/0 module | M |
| FR-OPS-03 | Nút START/STOP debounce 50ms rising-edge → controller (owner=DWIN) | M |
| FR-OPS-04 | Watchdog bus-off 1s: Stop/Start + re-notify + log | M |
| FR-OPS-05 | AutoRetransmission = ENABLE cả 2 bus | M |
| FR-OPS-06 | UART3 re-arm Receive_IT trong error callback | M |
| FR-OPS-07 | Filter CAN: 1 slot ext mask accept-all → FIFO0; reject std/remote | M |

---

## 4. Yêu cầu giao diện ngoài (External Interface Requirements)

### 4.1 Giao thức PC ↔ MCU (USB CDC)

Khung nhị phân:

```
+------+------+-----+-----+--------------------+------+
| 0xAA | 0x55 | CMD | LEN | PAYLOAD (LEN byte) | CRC8 |
+------+------+-----+-----+--------------------+------+
CRC8: poly 0x07, init 0x00, tính trên [CMD, LEN, PAYLOAD]
Payload: little-endian
```

#### 4.1.1 Lệnh PC → MCU

| CMD | Tên | Payload | Phản hồi |
|-----|-----|---------|----------|
| 0x01 | SET_VOLTAGE | f32 (V) | ACK/NACK |
| 0x02 | SET_CURRENT | f32 (A/module) | ACK/NACK |
| 0x03 | START | u8 manual_mode | ACK/NACK |
| 0x04 | STOP | — | ACK |
| 0x05 | SET_MODULE_ADDR | u8 addr, u8 group | ACK/NACK |
| 0x06 | PING | — | PONG (u32 version) |
| 0x07 | READ_REG | u8 idx, u16 reg | READ_REG / NACK |
| 0x08 | EMERGENCY_STOP | — | ACK |
| 0x09 | SET_DRIVER | u8 driver_id | ACK/NACK |
| 0x10–0x1A | Debug protocol | mục 4.1.5 | tương ứng |

Mã lỗi NACK: 0x01 bad CRC, 0x02 unknown cmd, 0x03 bad length, 0x04 CAN TX fail, 0x05 bad param.

#### 4.1.2 Response MCU → PC

| RSP | Tên | Payload |
|-----|-----|---------|
| 0x81 | STATUS | `PC_StatusReport_t` 51 byte (mục 6.4) |
| 0x82 | ACK | echo CMD |
| 0x83 | NACK | echo CMD + err |
| 0x84 | PONG | u32 version (major<<16 \| minor<<8 \| patch) |
| 0x85 | READ_REG | u8 idx, u16 reg, u8 type, data |
| 0x90–0x97 | Debug responses | mục 4.1.5 |

#### 4.1.3 Trình tự phiên làm việc (PC app)

```
PING ──► PONG(version)
SET_DRIVER(id) ──► ACK
SET_MODULE_ADDR(addr,group) ──► ACK      (mỗi module)
SET_CHARGE_CFG(243B, v6) ──► CHARGE_CFG  (tùy chọn)
READ_ALL / GET_SYSTEM                    (giám sát)
START(manual) ──► ACK
```

#### 4.1.4 Bảng READ_REG (module view)

| Reg | Field | Kiểu |
|-----|-------|------|
| 0x0001 | voltage (V) | f32 |
| 0x0002 | current (A) | f32 |
| 0x0003 | current_limit (A) | f32 |
| 0x0004 | temp_dcdc (°C) | f32 |
| 0x000B | temp_ambient (°C) | f32 |
| 0x0040 | alarm_status raw | u32 |
| 0x0048 | input_power (W) | u32 |
| 0x0100 | state (enum) | u8 |
| 0x0101 | online | u8 |
| 0x0102 | running | u8 |
| 0x0103 | addr | u8 |
| 0x0104 | group | u8 |

#### 4.1.5 Debug protocol (0x10–0x1A)

| CMD | Tên | RSP |
|-----|-----|-----|
| 0x10/0x11 | ENTER/EXIT debug mode | ACK |
| 0x12/0x13 | READ_ALL / READ_ONE module | 0x91 (123 B/module) / 0x90 |
| 0x14 | READ_STATS truyền thông | 0x92 |
| 0x15 | WRITE_REG module | ACK/ERROR |
| 0x16 | SEND_RAW_CAN | 0x95 |
| 0x17 | READ_BMS | 0x93 |
| 0x18 | GET_SYSTEM (68 B, mục 6.5) | 0x94 |
| 0x19/0x1A | GET/SET_CHARGE_CFG (243 B, v6) | 0x97 |

### 4.2 CAN1 — Module sạc (125 kbps, classic, ext-frame)

Frame RX được feed tới driver đang active qua `CHG_LIB_FeedCanFrame()`.

#### 4.2.1 Maxwell MXR (V1.50)

- **CAN ID 29-bit**: `[28:20]=PROTNO 0x060 | [19]=PTP | [18:11]=DST | [10:3]=SRC | [2:0]=GRP`
- Controller addr = 0xF0; Data 8B: `[func][rsv][reg_hi][reg_lo][float BE ×4]`
- Func: 0x03 write, 0x10 read; Resp: 0x41 float / 0x42 int; err 0xF0 OK / 0xF2 fail
- Thanh ghi: 0x21 V-set, 0x22 I-limit (ratio 0–1), 0x30 ON/OFF (1=stop/0=start), 0x40 alarm u32, 0x01/0x02 V/I, 0x04/0x0B/0x10 nhiệt, 0x08/0x0A PFC ±, 0x0C–0x0E pha AC, 0x11/0x12 rated

#### 4.2.2 Lianming (V2.0)

- **TX**: `0x1907C080 | addr` (1–60); **RX**: `0x1807C080 | addr`
- Byte[0] CMD: `0x00` set (byte1–3 dòng mA BE, byte4–7 áp mV BE), `0x01` status (byte2–3 dòng 0.1A, byte4–5 áp 0.1V, byte6–7 fault), `0x02` start/stop (byte7 = 0x55/0xAA)
- Diagnostic: AC `0x1907A0xx/0x1807A0xx`, nhiệt `0x190080xx/0x180080xx`

#### 4.2.3 TonHe (J1939 V1.3)

- **CAN ID**: `[28:26]=Priority | [23:16]=PF | [15:8]=PS | [7:0]=SA`; controller SA=0xA0
- Uplink: M_C_1 (0x000100) status — byte0 (0x01 ON/0x11 fault), byte1–2 áp 0.1V BE, byte3–4 dòng 0.01A BE, byte5–6 fault, byte7 PFC; M_C_2 confirm; M_C_3 pha AC; M_C_4 extended
- Downlink: C_M_24 (0x000600) start/stop riêng module (0xAA/0x55), C_M_2 param, C_M_3 timing 1s

### 4.3 CAN2 — BMS (250 kbps, Jikong BB_PKG V1.0)

#### 4.3.1 Frame BMS → Charger

| ID | Loại | Chu kỳ | Nội dung |
|----|------|--------|----------|
| 0x02F4 | Std | 20ms | BATT_ST1: u16 V×0.1; i16 I = raw×0.1−400; u8 SOC% |
| 0x04F4 | Std | 100ms | CELL_VOLT: u16 max mV + pos, u16 min mV + pos |
| 0x05F4 | Std | 500ms | CELL_TEMP: max/min/avg (raw = °C+50) + vị trí |
| 0x07F4 | Std | event | ALM_INFO: 13 alarm × 2 bit (0=none, 1=warning, 2=fault, 3=severe); severity 1 chỉ reporting, severity >=2 là actionable fault |
| 0x18F128F4 | Ext | 100ms | BATT_ST2: cap_remain 0.1Ah, rate_cap 0.1Ah, cycles, SOH% |
| 0x1806E5F4 | Ext | 1000ms | ChgRequest: **u16 BE** V×0.1, **u16 BE** I×0.1, byte4 switch, byte5 mode — the one exception to `CAN BMS_BB_PKG V1.0.pdf`'s default little-endian; see `docs/BMS_ChgRequest_addendum.md` for the second source + worked example that confirms it |
| 0x18F528F4 | Ext | 500ms | BmsSwSta: byte0 bit0 pre-dischg, bit1 dischg, bit2 charge relay |
| 0x18E028F4+E | Ext | 1000ms | CELL_VOLT_FULL: 4 cell/frame × 8 (32 cell) |
| 0x18F228F4 | Ext | 1000ms | CELL_TEMP_FULL: relay, shunt, 6 cell |

#### 4.3.2 Frame Charger → BMS

| ID | Chu kỳ | Nội dung |
|----|--------|----------|
| 0x18F0F428 Ctrl_INFO | 500ms | byte0 MaskCode (bit0 charge, bit1 discharge); byte1 ChgSw; byte2 DchgSw |

### 4.4 RS485 — HMI DWIN (115200-8N1, half-duplex, DE=PB1)

- Header **`A5 5A`** (⚠ chuẩn DGUS-II là `5A A5`; dự án build với `A5 5A` theo yêu cầu 2026-08-30 — `DWIN_HEADER_1/2` trong `dwin_protocol.h`, đổi 2 macro là revert); Write=0x82, Read=0x83; **CRC tắt** (DGUS CFG bit 0x05.7 = 0). Big-endian; giá trị 32-bit chiếm 2 VP liên tiếp, word cao ở địa chỉ thấp.
- VP map — nguồn sự thật: [`Modules/hmi/dwin_vp_map.h`](../Modules/hmi/dwin_vp_map.h), phải khớp project DGUS trong `ui/`.

  | VP | Ý nghĩa | Định dạng |
  |---|---|---|
  | `0x0084` | chuyển trang (`5A01` + page id) | 0=logo 1=dash 2=setting 3=alarm |
  | `0x1000..0x1003` | DC voltage | Text Display, 8 bytes / 4 VP, ví dụ `521.0`; đơn vị do DWIN vẽ; không có module → `---` |
  | `0x1004..0x1007` | DC current | Text Display, 8 bytes / 4 VP, ví dụ `12.0`; đơn vị do DWIN vẽ; không có module → `---` |
  | `0x1008..0x100B` | DC power | Text Display, 8 bytes / 4 VP, ví dụ `6.3`; đơn vị do DWIN vẽ; không có module → `---` |
  | `0x1010..0x1013` | BMS pack V | Text Display, 8 bytes / 4 VP, chỉ gửi value; BMS offline → `---` |
  | `0x1014..0x1017` | BMS max cell voltage | Text Display, 8 bytes / 4 VP, ASCII volts với 3 chữ số thập phân (ví dụ `3.315` cho 3315 mV); `---` khi BMS offline/giá trị không hợp lệ |
  | `0x1018..0x101B` | BMS Ah | Text Display, 8 bytes / 4 VP, chỉ gửi value; BMS offline → `---` |
  | `0x1020..0x1023` | AC L1 | Text Display, 8 bytes / 4 VP, chỉ gửi value; module offline/field invalid → `---` |
  | `0x1024..0x1027` | AC L2 | Text Display, 8 bytes / 4 VP, chỉ gửi value; module offline/field invalid → `---` |
  | `0x1028..0x102B` | AC L3 | Text Display, 8 bytes / 4 VP, chỉ gửi value; module offline/field invalid → `---` |
  | `0x1030..0x1033` | temperature battery | Text Display, 8 bytes / 4 VP, chỉ gửi value; BMS offline/field invalid → `---` |
  | `0x1034..0x1037` | temperature charge | Text Display, 8 bytes / 4 VP, chỉ gửi value; module offline/field invalid → `---` |
  | `0x1038..0x103B` | temperature jack | Text Display, 8 bytes / 4 VP, chỉ gửi value; ADC invalid → `---` |
  | `0x1041` | status icon | 0 READY 1 STARTING 2 CHARGING 3 COMPLETE 4 ERROR 5 OFFLINE |
  | `0x1042` | nhãn nút (MCU→panel) | 0 START 1 STOP 2 RESET 3 DISABLED — VAR Icon |
  | `0x1043` | nút chạm (panel→MCU) | Return-Key-Code upload khi nhấn; MCU không ghi VP này |
  | `0x1048..0x104B` | SOC | Text Display, 8 bytes / 4 VP; BMS online ví dụ `50%`, offline → `--%` |
  | `0x8003` | SOC Text Color | SP `0x8000` + 3 WORD; RGB565: unavailable `0x8410`, critical `0xF800`, low `0xFD20`, medium `0xD520` (muted amber), normal `0x2CEA` (muted green) |
  | `0x1100/1108/1110` | HW ver / FW ver / Device ID | ASCII 8 VP / 16 ký tự |
  | `0x1118` | uptime | u32 (0x1118–19) giây |
  | `0x009C` | RTC set | *chưa dùng — panel tự giữ giờ* |
  | `0x1200+` | bảng Alarm (20 VP/dòng ×5) | *Phase 2* |

- MCU → DWIN: `A5 5A [len] 82 [VP_hi] [VP_lo] [payload...]` — `DWIN_SendWords()` / `DWIN_SendString()` / `DWIN_SetPage()`; scatter 1 nhóm/50ms trong `DWIN_UpdateData()`, chỉ gửi trường nào đổi giá trị + full re-send mỗi 5s (`DWIN_ForceFullRefresh()`, để panel boot muộn / reboot bắt kịp). Text Display dùng ASCII-compatible GBK; `DWIN_SendString()` ghi đủ độ dài field và padding `0x00`. Mỗi field 8 byte có 4 VP; `LEN = 3 + payload_bytes`. Dữ liệu chưa có hoặc không hợp lệ → `---`, không dùng `0` làm giá trị thay thế.
- DWIN → MCU: Home action dùng `A5 5A 06 83 10 43 01 [val_hi] [val_lo]` → `DWIN_OnActionButton()`; Login/Pre-Charge Return Key dùng cùng frame format tại VP contract `0x1130`, `0x1304`, `0x1318` → `DWIN_OnKeyEvent(vp,key)`. Firmware là owner chuyển page.
- RX ring-buffer 128 byte, drain `BSP_RS485_Read()`; `DWIN_ParseRX()` byte-wise có resync + chặn LEN quá cỡ.

### 4.5 Debug log (USART1)

PA9 TX, 115200; `LOG(fmt,...)` buffer 128 B, blocking ≤50 ms; chỉ log sự kiện, không spam định kỳ.

---

## 5. Yêu cầu phi chức năng (Non-functional Requirements)

| ID | Yêu cầu | Tiêu chí |
|----|---------|----------|
| NFR-01 | Chu kỳ điều khiển 20 ms, non-blocking | — |
| NFR-02 | Phản hồi lệnh PC < 100 ms | ACK/NACK |
| NFR-03 | EMERGENCY_STOP → STOP module trong chu kỳ hiện tại | — |
| NFR-04 | Độ tin cậy CAN: AR on; bus-off recovery ≤ 1s; module offline ≤ 10s | — |
| NFR-05 | Toàn vẹn config: CRC32 + read-back verify; hỏng 1 record không ảnh hưởng record trước | — |
| NFR-06 | Bộ nhớ: Flash ≤ 128 KB, RAM ≤ 144 KB | hiện ~93% / ~12% |
| NFR-07 | LOG không blocking lâu (≤50ms), không malloc | — |
| NFR-08 | Fail-safe: FW không tự start sạc sau boot; chờ lệnh | — |
| NFR-09 | Kiến trúc phân lớp BSP/Modules/App; thêm driver không sửa core | — |
| NFR-10 | Interface có thể mock; static-assert kích thước struct | — |

---

## 6. Ràng buộc thiết kế và đặc tả dữ liệu

### 6.1 FSM Charge Controller

```
                    ┌──────────────────────────────► FAULT
                    │  (precondition fail / BMS alarm /
                    │   hard protection / mismatch 10s / E-STOP)
 IDLE ──Start──► READY ──OK──► RUNNING ──cell/SOC latch──► STOPPING ──► IDLE
  ▲                            │  ▲                        (owner=NONE)
  └───── STOP (mọi state) ─────┴──┴── STOP khi FAULT → xóa fault → IDLE
```

RUNNING gồm 3 mode handler: `run_manual_mode`, `run_standalone_mode`, `run_bms_controlled_mode`. Trạng thái chi tiết: `derating`, `inhibit`, `active_limit_source`, `active_stage_band`, `stop_reason`.

### 6.2 FSM module sạc

| State | Hành vi | Chuyển tiếp |
|-------|---------|-------------|
| IDLE | poll 1 reg/s | should_run → STARTING |
| STARTING | set V → 50ms → set I → 50ms → ON → xác nhận ×5 | có áp → RUNNING; quá hạn → FAULT |
| RUNNING | poll 15 reg round-robin | stop → STOPPING; alarm critical → FAULT |
| STOPPING | retry STOP 500ms ×5 | ACK → IDLE; quá hạn → FAULT |
| WARNING | mất RX 2–10s | có RX → RUNNING/IDLE |
| OFFLINE | chờ 3s | → RECOVERING |
| RECOVERING | cần 5 RX | đủ → IDLE/STARTING |
| FAULT | đọc alarm | alarm xóa → IDLE/STARTING |

### 6.3 `CHG_LIB_ModuleView_t`

addr, group, enabled, online, running, state; voltage/current/current_limit; temp_dcdc/ambient/pfc; ac_phase_a/b/c; pfc_bus_pos/neg; input_power (u32, cap 100 kW); rated_power/current; alarm_status (raw) + alarm_flags (chuẩn hóa); pfc_fault; last_rx/tx_tick; stats {tx, rx, error, timeout, recovery}.

### 6.4 `PC_StatusReport_t` — 51 byte (packed LE)

| Offset | Field | Kiểu |
|--------|-------|------|
| 0 | voltage, total_current, temp_dcdc, temp_ambient | 4×f32 |
| 16 | alarm_status, total_power_in | 2×u32 |
| 24 | modules_online, modules_fault, charging | 3×u8 |
| 27 | bms_voltage, bms_current, bms_chg_v_req, bms_chg_i_req | 4×f32 |
| 43 | bms_alarm | u32 |
| 47 | bms_soc, bms_state, btn_start, btn_stop | 4×u8 |

### 6.5 `DebugSystemInfo_t` — 68 byte (packed)

FW version, driver id, module counts, charging, controller state/derating/inhibit, source mode, limit source/band; total V/I/P, max temp, target V/I, active limit C; uptime, CAN1/2 TX/RX (offset 46–61); controller fault flags, stop_reason, bms_stale.

### 6.6 `ChargeCycleConfig_t` — 243 byte, v6 (packed)

| Nhóm | Trường |
|------|--------|
| Pin | capacity_ah, imin/imax/ipre/ilow_c, vmin/vmax/vpre/vlow_v, temp_limit_c |
| Cell-V stage | enabled, delta, 5 ngưỡng V, 4 C-rate |
| Temp stage | enabled, delta, 5 ngưỡng °C, 4 C-rate |
| SOC stage | enabled, delta, 5 ngưỡng %, 4 C-rate |
| Bảo vệ | jack_charge {en, delta_v, delay_s}; jack_temp {en, delay_s, threshold, delta, limit_pct} |
| Hệ thống | source_mode (0=BMS/1=Standalone), can_battery_id, source_module_count, module_type, module_u_min/max_v, module_i_min/max_a |
| Định danh (v4) | device_id[16], hw_rev[12] — chuỗi ASCII cho màn Setting |
| Admin (v6) | admin_pin u32, 6 digit (100000..999999), default 123456; owner chung của Flash/PC/DWIN Login |

### 6.7 Flash layout cấu hình

- Trang 63 `0x0801F800`, 2 KB, page-erase
- Record `{u32 magic=0x43434647, u16 ver=1, u16 len=243, u32 crc32, payload[243]}`, align 8; loader cũng nhận record v5 `len=239` để migrate append-only sang v6.
- Append vào offset trống; đầy → erase → ghi offset 0; read-back verify
- CRC32 poly 0xEDB88320 reflected, init/final XOR 0xFFFFFFFF

### 6.7.1 Flash layout bộ đếm năng lượng

- Trang 59--62 (`0x0801D800..0x0801F7FF`) dành riêng cho journal tổng điện năng.
- Mỗi record 32 byte: magic, version, length, sequence, `total_charged_ah_x1000`, `total_energy_kwh_x1000`, CRC32.
- Ghi checkpoint mỗi 5 phút khi charging và ghi ngay khi phiên sạc kết thúc bình thường.
- Journal chạy vòng tròn qua 4 page; record CRC lỗi hoặc ghi dở bị bỏ qua.
- Uptime không được dùng làm dữ liệu tích lũy; DWIN chỉ đọc giá trị đã khôi phục từ counter RAM.
- PC command `0x1F` reset counter, chỉ được xử lý khi payload rỗng và phải trả ACK/ERROR.

### 6.7.2 RTC và nguồn dự phòng

- RTC dùng calendar phần cứng STM32 với LSI; firmware lưu marker hợp lệ trong backup register.
- RTC giữ được thời gian qua reset hoặc mất VDD chỉ khi backup domain còn nguồn VBAT.
- Khi mất cả VDD và VBAT, RTC được đánh dấu invalid và không hiển thị thời gian giả.
- PC đồng bộ bằng `SET_RTC` với Unix epoch UTC; firmware chuyển sang UTC+7 trước khi ghi RTC.

### 6.8 Bố cục phần mềm

```
App/
 ├─ System/    app_main (init + loop 20ms)
 ├─ Protocol/  pc_protocol, pc_debug_protocol
 └─ Charge/    charge_controller, charge_cycle_config, charge_cycle_storage
Modules/
 ├─ chg_lib/   core, fsm, can_backend, driver_{maxwell,lianming,tonhe}
 ├─ bms/       bms_core, bms_protocol, bms_can
 └─ hmi/       dwin_protocol
BSP/           bsp_can, bsp_rs485, bsp_flash, bsp_gpio, bsp_sys
Core/          CubeMX generated (main, fdcan, usart, adc, ...)
USB_Device/    CDC class
Utils/Log/     debug_log
```

---

## 7. Phụ lục

### 7.1 Sequence chu kỳ sạc BMS-Controlled

```
PC                    MCU                                   CAN1           CAN2/BMS
 │ SET_DRIVER=1        │                                      │               │
 │ SET_MODULE_ADDR ───►│ add module + apply profile           │               │
 │ SET_CHARGE_CFG ────►│ validate + lưu flash                 │               │
 │ START ─────────────►│ precondition OK → READY → RUNNING    │               │
 │                     │ SetVoltageAll(vmax) ────────────────►│ set V         │
 │                     │ SetCurrentLimitAll(I/n) ────────────►│ set I         │
 │                     │ StartAll ───────────────────────────►│ ON            │
 │                     │◄── poll telemetry ───────────────────│ V/I/temp/alm  │
 │                     │◄─────── BATT_ST1 / CELL_VOLT ────────────────────────│
 │                     │ tính band → derating/inhibit         │               │
 │ RSP_STATUS ◄────────│ (khi PC request)                     │               │
 │                     │ cell V đạt max → latch → StopAll ───►│ OFF           │
 │                     │ STOPPING → IDLE                      │               │
```

### 7.2 Trạng thái đã biết / việc còn tồn

| Mã | Mô tả | Ưu tiên |
|----|-------|---------|
| ~~TBD-01~~ | ~~DWIN HMI chưa tích hợp vào App_Loop (protocol sẵn sàng)~~ | **Đã đóng 2026-08-30**: viết lại `Modules/hmi/dwin_protocol.*` theo project DGUS thật (`ui/`), thêm `Modules/hmi/dwin_vp_map.h`. `app_main.c` section (4) scatter update 8 nhóm field mỗi 50ms + `dwin_status_from_state()`/`dwin_btn_mode_from_status()`; nút màn hình dùng `app_action_button()` chung với nút PA15. Còn Phase 2: bảng Alarm `0x1200` (cần event-log) và RTC `0x009C` (chờ `BSP_RTC`). Verified: build FW Release, 4/4 check + host test dwin e2e. |
| ~~TBD-02~~ | ~~`BMS_ShouldCloseChargeRelay()` có API nhưng chưa nối vào flow relay~~ | **Đã đóng** — _2026-08-29: đã nối từ trước (không rõ session nào, không có ghi chú); xác nhận lại bằng code khi review thuật toán sạc theo yêu cầu người dùng: `update_relay_decision()` (`charge_controller.c`) gọi `BMS_ShouldCloseChargeRelay()` ở chế độ BMS-Controlled, `app_main.c` đọc `relay_should_close` và ghi GPIO mỗi 20ms. Xem thêm TBD-05._ |
| ~~TBD-03~~ | ~~Jack-temp ADC chưa đọc thực (giả lập 25°C)~~ | **Đã đóng** — _2026-08-29: đã đọc ADC thật từ trước (không rõ session nào); xác nhận lại bằng code: `app_main.c` đọc max 4 kênh NTC (`BSP_ADC_GetTempC`) mỗi chu kỳ, chỉ fallback 25°C khi cả 4 kênh disconnect (`<-50°C` sentinel), truyền vào `ChargeController_SetJackTempC()`. **Re-verified 2026-08-30**: đọc thì đúng nhưng ADC là single-shot + DMA one-shot, chỉ lấy mẫu 1 lần lúc boot → giá trị đóng băng, derating/fault jack-temp vô hiệu. Fix: `BSP_ADC_Process()` re-arm mỗi 200ms từ `App_Loop` (xem AUDIT_Findings BUG-14)._ |
| **TBD-04** | `bms_chg_v/i_request` được parse nhưng BMS-mode cố tình bỏ qua (dùng config nội bộ) | **Đã xác nhận nghiệp vụ 2026-08-29**: đúng thiết kế, không phải gap. BMS chỉ đóng vai trò giám sát/an toàn (online/offline, alarm, telemetry nuôi stage band, `BMS_ShouldCloseChargeRelay()`); U/I mục tiêu luôn do thuật toán sạc quyết định từ `ChargeCycleConfig_t` cấu hình cục bộ, không theo yêu cầu động của BMS. Đã ghi rõ trong comment `run_bms_controlled_mode()`. |
| ~~TBD-05~~ | ~~Relay 1/2/3 (PB14/PB15/PA8) chưa có logic điều khiển~~ | **Đã đóng 2026-08-29**: xác nhận với người dùng — thực tế chỉ cần 1 relay để đóng/cắt mạch sạc, cả 3 relay không phải 3 chức năng khác nhau. Sửa `app_main.c` để RELAY_3 (PA8) đóng/mở đồng thời, cùng điều kiện với RELAY_1/RELAY_2 (`relay_should_close`). |
| TBD-06 | UART5/LTE chưa triển khai | Thấp |

**Xác nhận nghiệp vụ khác (2026-08-29, khi review thuật toán sạc theo yêu cầu người dùng)**:
- Stage band cell-voltage/SOC: chỉ tiến (band N → N+1), không có chiều lùi trong 1 chu kỳ sạc, kể cả khi giá trị đo giảm tạm thời — khớp `eval_cell_stage()`/`eval_soc_stage()`'s high-watermark latch hiện tại, không cần sửa.
- Stage band nhiệt độ: là band **duy nhất** được phép lùi (N+1 → N), chỉ khi nhiệt độ giảm quá `threshold - temp_delta_c` (hysteresis) — khớp `eval_temp_stage()` hiện tại, không cần sửa.
- Chế độ Manual: giữ nguyên 1 setpoint cố định trong suốt phiên RUNNING, không áp dụng jack-temp soft derating tự động (khác Standalone/BMS-Controlled) — người vận hành Manual tự chịu trách nhiệm phần này; hard protection (jack-V fault, alarm module, E-STOP) vẫn áp dụng bình thường vì chạy trước khi dispatch theo mode. Đã ghi rõ trong comment `run_manual_mode()`.

### 7.3 Hướng dẫn bảo trì cấu hình CubeMX

Khi regenerate từ `Charger.ioc`, kiểm tra lại các giá trị sau trong code generated:

| File | Giá trị bắt buộc |
|------|------------------|
| `Core/Src/fdcan.c` | FDCAN1 `NominalPrescaler = 32` (125 kbps); FDCAN2 `NominalPrescaler = 16` (250 kbps); cả hai `AutoRetransmission = ENABLE`, `ExtFiltersNbr = 1` |
| `Core/Src/usart.c` | USART3 `AdvancedInit.Swap = UART_ADVFEATURE_SWAP_ENABLE` |

Các giá trị này đã được lưu trong `Charger.ioc` nhưng cần verify sau mỗi lần Generate.

---

*Hết tài liệu — SRS-CHG-CTRL-001 v1.0*
