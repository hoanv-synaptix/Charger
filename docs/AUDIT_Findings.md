# AUDIT Findings — Charger Controller
## Báo Cáo Kiểm Định Senior Product (vs SRS-CHG-CTRL-001 v1.0)

| | |
|---|---|
| **Mã tài liệu** | AUDIT-CHG-CTRL-001 |
| **Phiên bản** | 1.0 |
| **Ngày** | 2026-08-26 |
| **SRS tham chiếu** | SRS-CHG-CTRL-001 v1.0 (IEEE 830 / ISO 29148) |
| **Firmware** | v2.0.0 — STM32G0B1CBT6 @ 64 MHz |
| **Phương pháp** | 6 agents song song (chg_lib / BMS / Charge CTRL / Truyền thông / BSP / Checker) |
| **Tiêu chí Senior** | Robustness ≥8, Maintainability ≥8, Safety ≥8, Testability ≥8 (thang 10) — **hiện tại 5.0/10** |

> **Cách dùng**: mỗi issue có checkbox `- [ ]`. Đánh `x` khi fix xong + ghi commit hash. Sprint được gán sẵn; không tự đổi thứ tự khi chưa xong P0.

> **Re-verification 2026-08-27**: Tài liệu này được viết trước một đợt "Sprint 1 fixes" không rõ ràng trong lịch sử — nhiều bug Critical/Major bên dưới đã được fix trước khi đợt dọn dẹp kiến trúc (PR0-PR3) này bắt đầu. Đã đối chiếu từng dòng trong mục 3 (chg_lib) với source hiện tại; các dòng đã fix được đánh `[x]` kèm ghi chú. B-04/B-05/B-06 được fix commit `1b37101`; B-07/B-09/B-13 được fix commit `d1b427c` (đối chiếu thêm với docs protocol PDF của Maxwell/Lianming/TonHe khi cần). Các mục còn `[ ]` khác (B-08, B-10 đến B-12, B-16, B-17, D-03, D-05, S-04, và toàn bộ mục 4-7) **chưa được re-verify** — coi là hiện trạng thật cho đến khi kiểm tra lại.

---

## Mục lục

