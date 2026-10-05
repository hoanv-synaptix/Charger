# Changelog — Charger Firmware & Tools

Tất cả các thay đổi quan trọng theo từng phiên bản phát hành của hệ thống Charger Firmware và Tool điều khiển.

## [V2.0.22] - 2026-10-05

### 🚀 Chuẩn Hóa Báo Lỗi Module Độc Lập & Triệt Tiêu Chồng Lấn (Anti-Cascade Alarm Matrix)
- **Định danh nguồn gốc lỗi theo từng module (`source_id` 1..8):**
  - Mã hóa `source_id` vào byte cao của trường `code` (16-bit) trong `AlarmLogEntry_t`.
  - Tự động bổ sung hậu tố ` [M1]`..` [M8]` vào chuỗi thông báo lỗi UTF-16 tiếng Việt hiển thị trên màn hình DWIN (ví dụ `W010: Mất giao tiếp [M2]`), bảo toàn wire size 8 bytes giao tiếp.
- **Hoàn thiện 7 cây triệt tiêu lỗi gốc (Root-cause Cascade Suppression Trees):**
  - Mất điện lưới AC (`W011`) triệt tiêu lỗi PFC (`E015`), lỗi phần cứng module do xả tụ (`E010`), và mất kết nối module (`W010`).
  - Rút giắc sạc nóng khi đang tải (`E023`) triệt tiêu lỗi mất BMS CAN (`E021`) và không có áp pack (`E022`).
  - BMS hạ dòng do quá nhiệt (`E005`) triệt tiêu lỗi mất tải DC (`E023`).
  - Chế độ chờ sạc (`IDLE`) cho phép xe đi ngủ sâu cả năm mà không báo lỗi giả mất kết nối BMS (`E021`).

### 🛡️ Nâng Cấp Kiến Trúc Phần Mềm & Loại Bỏ Hoàn Toàn Rò Rỉ Trạng Thái FSM
- **Phân tầng kiến trúc một chiều nghiêm ngặt (Zero Architecture Violations):**
  - Gỡ bỏ hoàn toàn direct include `#include "bsp_sys.h"` trong `charge_controller.c` và `#include "main.h"` trong `ota_service.c`.
  - Chuẩn hóa hàm reset hệ thống trong OTA bằng ghi trực tiếp thanh ghi ARM Cortex-M0+ SCB AIRCR (`ota_system_reset()`).
  - Quét tĩnh 117 files: **0 Circular Dependencies**, **0 Dynamic Heap Allocation** (`malloc`/`free` = 0).
- **Triệt tiêu lỗi rò rỉ cờ giảm dòng súng sạc (`jack_temp_derating_active`):**
  - Đảm bảo xóa cờ giảm dòng súng sạc khi kết thúc phiên sạc, ngăn ngừa hiện tượng phiên sạc sau bị giới hạn công suất oan do phiên sạc trước.
- **Tự động hóa CI/CD không người trực (`test\run_all_local.bat`):**
  - Hỗ trợ cờ `NONINTERACTIVE=1` và `CI=1`, sửa triệt để lỗi unescaped `&`.
  - Tích hợp tự động 38 bài test Pytest Sprint 1, kiểm tra kiến trúc và 8 bộ mô phỏng máy chủ E2E (10/10 Stages PASS 100%).

## [V2.0.21] - 2026-09-30

### 🚀 Tối Ưu Hóa Giao Diện Màn Hình & Phản Hồi Cảm Ứng (Resistive Touchscreen Optimization)
- **Gỡ bỏ hoàn toàn chế độ Sleep Mode:**
  - Loại bỏ hoàn toàn tính năng tắt đèn nền sau 5 phút nhàn rỗi để tối ưu hóa tuyệt đối cho màn hình cảm ứng điện trở (Resistive Touchscreen) của DWIN DGUS.
  - Cố định độ sáng đèn nền màn hình ở mức **100%** ngay từ lúc khởi động (`DWIN_SetBrightness(100U)`).
- **Phản hồi cảm ứng & phím bấm không độ trễ (Zero-latency touch response):**
  - Loại bỏ cơ chế nuốt chạm và cửa sổ chặn 300ms. Mọi thao tác chạm của người dùng (nút Start/Stop, bàn phím đăng nhập, phím chuyển trang) và nút bấm vật lý PA15 đều được nhận và xử lý tức thì ngay ở lần chạm đầu tiên, không bị mất nhịp hay trễ lệnh.
