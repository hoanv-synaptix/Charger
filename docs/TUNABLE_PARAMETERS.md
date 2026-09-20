# Tunable Parameters Index

## Tại sao có file này

Firmware hiện có **~50 hằng số điều chỉnh được** (timeout, debounce, ngưỡng,
chu kỳ) nằm rải rác trong ~10 file thuộc 4 layer khác nhau (`App/Alarm`,
`App/Charge`, `App/Protocol`, `App/System`, `Modules/bms`, `Modules/chg_lib`,
`BSP`) — không có 1 file duy nhất để xem toàn cảnh "hệ thống đang tune thế
nào" hay "giá trị nào đã xác nhận trên hardware thật, giá trị nào còn là
đoán mặc định".

**File này KHÔNG di chuyển code** — mỗi hằng số vẫn nằm nguyên tại chỗ cũ,
ngay cạnh code dùng nó (đúng theo tinh thần đã thiết lập trong repo, VD
comment tại `BMS_CHARGE_VOLT_LIMIT_PCT` trong `Modules/bms/bms_core.h` giải
thích rõ lý do không tách nó ra làm magic number riêng). File này chỉ là
**bản đồ tra cứu** — mở 1 chỗ để biết cái gì đang tune ở đâu, giá trị bao
nhiêu, có đáng tin hay chưa.

**Cột "Xác nhận"**:
- `HW-confirmed` — đã kiểm chứng bằng hardware/scope thật.
- `HW-TBD` — giá trị khởi điểm hợp lý, **chưa** kiểm chứng trên hardware thật,
  cần đo lại (nguyên tắc dự án: không đoán mò timing khi chưa có scope/logic
  analyzer thật, `CLAUDE.md` §6).
- `Protocol spec` — lấy từ tài liệu giao thức nhà cung cấp module (Maxwell/
  Lianming/TonHe/BMS), không phải giá trị tự chọn.
- `Product decision` — giá trị do người dùng/sản phẩm quyết định trực tiếp
  (không phải kỹ thuật đo được), xem ghi chú.

---

## App/Alarm — `App/Alarm/alarm.c`

Toàn bộ block này đã tự đánh dấu **HW-TBD** ngay trong file gốc (dòng 12-15):
*"Tuning constants below are STARTING VALUES -- they gate relay / fault
behaviour and have not been checked against a real DC bus / scope"*.

| Hằng số | Dòng | Giá trị | Ý nghĩa | Xác nhận |
|---|---|---|---|---|
| `ALARM_I_LOAD_MIN_A` | 34 | 2.0 A | Dòng dưới mức này coi như "không tải" | HW-TBD |
| `ALARM_LOAD_LOST_I_FRAC` | 39 | 0.15 | Dòng đo sập dưới tỉ lệ này so với dòng lệnh → nghi mất tải DC | HW-TBD |
| `ALARM_V_AT_TARGET_FRAC` | 40 | 0.98 | Áp lệnh phải đạt tỉ lệ này so với target mới coi là "đã ổn định" | HW-TBD |
| `ALARM_LOAD_LOST_MS` | 41 | 800 ms | Debounce xác nhận mất tải DC | HW-TBD |
| `ALARM_DC_OUT_CONFIRM_MS` | 45 | 5000 ms | Sau lệnh đóng relay, dòng thật phải xuất hiện trong khoảng này | HW-TBD |
| `ALARM_V_PACK_FLOOR_FRAC` | 50 | 0.5 | BMS online nhưng áp pack dưới tỉ lệ này so với Vmax cấu hình → nghi chưa đấu pin | HW-TBD |
| `ALARM_AC_PHASE_EPS_V` | 54 | 20.0 V | Ngưỡng phân biệt "module không báo AC" (bỏ qua) và "AC thật = 0V" | HW-TBD |
| `ALARM_AC_PHASE_FLOOR_V` | 55 | 150.0 V | Dưới mức này coi là mất pha AC | HW-TBD |
| `ALARM_AC_PHASE_LOSS_FRAC` | 56 | 0.5 | Tỉ lệ lệch pha để coi là mất pha | HW-TBD |
| `ALARM_AC_UNDERVOLT_V` | 57 | 170.0 V | Ngưỡng AC under-voltage | HW-TBD |
| `ALARM_DB_MIRROR_CLEAR_MS` | 60 | 200 ms | Debounce clear cho alarm mirror (nguồn đã debounce sẵn) | HW-TBD |
| `ALARM_DB_COMM_SET_MS` | 61 | 200 ms | Debounce set mất comms | HW-TBD |
| `ALARM_DB_COMM_CLEAR_MS` | 62 | 500 ms | Debounce clear mất comms | HW-TBD |
| `ALARM_DB_NO_PACK_SET_MS` | 63 | 500 ms | Debounce set "chưa đấu pin" | HW-TBD |
| `ALARM_DB_NO_PACK_CLEAR_MS` | 64 | 1000 ms | Debounce clear "chưa đấu pin" | HW-TBD |
| `ALARM_DB_AC_MS` | 65 | 1000 ms | Debounce alarm AC | HW-TBD |
| `ALARM_LOG_MIN_INTERVAL_MS` | 73 | 1000 ms | Giới hạn tần suất LOG() console (tránh chặn ISR, xem comment tại chỗ) | Product decision (bảo vệ timing LOG, không phải ngưỡng vật lý) |

