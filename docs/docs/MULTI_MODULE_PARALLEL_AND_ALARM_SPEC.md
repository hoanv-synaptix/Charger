# Đặc Tả Kỹ Thuật Vận Hành Song Song & Hệ Thống Cảnh Báo (Multi-Module Parallel & Alarm Specification)

Tài liệu chuẩn kỹ thuật mô tả kiến trúc phân tầng điều khiển, cơ chế tự động cân bằng dòng (Autonomous Current Sharing), thuật toán chia tải vĩ mô của MCU, chính sách xử lý sự cố giảm tải (Degraded Mode / N+1 Redundancy) và danh mục 31 mã cảnh báo toàn trạm.

---

## 1. Kiến Trúc Vận Hành Song Song (Parallel System Architecture)

Hệ thống sạc sử dụng kiến trúc **Hybrid (Điều khiển tập trung vĩ mô + Cân bằng tải vi mô phần cứng)**:

```text
                        +----------------------------+
                        |      BMS (Pin Lithium)     |
                        +--------------+-------------+
                                       | CAN2 (250Kbps)
                                       v
                        +----------------------------+
                        |     MCU Master Controller  |
                        |      (STM32G0B1, 20ms)     |
                        +--------------+-------------+
                                       | CAN1 (125Kbps, Ext 29-bit)
               +-----------------------+-----------------------+
               | TX Broadcast CMD=0, 2 (V_target, I_per_mod)   |
               v                                               v
     +-------------------+                           +-------------------+
     | Module 1 (Addr 1) |                           | Module 2 (Addr 2) |
     +---------+---------+                           +---------+---------+
               |                                               |
               +<==== CAN Nội Bộ 0x180780xx (400ms Sync) =====>+
               |                                               |
               +-----------------------+-----------------------+
                                       | DC Output Bus (Song song)
                                       v
                             [ Contactor Sạc Chính ]
                                       |
                                       v
                                 [ Cọc Pin DC ]
```

### 1.1. Phân Tầng Điều Khiển (Control Hierarchy)

1. **Tầng vĩ mô (Hệ thống - MCU Master):**
   * Đọc yêu cầu từ Pin ($V_{target}$, $I_{bms\_max}$) qua CAN2.
   * Tính toán dòng mục tiêu toàn trạm ($I_{target\_total}$) qua bộ điều khiển nạp đa giai đoạn (CC/CV, Stage 1..Stage 4, giới hạn theo nhiệt độ NTC).
   * Chia đều dòng điện cho các module đang online và sẵn sàng:
     $$I_{per\_mod} = \frac{I_{target\_total}}{N_{active}}$$
   * Giới hạn $I_{per\_mod} \le I_{max\_rated}$ (định mức phần cứng của 1 module, ví dụ 50A).
   * Phát lệnh cài đặt đồng thời qua Broadcast `0x1907C080` (CMD=0) hoặc gửi Unicast từng module.

2. **Tầng vi mô (Nội bộ Module Lianming):**
   * Tự cấp địa chỉ (`Addr = 1, 2, ...`) theo thứ tự slot vật lý / khởi động CAN.
   * Tự động trao đổi dòng ra thực tế qua frame CAN nội bộ `0x180780xx` mỗi 400ms.
   * Vi chỉnh điện áp tham chiếu (Droop/Active trim) để dòng ra thực tế giữa các module cân bằng với sai số $< 3\% - 5\%$ (thực nghiệm đo đạc đạt $0.25\%$, lệch chỉ $0.20\text{A}$ tại tải $78.6\text{A}$).

---

## 2. Chính Sách Xử Lý Sự Cố Ở Mode Song Song (Fault Policy & Degraded Mode)

Khi mắc song song nhiều module, phân hệ Alarm (`alarm.c`) áp dụng chính sách **Dynamic Action Resolution (Phân giải hành động động)**:

