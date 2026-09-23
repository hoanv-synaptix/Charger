# Changelog — Charger Firmware & Tools

Tất cả các thay đổi quan trọng theo từng phiên bản phát hành của hệ thống Charger Firmware và Tool điều khiển.

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
