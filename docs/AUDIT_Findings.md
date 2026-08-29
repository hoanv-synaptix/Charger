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

> **Re-verification 2026-08-27**: Tài liệu này được viết trước một đợt "Sprint 1 fixes" không rõ ràng trong lịch sử — nhiều bug Critical/Major bên dưới đã được fix trước khi đợt dọn dẹp kiến trúc (PR0-PR3) này bắt đầu. Đã đối chiếu từng dòng trong mục 3 (chg_lib) với source hiện tại; các dòng đã fix được đánh `[x]` kèm ghi chú. B-04/B-05/B-06 được fix commit `1b37101`; B-07/B-09/B-13 được fix commit `d1b427c` (đối chiếu thêm với docs protocol PDF của Maxwell/Lianming/TonHe khi cần). Mục 4 (BMS) đã re-verify và đóng hoàn toàn (xem note trong mục đó). Mục 5-7 (Charge Controller/Config, Truyền thông PC-BMS-HMI, BSP/Platform) cũng đã re-verify hoàn toàn ngày 2026-08-27: phần lớn issue trong 3 mục này hoá ra đã được fix bởi cùng đợt "Sprint 1 fixes" không ghi rõ trong tài liệu (commit `d2a5a94`/`c79fbd0`) — Inf/NaN validation, IWDG, NVIC priority, ADC 4-channel, SPI 8-bit, FDCAN2 prescaler, DWIN frame pacing, CAN filter Std+Ext, TX queue locking, v.v. đều đã đúng trong source hiện tại. 3 bug thật còn sót được tìm thấy và fix trong đợt này (commit `fadfdaf`): BUG-02 (flash record OOB stack read), B-02 (PC protocol SOF resync mất byte lặp), I-06 (RS485 DE pin mặc định HIGH lúc boot). Một số mục nhỏ được để mở có chủ đích vì cần xác minh trên phần cứng thật (I-07 RS485 TX timing, I-10 POWER_EN power-up delay) hoặc là cải tiến thiết kế không khẩn cấp (DES-04, MNT-06, M-01/I-05) — xem ghi chú re-verify trong từng mục để biết lý do cụ thể.

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
- [x] **B-08 — COMM_FAIL latch vĩnh viễn** — `alarm_flags = parse | (flags & COMM_FAIL)` giữ bit. `chg_lib_maxwell.c:383,542` · **Major** — _Re-verified 2026-08-28: literal pattern **not reproducible** — `apply_response()`'s `ALARM_STATUS` case is a plain overwrite (`m->view.alarm_flags = parse_maxwell_alarm(...)`), not an OR-merge, so a stale COMM_FAIL bit is not latched. Re-verification surfaced a real, related issue instead — reclassified and fixed, see B-08b below._
- [x] **B-08b — FAULT recovery had no confirmation debounce (found during B-08 re-verification)** — all 3 drivers required only **1** clean status/alarm read with no fault bits before restarting the module out of `FAULT`, weaker than `OFFLINE→RECOVERING`'s existing 5-consecutive-read debounce for a mere comm gap — backwards for a real hardware/safety fault (over-voltage, short-circuit, over-temp). `chg_lib_maxwell.c` FAULT case, `chg_lib_lianming.c` FAULT case + `apply_status()`, `chg_lib_tonhe.c` FAULT case + `parse_status()` · **Major (safety-relevant, AGENTS.md §15)** — **FIXED 2026-08-28**: reused the existing `recovery_start_rx_count` field/threshold already used by `RECOVERING` — snapshot on `FAULT` entry, re-anchor on every read that still shows a fault (so reads taken *while* still faulted can't inflate the count), require 5 consecutive clean reads before leaving `FAULT`. Lianming and TonHe each had a second, independent bypass — `apply_status()`/`parse_status()` (their RX-callback path) transitioned straight out of `FAULT` on the first clean read regardless of the debounce added to `process_module()`'s polling path — caught by a regression test that clears the fault condition and asserts the module is *still* `FAULT` after only 1-2 clean reads, then recovers after 5; both gated the same way. Verified via `test/host_charge_sim` (`test_driver_fault_recovery_debounce` × 3 drivers, all pass) + `test/test_logic.c` + `check_architecture.py` + `check_ioc.py` + Release build, all clean.
- [x] **B-09 — NaN/Inf không lọc** — `rated_current_or_fallback` không check NaN, gán float trực tiếp. `chg_lib_maxwell.c:229-242,373-400` `lianming.c:301` `tonhe.c:214` · **Major** — _Re-verified 2026-08-27: **ĐÃ FIX** commit `d1b427c` (2026-08-27) — cả 3 driver reject setpoint non-finite qua `isfinite()` trong set_voltage/set_current_limit (clamp bằng `</>` không bắt được NaN)._
- [x] **B-10 — Round-robin starvation** — `CHG_LIB_Process` 1 module/lần → START 50ms thành 160ms với 8 module. `chg_lib_maxwell.c:685` `lianming.c:711` `tonhe.c:755` · **Major** — **FIXED 2026-08-28**: confirmed with the user this needs a real fix (real deployments run 6-8 modules) — all 3 drivers' `.process` entry point now iterates every enabled module every `CHG_LIB_Process()` call instead of advancing a `g_rr_index` one module per call; `g_rr_index` removed entirely. Safe inside the single critical section `CHG_LIB_Process()` already holds around the whole call (no driver call in `process_module()`'s tree blocks post-B-19). Re-verification while implementing this also found the round-robin bookkeeping's own `BSP_EnterCritical()/BSP_ExitCritical()` calls were a latent, independent bug: `BSP_EnterCritical()/ExitCritical()` is a plain `__disable_irq()/__enable_irq()` pair with **no nesting/depth counter** (`BSP/bsp_sys.c`), so calling it again *inside* the critical section `CHG_LIB_Process()` already holds re-enabled interrupts partway through -- silently undoing the B-13 fix for exactly the CAN-heavy, multi-module case B-10 itself is about. Removing the round-robin removed those nested calls too. Regression test: `test/host_charge_sim`'s `test_multi_module_timing_budget` (8 Maxwell modules) asserts every module reaches RUNNING within the same drive step, not staggered by round-robin slot -- verified this actually catches the regression by temporarily reverting to a round-robin `mx_process()` and confirming the test fails on the spread assertion, then restoring the fix and confirming it passes with all 18 scenarios green. Also verified via `test/test_logic.c` + `check_architecture.py` (0 new violations) + `check_ioc.py` (11/11) + Release build.
- [x] **B-11 — Boot online sai** — `last_rx_tick==0` → `since_rx = now` (<10s) nên chưa OFFLINE nhưng logic online sai. `chg_lib_maxwell.c:296-306,272` · **Major** — _Re-verified 2026-08-27: not reproduced as literally described — traced `CHG_LIB_FSM_CheckOfflineTimeout()` (`chg_lib_fsm.c:65`): a freshly-`AddModule()`'d module starts `state=IDLE`, `online=false` (via `memset`), and this function only forces `OFFLINE` once `now - 0 > offline_timeout_ms` (10s for Maxwell). That reads as intentional -- give a new module up to its offline-timeout worth of grace before ever having heard from it, rather than declaring it dead at tick 0 -- not an obvious defect. Confirmed with the user: no real-world symptom seen, only a code-review suspicion from the original audit. Closing as "grace period behaves as designed." No code change._ — _**Amended 2026-08-28, real HIL evidence (`test/integration_sync_test.py maxwell`/`lianming`)**: this WAS a live symptom, just not the one originally described. `now - 0 > offline_timeout_ms` is trivially true for any module registered (`PC_CMD_SET_MODULE_ADDR`) after the MCU has been up for longer than the offline timeout (10s Maxwell/Lianming) -- true in practice almost every time a technician configures a module well after boot, not just a contrived edge case. Consequence observed on real hardware: a freshly-registered Maxwell/Lianming module is forced straight to `OFFLINE` on its very first `process_module()` tick, then must wait `MXR_RECOVERY_DELAY_MS`(3000ms)/`LM_RECOVERY_DELAY_MS` before `RECOVERING`, then accumulate 5 consecutive clean reads before reaching `IDLE` -- roughly 3.5-4s from `SET_MODULE_ADDR` to `modules_online=1`, vs TonHe (broadcast, not gated the same way in practice) reaching online in ~1s in the same test run. Not a correctness bug (the module does come online and charge correctly once through this path -- confirmed: both reached RUNNING) and not safety-relevant (no relay/charge-control impact), just a slower-than-necessary first-contact delay for Maxwell/Lianming specifically. Left open as a minor UX/timing item, no code change in this pass -- reclassifying to Low and noting the concrete real-hardware numbers for whoever picks this up next, rather than re-closing it on review-only reasoning a second time._
- [x] **B-12 — TonHe timing 5s thay vì 1s** — `g_last_timing_tick` 5000 vs SRS 1s. `chg_lib_tonhe.c:757` · **Major** — _Re-verified 2026-08-28: already fixed (pre-existing, not from this session) — `chg_lib_tonhe.c:832` is `(now - g_last_timing_tick) >= 1000U`, matching SRS's 1s cadence. No code change._
- [x] **B-13 — Race ISR↔main** — `FeedCanFrame` ghi `view.*` trong khi `process_module` đọc/ghi không `__disable_irq`. `chg_lib_core.c:180-187` `can_backend.c:11,60` · **Major** — _Re-verified 2026-08-27: **ĐÃ FIX** commit `d1b427c` (2026-08-27) — `CHG_LIB_Process`/`CHG_LIB_FeedCanFrame` giờ giữ critical section quanh toàn bộ lệnh gọi driver, không chỉ đọc con trỏ `get_active()`._
- [x] **B-14 — SelectDriver deinit sai** — gọi `old_driver->init()` thay vì `deinit`. `chg_lib_core.c:39-41,65-68` · **Major** — _Re-verified 2026-08-27: **ĐÃ FIX** (pre-existing, xem `chg_lib_core.c:CHG_LIB_SelectDriver` gọi `old_driver->deinit()` với fallback `init()`)._
- [x] **B-15 — RemoveModule lỗ hổng** — không giảm `g_module_count`. `chg_lib_maxwell.c:591` `lianming.c:614` `tonhe.c:647` · **Major** — _Re-verified 2026-08-27: **ĐÃ FIX** (pre-existing, `mx_remove_module`/tương đương compact array + giảm `g_module_count`)._
- [x] **B-16 — Summary không nhất quán** — Maxwell đếm `IDLE` là online; TonHe chỉ RUNNING/STARTING/WARNING. `chg_lib_maxwell.c:713-737` `lianming.c:784-805` `tonhe.c:828-858` · **Major** — _Re-verified 2026-08-28: already fixed (pre-existing, not from this session) — all 3 drivers' `set_state()` now use the byte-for-byte identical pattern `online = (last_rx_tick != 0 && now - last_rx_tick <= OFFLINE_TIMEOUT_MS)`, applied uniformly across IDLE/STARTING/STOPPING/WARNING/FAULT (`chg_lib_maxwell.c:275,281`, `chg_lib_lianming.c:227,233`, `chg_lib_tonhe.c:483,489`). No per-driver divergence found. No code change._
- [x] **B-17 — Stats TX/RX lệch** — Lianming `lm_send_read` không cập nhật `last_tx_tick`. `chg_lib_lianming.c:280-287` · **Minor** — _Re-verified 2026-08-27: **ĐÃ FIX** commit `46c3fe5` (2026-08-27) — `lm_send_read`/`lm_set_output`/`lm_start_module`/`lm_stop_module`/`lm_send_ac_read`/`lm_send_temp_read` giờ đều cập nhật `last_tx_tick`._
- [x] **B-19 — TonHe: 3 LOG() đang chạy trong critical section, block CAN RX 50ms** — `CHG_LIB_Process()`/`CHG_LIB_FeedCanFrame()` (`chg_lib_core.c:147-160`) bọc *toàn bộ* lệnh gọi driver trong `BSP_EnterCritical()`, nên mọi hàm TonHe reachable từ `tonhe_process()`/`tonhe_feed_frame()` chạy với interrupt tắt. 3 dòng `LOG()` đang active (`parse_status()` "still ON forcing STOP"; `process_module()` STOPPING "STOP retry"/"STOP FAILED after N retries") vi phạm thẳng luật CLAUDE.md ("Never call LOG() inside a BSP_EnterCritical() section"). Tìm thấy khi review log 2026-08-28. `chg_lib_tonhe.c:344,568,572` (số dòng trước fix) · **Major** — đáng chú ý: "STOP FAILED" là *đúng lúc* fault an toàn cần CAN phản hồi nhanh nhất, lại là dòng gây block dài nhất. — _Fixed 2026-08-28: xoá cả 3 LOG() active + 6 dòng comment-out cùng loại (bằng chứng ai đó trước đây đã phát hiện đúng vấn đề nhưng chỉ tắt được 1 phần) — Maxwell/Lianming vốn đã 0 LOG() trong toàn bộ driver, giờ TonHe khớp theo đúng pattern đó. Thêm 1 comment giải thích rõ ràng ở đầu `chg_lib_tonhe.c` thay vì để rải rác. Verify: `test/host_charge_sim` 14 scenario + build Release đều pass._
- [x] **B-20 — `BSP_CAN_Transmit()` LOG() trên TX-FIFO-full cũng chạy trong critical section, ảnh hưởng cả 3 driver** — cùng họ bug với B-19 nhưng ở tầng BSP nên ảnh hưởng Maxwell/Lianming/TonHe chứ không chỉ TonHe: `BSP_CAN_Transmit()` gọi `LOG("[CAN TX FAIL]...")` khi `HAL_FDCAN_AddMessageToTxFifoQ()` fail (TX FIFO đầy) — hàm này được driver nào cũng gọi tới từ trong critical section của `CHG_LIB_Process()`/`CHG_LIB_FeedCanFrame()` (B-13). TX FIFO đầy dễ xảy ra hơn với nhiều module (B-10) — tức là đúng lúc bus bận nhất thì lại block CAN RX tới 50ms. Tìm thấy khi implement B-10. `BSP/bsp_can.c:106` (số dòng trước fix) · **Major** — _Fixed 2026-08-28: thay `LOG()` bằng counter `g_c1_tx_fail`/`g_c2_tx_fail` (cùng pattern `g_c1_tx`/`g_c2_tx` sẵn có), expose qua `BSP_CAN_GetTxFailStats()`. Chưa wire vào wire-protocol `DebugSystemInfo_t` (sẽ cần bump kích thước struct cố định + sửa debug_app phía Python — ngoài phạm vi session này, `debug_app/` không được đụng vào theo CLAUDE.md) — để ngỏ cho lần sau nếu cần poll giá trị này qua PC app. Verify: `check_architecture.py` + `check_ioc.py` + Release build đều pass (bsp_can.c không compile được trên host nên không chạy qua `test/host_charge_sim`)._
- [x] **B-18 — `CHG_LIB_SetModuleConfig` chưa bao giờ được gọi** — cả 2 nơi gọi `CHG_LIB_AddModule()` (`charge_cycle_config.c:179`, `pc_protocol.c:467`) không set rated current từ `cfg.module_i_max_a`, nên `rated_current_a` (Maxwell/Lianming) luôn khởi tạo 0. Tìm thấy khi review dead-code 2026-08-28. `chg_lib_maxwell.c:70,234-238` `chg_lib_lianming.c:665-666` · **Major** — _Fixed 2026-08-28: cả 2 call site giờ gọi `CHG_LIB_SetModuleConfig(idx, cfg.module_i_max_a)` ngay sau `AddModule()`. Đối chiếu PDF Maxwell chính hãng (mục 2.3.1: "current limit = required / rated"): trước fix, ratio current-limit gửi CAN dùng fallback cứng `MXR_DEFAULT_RATED_CURRENT_A`(20A) cho tới khi module tự trả lời poll thanh ghi `0x0012` (rated_current_or_fallback() đã ưu tiên giá trị tự báo cáo qua CAN trước 20A — nên tác động thực tế chỉ là cửa sổ thoáng qua ở lần current-limit đầu tiên sau Start, không phải sai suốt phiên sạc). Regression test: `test/host_charge_sim/test_charge_e2e.c:test_rated_current_seeded_from_config` — verify bằng cách tạm revert fix, xác nhận test FAIL đúng chỗ trước khi khôi phục._
- [x] **B-21 — `PC_CMD_SET_DRIVER` âm thầm đăng ký "phantom module"** — handler gọi `ChargeCycleConfig_Set()` chỉ để lưu `module_type` xuống flash, nhưng `ChargeCycleConfig_Set()` có side effect luôn tự động đăng ký `cfg.source_module_count` module ở địa chỉ mặc định `addr=1..N` (`charge_cycle_config.c:174-188`). App PC thực tế (`debug_app/main.py`'s `_sync_driver()`→`_sync_module_addr()`) gửi SET_DRIVER rồi SET_MODULE_ADDR riêng với địa chỉ do user nhập tùy ý — nếu `source_module_count` còn sót từ phiên trước (ví dụ >=1), SET_DRIVER âm thầm tạo 1 module ảo ở addr=1 trước khi app kịp gửi địa chỉ module thật; nếu địa chỉ thật trùng addr=1 thì `CHG_LIB_AddModule()` sau đó bị NACK do đụng địa chỉ (registration thật bị từ chối); nếu khác thì tồn tại song song 1 module ảo không có phần cứng thật, làm sai `modules_total`/`modules_online` hiển thị trên dashboard. `pc_protocol.c` `PC_CMD_SET_DRIVER` handler (dòng cũ ~430-447) · **Major (safety-relevant: sai số lượng module đang được điều khiển dòng/áp)** — Tìm thấy 2026-08-28 khi viết `test/host_protocol_sim/test_pc_protocol_e2e.c` (bộ test hành vi thật đầu tiên cho `pc_protocol.c`, thay cho `test/test_pc_debug_comm.py` vốn chỉ test một bản Python re-implement của FeedByte state machine, chưa từng compile/chạy code C thật) — regression `test_set_driver_and_module_addr_over_wire` fail đúng ở bước đếm module trước khi fix. — _Fixed 2026-08-28: chuyển lệnh gọi `CHG_LIB_Init()` (vốn đã có sẵn, trước đó nằm TRƯỚC block persist config) ra SAU block `ChargeCycleConfig_Set()`/`ChargeCycleStorage_Save()`, để dọn sạch side-effect auto-registration ngay sau khi nó xảy ra — `SET_MODULE_ADDR` sau đó là nguồn sự thật duy nhất cho danh sách module, đúng với flow thật của app. Không ảnh hưởng các caller khác của `ChargeCycleConfig_Set()` (boot load, `DEBUG_CMD_SET_CHARGE_CFG`) vì đây là thay đổi cục bộ trong 1 case-handler. Verify: `test/host_protocol_sim` 15/15 pass + `test/host_charge_sim` 18/18 + `test_logic.c` + `check_architecture.py` + `check_ioc.py` + Release build đều pass._

- [x] **B-22 — Maxwell: 2 thanh ghi chẩn đoán đầu vào (0x0005, 0x004B) đã khai báo nhưng không bao giờ poll** — người dùng yêu cầu tự đối chiếu code với PDF gốc của cả 3 module sạc, tìm data chưa parse hết thì bổ sung. Đối chiếu `docs/CAN Communication Protocol - Maxwell_V1.50.pdf` Table 1 (§3.1, danh sách thanh ghi đầy đủ) với `g_poll_regs[]`/`apply_response()` (`chg_lib_maxwell.c`): `CHG_LIB_REG_INPUT_DC_VOLTAGE` (0x0005, "input DC voltage") và `CHG_LIB_REG_INPUT_MODE_RD` (0x004B, "input working mode": 1=1-pha AC, 2=DC, 3=3-pha AC, 5=mode mismatch) đã có hằng số khai báo sẵn trong `priv/chg_lib_protocol.h` từ trước nhưng chưa từng được thêm vào chu kỳ poll — dữ liệu này không tồn tại ở bất kỳ đâu trong firmware. `chg_lib_maxwell.c` (`g_poll_regs[]`, `apply_response()`) · **Minor (diagnostic gap, không ảnh hưởng an toàn/điều khiển — chỉ là thiếu thông tin chẩn đoán hữu ích khi khắc phục sự cố đấu dây AC đầu vào)** — _Fixed 2026-08-29: thêm 2 field mới `input_dc_voltage`/`input_mode` vào `CHG_LIB_ModuleView_t` (`chg_lib.h`, an toàn vì struct nội bộ không có ràng buộc wire-compat), bump `MXR_POLL_REG_COUNT` 15→17, thêm 2 case trong `apply_response()`. Expose qua wire protocol bằng cách tái dùng field `vendor_data[]`/`vendor_data_len` đã dự trữ sẵn trong `DebugModuleData_t` (`pc_debug_protocol.c`) thay vì đổi kích thước struct cố định (tránh phá offset cố định trong parser Python của `debug_app`) — dùng `memcpy()` khi ghi thay vì gán con trỏ-ép-kiểu trực tiếp, tránh lỗi unaligned-write trên packed struct (cùng lớp lỗi đã gặp và sửa ở `BSP_CAN_GetStats()` trong session này). Lianming/TonHe để `input_dc_voltage=0`/`input_mode=0` (không poll, giữ nguyên `CHG_LIB_ModuleView_t` zero-init mặc định — 2 driver này không có thanh ghi tương đương). Simulator `sim_can_modules.c` cập nhật để trả lời 2 thanh ghi mới (400.0V, mode=3). Regression test: `test/host_charge_sim/test_charge_e2e.c:test_maxwell_input_diagnostics_polled` — chạy IDLE đủ lâu (20s, dài hơn 1 chu kỳ poll 17 thanh ghi × 1s) rồi assert cả 2 field khác 0 và đúng giá trị sim trả về; verify bằng cách tạm revert `MXR_POLL_REG_COUNT`/2 dòng trong `g_poll_regs[]`, xác nhận test FAIL đúng chỗ ("input_dc_voltage (0x0005) should have been polled by now"), rồi khôi phục và xác nhận PASS lại. Verify đầy đủ: `test/host_charge_sim` (20/20) + `test/host_protocol_sim` + `test/test_logic.c` + `check_architecture.py` + `check_ioc.py` + Release build (RAM 13.96%, FLASH 53.02%) đều pass. **Đính chính đồng thời**: đối chiếu tương tự với `docs/Lianming Power Digital Power Module CAN Communication Protocol V2.0.pdf` và `docs/TonHeCANcommunicationbetweenchargingmoduleandmonitor TONHE V1.3(1).pdf` cho thấy 2 driver này **đã đầy đủ** — Lianming đã có cơ chế `lm_read_status()`'s `diag_counter`/`diag_step` poll định kỳ AC 3-pha + nhiệt độ môi trường; TonHe đã dispatch đủ cả 4 PGN lên (M_C_1..M_C_4, bao gồm AC phase + nhiệt độ ở M_C_3, extended fault ở M_C_4) qua `tonhe_feed_frame()`. Không cần sửa gì cho 2 driver này — một đánh giá ban đầu trong phiên này (trước khi đọc lại kỹ code) đã sai khi cho rằng 2 driver này thiếu polling; đã đính chính trực tiếp với người dùng, ghi lại ở đây để tránh lặp lại nhầm lẫn tương tự trong lần audit sau. 6 thanh ghi Maxwell còn lại có trong PDF nhưng chưa poll (`0x0043` group/dial addr readback, `0x004A` altitude readback, `0x0054`/`0x0055` serial number, `0x0056`/`0x0057` version) — đánh giá là dữ liệu định danh/cấu hình tĩnh, không phải "operating data" cần theo dõi runtime — cố ý để ngỏ, không phải thiếu sót._

### 3.3 Issues — DESIGN / MAINTAINABILITY / SAFETY

- [x] **D-01 — Duplication 70% FSM** — 3 driver tự implement `set_state/check_offline/summary`, helper `chg_lib_fsm.c` chết. `chg_lib_fsm.c:12-32` · **Major** — _Re-verified 2026-08-27: **PHẦN LỚN ĐÃ FIX** (pre-existing) — `CHG_LIB_FSM_SetState`/`CHG_LIB_Summary_Accumulate` trong `chg_lib_fsm.c` được cả 3 driver dùng chung; vẫn còn state-machine timing logic riêng mỗi driver (do khác protocol thật sự, không phải duplication thừa)._
- [x] **D-02 — Summary fallback `extra_power_in==0`** — Maxwell 0 vs TonHe `V*I` → tổng công suất 2 kiểu. `chg_lib_fsm.c:52-54` · **Major** — _Re-verified 2026-08-27: **KHÔNG PHẢI BUG** — Re-verified: Maxwell/Lianming truyền `0.0f` và dùng `v->input_power` (giá trị đo thật từ thanh ghi) làm fallback; TonHe không có thanh ghi input-power nên tự tính `V*I` làm ước lượng. Vì hệ thống chỉ chạy 1 driver tại 1 thời điểm (Strategy pattern), không có tình huống trộn 2 kiểu input_power trong cùng 1 summary. Chỉ là khác biệt ý nghĩa số liệu giữa driver, không phải lỗi runtime — để nguyên, không cần sửa (tránh over-engineering)._
- [x] **D-03 — Magic number rải rác** — `20A`, `100kW`, `3/5 retry`, `50/100/500ms`. `chg_lib_maxwell.c:61-67` · **Minor** — _Re-verified 2026-08-27: **ĐÃ FIX** (Maxwell) commit `46c3fe5` (2026-08-27) — đặt tên `MXR_DEFAULT_RATED_CURRENT_A`, `MXR_STOP_RETRY_INTERVAL_MS`, `MXR_STOP_MAX_RETRIES`. TonHe/Lianming đã đủ rõ ràng từ trước, không cần sửa._
- [x] **D-04 — Union type-punning float UB** — `priv/chg_lib_protocol.h:71-92` · **Minor** — _Re-verified 2026-08-27: **ĐÃ FIX** (pre-existing, không còn `union` trong `priv/chg_lib_protocol.h`)._
- [x] **D-05 — CMake PRIVATE include** — driver include `priv/*` bằng relative. `CMakeLists.txt:10` · **Minor** — _Re-verified 2026-08-27: **ĐÃ FIX (guard, không phải reachable bug)** commit `46c3fe5` (2026-08-27) — `tools/check_architecture.py` giờ enforce: header trong thư mục `priv/` chỉ được include bởi file nằm trực tiếp trong thư mục module sở hữu nó (trừ `test/`). Hiện tại 0 vi phạm thật; đây là rào chắn cho tương lai, không phải sửa lỗi đang rò rỉ._
- [x] **S-01 — Không kiểm DLC>8** — `FeedCanFrame` bỏ qua im lặng. `chg_lib_maxwell.c:694` · **Minor** — _Re-verified 2026-08-27: **ĐÃ FIX** (pre-existing, `mx_feed_frame` có `if (dlc < 8 || data == 0) return;`)._
- [x] **S-04 — Code phụ thuộc filter BSP** — không assert trong `chg_lib`. `chg_lib_can_backend.c:21` · **Major** — _Re-verified 2026-08-28: likely already adequately mitigated, not urgent — every driver's `feed_frame()` independently validates frame identity in software before accepting it, regardless of what the hardware filter already let through: Maxwell checks PROTNO/PTP/DSTADDR; Lianming checks `id_base` against `LM_RESP_BASE`/`LM_AC_RESP_BASE`/etc. (`chg_lib_lianming.c:786-807`); TonHe matches `src_addr` against a registered module address (`chg_lib_tonhe.c:290,385,410,443`). None of the 3 blindly trusts the BSP filter did the gatekeeping. Closing as "defense already exists via each driver's own frame-identity check, just not via a literal `assert()`" -- adding one would be redundant, not a fix for a real gap. No code change._

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
- [x] **BUG-13 — `BMS_ALARM_BMS_OFFLINE` latch mãi mãi, không bao giờ clear khi hồi phục** — cùng họ bug với BUG-04 (STALE latch) nhưng cho bit `BMS_ALARM_BMS_OFFLINE` (`bms_core.h:88`, `1U<<13`) — bit này set khi vào `OFFLINE` (`bms_core.c:337,377` cũ) và được `update_alarm_flags()` (ISR) chủ động preserve (đúng, theo BUG-06), nhưng nhánh `OFFLINE→ONLINE` (`bms_core.c:317-325` cũ) chỉ set `online=true` chứ không hề clear bit này — khác hẳn `BMS_ALARM_STALE_DATA` ngay bên dưới vốn có set/clear rõ ràng. Hệ quả: BMS mất kết nối dù chỉ 1 lần >5s rồi nối lại — `Online: Yes` đúng, nhưng alarm "BMS offline" vẫn còn mãi, và vì bit này nằm trong critical alarm mask nên **đẩy state thẳng vào FAULT** dù kết nối đã hoàn toàn ổn định trở lại. Tìm thấy 2026-08-29 khi test HIL thật trên hardware (bơm data BMS qua ZLG CAN adapter, quan sát trên `debug_app`) — không phát hiện được qua review code tĩnh trước đó vì các host-sim cũ chưa từng test kịch bản offline-rồi-hồi-phục. `Modules/bms/bms_core.c` nhánh `OFFLINE→ONLINE` · **Major (hiển thị sai trạng thái, không phải an toàn — relay/charge logic không dựa vào alarm này)** — _Fixed 2026-08-29: thêm `BSP_EnterCritical()/ExitCritical()` bọc quanh `alarm_flags &= ~BMS_ALARM_BMS_OFFLINE` ngay khi `OFFLINE→ONLINE`, cùng pattern bảo vệ read-modify-write đã dùng cho STALE_DATA (alarm_flags bị ISR ghi song song, theo BUG-06). Regression test: `test/host_charge_sim/test_charge_e2e.c:test_bms_offline_then_recovers` — đưa BMS qua OFFLINE rồi resume, assert alarm bit clear + state về ONLINE; verify bằng cách tạm revert fix, xác nhận test FAIL đúng chỗ (log cho thấy state bị đẩy vào FAULT với alarm 0x2000 — đúng khớp triệu chứng quan sát được trên app thật) trước khi khôi phục. Verify đầy đủ: `test/host_charge_sim` (19/19) + `test/host_protocol_sim` + `test/test_logic.c` + `check_architecture.py` + `check_ioc.py` + Release build đều pass._
- [ ] **BUG-05 — Recovery bị STALE chặn** — `alarm_flags==NONE` bao gồm STALE. `bms_core.c:343` · **Medium** — _Re-verified 2026-08-27: **XEM XÉT LẠI, có thể không phải bug** — yêu cầu `!is_stale` trước khi FAULT→ONLINE nghĩa là không tự phục hồi khi chất lượng data đang kém, kể cả khi không còn alarm thật. Có thể là lựa chọn an toàn cố ý (không công nhận "đã ổn" khi data chưa đáng tin), không phải lỗi. Chưa sửa — cần xác nhận ý định thiết kế trước khi động vào._
- [x] **BUG-06 — ISR/main race không bảo vệ** — `g_bms_data/view/last_tick` không `volatile`/`__disable_irq`. `bms_core.c:24-31,202-358` · **High** — _Re-verified 2026-08-27: **ĐÃ FIX** commit `a8ec996` (2026-08-27) — (thu hẹp hơn audit mô tả, phần lớn đã `volatile`/critical-section từ trước). 2 vấn đề thật còn lại: (1) `update_alarm_flags()` (ISR) overwrite toàn bộ `alarm_flags` mỗi ALM_INFO frame, xóa mất bit `BMS_OFFLINE`/`STALE_DATA` do `BMS_Process` quản lý — giờ preserve 2 bit đó; (2) `BMS_Process()`'s `|=`/`&=` trên `STALE_DATA` là read-modify-write không được bảo vệ — giờ bọc `BSP_EnterCritical/ExitCritical` quanh đúng 2 dòng đó (không bọc LOG() vì LOG block tới 50ms)._
- [x] **BUG-07 — LOG blocking trong ISR** — `BMS_FeedFrame` log 1s từ `BSP_CAN RxCallback` (ISR). `bms_core.c:228-255` `debug_log.c:25` 50ms · **High** — _Re-verified 2026-08-27: **ĐÃ FIX** (pre-existing) — `BMS_FeedFrame` không LOG; log snapshot đã chuyển sang `BMS_Process` qua cờ `g_isr_new_data` + throttle 1s._
- [x] **BUG-08 — Critical mask thiếu** — chỉ 3/13 alarm. `bms_core.c:315-317,382-386` · **Medium** — _Re-verified 2026-08-27: **ĐÃ FIX theo quyết định người dùng** commit `37ee2fb` (2026-08-27) — thêm `BMS_ALARM_HIGH_PACK_VOLT` và `BMS_ALARM_TEMP_LOW_CHG` vào critical mask (từ 4/13 lên 6/13), và gộp 2 bản khai báo mask trùng lặp (`BMS_Process`/`BMS_HasCriticalAlarm`) thành 1 hàm dùng chung `bms_critical_alarm_mask()` để tránh lệch nhau về sau._
- [x] **BUG-09 — Severity mapping quá nhạy** — `sev>=1` → flag. `bms_core.c:118-124` · **Medium** — _Re-verified 2026-08-27: **ĐÃ FIX** commit `816a973` (2026-08-27) — theo đúng thang severity đã tài liệu hóa trong `bms_protocol.h` (0=none,1=warning,2=fault,3=severe), giờ yêu cầu `sev>=2` thay vì `sev>=1`._
- [x] **BUG-11 — `ShouldCloseChargeRelay()` stub** — luôn `return true`, bỏ qua `90%` check. `bms_core.c:399-421` · **Medium** — _Re-verified 2026-08-27: **ĐÃ FIX** (pre-existing) — không còn stub `return true`; check online + critical alarm + `charge_relay_closed` từ BMS. Ngưỡng SOC dừng sạc đã có ở `App/Charge/charge_controller.c` (5-band SOC staging), đúng phân lớp policy vs driver._
- [x] **DES-01 — Tracker Init sai giả thiết** — `g_last_valid_rx_tick = HAL_GetTick()` ngay init. `bms_core.c:194` · **Medium** — _Re-verified 2026-08-27: **ĐÃ FIX** commit `816a973` (2026-08-27) — `g_last_valid_rx_tick` khởi tạo `0` (đúng sentinel "chưa nhận gì") thay vì `BSP_GetTick()`. Thực tế không ảnh hưởng logic thoát OFFLINE (chỉ dựa `has_any_valid_bms_data()`), nhưng sửa để `BMS_View_t.last_rx_tick` (PC debug hiển thị) không nói dối lúc boot._
- [ ] **DES-02 — `update_view` phụ thuộc `valid` latch** — giữ mãi dù frame ngừng. `bms_core.c:57-113` · **Medium** — _Re-verified 2026-08-27: **XÁC NHẬN THẬT, giữ nguyên theo quyết định người dùng** — khi 1 loại frame (vd CELL_TEMP) ngừng >2s trong khi frame khác vẫn bình thường, `STALE_DATA` chỉ mang tính cảnh báo, không chặn `BMS_ShouldCloseChargeRelay()`. Người dùng đã chọn giữ hành vi này (chỉ cảnh báo, không chặn sạc) thay vì cho STALE_DATA chặn relay. Không sửa._
- [x] **DES-03 — Thiếu per-frame timeout** — global watchdog → `CELL_TEMP` mất 10s vẫn coi online. `bms_core.c:27,217` · **Medium** — _Re-verified 2026-08-27: **KHÔNG PHẢI BUG, đã có sẵn** — `BMS_Process()` đã loop qua từng `BMS_FRAME_MAX` kiểm tra tuổi riêng của từng loại frame (`BMS_STALE_THRESHOLD_MS`), set `is_stale` nếu bất kỳ frame nào (trừ BMS_SW_STA/CELL_VOLT_FULL/CELL_TEMP_FULL) vượt ngưỡng — không phải chỉ 1 watchdog toàn cục như audit mô tả._
- [x] **DES-04 — `parse_chg_request()` dùng BE trong khi `CAN BMS_BB_PKG V1.0.pdf` mặc định LE** — `bms_protocol.c:112-113` dùng `get_u16_be()`, còn PDF §3 (Physical interface) nói mặc định little-endian "unless otherwise specified", và bảng §5.7 (ChgRequest_INFO) của chính PDF đó không ghi chú ngoại lệ nào → nhìn qua giống bug thật. **Đây KHÔNG phải bug** — _Tìm thấy + xác nhận 2026-08-28: người dùng cung cấp 1 phần tài liệu khác (nguồn "极空/Jikong BMS-CAN 协议 V2.0" §7.2, chỉ có ảnh chụp phần này, không có file đầy đủ) ghi rõ **"注：本帧通信过程中数据采用大端"** (chú ý: dữ liệu bản tin này dùng big-endian) kèm ví dụ số cụ thể `03 48 00 C8` → 84.0V/20.0A chỉ khớp khi đọc BE (đọc LE ra 1843.5V, sai với chính ví dụ trong tài liệu). Xác nhận `get_u16_be()` hiện tại ĐÚNG, không sửa code. Đã bổ sung `docs/BMS_ChgRequest_addendum.md` (chép lại toàn bộ đoạn tài liệu + ví dụ) và comment trực tiếp tại `parse_chg_request()` trích dẫn addendum, để tránh ai đó sau này đọc lại PDF gốc rồi "sửa nhầm" về LE cho khớp các frame khác. Phạm vi: chỉ xác nhận riêng ChgRequest_INFO — các frame khác vẫn theo đúng `CAN BMS_BB_PKG V1.0.pdf`._
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
| FR-CTRL-18 | PASS — _Re-verified 2026-08-27: BMS view retains last-known values through STALE_DATA (see DES-02, section 4), so stage calc is effectively frozen on stale data without extra code._ |
| FR-CFG-01 | PASS |
| FR-CFG-02 | PASS — _Re-verified 2026-08-27: `value_is_invalid()` uses `isfinite()`, catches Inf too (BUG-01, already fixed)._ |
| FR-CFG-03/04/05 | PASS/PASS/PASS — _Re-verified 2026-08-27: OOB write fixed (BUG-02, commit fadfdaf); no `.c` vs `find_blank` drift found (BUG-03)._ |

### 5.2 Issues

- [x] **BUG-01 — Inf lọt validate** — `value_is_invalid = x!=x` không bắt `Inf`. `charge_cycle_config.c:7-9,64-122` · **High — Safety** — _Re-verified 2026-08-27: already fixed (Sprint 1, commit d2a5a94/c79fbd0) — `value_is_invalid()` uses `!isfinite(value)`, which rejects both NaN and ±Inf. Confirmed in current `charge_cycle_config.c:9-11`._
- [x] **BUG-02 — Flash OOB 5B** — `WriteBlock(...,224)` copy từ `&record` 219B. `charge_cycle_storage.c:130` · **High** — _Re-verified 2026-08-27: confirmed real, now fixed (commit fadfdaf) — `ChargeCycleStorage_Save()` copied `ALIGNED_RECORD_SIZE` (224B) from the 219B `record` local, an OOB stack read. Fixed by staging the write in a blank-initialized 224B buffer._
- [x] **BUG-03 — `find_blank_offset` check 219 thay vì 224** — `charge_cycle_storage.c:58` · **Low** — _Re-verified 2026-08-27: not reproducible — `find_blank_offset()` already scans in `ALIGNED_RECORD_SIZE` (224B) strides consistently with the writer; no 219-vs-224 mismatch found in current source. Likely stale from an earlier version of the file._
- [x] **BUG-04 — Stale không freeze target** — `run_bms_controlled_mode:917-951` vẫn tính stage. · **Medium** — _Re-verified 2026-08-27: not a bug given the DES-02 decision (BMS, section 4) — `BMS_View_t` fields are only cleared on the ONLINE/FAULT→OFFLINE transition, never on STALE_DATA alone, so `run_bms_controlled_mode()` naturally keeps computing off the last valid values while stale (no separate freeze needed). No code change._
- [x] **BUG-05 — Spam CAN 20ms** — `SetVoltageAll/SetCurrentLimitAll` mỗi vòng 20ms → ~800 frame/s. `charge_controller.c:263-269` · **Medium** — _Re-verified 2026-08-27: already fixed — `apply_charge_targets()` only calls `CHG_LIB_SetVoltageAll`/`SetCurrentLimitAll` when `target_voltage_v`/`target_current_per_module_a` actually changed since the last applied value (`charge_controller.c:281-288`). No unconditional per-tick CAN spam found in current source._
- [x] **BUG-08 — Manual target NaN** — `payload_float` không validate → `target=NaN`. `pc_protocol.c:314,329` `controller.c:824` · **High** — _Re-verified 2026-08-27: already fixed (Sprint 1) — `PC_CMD_SET_VOLTAGE`/`PC_CMD_SET_CURRENT` in `pc_protocol.c` call `isfinite()` on the decoded float and NACK with `PC_ERR_BAD_PARAM` before it reaches `ChargeController_SetManualTarget()`._
- [x] **SAF-04 — Flash không tắt ngắt** — `HAL_FLASH_Program` trong khi CAN ISR chạy → HardFault G0. `bsp_flash.c:35-54` · **High** — _Re-verified 2026-08-27: already fixed (Sprint 1) — `BSP_Flash_ErasePage`/`WriteDoubleWord`/`WriteBlock` all wrap `HAL_FLASH_Program`/`HAL_FLASHEx_Erase` in `__disable_irq()/__enable_irq()`. One residual nit: the error-path `LOG()` in `WriteBlock` fires before `__enable_irq()`, so a program failure would (rarely) hold IRQs for the LOG's 50ms UART timeout too — low-probability error path, noted for a future pass, not fixed here to keep this change scoped._
- [x] **DES-01 — DERATING dead state** — `charge_controller.h:22` · **Medium** — _Re-verified 2026-08-27: resolved as an intentional, documented design choice, not a defect — `charge_controller.c:8-10` now explicitly documents that DERATING is kept only as a protocol-compatible legacy value while runtime derating is tracked via `g_ctrl.derating` with state staying RUNNING._
- [x] **BUG-12 — Boot-time driver restore bỏ sót `module_type` mặc định** — `App_Init()` (`app_main.c`) tự re-implement mapping `module_type→driver_id` bằng công thức `module_type-1` + range check `[MAXWELL..TONHE]`, khác với mapping switch-case đúng trong `ChargeCycleConfig_Set()` (vốn coi `CHARGE_MODULE_TYPE_EVR_10KW_100A_100V`=1 là alias của TONHE) — trên flash trống thật sự (first boot, chưa từng lưu config), `ChargeCycleStorage_Init()` không gọi `ChargeCycleConfig_Set()` ở nhánh "no valid record", nên driver KHÔNG được chọn và KHÔNG module nào được đăng ký, dù `ChargeCycleConfig_GetDefaults()` đã set sẵn `module_type=EVR`. `app_main.c` dòng cũ ~121-131, `charge_cycle_storage.c:ChargeCycleStorage_Init()` · **Major** — Tìm thấy 2026-08-28 khi review App/System cho PR6 (App orchestration cleanup). — _Fixed 2026-08-28: chuyển gọi `ChargeCycleConfig_Set()` vào CẢ nhánh "no valid record" của `ChargeCycleStorage_Init()` (không chỉ nhánh load-thành-công), dùng đúng 1 mapping table duy nhất cho cả 2 đường — xoá hẳn block re-implement mapping trong `App_Init()` (dead/redundant sau fix). Tiện thể dọn: xoá `App_GetCurrentDriver()` (0 caller toàn repo), dời 2 biến `static` cục bộ trong `App_Loop()` (`last_dwin_tick`, `last_main_log`) lên file-scope cho nhất quán với các biến tick khác trong file. Verify: `check_architecture.py` + `check_ioc.py` + Release build đều pass (logic này phụ thuộc `bsp_flash.h` thật, không host-test được — xác nhận bằng compile-check + trace giá trị enum tay)._
- [ ] **DES-04 — Tick style không nhất quán** — 2 nơi 2 phong cách. · **Low** — _Re-verified 2026-08-27: reviewed, not a functional bug — all tick-delta comparisons in `charge_controller.c` use the same unsigned-subtraction idiom (`now_tick - g_ctrl.xxx_tick`), wraparound-safe since these fields are only written from the main loop (no ISR race, unlike the BMS case). Left as-is; a house-wide signed-diff helper is a style nice-to-have, out of scope here._
- [x] **MNT-06 — Không có seam test** — `static` + `g_ctrl` → không host-test. · **Medium** — _Re-verified 2026-08-27: acknowledged, deferred (see prior note). Re-verified again 2026-08-28: **ĐÃ ĐÓNG, qua hướng khác** — `test/host_charge_sim/` giờ host-compile và chạy chính `charge_controller.c` thật (không phải bản rút gọn) trong 13 kịch bản end-to-end (happy path, stage/derating, module fault, BMS offline/alarm/stale, relay latch...), dùng CAN backend pluggable (`CHG_LIB_CanBackend_Set`) + BMS/module simulator theo đúng byte layout thật thay vì mock giả lập rời. Đây không phải unit test kiểu inject-state như `bms_protocol.c` (vẫn giữ nguyên `g_ctrl` file-static — cân nhắc lại, split struct thuần vì "dễ đọc hơn" không đáng đánh đổi rủi ro động vào ~1300 dòng đang hoạt động đúng, đúng tinh thần AGENTS.md 2.1), nhưng lo ngại gốc ("không kiểm chứng được bằng test tự động") đã được giải quyết bằng phong cách integration-test. Không sửa thêm._
- [x] **B-23 — Review thuật toán sạc theo yêu cầu người dùng: đối chiếu `charge_controller.c` với SRS FR-CTRL-01..18, tìm 2 mục TBD lỗi thời + 1 gap relay thật** — Người dùng yêu cầu "check phần thuật toán sạc có chạy đúng chu trình như spec hay không". Đối chiếu từng FR-CTRL với code, kết luận chung: FSM/3-mode/3-stage-band/hard-protection/relay-latch đều khớp SRS đúng. Phát hiện + xử lý:
  1. **SRS TBD-02/TBD-03 đã lỗi thời** — code đã fix từ trước (không rõ session/commit nào, không có ghi chú ở đâu) nhưng SRS vẫn liệt kê là "chưa làm": `BMS_ShouldCloseChargeRelay()` đã được nối vào `update_relay_decision()` (`charge_controller.c`) và `app_main.c` đã đọc ADC NTC thật (max 4 kênh, fallback 25°C khi disconnect) truyền vào `ChargeController_SetJackTempC()`. Đã đóng cả 2 mục trong SRS §7.2, không cần sửa code.
  2. **TBD-04 (BMS-mode bỏ qua `bms.chg_volt_request`/`chg_curr_request`, dùng config nội bộ)** — người dùng xác nhận trực tiếp đây là thiết kế đúng: BMS chỉ giám sát/an toàn, U/I luôn do thuật toán sạc quyết định. Đóng TBD-04 trong SRS; strengthen comment tại `run_bms_controlled_mode()` ghi rõ quyết định + ngày xác nhận.
  3. **TBD-05 (Relay 1/2/3 chưa có logic) — gap thật, đã fix**: người dùng xác nhận phần cứng thực tế chỉ cần 1 relay để đóng/cắt, 3 relay (PB14/PB15/PA8) không phải 3 chức năng riêng biệt. Trước fix chỉ RELAY_1/RELAY_2 (`GPIOB`) được ghi theo `relay_should_close`; RELAY_3 (`GPIOA`, PA8) chỉ bị RESET lúc init và trong `bsp_failsafe.c`'s `Safety_Shutdown()` (mở khi lỗi), không có đường đóng nào cả. Fixed: `app_main.c` giờ ghi cả `MCU_PA8_RELAY_3_Pin` (GPIOA) cùng `relay_should_close` y hệt RELAY_1/RELAY_2. `Safety_Shutdown()` không cần sửa (đã tắt cả 3 relay từ trước).
  4. **Xác nhận nghiệp vụ, không phải bug — code đã đúng, chỉ làm rõ comment**: (a) cell-voltage/SOC band chỉ tiến (high-watermark latch, không lùi trong 1 chu kỳ) — khớp `eval_cell_stage()`/`eval_soc_stage()` hiện tại; (b) nhiệt độ là band DUY NHẤT được lùi, chỉ khi giảm quá `threshold - temp_delta_c` — khớp `eval_temp_stage()`'s hysteresis hiện tại; (c) Manual mode giữ nguyên setpoint cố định suốt phiên, không tự động derating theo jack-temp (khác Standalone/BMS-Controlled) — khớp code hiện tại (`run_manual_mode()` không gọi `apply_jack_temp_derating()`), người dùng xác nhận đây là chủ ý.
  5. **Code cleanup theo yêu cầu người dùng** ("làm rõ ràng thuật toán chỗ này ra để clear code"): `apply_band_current_limit()`'s ABOVE_MAX branch set `eval->inhibit=1` dùng chung cho cả 3 stage source, nhưng ý nghĩa khác nhau — với temperature là inhibit sống/hồi phục được (FR-CTRL-11), với cell/SOC là latch kết thúc chu kỳ (FR-CTRL-09/10, qua `g_ctrl.cell_full_latched`/`soc_full_latched`). Code cũ gán `g_ctrl.inhibit = stage_inhibit` TRƯỚC rồi mới check completion latch NGAY SAU đó để override bằng `transition_to(STOPPING)` — đúng chức năng nhưng đọc dễ hiểu lầm là "set rồi ghi đè cùng tick". Refactor: đảo thứ tự trong `run_bms_controlled_mode()` — check `cell_full_latched`/`soc_full_latched` NGAY sau `compute_stage_limits()`, trước khi đụng tới bất kỳ field `g_ctrl.inhibit`/`active_limit_source`/etc nào; thêm doc-comment đầy đủ ở `apply_band_current_limit()` giải thích rõ 2 ý nghĩa khác nhau của cùng 1 flag `.inhibit` theo caller. Behavior-preserving thuần túy — verify bằng cách chạy lại `test_tonhe_stage_derating` (kịch bản duy nhất exercise cell-band derating path) trước/sau refactor, output log giống hệt.
  `App/Charge/charge_controller.c` (`apply_band_current_limit()`, `run_manual_mode()`, `run_bms_controlled_mode()`), `App/System/app_main.c` (relay GPIO write), `docs/SRS_Charger_Controller.md` §7.2 · **Minor (documentation/clarity, 1 gap thật là relay — không an toàn nghiêm trọng vì cả 3 relay trước đó đều bị `bsp_failsafe.c` mở đồng thời khi lỗi, chỉ là RELAY_3 chưa từng được ĐÓNG trong vận hành bình thường)** — Verify: `test/host_charge_sim` (20/20, bao gồm `test_tonhe_stage_derating` xác nhận log giống hệt trước/sau refactor) + `test/host_protocol_sim` + `test/test_logic.c` + `check_architecture.py` (0 violation mới) + `check_ioc.py` (11/11) + Release build (RAM 13.96%, FLASH 53.03%) đều pass.
- [x] **B-24 — Relay-arm deadlock nghi ngờ khi đấu đúng chuẩn sạc→relay→pin (BMS-Controlled mode)** — Người dùng test thật trên hardware (2026-08-29): đấu **trực tiếp sạc→pin** (bỏ qua relay) thì relay logic đóng bình thường; đấu **đúng chuẩn sạc→relay→pin** thì relay **không bao giờ đóng**. `update_relay_decision()` (`charge_controller.c:242-299`) arm latch dựa trên **điện áp module sạc tự báo cáo** (`CHG_LIB_ModuleView_t.voltage`, đo tại đầu ra CHÍNH module, không phải tại pin) đạt `BMS_CHARGE_VOLT_LIMIT_PCT` (90%) target — nghi ngờ root cause: khi relay đúng nghĩa nằm trong mạch và đang mở, module sạc chạy **không tải hoàn toàn** (mạch hở), các module PFC+LLC như Maxwell/Lianming/TonHe thường có bảo vệ chống chạy không tải (không cho áp ra tự do tăng để tránh overshoot/cộng hưởng LLC) → điện áp tự báo cáo không bao giờ đạt 90% → latch không bao giờ arm → relay không bao giờ đóng → không tải → deadlock vĩnh viễn. Đấu trực tiếp bỏ qua relay thì luôn có tải thật (pin) ngay từ đầu nên không gặp vấn đề này — khớp chính xác quan sát thực tế của người dùng. · **Major — Safety/Functional** — _2026-08-29: **Đã fix**, sau khi test tiếp trên hardware thật (module đã lên áp >90% **target** rồi mà relay vẫn không đóng — loại B-24 riêng cho case đó, dẫn tới phát hiện bug B-25/§8d `BMS_SendCtrlInfo()` trước), người dùng quay lại xác nhận đúng root cause B-24 và chốt hướng fix: đổi ngưỡng 90% từ so với `target_voltage_v` (setpoint CUỐI CÙNG) sang so với `BmsView.batt_voltage` (điện áp pin THẬT, BMS đo trực tiếp, độc lập hoàn toàn với trạng thái relay). Đây không chỉ phá deadlock mà còn đúng nguyên tắc kỹ thuật pre-charge hơn hẳn: logic cũ đòi module ramp gần tới target CUỐI (VD pin đang 300V, target 500V → phải lên 450V mới đóng) → lúc đóng relay, áp module (450V) vênh xa áp pin thật (300V) → dòng inrush lớn; logic mới chỉ cần module ramp gần áp PIN HIỆN TẠI (300V×90%=270V) — đúng nguyên tắc "2 điện áp phải gần nhau mới đóng công tắc nối chúng", đồng thời module không cần đạt target xa vời (không cần tải nhiều) mới có relay để có tải → phá deadlock. Standalone (No-BMS) mode giữ nguyên dùng `target_voltage_v` (không có BMS để tham chiếu). Implementation + test: xem §8e phía dưới. (Bug độc lập `BMS_SendCtrlInfo()` không có caller — §8d — đã fix riêng trước đó cùng phiên, không phải cùng root cause với B-24.)_

---

## 6. Checklist — Truyền thông (PC/BMS/HMI)

**Test coverage note (2026-08-28):** trước ngày này, `pc_protocol.c`/
`pc_debug_protocol.c`/`dwin_protocol.c` không có test hành vi thật nào —
`test/test_pc_debug_comm.py` chỉ chạy một bản Python re-implement của
FeedByte state machine, tự assert với chính nó (không compile/chạy code C
thật); `dwin_protocol.c` không có test nào cả. Đã thêm
`test/host_protocol_sim/test_pc_protocol_e2e.c` (15 scenario, compile +
chạy code C thật của cả 2 file protocol) và
`test/host_protocol_sim/test_dwin_protocol_e2e.c` (7 scenario cho DWIN),
theo đúng pattern `test/host_charge_sim` đã dùng cho `chg_lib`/`charge`/
`bms`. Bộ test PC protocol mới này tìm ra B-21 (xem 3.2) ngay trong lần
viết đầu tiên. Đã thêm 2 API test/debug-support nhỏ, không đổi hành vi
(`PC_Protocol_GetTxQueueDepth()`, `PC_Protocol_PeekTxFrame()`) để test có
thể kiểm tra response thật đã enqueue mà không cần USB thật.

### 6.1 Compliance FR-USB / FR-HMI

| FR | Kết luận |
|---|---|
| FR-USB-01/03/05/06/07/09 | PASS |
| FR-USB-02 | PASS — _Re-verified 2026-08-27: SOF resync fixed (B-02, commit fadfdaf)._ |
| FR-USB-04 | PASS — _Re-verified 2026-08-27: queue backpressure policy reviewed and is well-defined (B-04); ISR race fixed with volatile+critical sections (B-10, Sprint 1)._ |
| FR-USB-08/10 | PASS — _Re-verified 2026-08-27: `SET_DRIVER` NACK uses the correct named error constant (B-05 not reproducible)._ |
| FR-HMI-01/02 | PASS (protocol) |
| FR-HMI-03/04 | PASS — _Re-verified 2026-08-27: `App_Loop` now calls `DWIN_UpdateData()` every 50ms (D-06, Sprint 1)._ |
| FR-OPS-04/05/06 | PASS |
| FR-OPS-07 | PASS — _Re-verified 2026-08-27: CAN filters now accept both Standard and Extended ID (B-01, Sprint 1)._ |

### 6.2 Issues

- [x] **B-01 — CAN filter reject Std** — Trùng BMS BUG-01 · **Critical** — _Re-verified 2026-08-27: already fixed (Sprint 1) — `BSP_CAN_ConfigFilters()` in `bsp_can.c` configures both a Standard-ID and an Extended-ID mask filter with mask 0x000/0x00000000 (accept-all) into RXFIFO0, so std-ID BMS frames are no longer rejected._
- [x] **B-02 — SOF re-sync bỏ lỡ `AA` lặp** — `pc_protocol.c:450-451` · **Medium** — _Re-verified 2026-08-27: confirmed real, now fixed (commit fadfdaf) — `PC_Protocol_FeedByte()`'s `ST_SOF2` case silently dropped a repeated `PC_SOF1` byte instead of treating it as a new frame start. Fixed to re-enter `ST_SOF2` on a repeated SOF1, matching the resync already done in `dwin_protocol.c`._
- [x] **B-04 — TX queue full drop im lặng** — `pc_protocol.c:88-91,109-112` · **Medium** — _Re-verified 2026-08-27: reviewed — current `enqueue_frame()` behavior is a deliberate, defined backpressure policy, not a silent drop: it evicts only the oldest queued (not-yet-in-flight) frame to make room, and returns `false` only when the queue is full *and* the head frame is mid-DMA transfer._
- [x] **B-05 — NACK sai code `SET_DRIVER`** — `0x04` thay vì `0x05`. `pc_protocol.c:379` · **Low** — _Re-verified 2026-08-27: not reproducible — `PC_CMD_SET_DRIVER`'s failure path uses the named `PC_ERR_BAD_PARAM` constant via `send_nack()`, not a raw `0x04` byte._
- [x] **B-07 — FW version lệch** — `pc_protocol.h:2.0.0` vs `BuildSystemInfo 1.0.0`. `pc_debug_protocol.c:177-179` · **Medium** — _Re-verified 2026-08-27: not reproducible — `PC_Protocol_SendPong()` and `DebugProtocol_BuildSystemInfo()` both read the same `FW_VERSION_MAJOR/MINOR/PATCH` macros; single firmware-version source, no drift possible._
- [x] **B-08 — `bms_stale` không gán** — dòng 216 trống → luôn 0. `pc_debug_protocol.c:214-216` · **Medium** — _Re-verified 2026-08-27: not reproducible — `DebugProtocol_BuildSystemInfo()` (`pc_debug_protocol.c:214`) assigns `info->bms_stale` from `bms_view.alarm_flags & BMS_ALARM_STALE_DATA`; the line is not blank in current source._
- [x] **B-10 — TX queue race ISR/main** — `g_tx_count/head/tail` không `volatile`/`__disable_irq`. `pc_protocol.c:30-35,83-146` · **High** — _Re-verified 2026-08-27: already fixed (Sprint 1) — every access to `g_tx_head/tail/count/in_flight` in `pc_protocol.c` is `volatile` and wrapped in `BSP_EnterCritical()/BSP_ExitCritical()`._
- [x] **D-01 — LOG blocking trong USB ISR** — `pc_protocol.c:433,473` `debug_log.c:25` 50ms · **High** — _Re-verified 2026-08-27: already fixed (Sprint 1) — every USB-ISR-context function in `pc_protocol.c`/`pc_debug_protocol.c` is explicitly commented "no LOG in ISR" and verified free of `LOG()` calls._
- [x] **D-03 — DWIN FSM không align** — `dwin_protocol.c:62-81` chunk giữa frame bị discard · **High** — _Re-verified 2026-08-27: not reproducible — `DWIN_ParseRX()` keeps `rx_idx`/`expected_len` in `static` locals across calls, so a frame split across UART reads resumes correctly; a repeated header byte mid-search is also handled (`dwin_protocol.c:75-79`)._
- [x] **D-04 — DE không guard-time** — `bsp_rs485.c:27-36` thiếu đợi `TC` + delay 200µs · **Medium** — _Re-verified 2026-08-27: already fixed (Sprint 1) — `UART_Transmit_To_DWIN()` waits a 2ms guard time before transmitting, and the blocking `HAL_UART_Transmit()` only returns after transmission completes, so DE is never dropped early._
- [x] **D-06 — `DWIN_UpdateData` spam 11 frame** — blocking ~7.7ms, phá 20ms loop. `dwin_protocol.c:46-59` · **Medium** — _Re-verified 2026-08-27: already fixed (Sprint 1) — `DWIN_UpdateData()` is now a 12-step state machine sending one VP write per call, and `App_Loop` calls it once every 50ms — ~1 frame/50ms instead of 11 frames back-to-back. `FR-HMI-03/04` (dead code) is also resolved: `App_Loop` does call it._
- [x] **B-11 — DWIN `VP_FAULT_CODE` gửi raw bitmask thay vì fault code tuần tự** — `app_main.c:296` (dòng cũ) làm `dwin_data.fault_code = cc_view.fault_flags;` trực tiếp — `fault_flags` (`charge_controller.h`) là bitmask `uint32_t` có thể OR nhiều `CHARGE_CTRL_FAULT_*` (vd `BMS_OFFLINE=(1<<3)=8`, `EMERGENCY_STOP=(1<<11)=2048`), trong khi màn hình DWIN mong đợi `DWIN_FaultCode_e` (`dwin_protocol.h`) — mã tuần tự 0-7, tương ứng 1 icon lỗi. Khi có lỗi thật (vd BMS mất kết nối), màn hình nhận giá trị 8 thay vì 1 → hiện icon sai hoặc không khớp icon nào, đánh lừa người vận hành đúng lúc cần biết chính xác nhất. `pc_protocol.c`/`app_main.c` (dòng cũ ~296) · **Major (an toàn vận hành — hiển thị sai trạng thái lỗi)** — Tìm thấy 2026-08-28 khi review App/System+HMI cho PR7 (PC+DWIN protocol cleanup). — _Fixed 2026-08-28: thêm hàm `dwin_fault_code_from_flags()` trong `app_main.c` dịch bitmask sang `DWIN_FaultCode_e` theo thứ tự ưu tiên (nghiêm trọng nhất trước: EMERGENCY_STOP→HARDWARE, BMS_OFFLINE→BMS_OFFLINE, v.v., catch-all→HARDWARE cho các bit chưa có icon riêng). Đây là bảng ưu tiên UX tốt nhất-có-thể, KHÔNG phải safety interlock (relay/charge logic đọc `fault_flags` trực tiếp, không qua bảng này) — cần người phụ trách bộ icon DWIN (`ui/DWIN_SET/`) xác nhận lại thứ tự nếu chưa đúng ý đồ UI. Verify: `check_architecture.py` + `check_ioc.py` + Release build đều pass; chưa có test tự động cho hàm này (thuần logic, có thể tách ra host-test được nếu cần, nhưng chưa làm trong lần sửa này)._
- [x] **B-12 — Stack buffer overflow khi build debug ALL_MODULES với ≥3 module** — `DebugProtocol_BuildModuleData(idx, data)` (`pc_debug_protocol.c`, cũ) KHÔNG có tham số `max_len` — luôn ghi cứng `sizeof(DebugModuleData_t)`=123 byte vào `data`, bất kể còn bao nhiêu chỗ trống. `DebugProtocol_BuildAllModulesData()` gọi hàm này ở offset tăng dần vào buffer stack 255 byte (`PC_MAX_PAYLOAD`, dùng chung bởi `DebugProtocol_SendStream()` mỗi 1s KHI debug mode bật, và `DEBUG_CMD_READ_ALL`), rồi mới kiểm tra `written+mod_len>max_len` — SAU KHI đã ghi. Với header 2 byte, 2 module vừa khít (2+123×2=248≤255), nhưng module thứ 3 ghi từ offset 248 → tràn 116 byte ra ngoài buffer stack. Hệ thống thật chạy 6-8 module (xem B-10) → tràn stack xảy ra thường xuyên, không phải edge case. `pc_debug_protocol.c:55-118,137` (dòng cũ) · **Critical (memory-safety, stack corruption)** — Tìm thấy 2026-08-28 khi review PC debug protocol. — _Fixed 2026-08-28: thêm tham số `max_len` cho `DebugProtocol_BuildModuleData()`, kiểm tra `max_len < sizeof(DebugModuleData_t)` và trả về 0 TRƯỚC khi ghi bất cứ gì — fix đúng chỗ (bên trong hàm ghi, không chỉ ở call site vì call site tự nó không đủ, hàm bị gọi có thể tràn trước khi caller kịp biết). Cả 2 call site (`BuildAllModulesData`'s loop, `DEBUG_CMD_READ_ONE`) cập nhật truyền đúng dung lượng còn lại. Tiện thể sửa thêm: `DebugProtocol_BuildBMSData()`'s check `max_len<54` không khớp số byte thực ghi (50) — sửa về đúng 50; `DebugProtocol_BuildSystemInfo()` đọc CAN tx/rx counter qua `extern` trực tiếp vào biến nội bộ của `bsp_can.c` thay vì gọi `BSP_CAN_GetStats()` — đổi sang gọi accessor, đọc vào biến local trước khi gán vào struct `__attribute__((packed))` (tránh unaligned write qua `&info->canN_xx_count`, vốn không đảm bảo align 4 byte trên struct packed — Cortex-M0+ không hỗ trợ unaligned access phần cứng, bắt được cảnh báo `-Waddress-of-packed-member` ngay khi thử cách trực tiếp). Regression test: `test/host_protocol_sim/test_pc_protocol_e2e.c` thêm `test_build_module_data_refuses_when_too_small` (unit-level, kiểm tra contract của hàm) và `test_build_all_modules_data_does_not_overflow_buffer` (canary byte ngay sau buffer, đăng ký 5 module) — xác nhận cả 2 test FAIL đúng chỗ khi tạm revert fix (canary bị ghi đè 0x00), rồi khôi phục và xác nhận pass. Verify: 4 bộ test host (mới thêm mock `BSP_CAN_GetStats()` vào `test/mock_hal/`) + `check_architecture.py` + `check_ioc.py` + Release build đều pass._
- [ ] **M-01 — DMA vs IT mâu thuẫn** — `.ioc` DMA Ch3 cho USART3_RX nhưng dùng `IT 1B`. `usart.c:246-262` `bsp_rs485.c:24` · **Low** — _Re-verified 2026-08-27: confirmed present but reclassified as unused-resource cleanup, not a functional bug — `usart.c` inits a DMA channel for USART3 RX, but `bsp_rs485.c` actually uses `HAL_UART_Receive_IT()` (1-byte interrupt mode). Harmless (IT-mode RX works correctly with the ring buffer), but wastes a DMA channel. Left as low-priority cleanup rather than risk changing a working RX path without hardware to verify a DMA-mode replacement._

---

## 7. Checklist — BSP / Platform / CubeMX

### 7.1 Drift `.ioc` ↔ generated (đã fix 80%, 2 drift chết người còn lại)

| # | Ngoại vi | `.ioc` | Generated `Core/Src/*` | Kết luận |
|---|---|---|---|---|
| D-01 | FDCAN1 Prescaler/Seg/AR | `NominalPrescaler=32, Seg1=12 Seg2=3, AR=ENABLE, Ext=1` | Khớp (`fdcan.c:48-57`) → 125K | ✅ OK |
| D-02 | FDCAN2 Prescaler | Chưa có dòng `FDCAN2.NominalPrescaler` (chỉ Seg) | `fdcan.c:85 =16` → 250K | ✅ RE-VERIFIED OK 2026-08-27 — `Charger.ioc:117` already has explicit `FDCAN2.NominalPrescaler=16`, matching `fdcan.c:85`. `check_ioc.py` asserts this and passes. |
| D-08 | ADC | `.ioc:7` yêu cầu 4 kênh NTC | `adc.c:50-74` chỉ `CH0` | ✅ RE-VERIFIED OK 2026-08-27 — `adc.c` has `ScanConvMode=ENABLE`, `NbrOfConversion=4`, ranks CH0-CH3. `check_ioc.py` passes. |
| D-10 | SPI | `.ioc` không khai `DataSize` | `spi.c:44,78` `4BIT` | ✅ RE-VERIFIED OK 2026-08-27 — both `hspi1`/`hspi2` `DataSize=SPI_DATASIZE_8BIT` in current `spi.c`. `check_ioc.py` passes. |

### 7.2 Compliance FR-OPS / C / NFR liên quan BSP

| ID | Kết luận |
|---|---|
| FR-OPS-01 | PARTIAL — _Re-verified 2026-08-27: PB1 (RS485 DE) boot glitch fixed (I-06, commit fadfdaf); PA4 POWER_EN still has no explicit power-up delay (I-10, left open, needs hardware timing verification)._ |
| FR-OPS-02/05/07/C-03/C-04/C-05 | PASS |
| C-01/NFR-01 | PARTIAL — _Re-verified 2026-08-27: most blocking-TX/LOG-in-ISR paths removed (D-01, B-10, Sprint 1); RS485 TX to DWIN is still a blocking call with a 100ms timeout ceiling (I-07, left open, needs hardware verification to convert to IT/DMA safely)._ |
| NFR-04/06/07 | PASS — _Re-verified 2026-08-27: LOG banner text correct (I-08 not reproducible); stack already 2KB / heap 512B (I-16 not reproducible)._ |
| FR-OPS-04 | PARTIAL (bus-off restart nông) |
| C-07 | RISK (không có guard assert sau regen) |

### 7.3 Issues — BSP

- [x] **I-01 — Không có IWDG/WWDG** — `stm32g0xx_hal_conf.h:48,60` disable, `.ioc` không config · **Critical** — _Re-verified 2026-08-27: already fixed (Sprint 1) — `HAL_IWDG_MODULE_ENABLED` defined, `MX_IWDG_Init()` configures ~1s timeout (LSI/32, reload 1000), called from `main()` after `App_Init()`. `check_ioc.py`'s IWDG check passes._
- [x] **I-02 — `Error_Handler`/`HardFault` treo không safe-state** — `main.c:180-188` `it.c:97-107` `while(1)` · **Critical** — _Re-verified 2026-08-27: already fixed (Sprint 1) — both `Error_Handler()` and `HardFault_Handler()` call `Safety_Shutdown()` before their diagnostic dump / infinite loop._
- [x] **I-03 — Thiếu IWDG refresh** — `App_Loop` không `HAL_IWDG_Refresh` · **High** (gộp I-01) — _Re-verified 2026-08-27: already fixed (Sprint 1) — `App_Loop()` calls `MX_IWDG_Refresh()` once per iteration, main-loop only, never from an ISR._
- [x] **I-04 — NVIC đồng mức 0** — tất cả IRQ `0,0` trừ SysTick 3. `Charger.ioc:198-213` `fdcan.c:147` · **High** — _Re-verified 2026-08-27: already fixed (Sprint 1) — NVIC priorities are no longer uniformly 0: FDCAN/USB=0, USART/DMA=1, ADC/TIM=2._
- [x] **I-05 — RS485 DMA vs IT drift** — `Charger.ioc:69` DMA Ch3 nhưng IT 1-byte. · **High** — _Re-verified 2026-08-27: see B-01/M-01 in section 6 — the DMA-vs-IT drift is real but reclassified as unused-resource cleanup, not a functional or safety bug._
- [x] **I-06 — PB1 DE glitch boot** — `gpio.c:61` `SET` (TX) trước `RESET` (RX). `Charger.ioc:291` · **High** (Medium sau SWAP fix) — _Re-verified 2026-08-27: confirmed real, now fixed (commit fadfdaf) — `MX_GPIO_Init()` defaulted the RS485 DE pin HIGH (TX-enable) before `BSP_RS485_Init()` set it LOW, leaving a boot-time bus-drive window. Changed the CubeMX default to LOW in both `gpio.c` and `Charger.ioc`._
- [ ] **I-07 — RS485 TX blocking 100ms** — `bsp_rs485.c:33` · **High** — _Re-verified 2026-08-27: reviewed, left open with rationale — `UART_Transmit_To_DWIN()` still blocks with a 100ms timeout ceiling; typical transfers are ≤8 bytes (~1ms) plus the 2ms guard, so the 100ms is only a worst-case ceiling, not the normal cost. Converting to interrupt/DMA-driven TX would need a scope/logic-analyzer on real RS485 hardware to validate the DE turnaround — not safe to change blind. Documented as a follow-up._
- [x] **I-08 — LOG blocking 50ms + banner sai** — `debug_log.c:13,25,35` FDCAN1 ghi `@250K` sai (phải 125K) · **Medium** — _Re-verified 2026-08-27: not reproducible — `debug_log.c:36` already prints `FDCAN1 @125K / FDCAN2 @250K`, matching the actual configured bit rates._
- [x] **I-09 — SPI 4BIT** — `spi.c:44,78` · **High latent** — _Re-verified 2026-08-27: not reproducible — see D-10 above; SPI is already 8-bit in current source._
- [ ] **I-10 — POWER_EN không delay** — `app_main.c:69` `SET` → ngay `BSP_CAN_Start`. B1205S cần ~20ms · **Medium** — _Re-verified 2026-08-27: reviewed, left open — `app_main.c:75` sets `POWER_EN` HIGH with no explicit delay before `BSP_CAN_Start()`. Adding a fixed delay is straightforward, but the correct value depends on the transceiver's datasheet timing and hasn't been verified against real hardware in this session._
- [x] **I-13 — Flash align check thiếu** — `bsp_flash.c:35-54` không verify `address%8`, biên trang · **Medium** — _Re-verified 2026-08-27: not reproducible — `BSP_Flash_ErasePage`/`WriteDoubleWord`/`WriteBlock` all already check `(address % 8U) != 0U` and bound-check against the flash end address._
- [x] **I-16 — Stack/Heap nhỏ** — `STM32G0B1xx_FLASH.ld:66-67` 512B/1KB, còn FLASH dư 8K — tăng Stack 0x800 khi feature creep · **Medium** — _Re-verified 2026-08-27: not reproducible — `STM32G0B1xx_FLASH.ld` already sets `_Min_Stack_Size = 0x800` (2KB) / `_Min_Heap_Size = 0x200` (512B), not the smaller values the audit describes._

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

_Update 2026-08-28: item 3.3's HIL half is no longer blocked — `test/integration_sync_test.py` now runs a real end-to-end smoke test against actual hardware (ZLG USBCAN adapter simulating BMS+TonHe module CAN traffic, driving the real firmware over its real USB CDC debug protocol), replacing the old PCAN-only placeholder that never ran against this project's actual hardware. Verified: real MCU (current Release build flashed via ST-Link) reached RUNNING and returned to IDLE cleanly against the simulated bus. See the file's own docstring for scope (TonHe-only for now, matches the currently-configured hardware; Maxwell/Lianming HIL coverage is not yet built) and setup requirements (ZLG USBCAN vendor driver, `ZLG_USBCAN_DLL_PATH` env var). Still open: multi-module HIL, fuzz, power-cycle, 72h soak (2 mock modules and beyond)._

---

## 8b. Integration test end-to-end trên hardware thật (2026-08-29)

Theo yêu cầu người dùng: test toàn bộ chu trình thật — PC app (`debug_app`) → USB CDC → MCU → CAN1 (module sạc) + CAN2 (BMS) — với điều kiện chuẩn (user chọn driver/module trên app, set Charge Config, bấm START, chu trình sạc chạy theo đúng thuật toán + config) và các điều kiện fail/bảo vệ (alarm module, alarm BMS, BMS offline, emergency stop).

**Setup**: MCU thật (firmware tại commit `32e96a6`, đã flash) + ZLG USBCAN adapter 2 kênh giả lập đồng thời BMS (CAN2, `BmsSim` mở rộng từ `test/integration_sync_test.py`) và module TonHe (CAN1, `TonheModuleSim` mở rộng, có ramp điện áp thực tế + inject alarm/offline theo yêu cầu qua control file, không cần restart script). Người dùng thao tác trực tiếp trên `debug_app` thật qua COM12 (song song, không tranh cổng vì ZLG là thiết bị USB riêng).

**Phát hiện + fix trong lúc test** (đã commit riêng trước đó — `32e96a6`, các sửa `debug_app/main.py` commit cùng đợt với ghi chú này):
- `debug_app`'s `_add_module()` chỉ check trùng module theo dict nội bộ `self.modules`, trong khi auto-sync lúc Connect (`_sync_driver()`→`_sync_module_addr()`) đã đăng ký module thật lên MCU nhưng không cập nhật dict/bảng UI → lần "Add" đầu tiên của người dùng luôn bị NACK "Module already exists" dù chưa từng add gì trên UI, chặn cứng không thể START. Fix: (1) `_add_module()` gặp module trùng thì re-select/re-sync thay vì chặn cứng; (2) `_update_module_from_data()` (nhận stream `ALL_MODULES` từ MCU mỗi 1s) giờ đánh dấu module tự phát hiện là `user_added=True` ngay lập tức, hiện thẳng lên bảng Control screen — đúng theo yêu cầu người dùng "MCU có load config module_type từ flash, cần cập nhật nó luôn vào màn control".

**Kết quả test** (tất cả PASS):

| Kịch bản | Kỳ vọng (theo SRS/AGENTS.md) | Kết quả thật |
|---|---|---|
| Golden path: chọn driver+module trên app → SET_CHARGE_CFG → START | IDLE→READY→RUNNING, áp/dòng đúng `vmax_v`/band hiện tại | ✅ Module RUNNING, V=58.40V (=`vmax_v`), I theo đúng band cell-voltage đang active |
| Band walkthrough cell-voltage Level 1→2→3→4→ABOVE_MAX | Derating giảm dần đúng current-limit từng band (1.00C→0.80C→0.50C→0.30C), high-watermark latch không lùi (đã xác nhận với người dùng: chỉ nhiệt độ được lùi theo hysteresis, cell/SOC chỉ tiến) | ✅ Current chuyển đúng 100A→80A→50A→30A theo từng lần đẩy cell-voltage qua ngưỡng; đạt ABOVE_MAX (≥`cell_volt_5_v`) → `cell_full_latched` → STOPPING → IDLE, `stop_reason=CELL_VOLTAGE_REACHED` |
| Module alarm (short-circuit, TonHe fault_bits bit15) | Module → FAULT ngay; Controller phát hiện module rời "active set" → sau `module_mismatch` timer 10s → FAULT (`CHARGE_CTRL_FAULT_MODULE_COUNT_MISMATCH`) | ✅ Module FAULT ngay ("Short circuit; State fault"), Controller Stopping sau ~10s, Stop Reason "Module mismatch", fault flag `0x00000004` |
| BMS alarm critical (High Cell Voltage, severity≥2) | Dừng NGAY LẬP TỨC (không đợi timer, khác module alarm vì đây là an toàn pin) — `CHARGE_CTRL_FAULT_BMS_ALARM` | ✅ BMS State FAULT ("High cell voltage"), Controller Stopping tức thì, fault flag `0x00000020` |
| BMS offline ≥5s | `BMS_STATE_OFFLINE`, cache xoá (FR-BMS-03), Controller FAULT (`CHARGE_CTRL_FAULT_BMS_OFFLINE`); khi BMS phục hồi, không dính latch mãi mãi (BUG-13) | ✅ OFFLINE đúng sau ~5-7s, Battery Pack/Cell Extremes reset về 0, Stop Reason "BMS offline"; sau khi resume, BMS Online=Yes + Alarms=None ngay, không dính latch — xác nhận lại BUG-13 vẫn đứng vững |
| EMERGENCY_STOP giữa lúc RUNNING | `CHG_LIB_EmergencyStop()` dừng module NGAY trong chu kỳ hiện tại (NFR-03), không qua STOPPING thông thường | ✅ Module nhận lệnh STOP tức thì (quan sát trực tiếp qua CAN1) |

**Đối chiếu schematic thật**: nhân tiện review thuật toán sạc, đã đối chiếu `docs/CHARGER_CTRL_Ver1.0_Schematic_2026-08-23.PDF` (Sheet 7/10, `05.LED_NTC_COM.SchDoc`) cho mạch đo nhiệt độ jack — pull-up thật = 10kΩ 1%, khớp chính xác `R_REF=10000.0f` trong `BSP/bsp_adc.c`. NTC 10K/B=3950 (theo người dùng xác nhận) khớp `NTC_R25`/`NTC_B`. Không có sai lệch phần cứng ↔ firmware.

**Xác nhận nghiệp vụ khác trong lúc review** (không phải bug, ghi lại để tránh audit sau nhầm lẫn): cắt sạc khi **1 cell bất kỳ** (không phải trung bình) chạm trần điện áp là đúng chuẩn an toàn pin Lithium (tránh over-voltage cell lệch cân bằng) — `eval_cell_stage()` dùng `bms->max_cell_volt` đúng thiết kế, không cần sửa.

## 8c. Bug thật từ HIL: module mắc kẹt RECOVERING vĩnh viễn sau STOP/EMERGENCY_STOP (2026-08-29)

**Phát hiện**: Sau bài test §8b, người dùng tắt toàn bộ simulator để kiểm tra hành vi khi mất kết nối module thật ("hãy tắt toàn bộ simu đi nhé"). Với module đang ở RECOVERING (do simulator ngừng phát), người dùng bấm STOP rồi EMERGENCY_STOP — module vẫn đứng nguyên ở RECOVERING mãi mãi, không bao giờ về IDLE hay OFFLINE.

**Root cause**: hai lỗ hổng cộng hưởng trong `Modules/chg_lib/`:
1. `CHG_LIB_FSM_CheckOfflineTimeout()` (`chg_lib_fsm.c`) — watchdog đẩy module RUNNING/STARTING/WARNING → OFFLINE khi mất tin — chạy **vô điều kiện**, không quan tâm `should_run`. Một module người vận hành đã chủ động STOP (nên `should_run=false`) vẫn tiếp tục bị watchdog này "chấm điểm" theo tình trạng comms mà nó không còn được yêu cầu phải có.
2. `xxx_stop()` của cả 3 driver (Maxwell/Lianming/TonHe) chỉ xử lý state RUNNING/STARTING (gửi frame STOP thật + chuyển STOPPING) — state WARNING/OFFLINE/RECOVERING bị bỏ qua hoàn toàn. Kết hợp với early-return sẵn có của watchdog cho chính OFFLINE/RECOVERING (dòng 78-80 `chg_lib_fsm.c`), một khi module đã rơi vào 1 trong 3 state đó, **không có đường nào đưa nó ra** nữa nếu comms không tự phục hồi.

**Xác nhận nghiệp vụ với người dùng** (quyết định thiết kế, không phải suy đoán): "chúng ta sẽ chỉ connect với module sạc khi trong chu kì sạc... ấn stop thì đâu cần liên tục hỏi nó hay recovering đâu, khác trường hợp mất kết nối module thì mới recovering, còn đây là người dùng chủ động mà, vậy có nghĩa là ấn stop hay emergency phải quay về idle mới hợp logic." → RECOVERING/OFFLINE/WARNING chỉ nên đại diện cho mất-comms **ngoài ý muốn giữa chu kỳ sạc**, không phải "chưa ai hỏi thăm nó kể từ khi STOP". STOP/EMERGENCY_STOP là hành động chủ động, kết quả phải đọc ra IDLE ngay, không phải một comms-health state chờ reconnect (có thể không bao giờ xảy ra).

**Fix** (`Modules/chg_lib/chg_lib_fsm.c`, `chg_lib_tonhe.c`, `chg_lib_maxwell.c`, `chg_lib_lianming.c`):
- `CHG_LIB_FSM_CheckOfflineTimeout()`: thêm gate `if (!should_run) return;` — watchdog chỉ chạy khi module thực sự đang được yêu cầu hoạt động.
- `xxx_stop()` mỗi driver: thêm nhánh `else if (state == WARNING||OFFLINE||RECOVERING) set_state(IDLE)` — STOP ép các state comms-health này về IDLE ngay lập tức. **FAULT cố ý không nằm trong nhánh này** — vẫn giữ debounce 5-lần-đọc-sạch hiện có (B-08); một FAULT phần cứng thật không nên bị STOP xoá đi dễ dàng.
- `check_offline_timeout()` mỗi driver: thêm dòng refresh `view.online` vô điều kiện (độc lập với gate `should_run` ở trên) — module đã dừng vẫn phải đọc `online` trung thực theo `last_rx_tick` cho Monitor UI, chỉ là không bị watchdog kéo qua WARNING/OFFLINE/RECOVERING để có được giá trị đó.

**Test**: `test/host_charge_sim/test_charge_e2e.c::test_stop_forces_idle_from_recovering` — mô phỏng module RUNNING → im lặng 16s → xác nhận RECOVERING → gọi `CHG_LIB_Stop()` → xác nhận IDLE ngay, `running=false` → im lặng tiếp 16s nữa → xác nhận **vẫn** IDLE (không bị kéo lại OFFLINE) và `online=false` (đọc trung thực). Xác nhận bracket rõ ràng: build lại với `chg_lib_fsm.c`/`chg_lib_tonhe.c`/`chg_lib_maxwell.c`/`chg_lib_lianming.c` ở trạng thái HEAD (trước fix) → test FAIL đúng dòng assert "module must go straight to IDLE on STOP, not stay stuck in RECOVERING"; build lại với fix → PASS toàn bộ suite. Verification loop đầy đủ (per-file `-fsyntax-only`, `check_architecture.py`, `check_ioc.py`, `test_logic.c`, Release build) đều sạch.

**Lưu ý HIL riêng biệt** (không phải firmware bug, chỉ ghi lại tránh nhầm lẫn khi debug sau này): trong lúc tái hiện bug này, có một lần tưởng module vẫn RECOVERING dù simulator "đang chạy" — hoá ra là do ZLG USBCAN adapter's `VCI_Transmit()` âm thầm fail sau ~20-30 phút chạy liên tục (không throw exception, chỉ counter `uplink_tx_ok` đứng yên trong khi `uplink_tx_attempts` vẫn tăng) — restart lại process simulator là đủ, không phải lỗi code.

## 8d. B-24 cập nhật + bug thật thứ 2 đã xác nhận + fix: `BMS_SendCtrlInfo()` không có caller (2026-08-29)

Theo dõi tiếp B-24 (§8c phía trên là mục khác, B-24 nằm trong §5.2): người dùng test HIL tiếp, lần này báo module **đã lên áp >90% target rồi** mà relay vẫn không đóng — loại trừ ngay giả thuyết "module không ramp được áp không tải" của B-24 cho đúng case này, dồn nghi vấn sang điều kiện độc lập thứ 2 trong `update_relay_decision()`: `BMS_ShouldCloseChargeRelay()`.

**Root cause xác nhận 100% từ code** (không cần đo hardware để chứng minh, khác B-24): `BMS_ShouldCloseChargeRelay()` (`bms_core.c:510-531`) đòi hỏi BMS tự báo relay nội bộ của **chính nó** đã đóng (`charge_relay_closed`, lấy từ bit `charge_sta` trong frame `BmsSwSta`). Theo FR-BMS-06 (Mandatory), muốn BMS đóng relay nội bộ, mình phải gửi `Ctrl_INFO` mỗi 500ms báo `chg_sw=1`. Frame này CÓ gửi đúng chu kỳ (`transmit_ctrl_info()` chạy trong `BMS_Process()`), nhưng nội dung đọc từ `g_charge_ctrl.allow_charge` — biến này chỉ được set bởi `BMS_SendCtrlInfo()`, và grep toàn repo xác nhận **hàm này không có bất kỳ caller nào** (kể cả trong `charge_controller.c`). `g_charge_ctrl` zero-init lúc `BMS_Init()` và không đổi bao giờ → `chg_sw` gửi đi luôn luôn = 0, kể cả khi đang RUNNING thật. Nếu BMS thật gate relay nội bộ theo tín hiệu này (rất khả năng, đúng thiết kế chuẩn BMS) → `charge_relay_closed` mãi = false → relay MCU không bao giờ đóng, độc lập hoàn toàn với điện áp module.

**Fix** (`App/Charge/charge_controller.c`): thêm `update_bms_charge_allow()`, gọi mỗi tick cạnh `update_relay_decision()` (cuối `ChargeController_Process()`). Người dùng xác nhận hướng A: `allow_charge=true` **chỉ khi** `state==RUNNING` (không phải READY/STOPPING/FAULT) — an toàn nhất, đúng nghĩa "đang trong chu kỳ sạc thật". Edge-triggered (chỉ gọi `BMS_SendCtrlInfo()` khi giá trị mong muốn đổi, qua field mới `g_ctrl.bms_charge_allow_sent`) để không phá nhịp 500ms sẵn có của `Ctrl_INFO` — cùng lý do đã áp dụng khi fix BUG-05 (spam CAN) cho `apply_charge_targets()`.

**Test**: thêm spy nhẹ vào `test/mock_hal/mock_stubs.c` (`MockCan_GetLastTx()`, bảng 4 slot theo ext_id) để test có thể assert **nội dung** frame CAN đã gửi, không chỉ đếm số lần gửi — trước đây `BSP_CAN_Transmit()` mock là no-op hoàn toàn, không có cách nào quan sát được bug này qua test tự động. `test_bms_ctrl_info_allow_charge_wired` (`test_charge_e2e.c`): xác nhận `chg_sw=0` suốt warmup (chưa RUNNING) → RUNNING → `chg_sw=1` (`MaskCode` bit0 cũng set) → `Stop()` → về IDLE → `chg_sw=0` trở lại. Bracket rõ ràng: build với `charge_controller.c` ở HEAD (trước fix) → FAIL đúng dòng assert "chg_sw must be 1 while RUNNING"; build với fix → PASS toàn bộ suite (kể cả 22 test khác không bị ảnh hưởng, vì `set_healthy_bms()`/simulator tự set `bms_relay_allow=true` trực tiếp trong control state, không phụ thuộc nội dung `Ctrl_INFO` mình gửi — nên bug này trước giờ vô hình với mọi test host-sim hiện có, chỉ lộ ra khi BMS thật gate theo tín hiệu đó). Verification loop đầy đủ (`-fsyntax-only` cả `charge_controller.c` lẫn `mock_stubs.c`, `check_architecture.py`, `check_ioc.py`, `test_logic.c`, Release build) đều sạch.

## 8e. B-24 fix: ngưỡng đóng relay đổi từ so với target sang so với điện áp pin thật từ BMS (2026-08-29)

Fix cho B-24 (§5.2) — người dùng xác nhận trực tiếp công thức trước khi sửa:

```
Cũ : min_voltage(module) < target_voltage_v  × 90%  →  chưa đóng relay
Mới: min_voltage(module) < BmsView.batt_voltage × 90% →  chưa đóng relay   (chỉ áp dụng mode BMS-Controlled)
```

**`App/Charge/charge_controller.c`** (`update_relay_decision()`): thêm biến `voltage_ref`, mặc định = `target_voltage_v` (giữ nguyên hành vi cho Standalone/No-BMS mode — không có BMS để tham chiếu). Ở mode `CHARGE_SOURCE_BMS_CONTROLLED`, đọc `BMS_GetView()` và dùng `batt_voltage` làm `voltage_ref` nếu > 0 (an toàn vì `BMS_ShouldCloseChargeRelay()` đã yêu cầu BMS online + có data mới trước đó trong cùng hàm). So sánh `min_voltage(module) < voltage_ref × 90%` như cũ, chỉ đổi vế tham chiếu.

**Test** (`test/host_charge_sim/test_charge_e2e.c`):
- `test_relay_bms_mode` cập nhật lại: đổi mốc so sánh từ `target` (500V) sang `bms_ref` (400V, từ `set_healthy_bms(400.0f,...)`) cho cả 3 mốc điện áp test (80%/95%/70%).
- `test_relay_arms_off_bms_voltage_not_target` (mới) — chứng minh trực tiếp B-24 đã hết: pin sâu (BMS báo 300V), target vẫn 500V (config cố định, không đổi theo pack — đã xác nhận TBD-04 trước đó), module chỉ lên tới 280V (≥90% của 300V nhưng cách rất xa 90% của 500V=450V) → relay vẫn phải đóng. Dưới logic cũ test này chắc chắn FAIL (không bao giờ đóng); dưới logic mới PASS.
- Bracket đầy đủ: build với `charge_controller.c` ở trạng thái trước cả 2 fix trong phiên này (`BMS_SendCtrlInfo` §8d + B-24) → cả 3 test (`test_relay_bms_mode`, `test_relay_arms_off_bms_voltage_not_target`, `test_bms_ctrl_info_allow_charge_wired`) đều FAIL đúng dòng assert tương ứng; build với fix đầy đủ → PASS toàn bộ 24/24 test suite.

Verification loop đầy đủ (`-fsyntax-only`, `check_architecture.py`, `check_ioc.py`, `test_logic.c`, Release build RAM 13.95%/FLASH 53.21%) đều sạch.

## 8f. B-24 vẫn không đóng sau fix §8e -- bỏ hẳn gate `charge_relay_closed` (2026-08-29)

Sau fix §8e (ngưỡng 90% so với `BmsView.batt_voltage`), người dùng test lại trên hardware thật, xác nhận qua ảnh chụp Monitor tab thật: điện áp module (52.80V) đã vượt 90% điện áp pack BMS báo (53.20V, ~99.2%) — điều kiện điện áp **đã thỏa** — nhưng relay MCU vẫn không đóng. Field **Charge Relay** trên panel BMS Overview hiện **Open**, đứng yên kể cả để chạy lâu.

**Root cause**: `BMS_ShouldCloseChargeRelay()` (`bms_core.c`) còn 1 điều kiện thứ 3 chưa đụng tới: đòi `snap.charge_relay_closed` (bit `charge_sta` BMS tự báo qua `BmsSwSta`) phải = true. Dù đã fix `BMS_SendCtrlInfo()` (§8d) gửi đúng `chg_sw=1` suốt lúc RUNNING, bit này trên BMS thật **không bao giờ tự đóng** — xác nhận: **mình không thực sự điều khiển được relay nội bộ của BMS trong triển khai thực tế này**, nó tự quyết theo logic riêng, không theo lệnh `chg_sw`.

**Phát hiện thêm khi rà lại mô phỏng theo yêu cầu người dùng** ("hôm qua mô phỏng có vẻ chúng ta đã điều khiển relay bms, check lại"): cả 2 nơi mô phỏng trước giờ đều **hardcode** field này thành `true` mặc định, hoàn toàn độc lập với `chg_sw` firmware gửi đi:
- `test/host_charge_sim/sim_bms.c:26`: `b->bms_relay_allow = true;` (mặc định "BMS khỏe mạnh", chỉ đổi khi 1 test cụ thể tự set `false` để test path lỗi).
- `live_test_sim.py`/`integration_sync_test.py`'s `BmsSim.send_bms_sw_sta()`: đọc từ `control.json`'s `bms_relay_allow`, cũng là field cố định set tay.

→ Không có test/mô phỏng nào trước giờ THẬT SỰ kiểm chứng "gửi `chg_sw=1` có khiến bit `charge_sta` bật lên hay không" — luôn pass vì giả lập "dễ tính", tạo cảm giác sai là loop này đã đóng kín. Hardware thật lần này mới lộ ra: không hề đóng kín.

**Fix, xác nhận trực tiếp với người dùng** ("chúng ta sẽ điều khiển relay trên mạch mà k cần liên quan đến relay bms nữa"): bỏ hẳn điều kiện `charge_relay_closed` khỏi `BMS_ShouldCloseChargeRelay()`, chỉ còn giữ `BMS_IsOnline()` và `BMS_HasCriticalAlarm()`. Field `charge_relay_closed` vẫn giữ nguyên trong `BMS_View_t` (chỉ để hiển thị/theo dõi trên Monitor tab của `debug_app`), không còn dùng để gate quyết định đóng relay của MCU nữa.

**File sửa**: `Modules/bms/bms_core.c` (`BMS_ShouldCloseChargeRelay()`), `App/Charge/charge_controller.c` (cập nhật doc-comment `update_relay_decision()`), `test/host_charge_sim/test_charge_e2e.c` (`test_relay_bms_mode`'s đoạn test cuối đổi từ "relay phải mở khi `bms_relay_allow=false`" sang "relay phải KHÔNG bị ảnh hưởng bởi field này nữa").

**Test**: Bracket rõ ràng — build với `bms_core.c` ở trạng thái trước fix (commit `ccc8bc9`) → `test_relay_bms_mode` FAIL đúng dòng assert mới; build với fix → PASS toàn bộ 24/24. Verification loop đầy đủ (`-fsyntax-only` cả `bms_core.c` lẫn `charge_controller.c`, `check_architecture.py`, `check_ioc.py`, `test_logic.c`, Release build RAM 13.95%/FLASH 53.20%) đều sạch.

**Bài học quy trình**: cả host-sim lẫn live HIL simulator cần một scenario riêng test đúng "BMS chỉ đóng relay nội bộ SAU khi thấy `chg_sw=1`" (thay vì hardcode `true`) nếu sau này có nhu cầu verify lại loop tương tự cho tín hiệu CAN khác — ghi chú lại đây để không lặp lại lỗ hổng kiểm thử tương tự.

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

