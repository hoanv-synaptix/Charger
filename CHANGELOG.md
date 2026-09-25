# Changelog — Charger Firmware & Tools

Tất cả các thay đổi quan trọng theo từng phiên bản phát hành của hệ thống Charger Firmware và Tool điều khiển.

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