### 2.1. Lỗi Cục Bộ 1 Module (Local Module Faults) -> Chạy Giảm Tải (Degraded Mode)
Nếu hệ thống có $\ge 2$ module cấu hình, khi **1 module gặp sự cố**:
* Các mã lỗi cục bộ của module:
  * `E010`: Lỗi phần cứng bộ sạc (`ALARM_MOD_HW_FAULT`)
  * `E011`: Quá nhiệt độ bộ sạc (`ALARM_MOD_OVER_TEMP`)
  * `E014`: Quá dòng đầu ra bộ sạc (`ALARM_MOD_OVER_CURR_OUT`)
  * `E015`: Lỗi khối nguồn PFC (`ALARM_MOD_PFC_FAULT`)
  * `E016`: Lỗi quạt tản nhiệt (`ALARM_MOD_FAN_FAULT`)
  * `E017`: Quá áp AC ngõ vào (`ALARM_MOD_AC_OVER_VOLT`)
  * `W010`: Mất giao tiếp CAN module (`ALARM_MOD_COMM_FAIL`)
* **Hành vi xử lý:**
  1. Hành động được tự động hạ cấp từ `ALARM_ACT_STOP` xuống **`ALARM_ACT_INFO`**.
  2. Trạm **KHÔNG DỪNG SẠC**.
  3. Controller phát hiện số module active giảm (`actual_module_count < source_module_count`), lập tức kích hoạt cờ **`Derating = 1`** và mã cảnh báo **`E028` (Số bộ sạc không khớp)**.
  4. Trần dòng sạc trạm được tự động kẹp (clamp) về định mức an toàn của số module còn lại:
     $$I_{station\_cap} = N_{active} \times I_{max\_mod}$$
  5. Module còn khỏe tiếp tục duy trì sạc an toàn cho pin xe.

### 2.2. Lỗi Thanh Cái Chung (Bus / System Level Faults) -> Dừng Khẩn Cấp (ESTOP / STOP)
Bất kể đang chạy bao nhiêu module, các lỗi ảnh hưởng trực tiếp đến thanh cái DC hoặc an toàn pin **BẮT BUỘC DỪNG NGAY LẬP TỨC**:
* `E012`: Quá áp DC đầu ra (`ALARM_MOD_OVER_VOLT_OUT`) -> **`ALARM_ACT_ESTOP`**.
* `E013`: Ngắn mạch đầu ra DC (`ALARM_MOD_SHORT_CIRCUIT`) -> **`ALARM_ACT_ESTOP`**.
* `E027`: Mất toàn bộ module sạc (`ALARM_CTRL_NO_MODULE`, $N_{active} = 0$) -> Khóa Start.
* `E003`, `E004`, `E007`: Quá áp/quá dòng pin phía BMS -> **`ALARM_ACT_STOP`**.

### 2.3. Cơ Chế Định Danh Nguồn Lỗi & Hiển Thị Đích Danh Module (Per-Module Fault Identification)

Phân hệ cảnh báo hỗ trợ mã hóa nguồn gốc lỗi (`source_id`) trực tiếp vào 16-bit log code (`AlarmLogEntry_t.code`):
* **Hệ thống 1 Module (`source_module_count == 1`):**
  * Báo lỗi **như bình thường**: `source_id = 0` (`ALARM_SOURCE_STATION`).
  * Text hiển thị trên màn hình DWIN giữ nguyên dạng chuẩn (ví dụ: `W010: Mất giao tiếp bộ sạc`, `E011: Nhiệt độ bộ sạc cao`), không có hậu tố `[M1]`.
  * Khi module gặp sự cố, trạm lập tức ngắt sạc an toàn (STOP / ESTOP) vì không còn module nào gánh tải.
* **Hệ thống Đa Module Song Song (`source_module_count >= 2`):**
  * Khi module thứ $k$ ($k = 1..8$) gặp lỗi: `source_id = k`.
  * Màn hình DWIN tự động nối thêm hậu tố định danh vào text lỗi: ` [M1]`, ` [M2]`, ..., ` [M8]` (ví dụ: `W010: Mất giao tiếp bộ sạc [M2]`).
  * Cho phép người vận hành xác định chính xác module vật lý gặp sự cố để bảo trì, thay thế mà không làm gián đoạn phiên sạc của các module còn lại.