- **Duy trì ổn định các tính năng cốt lõi:**
  - Lưu trữ tổng thời gian sạc tích lũy vào Flash V2 (40 bytes) và hiển thị định dạng giờ nguyên (`%luh`) trên trang Setting.
  - Phân bổ dòng song song chuẩn xác cho module Lianming với khả năng tự động thích ứng bảo vệ giảm tải (Derating).

## [V2.0.20] - 2026-09-30

### 🚀 DWIN Sleep Mode & First-Touch Wakeup Nuốt Lệnh (Anti-Misoperation)
- **Cơ chế ngủ tiết kiệm năng lượng và bảo vệ màn hình (`DWIN_SetBrightness`):**
  - Khi hệ thống ở trạng thái `IDLE` (chờ sạc) không có lỗi cảnh báo, sau **5 phút** nhàn rỗi (`DWIN_IDLE_SLEEP_TIMEOUT_MS = 300000U`), MCU tự động gửi lệnh ghi thanh ghi DGUS `VP 0x0082` tắt đèn nền màn hình (độ sáng 0%).
  - Khi đang sạc (`RUNNING`, `STARTING`, `STOPPING`, `PRECHARGE`) hoặc khi có cảnh báo lỗi (`FAULT`, `ERROR`), màn hình luôn sáng 100%, tuyệt đối không ngủ.
- **Cơ chế đánh thức giống điện thoại thông minh (First-Touch Event Swallowed):**
  - Khi màn hình đang tối, nếu người dùng chạm tay vào màn hình DWIN hoặc ấn nút cứng PA15:
    - MCU lập tức đánh thức màn hình bật sáng 100%.
    - Kích hoạt cửa sổ bảo vệ chống dội cảm ứng 300ms (`DWIN_WAKEUP_GUARD_MS = 300U`).
    - **Nuốt lệnh / Drop hoàn toàn** gói tin chạm đầu tiên này, không kích hoạt Start/Stop hay bất kỳ phím chức năng nào bên dưới.
  - Khi có sự cố lỗi hệ thống, cắm pin nhận diện BMS online, hoặc nhận lệnh từ PC App: màn hình tự động thức dậy sáng 100% tức thì.

### 🚀 Tích Lũy Tổng Thời Gian Sạc Vào Flash & Hiển Thị HMI (`VP_SET_UPTIME 0x1128`)
- **Nâng cấp cấu trúc lưu trữ Flash Version 2 (`EnergyRecord_t` 40 bytes):**
  - Mở rộng cấu trúc bản ghi journal 4 sector SPI Flash ngoại lên Version 2, bổ sung trường `uint32_t total_charge_seconds` và bảo toàn căn chỉnh 8-byte, CRC32 độc lập.
  - **Tương thích ngược tuyệt đối (Backward Compatibility):** Tự động nhận diện bản ghi V1 cũ (32 bytes) trên các trạm ngoài thực địa, migrate số Ah và kWh sẵn có mà không làm mất dữ liệu lịch sử.
- **Tích lũy chính xác theo trạng thái sạc thực tế:**
  - Đồng hồ thời gian sạc chỉ tích lũy khi thực sự có dòng nạp (`is_charging == true`). Dừng sạc hoặc tạm dừng do quá nhiệt thì đồng hồ đứng yên.
- **Đồng bộ cơ chế Reset toàn diện:**
  - Lệnh PC App `PC_CMD_ERASE_FLASH` (0x28) xóa sạch cả 3 thông số: `Ah`, `kWh`, `Total Charge Time`.
- **Hiển thị tối ưu trên HMI:**
  - Định dạng hiển thị giờ `"%luh"` (ví dụ: `1000h`, `125h`, `0h`) truyền vào `VP_SET_UPTIME (0x1128)` theo yêu cầu, tích hợp bộ lọc vi sai diff-suppression theo giờ (`cur_h != prev_h`) giúp tiết kiệm triệt để băng thông bus RS485 và xử lý mượt mà khi tích lũy hàng nghìn giờ sạc.