## App/Charge — `App/Charge/charge_controller.c`

| Hằng số | Dòng | Giá trị | Ý nghĩa | Xác nhận |
|---|---|---|---|---|
| `CHARGE_CTRL_MODULE_VOLTAGE_MAX_AGE_MS` | 24 | 2000 ms | Tuổi tối đa dữ liệu áp module dùng cho hoàn thành Standalone | HW-TBD |
| `CHARGE_CTRL_STANDALONE_VMAX_CONFIRM_MS` | 25 | 1000 ms | Giữ áp ≥Vmax bao lâu mới coi Standalone hoàn thành (FR-CTRL-05) | Product decision (xác nhận qua SRS) |
| `RELAY_OPEN_CURRENT_THRESHOLD_A` | 34 | 1.0 A | Ngưỡng "dòng ≈ 0" để cho phép mở relay (tránh hồ quang) | HW-TBD — xác nhận 2026-08-29, xem `docs/AUDIT_Findings.md` §8g |
| `CHARGE_CTRL_RAMP_STEP_MS` | 58 | 100 ms | Chu kỳ bước ramp áp/dòng | HW-TBD |
| `CHARGE_CTRL_CURRENT_RAMP_A_PER_S` | 68 | 5.0 A/s | Tốc độ ramp-up dòng khởi động mềm | HW-confirmed (CL-01 HIL pass) |
| `PRECHARGE_HOLD_MS` | 72 | 60000 ms | Thời gian giữ nạp hồi phục sau khi BMS thức tỉnh | HW-confirmed (PC-01/02 HIL pass) |
| `PRECHARGE_VOLTAGE_TOLERANCE_V` | 71 | 1.0 V | Dung sai áp module so với Vlow khi xác nhận precharge | HW-confirmed (HIL pass) |
| `DEFAULT_IMAX_A` | 25 | 100.0 A | Trần dòng nạp an toàn tuyệt đối tính theo Ampe (A) | Product decision (Config V8) |

## App/Protocol — `App/Protocol/pc_debug_protocol.h`

| Hằng số | Dòng | Giá trị | Ý nghĩa | Xác nhận |
|---|---|---|---|---|
| `DEBUG_STREAM_INTERVAL_MS` | 42 | 1000 ms | Chu kỳ MCU tự đẩy ALL_MODULES+SYSTEM_INFO+BMS_DATA cho PC app | Product decision (đổi từ 200ms→1000ms, xác nhận qua live test — commit `004b2ee`) |

## App/System — `App/System/app_main.c`

| Hằng số | Dòng | Giá trị | Ý nghĩa | Xác nhận |
|---|---|---|---|---|
| `APP_PROCESS_INTERVAL_MS` | 38 | 20 ms | Chu kỳ control loop chính | HW-TBD, nhưng đã trải qua nhiều đợt HIL test thật trong phiên làm việc này (relay, module, BMS) không phát sinh vấn đề — chưa có bench đo timing chính thức riêng cho giá trị này |
| `APP_LED_INTERVAL_MS` | 39 | 100 ms | Chu kỳ cập nhật LED RUN/FAULT | HW-TBD (thẩm mỹ, không an toàn) |
| `APP_BTN_DEBOUNCE_MS` | 40 | 50 ms | Debounce nút vật lý (BUTTON_1) | HW-TBD |

## Modules/bms — `Modules/bms/bms_core.h`