1. [Tổng quan Compliance](#1-tổng-quan-compliance)
2. [Chuỗi lỗi Domino](#2-chuỗi-lỗi-domino)
3. [Checklist — Phân hệ chg_lib](#3-checklist--phân-hệ-chglib)
4. [Checklist — Phân hệ BMS](#4-checklist--phân-hệ-bms)
5. [Checklist — Charge Controller + Config/Storage](#5-checklist--charge-controller--configstorage)
6. [Checklist — Truyền thông (PC/BMS/HMI)](#6-checklist--truyền-thông-pc-bms-hmi)
7. [Checklist — BSP / Platform / CubeMX](#7-checklist--bsp--platform--cubemx)
8. [Kế hoạch Sprint tổng hợp](#8-kế-hoạch-sprint-tổng-hợp)
9. [Phụ lục — File tham chiếu & Guard Checklist](#9-phụ-lục--file-tham-chiếu--guard-checklist)

---

## 1. Tổng quan Compliance

| Phân hệ | FR PASS | PARTIAL | FAIL | Kết luận |
|---|---|---|---|---|
| chg_lib (FR-CHG) | 4 | 7 | 0 | Interface đúng, Maxwell bám spec; 7 PARTIAL chạm timeout/alarm/recovery |
| BMS (FR-BMS) | 0 | 5 | 1 | Parse LE/BE đúng nhưng filter std triệt tiêu 4/9 frame → runtime không đạt |
| Charge CTRL (FR-CTRL) | 12 | 4 | 0 | FSM 3 mode + latch đúng; 4 PARTIAL mang rủi ro Inf/flash/stale |
| Config/Storage (FR-CFG) | 3 | 2 | 0 | Record + CRC đúng; 2 PARTIAL là Inf guard + flash OOB |
| Truyền thông (FR-USB/FR-HMI) | 9 | 2 | 2 | Khung AA55/CRC8 chuẩn; DWIN dead code, filter mâu thuẫn SRS |
| BSP/Platform (FR-OPS) | 7 | 7 | 0 | Clock/125K-250K/SWAP đúng; thiếu IWDG, NVIC đồng mức 0, ADC drift |

**Điểm 4 tiêu chí (checker):** Robustness 4.0 · Maintainability 5.5 · Safety 4.5 · Testability 6.0 → **Tổng 5.0/10 — CHƯA đạt Senior.**

---

## 2. Chuỗi lỗi Domino

Một lỗi gốc kéo sập chuỗi phân hệ:

```
[1] CAN filter REJECT std (bsp_can.c:28)
    → BMS 4 frame Std (02F4/04F4/05F4/07F4) mất → BMS OFFLINE
    → Charge CTRL: BMS-Controlled → FAULT BMS_OFFLINE → LED_FAULT

[2] LOG (50ms) trong ISR + TX queue spam 20ms
    → Treo ISR >1ms → mất FDCAN FIFO0 → timeout → RECOVERING loop
    → jitter 20ms → mismatch 10s sai lệch

[3] SelectDriver deinit sai + RemoveModule không giảm count
    → Đổi driver → addr trùng → CAN ID collision 125k

[4] NaN/Inf từ PC → manual_target=NaN → float BE qua CAN → telemetry NaN
    → check_standalone (voltage<=0) fail → sạc không kết thúc

[5] Flash stall + không tắt IRQ → HardFault khi BMS ISR pending
    → mất last_rx_tick → BMS timeout giả → domino [1] lặp
```

---

## 3. Checklist — Phân hệ chg_lib

### 3.1 Compliance FR-CHG

| ID | Kết luận | Bằng chứng |
|---|---|---|
| FR-CHG-01 | PASS | `chg_lib.h:152-172` interface 17 ops; `chg_lib_core.c:20-83` |
| FR-CHG-02 | PASS | `chg_lib.h:50-57`, `CHG_LIB_MAX_DRV 8`, 3 driver MAX 8 |
| FR-CHG-03 | PARTIAL | 8 state đủ nhưng TonHe `OFFLINE/RECOVERING` treo |
| FR-CHG-04 | PARTIAL | Chỉ Maxwell check `Vout>0`, 2 driver lệch |
| FR-CHG-05 | PARTIAL | Chỉ Maxwell poll đủ 15 reg |
| FR-CHG-06 | PARTIAL | RECOVERING retry 3 vs yêu cầu 5 RX |
| FR-CHG-07 | PARTIAL | Maxwell chỉ FAULT khi RUNNING; TonHe chỉ `status==0x11` |
| FR-CHG-08 | PARTIAL | Chỉ Maxwell fallback 20A đúng |
| FR-CHG-09 | PASS | Cả 3 gửi STOP tức thì |
| FR-CHG-10 | PASS | `ModuleView_t` chuẩn |
| FR-CHG-11 | PASS | `CanBackend_t {transmit,now_tick}` |
| NFR-09/10 | PARTIAL | Duplication 70%, helper `chg_lib_fsm.c` chết |

### 3.2 Issues — BUG

- [x] **B-01 — TonHe OFFLINE/RECOVERING chết** — `process_module:OFFLINE/RECOVERING` không tự chuyển sau 3s, không cần 5 RX. `chg_lib_tonhe.c:573-579` · **Critical** · Module mất CAN 10s → treo vĩnh viễn. — _Re-verified 2026-08-27: **ĐÃ FIX** (pre-existing trước audit này, xem `chg_lib_tonhe.c` OFFLINE->RECOVERING + `rx_count - recovery_start_rx_count >= 5`)._
- [x] **B-02 — Maxwell RECOVERING không bao giờ đủ 5 RX** — `retry>=3→OFFLINE` vs cần `rx - recovery_start >=5`. `chg_lib_maxwell.c:527-538` · **Critical** — _Re-verified 2026-08-27: **ĐÃ FIX** (pre-existing, xem `chg_lib_maxwell.c` RECOVERING branch dùng `rx_count - recovery_start_rx_count >= 5`)._
- [x] **B-03 — Lianming RECOVERING tương tự** — `chg_lib_lianming.c:541-552` · **Critical** — _Re-verified 2026-08-27: **ĐÃ FIX** (pre-existing, RECOVERING branch dùng `rx_count - recovery_start_rx_count >= 5` giống Maxwell/TonHe)._
- [x] **B-04 — Maxwell alarm chỉ FAULT khi RUNNING** — `if(alarm && state==RUNNING)` bỏ WARNING/STARTING. `chg_lib_maxwell.c:436-439` · **Major** — _Re-verified 2026-08-27: **ĐÃ FIX** commit `1b37101` (2026-08-27) — bỏ điều kiện `state==RUNNING`, giờ unconditional trừ khi đã FAULT._
- [x] **B-05 — TonHe alarm không tự FAULT** — chỉ FAULT khi `status==0x11`, dù `SHORT_CIRCUIT/OVER_TEMP`. `chg_lib_tonhe.c:312-345` · **Major** — _Re-verified 2026-08-27: **ĐÃ FIX** commit `1b37101` (2026-08-27) — check `alarm_flags` trước status byte, giống Lianming._
- [x] **B-06 — Maxwell STOPPING ACK quá rộng** — mọi `REG_SET_*/ON_OFF` ACK đều `IDLE`. `chg_lib_maxwell.c:401-409` · **Major** — _Re-verified 2026-08-27: **ĐÃ FIX** commit `1b37101` (2026-08-27) — chỉ ACK của `REG_ON_OFF` mới xác nhận STOPPING->IDLE._
- [x] **B-07 — Lianming STOPPING không timeout→FAULT** — chỉ poll 500ms. `chg_lib_lianming.c:527-533` · **Major** — _Re-verified 2026-08-27: **ĐÃ FIX** commit `d1b427c` (2026-08-27) — retry lệnh STOP tối đa `LM_STOP_MAX_RETRY`=5 lần trước khi báo COMM_FAIL/FAULT, giống Maxwell._
- [ ] **B-08 — COMM_FAIL latch vĩnh viễn** — `alarm_flags = parse | (flags & COMM_FAIL)` giữ bit. `chg_lib_maxwell.c:383,542` · **Major**
- [x] **B-09 — NaN/Inf không lọc** — `rated_current_or_fallback` không check NaN, gán float trực tiếp. `chg_lib_maxwell.c:229-242,373-400` `lianming.c:301` `tonhe.c:214` · **Major** — _Re-verified 2026-08-27: **ĐÃ FIX** commit `d1b427c` (2026-08-27) — cả 3 driver reject setpoint non-finite qua `isfinite()` trong set_voltage/set_current_limit (clamp bằng `</>` không bắt được NaN)._
- [ ] **B-10 — Round-robin starvation** — `CHG_LIB_Process` 1 module/lần → START 50ms thành 160ms với 8 module. `chg_lib_maxwell.c:685` `lianming.c:711` `tonhe.c:755` · **Major**
- [ ] **B-11 — Boot online sai** — `last_rx_tick==0` → `since_rx = now` (<10s) nên chưa OFFLINE nhưng logic online sai. `chg_lib_maxwell.c:296-306,272` · **Major**
- [ ] **B-12 — TonHe timing 5s thay vì 1s** — `g_last_timing_tick` 5000 vs SRS 1s. `chg_lib_tonhe.c:757` · **Major**
- [x] **B-13 — Race ISR↔main** — `FeedCanFrame` ghi `view.*` trong khi `process_module` đọc/ghi không `__disable_irq`. `chg_lib_core.c:180-187` `can_backend.c:11,60` · **Major** — _Re-verified 2026-08-27: **ĐÃ FIX** commit `d1b427c` (2026-08-27) — `CHG_LIB_Process`/`CHG_LIB_FeedCanFrame` giờ giữ critical section quanh toàn bộ lệnh gọi driver, không chỉ đọc con trỏ `get_active()`._
- [x] **B-14 — SelectDriver deinit sai** — gọi `old_driver->init()` thay vì `deinit`. `chg_lib_core.c:39-41,65-68` · **Major** — _Re-verified 2026-08-27: **ĐÃ FIX** (pre-existing, xem `chg_lib_core.c:CHG_LIB_SelectDriver` gọi `old_driver->deinit()` với fallback `init()`)._
- [x] **B-15 — RemoveModule lỗ hổng** — không giảm `g_module_count`. `chg_lib_maxwell.c:591` `lianming.c:614` `tonhe.c:647` · **Major** — _Re-verified 2026-08-27: **ĐÃ FIX** (pre-existing, `mx_remove_module`/tương đương compact array + giảm `g_module_count`)._
- [ ] **B-16 — Summary không nhất quán** — Maxwell đếm `IDLE` là online; TonHe chỉ RUNNING/STARTING/WARNING. `chg_lib_maxwell.c:713-737` `lianming.c:784-805` `tonhe.c:828-858` · **Major**
- [x] **B-17 — Stats TX/RX lệch** — Lianming `lm_send_read` không cập nhật `last_tx_tick`. `chg_lib_lianming.c:280-287` · **Minor** — _Re-verified 2026-08-27: **ĐÃ FIX** commit `46c3fe5` (2026-08-27) — `lm_send_read`/`lm_set_output`/`lm_start_module`/`lm_stop_module`/`lm_send_ac_read`/`lm_send_temp_read` giờ đều cập nhật `last_tx_tick`._

### 3.3 Issues — DESIGN / MAINTAINABILITY / SAFETY

- [x] **D-01 — Duplication 70% FSM** — 3 driver tự implement `set_state/check_offline/summary`, helper `chg_lib_fsm.c` chết. `chg_lib_fsm.c:12-32` · **Major** — _Re-verified 2026-08-27: **PHẦN LỚN ĐÃ FIX** (pre-existing) — `CHG_LIB_FSM_SetState`/`CHG_LIB_Summary_Accumulate` trong `chg_lib_fsm.c` được cả 3 driver dùng chung; vẫn còn state-machine timing logic riêng mỗi driver (do khác protocol thật sự, không phải duplication thừa)._
- [x] **D-02 — Summary fallback `extra_power_in==0`** — Maxwell 0 vs TonHe `V*I` → tổng công suất 2 kiểu. `chg_lib_fsm.c:52-54` · **Major** — _Re-verified 2026-08-27: **KHÔNG PHẢI BUG** — Re-verified: Maxwell/Lianming truyền `0.0f` và dùng `v->input_power` (giá trị đo thật từ thanh ghi) làm fallback; TonHe không có thanh ghi input-power nên tự tính `V*I` làm ước lượng. Vì hệ thống chỉ chạy 1 driver tại 1 thời điểm (Strategy pattern), không có tình huống trộn 2 kiểu input_power trong cùng 1 summary. Chỉ là khác biệt ý nghĩa số liệu giữa driver, không phải lỗi runtime — để nguyên, không cần sửa (tránh over-engineering)._
- [x] **D-03 — Magic number rải rác** — `20A`, `100kW`, `3/5 retry`, `50/100/500ms`. `chg_lib_maxwell.c:61-67` · **Minor** — _Re-verified 2026-08-27: **ĐÃ FIX** (Maxwell) commit `46c3fe5` (2026-08-27) — đặt tên `MXR_DEFAULT_RATED_CURRENT_A`, `MXR_STOP_RETRY_INTERVAL_MS`, `MXR_STOP_MAX_RETRIES`. TonHe/Lianming đã đủ rõ ràng từ trước, không cần sửa._
- [x] **D-04 — Union type-punning float UB** — `priv/chg_lib_protocol.h:71-92` · **Minor** — _Re-verified 2026-08-27: **ĐÃ FIX** (pre-existing, không còn `union` trong `priv/chg_lib_protocol.h`)._
- [x] **D-05 — CMake PRIVATE include** — driver include `priv/*` bằng relative. `CMakeLists.txt:10` · **Minor** — _Re-verified 2026-08-27: **ĐÃ FIX (guard, không phải reachable bug)** commit `46c3fe5` (2026-08-27) — `tools/check_architecture.py` giờ enforce: header trong thư mục `priv/` chỉ được include bởi file nằm trực tiếp trong thư mục module sở hữu nó (trừ `test/`). Hiện tại 0 vi phạm thật; đây là rào chắn cho tương lai, không phải sửa lỗi đang rò rỉ._
- [x] **S-01 — Không kiểm DLC>8** — `FeedCanFrame` bỏ qua im lặng. `chg_lib_maxwell.c:694` · **Minor** — _Re-verified 2026-08-27: **ĐÃ FIX** (pre-existing, `mx_feed_frame` có `if (dlc < 8 || data == 0) return;`)._
- [ ] **S-04 — Code phụ thuộc filter BSP** — không assert trong `chg_lib`. `chg_lib_can_backend.c:21` · **Major**

---

## 4. Checklist — Phân hệ BMS

### 4.1 Compliance FR-BMS

| ID | Kết luận (gốc) | Kết luận (2026-08-27) | Bằng chứng |
|---|---|---|---|
| FR-BMS-01 | PARTIAL | **PASS** | Filter Std frames đã fix từ trước (FDCAN2 `StdFiltersNbr=1`); parse 9 loại RX đúng BE/LE + DLC check |
| FR-BMS-02 | PASS | PASS | `if(!ParseFrame) return;` không refresh watchdog |
| FR-BMS-03 | PARTIAL | **PASS** | `g_bms_view` giờ được memset cùng lúc với `g_bms_data` khi OFFLINE (commit `a8ec996`) |
| FR-BMS-04 | PARTIAL | **PASS** | Nhánh ONLINE giờ có else-clear STALE_DATA giống FAULT (commit `ea6dd4a`) |
| FR-BMS-05 | PARTIAL | **PASS** | Critical mask mở rộng 4→6/13 theo quyết định người dùng (commit `37ee2fb`); `sev>=2` đúng thang tài liệu (commit `816a973`); recovery-bị-STALE-chặn (BUG-05) đánh giá là thiết kế cố ý, không phải bug |
| FR-BMS-06 | PASS | PASS | Ctrl_INFO 500ms đúng |
| FR-BMS-07 | PARTIAL | **PASS** | Race ISR/main trên `alarm_flags` đã fix (preserve-mask ở ISR + critical section ở main loop, commit `a8ec996`) |
| FR-BMS-08 | FAIL | **PASS** | `bms_tick_elapsed()` dùng signed-diff idiom, xử lý đúng cả wrap 49.7 ngày (commit `ea6dd4a`) |

### 4.2 Issues

- [x] **BUG-01 — CAN2 filter reject Std frames** — `StdFiltersNbr=0` + `REJECT`. `BSP/bsp_can.c:16-29`, `Core/Src/fdcan.c:56,93` · **Critical — Safety** · CELL_VOLT/TEMP/ALM_INFO mất hoàn toàn — _Re-verified 2026-08-27: **ĐÃ FIX** (pre-existing) — `Core/Src/fdcan.c` FDCAN2 `StdFiltersNbr=1` + `BSP/bsp_can.c` cấu hình filter Standard-ID riêng chấp nhận vào RXFIFO0; `check_ioc.py` đã guard giá trị này._
- [x] **BUG-02 — Tick overflow sai** — `elapsed=0` khi `now<last` thay vì `now-last` unsigned. `bms_core.c:271-276` · **High** — _Re-verified 2026-08-27: **ĐÃ FIX** commit `ea6dd4a` (2026-08-27) — thêm helper `bms_tick_elapsed()` dùng signed-diff idiom, xử lý đúng cả race ISR/main lẫn tick wraparound thật._
- [x] **BUG-03 — OFFLINE không xóa `g_bms_view`** — `memset(data)` giữ view stale. `bms_core.c:304,332` · **High** — _Re-verified 2026-08-27: **ĐÃ FIX** commit `a8ec996` (2026-08-27) — memset toàn bộ `g_bms_view` khi chuyển OFFLINE (từ cả ONLINE và FAULT), rồi mới set lại `online`/`alarm_flags`._
- [x] **BUG-04 — STALE latch không clear** — OR mỗi chu kỳ. `bms_core.c:311-312,336-337` · **Medium** — _Re-verified 2026-08-27: **ĐÃ FIX** commit `ea6dd4a` (2026-08-27) — nhánh ONLINE giờ có else-clear giống nhánh FAULT._
- [ ] **BUG-05 — Recovery bị STALE chặn** — `alarm_flags==NONE` bao gồm STALE. `bms_core.c:343` · **Medium** — _Re-verified 2026-08-27: **XEM XÉT LẠI, có thể không phải bug** — yêu cầu `!is_stale` trước khi FAULT→ONLINE nghĩa là không tự phục hồi khi chất lượng data đang kém, kể cả khi không còn alarm thật. Có thể là lựa chọn an toàn cố ý (không công nhận "đã ổn" khi data chưa đáng tin), không phải lỗi. Chưa sửa — cần xác nhận ý định thiết kế trước khi động vào._
- [x] **BUG-06 — ISR/main race không bảo vệ** — `g_bms_data/view/last_tick` không `volatile`/`__disable_irq`. `bms_core.c:24-31,202-358` · **High** — _Re-verified 2026-08-27: **ĐÃ FIX** commit `a8ec996` (2026-08-27) — (thu hẹp hơn audit mô tả, phần lớn đã `volatile`/critical-section từ trước). 2 vấn đề thật còn lại: (1) `update_alarm_flags()` (ISR) overwrite toàn bộ `alarm_flags` mỗi ALM_INFO frame, xóa mất bit `BMS_OFFLINE`/`STALE_DATA` do `BMS_Process` quản lý — giờ preserve 2 bit đó; (2) `BMS_Process()`'s `|=`/`&=` trên `STALE_DATA` là read-modify-write không được bảo vệ — giờ bọc `BSP_EnterCritical/ExitCritical` quanh đúng 2 dòng đó (không bọc LOG() vì LOG block tới 50ms)._
- [x] **BUG-07 — LOG blocking trong ISR** — `BMS_FeedFrame` log 1s từ `BSP_CAN RxCallback` (ISR). `bms_core.c:228-255` `debug_log.c:25` 50ms · **High** — _Re-verified 2026-08-27: **ĐÃ FIX** (pre-existing) — `BMS_FeedFrame` không LOG; log snapshot đã chuyển sang `BMS_Process` qua cờ `g_isr_new_data` + throttle 1s._
- [x] **BUG-08 — Critical mask thiếu** — chỉ 3/13 alarm. `bms_core.c:315-317,382-386` · **Medium** — _Re-verified 2026-08-27: **ĐÃ FIX theo quyết định người dùng** commit `37ee2fb` (2026-08-27) — thêm `BMS_ALARM_HIGH_PACK_VOLT` và `BMS_ALARM_TEMP_LOW_CHG` vào critical mask (từ 4/13 lên 6/13), và gộp 2 bản khai báo mask trùng lặp (`BMS_Process`/`BMS_HasCriticalAlarm`) thành 1 hàm dùng chung `bms_critical_alarm_mask()` để tránh lệch nhau về sau._
- [x] **BUG-09 — Severity mapping quá nhạy** — `sev>=1` → flag. `bms_core.c:118-124` · **Medium** — _Re-verified 2026-08-27: **ĐÃ FIX** commit `816a973` (2026-08-27) — theo đúng thang severity đã tài liệu hóa trong `bms_protocol.h` (0=none,1=warning,2=fault,3=severe), giờ yêu cầu `sev>=2` thay vì `sev>=1`._
- [x] **BUG-11 — `ShouldCloseChargeRelay()` stub** — luôn `return true`, bỏ qua `90%` check. `bms_core.c:399-421` · **Medium** — _Re-verified 2026-08-27: **ĐÃ FIX** (pre-existing) — không còn stub `return true`; check online + critical alarm + `charge_relay_closed` từ BMS. Ngưỡng SOC dừng sạc đã có ở `App/Charge/charge_controller.c` (5-band SOC staging), đúng phân lớp policy vs driver._
- [x] **DES-01 — Tracker Init sai giả thiết** — `g_last_valid_rx_tick = HAL_GetTick()` ngay init. `bms_core.c:194` · **Medium** — _Re-verified 2026-08-27: **ĐÃ FIX** commit `816a973` (2026-08-27) — `g_last_valid_rx_tick` khởi tạo `0` (đúng sentinel "chưa nhận gì") thay vì `BSP_GetTick()`. Thực tế không ảnh hưởng logic thoát OFFLINE (chỉ dựa `has_any_valid_bms_data()`), nhưng sửa để `BMS_View_t.last_rx_tick` (PC debug hiển thị) không nói dối lúc boot._
- [ ] **DES-02 — `update_view` phụ thuộc `valid` latch** — giữ mãi dù frame ngừng. `bms_core.c:57-113` · **Medium** — _Re-verified 2026-08-27: **XÁC NHẬN THẬT, giữ nguyên theo quyết định người dùng** — khi 1 loại frame (vd CELL_TEMP) ngừng >2s trong khi frame khác vẫn bình thường, `STALE_DATA` chỉ mang tính cảnh báo, không chặn `BMS_ShouldCloseChargeRelay()`. Người dùng đã chọn giữ hành vi này (chỉ cảnh báo, không chặn sạc) thay vì cho STALE_DATA chặn relay. Không sửa._
- [x] **DES-03 — Thiếu per-frame timeout** — global watchdog → `CELL_TEMP` mất 10s vẫn coi online. `bms_core.c:27,217` · **Medium** — _Re-verified 2026-08-27: **KHÔNG PHẢI BUG, đã có sẵn** — `BMS_Process()` đã loop qua từng `BMS_FRAME_MAX` kiểm tra tuổi riêng của từng loại frame (`BMS_STALE_THRESHOLD_MS`), set `is_stale` nếu bất kỳ frame nào (trừ BMS_SW_STA/CELL_VOLT_FULL/CELL_TEMP_FULL) vượt ngưỡng — không phải chỉ 1 watchdog toàn cục như audit mô tả._
- [x] **MNT-01 — Thêm frame mới sửa 4 nơi** — không table-driven. `bms_protocol.{h,c}`, `bms_core.c` · **High** — _Re-verified 2026-08-27: **PHẦN LỚN ĐÃ ỔN** — `bms_protocol.c` đã table-driven (`g_handlers[]`) từ trước; đã bỏ thêm 1 điểm thủ công (`has_any_valid_bms_data()`, commit `d7cd710`, giờ tự suy ra từ bảng thay vì OR-chain tay). Các điểm còn lại (struct field, hàm parse, mapping trong `update_view_from_data`) là bản chất không tránh được của C tĩnh kiểu, không cố gộp thêm để tránh over-engineering (macro/codegen)._

---

## 5. Checklist — Charge Controller + Config/Storage

### 5.1 Compliance

| FR | Kết luận |
|---|---|
| FR-CTRL-01..03 | PASS (DERATING là cờ, không phải state — chết mã nhưng runtime đúng) |
| FR-CTRL-04..07 | PASS |
| FR-CTRL-08 | PARTIAL (hysteresis chỉ chiều giảm) |
| FR-CTRL-09..12 | PASS |
| FR-CTRL-13 | PARTIAL (hard-coded 25°C) |
| FR-CTRL-14..17 | PASS |
| FR-CTRL-18 | PARTIAL (stale không freeze target) |
| FR-CFG-01 | PASS |
| FR-CFG-02 | PARTIAL (chỉ `x!=x`, không bắt `Inf`) |
| FR-CFG-03/04/05 | PARTIAL/PASS/PASS (OOB 5B, .c vs find_blank drift) |

### 5.2 Issues

- [ ] **BUG-01 — Inf lọt validate** — `value_is_invalid = x!=x` không bắt `Inf`. `charge_cycle_config.c:7-9,64-122` · **High — Safety**
- [ ] **BUG-02 — Flash OOB 5B** — `WriteBlock(...,224)` copy từ `&record` 219B. `charge_cycle_storage.c:130` · **High**
- [ ] **BUG-03 — `find_blank_offset` check 219 thay vì 224** — `charge_cycle_storage.c:58` · **Low**
- [ ] **BUG-04 — Stale không freeze target** — `run_bms_controlled_mode:917-951` vẫn tính stage. · **Medium**
- [ ] **BUG-05 — Spam CAN 20ms** — `SetVoltageAll/SetCurrentLimitAll` mỗi vòng 20ms → ~800 frame/s. `charge_controller.c:263-269` · **Medium**
- [ ] **BUG-08 — Manual target NaN** — `payload_float` không validate → `target=NaN`. `pc_protocol.c:314,329` `controller.c:824` · **High**
- [ ] **SAF-04 — Flash không tắt ngắt** — `HAL_FLASH_Program` trong khi CAN ISR chạy → HardFault G0. `bsp_flash.c:35-54` · **High**
- [ ] **DES-01 — DERATING dead state** — `charge_controller.h:22` · **Medium**
- [ ] **DES-04 — Tick style không nhất quán** — 2 nơi 2 phong cách. · **Low**
- [ ] **MNT-06 — Không có seam test** — `static` + `g_ctrl` → không host-test. · **Medium**

---

## 6. Checklist — Truyền thông (PC/BMS/HMI)

### 6.1 Compliance FR-USB / FR-HMI

| FR | Kết luận |
|---|---|
| FR-USB-01/03/05/06/07/09 | PASS |
| FR-USB-02 | PARTIAL (SOF re-sync mất `AA AA 55`) |
| FR-USB-04 | PASS nhưng DESIGN FLAW (queue full drop im lặng, race) |
| FR-USB-08/10 | PASS (HMI) với `SET_DRIVER` sai mã lỗi |
| FR-HMI-01/02 | PASS (protocol) |
| FR-HMI-03/04 | FAIL (dead code — `App_Loop` không gọi DWIN) |
| FR-OPS-04/05/06 | PASS |
| FR-OPS-07 | FAIL (mâu thuẫn SRS — reject std triệt BMS) |

### 6.2 Issues

- [ ] **B-01 — CAN filter reject Std** — Trùng BMS BUG-01 · **Critical**
- [ ] **B-02 — SOF re-sync bỏ lỡ `AA` lặp** — `pc_protocol.c:450-451` · **Medium**
- [ ] **B-04 — TX queue full drop im lặng** — `pc_protocol.c:88-91,109-112` · **Medium**
- [ ] **B-05 — NACK sai code `SET_DRIVER`** — `0x04` thay vì `0x05`. `pc_protocol.c:379` · **Low**
- [ ] **B-07 — FW version lệch** — `pc_protocol.h:2.0.0` vs `BuildSystemInfo 1.0.0`. `pc_debug_protocol.c:177-179` · **Medium**
- [ ] **B-08 — `bms_stale` không gán** — dòng 216 trống → luôn 0. `pc_debug_protocol.c:214-216` · **Medium**
- [ ] **B-10 — TX queue race ISR/main** — `g_tx_count/head/tail` không `volatile`/`__disable_irq`. `pc_protocol.c:30-35,83-146` · **High**
- [ ] **D-01 — LOG blocking trong USB ISR** — `pc_protocol.c:433,473` `debug_log.c:25` 50ms · **High**
- [ ] **D-03 — DWIN FSM không align** — `dwin_protocol.c:62-81` chunk giữa frame bị discard · **High**
- [ ] **D-04 — DE không guard-time** — `bsp_rs485.c:27-36` thiếu đợi `TC` + delay 200µs · **Medium**
- [ ] **D-06 — `DWIN_UpdateData` spam 11 frame** — blocking ~7.7ms, phá 20ms loop. `dwin_protocol.c:46-59` · **Medium**
- [ ] **M-01 — DMA vs IT mâu thuẫn** — `.ioc` DMA Ch3 cho USART3_RX nhưng dùng `IT 1B`. `usart.c:246-262` `bsp_rs485.c:24` · **Low**

---

## 7. Checklist — BSP / Platform / CubeMX

### 7.1 Drift `.ioc` ↔ generated (đã fix 80%, 2 drift chết người còn lại)

| # | Ngoại vi | `.ioc` | Generated `Core/Src/*` | Kết luận |
|---|---|---|---|---|
| D-01 | FDCAN1 Prescaler/Seg/AR | `NominalPrescaler=32, Seg1=12 Seg2=3, AR=ENABLE, Ext=1` | Khớp (`fdcan.c:48-57`) → 125K | ✅ OK |
| D-02 | FDCAN2 Prescaler | Chưa có dòng `FDCAN2.NominalPrescaler` (chỉ Seg) | `fdcan.c:85 =16` → 250K | ⚠️ **DRIFT TIỀM ẨN — P0** — lần Generate tiếp có thể về default 32 → sai 125K |
| D-08 | ADC | `.ioc:7` yêu cầu 4 kênh NTC | `adc.c:50-74` chỉ `CH0` | ❌ **P0 — mất 3 kênh NTC** |
| D-10 | SPI | `.ioc` không khai `DataSize` | `spi.c:44,78` `4BIT` | ❌ **P1 — sai frame (SD/LTE cần 8BIT)** |

### 7.2 Compliance FR-OPS / C / NFR liên quan BSP

| ID | Kết luận |
|---|---|
| FR-OPS-01 | PARTIAL (PA4 HIGH sau 2-5ms LOW, PB1 glitch TX) |
| FR-OPS-02/05/07/C-03/C-04/C-05 | PASS |
| C-01/NFR-01 | PARTIAL (blocking TX/LOG phá 20ms) |
| NFR-04/06/07 | PARTIAL (LOG 100ms vượt 50ms, stack 1KB cận biên) |
| FR-OPS-04 | PARTIAL (bus-off restart nông) |
| C-07 | RISK (không có guard assert sau regen) |

### 7.3 Issues — BSP

- [ ] **I-01 — Không có IWDG/WWDG** — `stm32g0xx_hal_conf.h:48,60` disable, `.ioc` không config · **Critical**
- [ ] **I-02 — `Error_Handler`/`HardFault` treo không safe-state** — `main.c:180-188` `it.c:97-107` `while(1)` · **Critical**
- [ ] **I-03 — Thiếu IWDG refresh** — `App_Loop` không `HAL_IWDG_Refresh` · **High** (gộp I-01)
- [ ] **I-04 — NVIC đồng mức 0** — tất cả IRQ `0,0` trừ SysTick 3. `Charger.ioc:198-213` `fdcan.c:147` · **High**
- [ ] **I-05 — RS485 DMA vs IT drift** — `Charger.ioc:69` DMA Ch3 nhưng IT 1-byte. · **High**
- [ ] **I-06 — PB1 DE glitch boot** — `gpio.c:61` `SET` (TX) trước `RESET` (RX). `Charger.ioc:291` · **High** (Medium sau SWAP fix)
- [ ] **I-07 — RS485 TX blocking 100ms** — `bsp_rs485.c:33` · **High**
- [ ] **I-08 — LOG blocking 50ms + banner sai** — `debug_log.c:13,25,35` FDCAN1 ghi `@250K` sai (phải 125K) · **Medium**
- [ ] **I-09 — SPI 4BIT** — `spi.c:44,78` · **High latent**
- [ ] **I-10 — POWER_EN không delay** — `app_main.c:69` `SET` → ngay `BSP_CAN_Start`. B1205S cần ~20ms · **Medium**
- [ ] **I-13 — Flash align check thiếu** — `bsp_flash.c:35-54` không verify `address%8`, biên trang · **Medium**
- [ ] **I-16 — Stack/Heap nhỏ** — `STM32G0B1xx_FLASH.ld:66-67` 512B/1KB, còn FLASH dư 8K — tăng Stack 0x800 khi feature creep · **Medium**

---

## 8. Kế hoạch Sprint tổng hợp

### Sprint 1 — P0 Critical (không release nếu chưa xong) — ~8 ngày FW + 2 ngày QA

| # | Task (duy nhất, gộp trùng) | P | Eff | Owner | Acceptance |
|---|---|---|---|---|---|
| **1.1** | **Sửa CAN filter BMS std+ext** — `bsp_can.c` tách `FDCAN1` ext-only vs `FDCAN2` dual Std(1 mask 0x7FF)+Ext; `fdcan.c` `StdFiltersNbr=1` cho FDCAN2; `Charger.ioc` `FDCAN2.NominalPrescaler=16` | P0 | M | FW | BMS std `02F4/04F4` pass 100%; ext vẫn pass |
| **1.2** | **Khóa ISR race + bỏ LOG khỏi ISR** — `volatile` + `__disable_irq` quanh `BMS_GetView` copy & `CHG_LIB` & `g_tx_queue`; `BMS_FeedFrame` chỉ set flag; `LOG` chuyển ra `BMS_Process` 1s | P0 | M | FW | Grep không còn LOG trong ISR; stress 1000 frame/s không rách view |
| **1.3** | **Sửa SelectDriver + RemoveModule + RECOVERING logic** — thêm ops `deinit`, compact array; Maxwell/Lianming RECOVERING cần 5 RX; TonHe OFFLINE→RECOVERING tự động | P0 | M | FW | Đổi driver 10 lần không leak; RECOVERING đủ 5 RX mới IDLE |
| **1.4** | **Flash an toàn + guard Inf/NaN** — `bsp_flash.c` `__disable_irq` quanh erase/program; `charge_cycle_config.c` `!isfinite`; `pc_protocol.c` reject NaN/Inf trước `SetManualTarget` | P0 | M | FW | Erase giữa CAN ISR không HardFault; fuzz NaN đều NACK |
| **1.5** | **IWDG + safe-state Error/HardFault** — CubeMX IWDG 1s, refresh trong `App_Loop`; `Safety_Shutdown()` tắt relay, hạ POWER_EN, Stop CAN, `NVIC_SystemReset` | P0 | M | FW+HW | Soak 24h không treo; bus-off recovery <1s |

*Exit Sprint 1: BMS online với pack thật, đổi driver ổn, flash-save 100 lần không fault, fuzz NaN không sập.*

### Sprint 2 — P1 High (safety & integrity) — ~8 ngày

| # | Task | P | Eff | Owner | Dep |
|---|---|---|---|---|---|
| **2.1** | Flash OOB + version thống nhất (`DebugSystemInfo 2.0.0`, `is_flash_blank` check 224, pad rõ) | P1 | S | FW | 1.4 |
| **2.2** | BMS STALE/OFFLINE semantics + `BMS_GetView` atomic + `bms_stale` gán | P1 | M | FW | 1.2 |
| **2.3** | Throttle CAN (chỉ gửi khi delta), hysteresis temp 2 chiều, ghi chú DERATING | P1 | M | FW | 2.2 |
| **2.4** | SOF re-sync + TX queue volatile/irq + debounce rising-edge chuẩn + NACK code + `bms_stale` | P1 | M | FW+PC | 1.2 |
| **2.5** | FSM RECOVERING/STOPPING chuẩn SRS 500ms×5 cho cả 3 driver; alarm critical mọi state | P1 | M | FW | — |
| **2.6** | Sửa POWER_EN/DE glitch + RS485 guard-time + ring buffer safe | P1 | S | FW | — |
| **2.7** | Sửa SPI 4BIT→8BIT + ADC 4 kênh (quyết định: bỏ DMA hoặc scan 4CH) | P1 | S | FW | — |
| **2.8** | NVIC phân cấp (FDCAN 0, USB/DMA 1, USART 2, SysTick 3) + POWER_EN delay 20ms | P1 | S | FW | — |

### Sprint 3 — P2 Medium (product polish) — ~8 ngày

| # | Task | P | Eff | Owner | Dep |
|---|---|---|---|---|---|
| **3.1** | Build & CubeMX guardrail — `check_ioc.py` CI + tăng stack 0x800 | P2 | S | FW/QA | — |
| **3.2** | DWIN + Relay + ADC NTC thực (nối `BSP_RS485_Read`→DWIN, `BMS_ShouldCloseChargeRelay`→PB14/15) | P2 | L | FW+HW | 2.2 |
| **3.3** | Integration & fuzz: HIL BMS, 2 module mock, PC fuzz, power-cycle, 72h soak | P2 | L | QA/FW | S1+S2 |

---

## 9. Phụ lục — File tham chiếu & Guard Checklist

### 9.1 File cần sửa theo Sprint

| Sprint | File |
|---|---|
| 1.1 | `BSP/bsp_can.c:20-29`, `Core/Src/fdcan.c:56,93`, `Charger.ioc` FDCAN2 |
| 1.2 | `Modules/bms/bms_core.c:24-30,222,391`, `Modules/chg_lib/chg_lib_core.c:180`, `App/Protocol/pc_protocol.c:30-35,83-146` |
| 1.3 | `Modules/chg_lib/chg_lib_core.c:39,66`, `chg_lib_*.c` RemoveModule, RECOVERING branches |
| 1.4 | `BSP/bsp_flash.c:35-54`, `App/Charge/charge_cycle_config.c:7-20`, `App/Protocol/pc_protocol.c:314` |
| 1.5 | `Charger.ioc` IWDG, `Core/Src/main.c:180`, `Core/Src/stm32g0xx_it.c:97`, `BSP/bsp_gpio` + relay |
| 2.x | `App/Charge/*`, `App/Protocol/pc_protocol.c:450`, `pc_debug_protocol.c:177`, `BSP/bsp_rs485.c:27-36`, `Core/Src/spi.c`, `adc.c`, `Charger.ioc` NVIC |
| 3.x | `cmake/`, `STM32G0B1xx_FLASH.ld`, `App/System/app_main.c` (DWIN integration) |

### 9.2 Checklist sau mỗi lần Generate từ CubeMX

- [ ] `Core/Src/fdcan.c`: FDCAN1 `NominalPrescaler = 32` (125k); FDCAN2 `= 16` (250k); cả hai `AR=ENABLE`, `ExtFiltersNbr=1` (FDCAN2 `StdFiltersNbr=1`)
- [ ] `Core/Src/usart.c`: USART3 `Swap=ENABLE` (ADVFEATURE_SWAP_ENABLE)
- [ ] `Core/Src/adc.c`: `ScanConvMode` + `NbrOfConversion` khớp `.ioc` (1 hay 4 kênh)
- [ ] `Core/Src/spi.c`: `DataSize = 8BIT`
- [ ] `Charger.ioc`: `PB1 PinState = RESET` (RX), `PA4 PinState = SET` (POWER_EN), `NVIC` phân cấp, `IWDG` enabled
- [ ] Build `FLASH < 128K`, boot log `FDCAN1 @125K / FDCAN2 @250K`

---

*Hết tài liệu — AUDIT-CHG-CTRL-001 v1.0*
*Sinh bởi 6 agents (chg_lib / BMS / Charge CTRL / Truyền thông / BSP / Checker) — tổng hợp & cross-check.*