### 2.4. Phương Thức Chia Dòng Giữa Các Module (Current Sharing Method)
* **Phương thức vận hành**: Hệ thống sử dụng cơ chế **MCU Centralized Load Balancing** ($I_{\text{target\_per\_mod}} = I_{\text{BMS}} / N_{\text{active}}$) chu kỳ 20ms thay vì phụ thuộc tính năng share dòng tự động qua CAN nội bộ của hãng (vốn không khả thi do thiếu đường chia dòng analog hoặc xung đột vai trò Master giữa các bộ sạc).
* **Đặc tính an toàn**: MCU liên tục giám sát trạng thái từng module, nếu 1 module ngắt kết nối hoặc lỗi, MCU lập tức tính toán lại tải và tái phân bổ ngay cho các module còn lại trong vòng 20ms.

---

## 3. Bảng Danh Mục 31 Mã Cảnh Báo & Bảo Vệ Hệ Thống (Alarm Catalog)

| Mã HMI | Mã Enum C Code | Tên Sự Cố / Cảnh Báo | Hành Động Đơn | Hành Động Khi Song Song | Debounce Set | Debounce Clear |
| :---: | :--- | :--- | :---: | :---: | :---: | :---: |
| **E001** | `ALARM_BMS_LOW_PACK_VOLT` | Điện áp pin thấp | STOP | STOP | 0 ms | 200 ms |
| **E002** | `ALARM_BMS_LOW_CELL_VOLT` | Điện áp cell pin thấp | INFO | INFO | 0 ms | 200 ms |
| **E003** | `ALARM_BMS_HIGH_PACK_VOLT` | Điện áp pin cao | STOP | STOP | 0 ms | 200 ms |
| **E004** | `ALARM_BMS_HIGH_CELL_VOLT` | Điện áp cell pin cao | STOP | STOP | 0 ms | 200 ms |
| **E005** | `ALARM_BMS_TEMP_HIGH_CHG` | Pin quá nóng khi sạc | INFO (Clamp 0A) | INFO (Clamp 0A) | 0 ms | 200 ms |
| **W001** | `ALARM_BMS_TEMP_HIGH_DCHG` | Pin quá nóng khi xả | INFO | INFO | 0 ms | 200 ms |
| **E006** | `ALARM_BMS_TEMP_LOW_CHG` | Pin quá lạnh khi sạc | STOP | STOP | 0 ms | 200 ms |
| **W002** | `ALARM_BMS_TEMP_LOW_DCHG` | Pin quá lạnh khi xả | INFO | INFO | 0 ms | 200 ms |
| **W003** | `ALARM_BMS_TEMP_RELAY_HIGH` | Nhiệt độ MOS / Relay cao | INFO | INFO | 0 ms | 200 ms |
| **E007** | `ALARM_BMS_OVER_CHG_CURR` | Quá dòng sạc pin | STOP | STOP | 0 ms | 200 ms |
| **W004** | `ALARM_BMS_OVER_DCHG_CURR` | Quá dòng xả pin | INFO | INFO | 0 ms | 200 ms |
| **E010** | `ALARM_MOD_HW_FAULT` | Lỗi phần cứng bộ sạc | STOP | **INFO (Giảm tải)** | 1.0s / 10s | 200 ms |
| **W010** | `ALARM_MOD_COMM_FAIL` | Mất giao tiếp bộ sạc | INFO | **INFO (Giảm tải)** | 2.0s | 200 ms |
| **E011** | `ALARM_MOD_OVER_TEMP` | Nhiệt độ bộ sạc cao | STOP | **INFO (Giảm tải)** | 0 ms | 200 ms |
| **E012** | `ALARM_MOD_OVER_VOLT_OUT` | Điện áp DC đầu ra sạc cao | **ESTOP** | **ESTOP (Toàn trạm)** | 0 ms | 200 ms |
| **E013** | `ALARM_MOD_SHORT_CIRCUIT` | Ngắn mạch đầu ra sạc | **ESTOP** | **ESTOP (Toàn trạm)** | 0 ms | 200 ms |
| **W011** | `ALARM_MOD_AC_UNDER_VOLT` | Điện lưới AC bị yếu | INFO | INFO | 1.0s | 3.0s |
| **E014** | `ALARM_MOD_OVER_CURR_OUT` | Quá dòng đầu ra bộ sạc | STOP | **INFO (Giảm tải)** | 0 ms | 200 ms |
| **E015** | `ALARM_MOD_PFC_FAULT` | Lỗi khối nguồn bộ sạc | STOP | **INFO (Giảm tải)** | 0 ms | 200 ms |
| **E016** | `ALARM_MOD_FAN_FAULT` | Lỗi quạt tản nhiệt bộ sạc | STOP | **INFO (Giảm tải)** | 0 ms | 200 ms |
| **E017** | `ALARM_MOD_AC_OVER_VOLT` | Điện áp AC đầu vào sạc cao | STOP | **INFO (Giảm tải)** | 0 ms | 200 ms |
| **E021** | `ALARM_BMS_COMM_LOST` | Mất giao tiếp CAN pin | INFO (Stop an toàn) | INFO (Stop an toàn) | 5.0s | 1.0s |
| **E022** | `ALARM_BMS_NO_PACK_VOLTAGE` | Không có PIN | STOP | STOP | 1.0s | 2.0s |
| **E023** | `ALARM_DC_LOAD_LOST` | Mất tải đột ngột | STOP | STOP | 1.0s | 2.0s |
| **E024** | `ALARM_DC_OUT_NOT_ESTABLISHED`| Chưa thiết lập đầu ra DC | STOP | STOP | 0 ms | 2.0s |
| **E027** | `ALARM_CTRL_NO_MODULE` | Lỗi mất hết bộ sạc | INFO (Khóa Start) | INFO (Khóa Start) | 0 ms | 200 ms |
| **E028** | `ALARM_CTRL_MODULE_MISMATCH` | Số bộ sạc không khớp | INFO | **INFO (Bật Derating)** | 0 ms | 200 ms |
| **E029** | `ALARM_CTRL_INVALID_CONFIG` | Lỗi cấu hình sạc | INFO (Khóa Start) | INFO (Khóa Start) | 0 ms | 200 ms |
| **E030** | `ALARM_CTRL_JACK_OVER_V` | Sụt áp jack sạc | STOP | STOP | 0 ms | 200 ms |
| **E031** | `ALARM_CTRL_JACK_OVER_TEMP` | Nhiệt độ jack sạc cao | STOP | STOP | 0 ms | 200 ms |
| **E032** | `ALARM_BMS_VOLT_MISMATCH` | Lỗi nhầm sạc | STOP | STOP | 1.0s | 1.0s |