| Hằng số | Dòng | Giá trị | Ý nghĩa | Xác nhận |
|---|---|---|---|---|
| `BMS_OFFLINE_TIMEOUT_MS` | 46 | 5000 ms | BMS coi là OFFLINE sau khoảng này không có data | Product decision (bội số hợp lý của chu kỳ frame BMS, không phải số lấy thẳng từ tài liệu spec) |
| `BMS_STALE_THRESHOLD_MS` | 47 | 2000 ms | Data coi là "stale" (cảnh báo, không gate relay — xem DES-02) sau khoảng này | Product decision |
| `BMS_CTRL_TX_INTERVAL_MS` | 48 | 500 ms | Chu kỳ gửi Ctrl_INFO cho BMS (FR-BMS-06, Mandatory) | Protocol spec |

Ghi chú: runtime hiện dùng timeout riêng theo từng frame định kỳ; `ALM_INFO` là
frame event-triggered và không được đánh stale chỉ vì không xuất hiện.

## Modules/chg_lib — driver-internal (KHÔNG tách khỏi file gốc, xem lý do trong phần "Vì sao không gom hết")

### TonHe — `Modules/chg_lib/chg_lib_driver_tonhe.h`

| Hằng số | Dòng | Giá trị | Xác nhận |
|---|---|---|---|
| `TONHE_WARNING_TIMEOUT_MS` | 65 | 2000 ms | Product decision (giá trị giống hệt nhau ở cả 3 driver -- house watchdog policy, không phải số riêng từng vendor spec) |
| `TONHE_OFFLINE_TIMEOUT_MS` | 66 | 10000 ms | Product decision (giá trị giống hệt nhau ở cả 3 driver -- house watchdog policy, không phải số riêng từng vendor spec) |
| `TONHE_RECOVERY_DELAY_MS` | 67 | 3000 ms | Product decision (giá trị giống hệt nhau ở cả 3 driver -- house watchdog policy, không phải số riêng từng vendor spec) |
| `TONHE_CONFIRM_TIMEOUT_MS` | 68 | 1000 ms | Protocol spec |
| `TONHE_POLL_INTERVAL_MS` | 69 | 20 ms | Protocol spec |
| `TONHE_HEARTBEAT_INTERVAL_MS` | 70 | 1000 ms | Protocol spec |
| `TONHE_MAX_RETRY` | 71 | 3 | Product decision |

### Lianming — `Modules/chg_lib/chg_lib_lianming.c`

| Hằng số | Dòng | Giá trị | Xác nhận |
|---|---|---|---|
| `LM_WARNING_TIMEOUT_MS` | 78 | 2000 ms | Product decision (giá trị giống hệt nhau ở cả 3 driver -- house watchdog policy, không phải số riêng từng vendor spec) |
| `LM_OFFLINE_TIMEOUT_MS` | 79 | 10000 ms | Product decision (giá trị giống hệt nhau ở cả 3 driver -- house watchdog policy, không phải số riêng từng vendor spec) |
| `LM_RECOVERY_DELAY_MS` | 80 | 3000 ms | Product decision (giá trị giống hệt nhau ở cả 3 driver -- house watchdog policy, không phải số riêng từng vendor spec) |
| `LM_STEP_DELAY_MS` | 81 | 50 ms | Protocol spec |
| `LM_START_CONFIRM_TIMEOUT_MS` | 82 | 500 ms | Protocol spec |
| `LM_MAX_RETRY` | 83 | 3 | Product decision |
| `LM_STOP_MAX_RETRY` | 84 | 5 | Product decision (B-07) |
| `LM_DIAG_INTERVAL` | 85 | 4 chu kỳ | Product decision |

### Maxwell — `Modules/chg_lib/chg_lib_maxwell.c`

| Hằng số | Dòng | Giá trị | Xác nhận |
|---|---|---|---|
| `MXR_WARNING_TIMEOUT_MS` | 60 | 2000 ms | Product decision (giá trị giống hệt nhau ở cả 3 driver -- house watchdog policy, không phải số riêng từng vendor spec) |
| `MXR_OFFLINE_TIMEOUT_MS` | 61 | 10000 ms | Product decision (giá trị giống hệt nhau ở cả 3 driver -- house watchdog policy, không phải số riêng từng vendor spec) |
| `MXR_RECOVERY_DELAY_MS` | 62 | 3000 ms | Product decision (giá trị giống hệt nhau ở cả 3 driver -- house watchdog policy, không phải số riêng từng vendor spec) |
| `MXR_START_VOLTAGE_DELAY_MS` | 66 | 50 ms | Protocol spec |
| `MXR_START_CURRENT_DELAY_MS` | 67 | 50 ms | Protocol spec |
| `MXR_START_CONFIRM_WAIT_MS` | 68 | 100 ms | Protocol spec |
| `MXR_START_CONFIRM_READS` | 69 | 5 lần đọc | Product decision |
| `MXR_STOP_RETRY_INTERVAL_MS` | 71 | 500 ms | Product decision (D-03) |