---

### 🚀 Phân Tầng Debounce Động (Dynamic State-Dependent Debounce) Cho Lỗi Phần Cứng (E010)
- **Tối ưu thời gian lọc trễ theo trạng thái vận hành (`ALARM_MOD_HW_FAULT`):**
  - **Khi ở chế độ chờ IDLE:** Thiết lập debounce **10.0 giây** (`ALARM_DB_HW_FAULT_IDLE_SET_MS = 10000U`). Bao trọn toàn bộ chu trình xả nạp tụ 3-8s của module LianMing/TonHe khi người dùng bật/tắt Aptomat, loại bỏ 100% hiện tượng báo lỗi phần cứng ảo `E010` lúc tắt máy. Đồng thời, nếu module thật sự bị hỏng hóc phần cứng trong lúc cắm điện chờ ở IDLE (lỗi duy trì liên tục $> 10\text{s}$), hệ thống vẫn ghi nhận và cảnh báo `E010` chuẩn xác, không bỏ sót lỗi.
  - **Khi đang trong chu trình sạc (RUNNING / PRECHARGE / STARTING):** Duy trì debounce **1.0 giây** (`ALARM_DB_HW_FAULT_ACTIVE_SET_MS = 1000U`), bảo đảm phản ứng khẩn trương dừng sạc (`ALARM_ACT_STOP`), ngắt dòng điện và mở contactor DC bảo vệ khối pin và thiết bị kịp thời.
- **Hoàn thiện bộ test tự động E2E:**
  - Bổ sung test case kiểm chứng phân tầng: Phản ứng ngắt sạc nhanh sau 1.0s khi đang RUNNING, và lọc trễ 10.0s chống nhiễu xả nạp tụ khi ở IDLE.

---

## [V2.0.13] - 2026-09-28

### 🚀 Triệt Tiêu Lỗi Ảo Phần Cứng (E010) Khi Ngắt Nguồn AC Ở Chế Độ IDLE
- **Sửa ánh xạ cờ trạng thái module TonHe (`chg_lib_tonhe.c`):**
  - Chuyển phân loại cờ Bit 11 (`PFC shutdown`) và Bit 8 (`Bus exception`) từ `CHG_LIB_ALARM_HW_FAULT` sang `CHG_LIB_ALARM_PFC_FAULT`. Khắc phục triệt để việc module bị nhận diện nhầm là "Lỗi phần cứng" khi tụ DC-link xả điện lúc ngắt nguồn AC/Aptomat.
  - Sửa cờ `pfc_bits bit 3` (`DCTz fault`) ánh xạ đúng vào `CHG_LIB_ALARM_PFC_FAULT`.
- **Bổ sung bộ lọc trễ cho cảnh báo phần cứng (`ALARM_MOD_HW_FAULT`):**
  - Thiết lập `ALARM_DB_HW_FAULT_SET_MS = 1000U` (1.0 giây). Lọc sạch các frame nhiễu/hấp hối (dying frames) trong giai đoạn vi điều khiển module sạc tắt nguồn, loại bỏ hoàn toàn hiện tượng báo lỗi rồi tự xóa về `CODE: 0000` trước khi sập nguồn.
- **Nâng cấp cơ chế khóa chặn cảnh báo tầng (`ev_mod_hw_fault` & `ev_mod_pfc`):**
  - Mở rộng mặt nạ triệt tiêu `E010`: Bất cứ khi nào nguồn AC xuất hiện bất thường (sụt áp, mất pha, lỗi PFC, lệch tần số), cảnh báo `E010 Lỗi phần cứng bộ sạc` sẽ tự động bị khóa chặn.
  - Ở trạng thái IDLE (Standby), nếu phát hiện module chuyển sang offline do người dùng ngắt nguồn AC, hệ thống sẽ ức chế hoàn toàn các cờ báo lỗi phần cứng và PFC, đảm bảo quá trình tắt máy (power-down) diễn ra êm đềm, không ghi log rác vào màn hình DWIN HMI.

---

## [V2.0.12] - 2026-09-28