---

## 4. Bảng Tra Cứu ZCAN Test Nhanh 2 Module Lianming Song Song

* **Cấu hình ZCAN:** 125 Kbps, Extended Frame 29-bit.
* **Ví dụ mẫu:** Sạc $56.8\text{V}$, tổng dòng sạc $60\text{A}$ ($30\text{A}$/module):

| Bước | Mục Đích | CAN ID (Hex) | DLC | Dữ Liệu DATA (Hex) | Chu Kỳ | Phản Hồi Mong Đợi |
| :---: | :--- | :---: | :---: | :--- | :---: | :--- |
| **1** | Bật tự share dòng | `0x19C21880` | `6` | `00 00 00 AA 00 00` | 1 lần | Broadcast Addr 0, không ACK |
| **2** | Cài đặt 56.8V, 30A/con | `0x1907C080` | `8` | `00 00 75 30 00 00 DD E0` | 1 lần/1s | Cả 2 module cùng nhận lệnh |
| **3** | Bật nguồn (Power ON) | `0x1907C080` | `8` | `02 00 00 00 00 00 00 55` | 1 lần | Cả 2 module khởi động ngõ ra |
| **4a**| Đọc thông số Module 1 | `0x1907C081` | `8` | `01 00 00 00 00 00 00 00` | 200ms | Nhận `0x1807C081`: V, I, Flags M1 |
| **4b**| Đọc thông số Module 2 | `0x1907C082` | `8` | `01 00 00 00 00 00 00 00` | 200ms | Nhận `0x1807C082`: V, I, Flags M2 |
| **—** | Sync dòng tự động | `0x18078081/82`| `8`| *(Chạy ngầm nội bộ)* | 400ms | Thấy 2 module tự trao đổi |
| **5** | Tắt nguồn (Power OFF)| `0x1907C080` | `8` | `02 00 00 00 00 00 00 AA` | Khi dừng| Cả 2 module cùng ngắt an toàn |