### Shared FSM — `Modules/chg_lib/chg_lib_fsm.c`

| Hằng số | Dòng | Giá trị | Ý nghĩa | Xác nhận |
|---|---|---|---|---|
| `FSM_RECOVERY_DELAY_MS` | 10 | 3000 ms | Dùng chung cho cả 3 driver qua helper FSM chia sẻ | Product decision (khớp RECOVERY_DELAY_MS của cả 3 driver) |

## BSP — `BSP/bsp_adc.c`

| Hằng số | Dòng | Giá trị | Ý nghĩa | Xác nhận |
|---|---|---|---|---|
| `BSP_ADC_RESAMPLE_INTERVAL_MS` | 26 | 200 ms | Chu kỳ lấy mẫu lại NTC ADC | HW-TBD — cơ chế re-arm đã fix + verify qua host test/build (commit `fcd4953`), nhưng commit đó tự ghi rõ "still needs a bench check: unplug/heat an NTC" — **chưa** xác nhận trên hardware thật |

---

## Feature toggle (compile-time, đã tập trung đúng chỗ — không cần gom)

Khác với các hằng số tuning ở trên, cờ bật/tắt tính năng biên dịch đã tập
trung sẵn ở `CMakeLists.txt` (đúng chỗ, vì đây là thứ quyết định code có
được compile vào hay không, không phải giá trị số để `#define` header gate
gọn được):

| Cờ | Định nghĩa tại | Mặc định | Ý nghĩa |
|---|---|---|---|
| `CHG_DEBUG_DWIN` | `CMakeLists.txt:17` | OFF | Bật lệnh debug relay USB→RS485 DWIN (chỉ dùng bench) |
| `CHG_DEBUG_RAW_CAN` | `CMakeLists.txt:60-65` | ON (Debug build) | Bật `DEBUG_CMD_SEND_RAW_CAN`, tự tắt ở Release |

---

## Vì sao không gom hết vào 1 file code

Đã cân nhắc tách toàn bộ ~50 hằng số này vào 1 header dùng chung. Quyết định
**không làm vậy** làm phương án chính, vì:

1. Repo đã có tiền lệ rõ ràng: giữ hằng số **ngay cạnh code dùng nó**, chú
   thích chéo thay vì gom — xem `BMS_CHARGE_VOLT_LIMIT_PCT`
   (`Modules/bms/bms_core.h`) có hẳn đoạn comment giải thích lý do KHÔNG
   tách nó thành magic number riêng ở `charge_controller.c`.
2. Các hằng số nội bộ driver (`TONHE_*`/`LM_*`/`MXR_*`) gắn chặt với protocol
   CAN riêng từng hãng — kỹ sư chỉnh "Lianming retry mấy lần thì bỏ cuộc"
   cần đọc đúng ngữ cảnh driver đó, tách ra chỗ khác chỉ thêm 1 lớp gián
   tiếp không giúp ích gì nhiều.
3. Gom tất cả vào 1 file có nguy cơ biến file đó thành đúng kiểu "thùng rác
   không phân loại" mà việc này đang muốn tránh (AGENTS.md 2.1: không thêm
   abstraction không xứng đáng).

File `TUNABLE_PARAMETERS.md` này giải quyết đúng nhu cầu thật (1 chỗ để
THẤY toàn cảnh + biết cái nào đã xác nhận hardware) mà không cần đổi code,
không rủi ro hành vi, không phá vỡ tính cục bộ code↔hằng số đang có.

Nếu sau này thực sự cần 1 file code tập trung (VD 1 nhóm hằng số product-
level như alarm/relay/button/LED muốn tune cùng lúc mà không đọc driver
internals), có thể làm phase 2 riêng — đặt tại `Utils/app_tuning.h` (layer
`Utils/` đã được `tools/check_architecture.py` cho phép include từ MỌI
layer khác, không cần sửa checker) — nhưng đây là quyết định làm thêm, cần
người dùng xác nhận trước, không nằm trong phạm vi cập nhật lần này.