### 🚀 Tối Ưu Hệ Thống Cảnh Báo (Alarm Debounce) & Bảng Hiển Thị Lịch Sử DWIN HMI
- **Chuẩn hóa bộ lọc trễ sụt áp AC lưới đầu vào (`ALARM_MOD_AC_UNDER_VOLT` - W011):**
  - Tối ưu thời gian kích hoạt lỗi `ALARM_DB_AC_UNDERVOLT_SET_MS` về mức 1.0 giây (1000ms), đảm bảo cảnh báo sụt áp AC được xác lập trước khi tụ điện nguồn DC-link trong module sạc xả hết (1.5s - 2.5s).
  - Khóa chặn hoàn toàn chuỗi lỗi dây chuyền ảo `W010 Module comms fail` khi nguồn AC bị cắt đột ngột.
  - Thiết lập thời gian trễ ổn định lưới điện phục hồi `ALARM_DB_AC_UNDERVOLT_CLEAR_MS = 3000ms`, triệt tiêu hoàn toàn hiện tượng chập chờn lưới (AC grid chattering/flapping), ngăn ngừa spam log và nhấp nháy màn hình HMI.
- **Tối ưu hiển thị bảng cảnh báo DWIN HMI theo thứ tự thời gian mới nhất (Chronological Latest-First):**
  - Bổ sung hàm API `DWIN_Alarm_SyncTable` bảo toàn trật tự hiển thị các sự kiện lỗi mới nhất lên các dòng đầu tiên (trang 1) của bảng Alarm DWIN HMI.
  - Khắc phục lỗi đọc dữ liệu rác trên stack do biến uninitialized `s_last_log_sequence`.
  - Triển khai cơ chế lọc dữ liệu vi sai theo từng dòng (`s_alarm_row_cached`), giảm 100% lưu lượng UART không cần thiết ở trạng thái tĩnh.
  - Lập lịch công bằng vòng tròn (Round-Robin Fair Scheduling) với biến `s_alarm_emit_rr`, loại bỏ hoàn toàn hiện tượng nghẽn hiển thị trang 2 (dòng 4..7) và trang 3 (dòng 8..11).
  - Tăng chu kỳ heartbeat đồng bộ cài đặt DWIN lên 10.000ms để tối ưu băng thông đường truyền RS485.
- **Hoàn thiện bộ kiểm thử tự động (Unit & E2E Tests):**
  - 100% vượt qua 23 test cases của bộ kiểm thử giao thức DWIN (`test_dwin_protocol_e2e.c`).
  - 100% vượt qua 35 test cases của bộ mô phỏng hệ thống cảnh báo E2E (`test_alarm_e2e.c`).

---

## [V2.0.11] - 2026-09-28

### 🚀 Kiểm Thử Đối Chuẩn Độc Lập HIL Toàn Diện & Chuẩn Hóa Logic Sạc
- **Kiểm thử đối chuẩn HIL độc lập 100% cho 2 Module TonHe & LianMing:**
  - Hoàn tất và vượt qua toàn bộ 34 test cases $\times$ 2 module = 68 lượt chạy closed-loop thực tế trên vi điều khiển STM32G0B0, bao gồm: Full Automation (7 cases), In-Charge Real-World (12 cases), Pre-Charge (6 cases), Charging Logic (9 cases).
- **Chuẩn hóa giá trị mặc định cấu hình phân tầng (`ChargeCycleConfig_GetDefaults`):**
  - Khởi tạo đầy đủ và chính xác các mốc điện áp cell ($3.20\text{V} \rightarrow 3.60\text{V}$, $1.0\text{C} \rightarrow 0.3\text{C}$), phân tầng nhiệt độ pin ($10^\circ\text{C} \rightarrow 55^\circ\text{C}$, trễ $5.0^\circ\text{C}$) và SOC ($20\% \rightarrow 95\%$) theo đúng tài liệu kỹ thuật khi khôi phục cài đặt gốc hoặc khởi động lần đầu.
- **Nâng cấp lệnh xóa lỗi PC Protocol (`PC_CMD_RESET_FAULT` 0x0A):**
  - Tích hợp thêm `Alarm_Acknowledge()`, xóa trạng thái dừng khẩn cấp E-Stop, chốt lỗi bộ điều khiển sạc và xác nhận hoàn tất chu trình sạc chỉ trong một lệnh duy nhất.
- **Tối ưu truyền thông DWIN RS485 HMI:**
  - Cải tiến cửa sổ tĩnh lặng chống xung đột truyền nhận trên đường truyền RS485 DWIN (cửa sổ 8ms), giúp nhận dạng tức thời các thao tác cảm ứng phím bấm từ người dùng.
- **Định dạng thời gian Alarm Rollover:**
  - Chuẩn hóa chuỗi thời gian khi chuyển sang ngày khác thành định dạng liền mạch `06h26/09` (`%02uh%02u/%02u`, 8 ký tự), loại bỏ khoảng trống thừa trên màn hình DWIN.
- **Tăng lọc trễ lỗi sụt áp AC lưới đầu vào:**
  - Tăng thời gian lọc trễ bảo vệ sụt áp lưới AC (`ALARM_DB_AC_UNDERVOLT_SET_MS`) từ 5 giây lên 10 giây nhằm chống cảnh báo ảo khi tắt máy do tụ nguồn xả điện.
- **Cải tiến đo lường cảm biến nhiệt độ jack sạc NTC:**
  - Tích hợp hiệu chuẩn ADC phần cứng lúc khởi động (`HAL_ADCEx_Calibration_Start`), tăng thời gian lấy mẫu lên 79.5 cycles, bổ sung bộ lọc số mũ EMA ($\alpha = 0.20$) và giám sát lỗi đứt/chập mạch cảm biến.
- **Định dạng Hardware Version:**
  - Chuẩn hóa chuỗi hiển thị HW Revision thành `V1.0.0` (3 trường major.minor.patch).

---

## [V2.0.9] - 2026-09-26

### 🚀 Tích hợp Địa chỉ CAN Module Sạc (`module_address`)
- **Tùy biến địa chỉ CAN Module sạc:**
  - Bổ sung trường `module_address` vào `ChargeCycleConfig_t` (struct Version 9, kích thước 254 bytes), cho phép người dùng cấu hình địa chỉ trạm CAN của module linh hoạt từ 0 đến 240 (mặc định là 1).
  - Hỗ trợ module Maxwell có địa chỉ xuất phát từ `0` (`00..63` theo chuẩn Maxwell V1.50 §2.2.3), Lianming (`1..60`), và TonHe (`1..240`).
  - Khi khởi động hoặc khi nạp cấu hình mới, MCU đăng ký danh sách module theo dải `[base_addr .. base_addr + count - 1]`.
- **Tương thích ngược & Migration Flash:**
  - Tự động nhận diện và migrate các bản ghi Flash cũ (v5: 239B, v6: 243B, v7: 249B, v8: 253B) lên v9 (254B), tự động gán `module_address = 1` cho các bản ghi cũ mà không làm mất cấu hình trạm.
  - Giao thức nhị phân `DEBUG_CMD_SET_CHARGE_CFG` (0x1A) hỗ trợ cả gói tin 254 bytes mới và các gói tin legacy cũ.

---

## [V2.0.8] - 2026-09-24

### 🚀 Nâng cấp & Khắc phục Module Lianming & Hệ thống
- **Thời gian xác lập dòng ra DC (E024):** Tăng deadline timeout từ 5s lên 12s (`ALARM_DC_OUT_CONFIRM_MS 12000U`), cho phép module Lianming có đủ thời gian sinh dòng mượt mà mà không kích hoạt cảnh báo E024 giả. Ngay khi có dòng (> 2.0A), hệ thống tiếp tục chu trình sạc tức thì.
- **Tương thích giao thức CAN Lianming V2.0:**
  - Hỗ trợ cả 2 Base Return ID cho nhiệt độ (`0x18008080` theo bảng tổng hợp và `0x19008080` theo Message Example 8 trong PDF hãng), đảm bảo nhận và giải mã nhiệt độ môi trường/tản nhiệt 100% với mọi phiên bản firmware module.
  - Map dữ liệu nhiệt độ sang `temp_dcdc` giúp màn hình DWIN và PC App hiển thị nhiệt độ module chính xác.
  - Gửi lệnh đọc nhiệt độ với `DLC = 0` chuẩn theo đặc tả hãng.
  - Tối ưu gateway `lm_feed_frame` chấp nhận các khung ACK 2 bytes (`dlc >= 2`), kiểm tra chặt chẽ DLC theo từng loại lệnh trong parser.
- **Quản lý bộ nhớ Flash từ xa:** Bổ sung giao thức `PC_CMD_ERASE_FLASH` (0x28) hỗ trợ xóa phân vùng cấu hình, lịch sử lỗi và log từ xa qua PC App có bảo mật xác thực Admin PIN.

---

## [V2.0.7] - 2026-09-23

### 🚀 Cảnh báo đa giác quan đồng bộ (Multi-sensory Synchronized Alarm)
- **Đèn START (LED_RUN - PC6):**
  - Khi hệ thống rơi vào trạng thái lỗi (`DWIN_STATUS_ERROR`), đèn vật lý START trên nút nhấn nhấp nháy chu kỳ 1 Hz (500ms SÁNG / 500ms TẮT) để cảnh báo trực quan cho người vận hành từ xa.
  - Khi sạc bình thường (`PC_Protocol_IsCharging` & module online): giữ sáng liên tục như cũ.
  - Khi ở trạng thái Ready / Idle: tắt.
- **Biểu tượng trạng thái ERROR trên màn DWIN (`VP_SYS_STATUS_ICON 0x1041` & `VP_PRECHARGE_STATUS_ICON 0x1518`):**
  - Nhấp nháy đồng bộ 1 Hz cùng đèn START: 500ms hiển thị icon ERROR (4) / 500ms ẩn (ghi `0xFFFF` - DGUS render trong suốt).
- **Mã lỗi Topbar (`VP_TOPBAR_FAULT_CODE 0x1044`):**
  - Nhấp nháy đồng bộ 1 Hz: 500ms hiển thị mã lỗi ưu tiên cao nhất (ví dụ `E006`) / 500ms tắt (ghi `"    "` - 4 khoảng trắng).
- **Nút bấm hành động (Action Button):**
  - Nút `RESET` trên màn hình DWIN luôn được giữ cố định (solid), tuyệt đối không nhấp nháy, giúp người dùng luôn xác định rõ vị trí chạm để thao tác an toàn.
- **Còi cảnh báo DWIN Buzzer (`VP_SYS_BUZZER 0x00A0`):**
  - Kêu bíp ngắn (~160ms = 20 * 8ms) ở đầu mỗi chu kỳ 1 giây (`ERROR_BLINK_PERIOD_MS = 1000ms`), tạo nhịp báo động dồn dập nhưng không gây chói tai liên tục.
  - Tắt còi ngay lập tức (`DWIN_Beep(0)`) khi người dùng nhấn nút xóa lỗi (Reset) hoặc hệ thống tự phục hồi về trạng thái an toàn.
- **Đảm bảo thông suốt đường truyền RS485 DWIN:**
  - Cơ chế scatter diff-suppression được bảo toàn trọn vẹn: MCU chỉ gửi frame khi có sự thay đổi giá trị (1 frame/500ms cho icon/text và 1 frame/1000ms cho còi), tải bus RS485 tăng thêm không đáng kể (~0.1%), không gây nghẽn bus.

### 🧪 Kiểm thử
- **Unit Test DWIN Protocol:** Thêm `test_dwin_beep()` trong `test/host_protocol_sim/test_dwin_protocol_e2e.c`.
- **Unit Test UI & Alarm Blink:** Tạo `test/host_alarm_sim/test_alarm_ui_blink.c` kiểm tra toàn diện LED_RUN 1Hz, Status icon 1Hz, Fault code 1Hz, nút RESET không nháy, Buzzer 160ms và câm ngay khi clear error.
- **Tích hợp Local CI:** Đưa kiểm thử vào `test/run_all_local.bat`.

---

## [V2.0.6] - 2026-09-23

### 🚀 Tính năng
- **Hiển thị ngày/giờ thông minh trong log Alarm:**
  - Sự kiện xảy ra **hôm nay**: hiển thị `08:30:11` (HH:MM:SS, giữ nguyên như cũ).
  - Sự kiện xảy ra **ngày hôm trước**: hiển thị `08h23/09` (giờ + ngày/tháng, 8 ký tự, không thay đổi VP layout DWIN).
  - Khi RTC chưa được đồng bộ: tiếp tục fallback về uptime `HH:MM:SS` (không thay đổi hành vi cũ).
  - Logic tính ngày dựa trên delta uptime + UTC epoch → convert sang local time (UTC+7) để so sánh ngày.

### 🧪 Kiểm thử
- **Unit Test:** Bổ sung `test_alarm_time_format_smart` trong `test/host_alarm_sim/test_alarm_e2e.c` kiểm tra 4 case: RTC invalid, cùng ngày, ngày hôm trước 25h, edge case 23:59 hôm trước.
- **Mock stubs:** Thêm `BSP_RTC_EpochToDateTime` và `BSP_RTC_DateTimeToEpoch` vào `test/mock_hal/mock_stubs.c`.

---

## [V2.0.5] - 2026-09-23

### 🚀 Tính năng & Logic sạc
- **Ưu tiên dung lượng định mức từ BMS (`rate_cap`):**
  - Khi sạc có kết nối BMS, hệ thống tự động ưu tiên lấy trực tiếp dung lượng thiết kế do BMS báo về (`bms->rate_cap * 0.1f`) thay vì lấy giá trị nhỏ nhất `min(config, bms)` như trước.
  - Bỏ giới hạn trần trên, hỗ trợ mọi pack pin dung lượng lớn (1000 Ah, 2000 Ah, 5000 Ah,...).
  - Bảo vệ quá dòng phần cứng vẫn được kiểm soát tuyệt đối qua `cfg.imax_a` và công suất tối đa của module nguồn (`cfg.module_i_max_a`).
- **Cơ chế Fallback an toàn:**
  - Khi sạc chế độ Standalone (không BMS) hoặc khi BMS chưa kịp gửi frame / trả về giá trị rỗng (`0` hoặc `0xFFFF`), hệ thống tự động fallback về dung lượng cài đặt trong bộ nhớ (`cfg->battery_capacity_ah`).

### 🧪 Kiểm thử & Đặc tả
- **E2E Simulation:** Bổ sung các test case kiểm tra dung lượng BMS lớn hơn / nhỏ hơn / rỗng trong `test/host_charge_sim/test_charge_e2e.c`.
- **SRS Document:** Đồng bộ đặc tả yêu cầu chức năng `FR-CTRL-06` trong `docs/SRS_Charger_Controller.md`.

---

## [V2.0.4] - 2026-09-21

### 🚀 Tính năng
- **Đồng bộ thời gian mạng qua LTE:** Triển khai lệnh AT `AT+CTZU=1` và `AT+QLTS=1` lấy thời gian thực từ trạm BTS mạng di động với cơ chế chống lùi thời gian (anti-rollback).
- **Nhận diện phần cứng 4G:** Hỗ trợ parse model module Quectel EG800K, hiển thị trạng thái và fallback an toàn `--` khi mất kết nối.
- **HIL Test Suite:** Bổ sung kịch bản kiểm thử tích hợp HIL cho 4G và thời gian thực.

---

## [V2.0.3] - 2026-09-20

### 🚀 Tính năng & Sửa lỗi
- **Bảo vệ E-Stop & Pre-charge:** Cải tiến quy trình hồi phục sau ngắt khẩn cấp E-Stop, cơ chế bypass lỗi E021 khi kích sạc pin cạn.
- **Đồng bộ cấu hình Config V8:** Tối ưu hóa cấu trúc lưu trữ Flash và quản lý tham số sạc.
- **Tài liệu & SRS:** Cập nhật bảng tham số kỹ thuật và tài liệu kiến trúc.

---

## [V2.0.2] - 2026-09-17

### 🚀 Tính năng
- **OTA Tinh gọn & SPI Flash:** Hỗ trợ nạp nâng cấp qua SPI Flash đệm ngoài với tính toán CRC32 và SHA-256.
- **USB CDC Debug Port:** Ổn định hóa luồng giao tiếp dữ liệu PC và các lệnh điều khiển bench test.

---

## [V2.0.1] - 2026-09-12

### 🚀 Tính năng ban đầu
- Tích hợp hệ thống quản lý cảnh báo (Alarm Subsystem), hiển thị DWIN HMI và điều khiển sạc đa module (Maxwell, Lianming, TonHe).
