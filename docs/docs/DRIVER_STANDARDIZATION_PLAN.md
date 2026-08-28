# Plan: Chuẩn Hóa 3 Driver Sạc Thành Thư Viện

**Ngày:** 2026-06-29
**Trạng thái:** Draft — chờ user phê duyệt
**Priority:** Phase 1 → Phase 2 → Phase 3 → Phase 4

---

## Tổng Quan

Dựa trên senior code review + verify chi tiết (simulation, protocol doc, ground-truth testing),
3 driver (Maxwell, Lianming, TonHe) đã hoạt động đúng trên hardware thực.
Các vấn đề hiện tại là **code quality / structure**, không phải bug runtime.

---

## Các Vấn Đề Đã Xác Nhận (Confirmed)

### Phase 1 — Cần làm (Low risk, high value)

| # | Vấn đề | File | Mô tả |
|---|---------|------|--------|
| P1-1 | Mock code trong production | `bsp_can.c:103-193` | `Mock_CAN2_Process_RX` + 5 mock vars — dead code, ~90 dòng, không có call site nào |
| P1-2 | `BSP_CAN2_Start` trong `bsp_can.c` | `bsp_can.c:72-101` | CAN2 thuộc BMS, không thuộc bsp_can (charger CAN1) |
| P1-3 | Naming không thống nhất | Cả 3 driver | Maxwell/TonHe: `g_current_idx`, Lianming: `g_rr_index` |
| P1-4 | Switch thiếu `default:` | Cả 3 driver | Vi phạm MISRA R14.1, có `-Wswitch-enum` trong flags |
| P1-5 | Magic numbers còn sót | driver_maxwell.c, driver_tonhe.c | `3`, `50`, `100`, `1000` hardcoded |
| P1-6 | TonHe `set_state()` không đầy đủ | driver_tonhe.c:379 | Maxwell/Lianming set `online+running+start_attempts+state_enter_tick`, TonHe chỉ set 2 trường |
| P1-7 | TonHe direct `mod->view.state = ` | driver_tonhe.c:268,303,422,436 | Bypass helper `set_state()`, 4 chỗ |
| P1-8 | TonHe `set_state()` unused param | driver_tonhe.c:379 | `uint32_t now` được pass nhưng không dùng |
| P1-9 | Lianming unused param | driver_lianming.c:627 | `uint8_t dlc` trong `lm_process_rx` |
| P1-10 | Test exe lỗi thời | test/test_drivers.exe | Build 10 tiếng trước driver code, không reflect source mới |

### Phase 2 — Nên làm (Medium risk, cho thư viện)

| # | Vấn đề | File | Mô tả |
|---|---------|------|--------|
| P2-1 | Hard-couple `extern HAL_GetTick` | 7+ nơi | Mỗi driver extern riêng, ảnh hưởng testability |
| P2-2 | Hard-couple `#include "bsp_can.h"` | Cả 3 driver | Không thể swap CAN backend (SocketCAN, mock) |
| P2-3 | `find_by_addr()` lặp O(n) | Cả 3 driver | 3 copy cùng logic, với max 8 modules thì OK nhưng không extensible |
| P2-4 | TonHe byte offset comment sai | driver_tonhe.c:246-252 | Comment nói "Byte 2-3" nhưng C array 0-indexed, nên là "Byte 1-2" |

### Phase 3 — Optional (High effort, cho reusability)

| # | Vấn đề | File | Mô tả |
|---|---------|------|--------|
| P3-1 | CAN Backend abstraction | driver_*.c | Inject `CHG_CanBackend_t` để drivers không phụ thuộc BSP |
| P3-2 | Round-robin helper chung | driver_*.c | Trích `g_rr_index` thành helper `chg_process_next()` |

### Phase 4 — Test

| # | Vấn đề | File | Mô tả |
|---|---------|------|--------|
| P4-1 | Test `set_current_limit` TX encoding | test_drivers.c | 4 calls `set_current_limit` nhưng không verify frame content |
| P4-2 | Test `rx_count` behavior | driver_lianming.c:454 | `rx_count++` trong `process_module` CHG_STATE_RUNNING thay vì `apply_status` |

### Cần Clarify (trước khi sửa)

| # | Vấn đề | File | Mô tả |
|---|---------|------|--------|
| C-1 | Maxwell `rated_current` | driver_maxwell.c:482 | Dùng `view.current` (output hiện tại) làm rated. Spec nói READ 0x0003 trả Amps, WRITE dùng ratio. Cần hardware confirm. |
| C-2 | `PC_CMD_SET_DRIVER` reset | pc_protocol.c:213 | Gọi `CHG_Init()` sau `SelectDriver` — design intent hay bug? |

---

## Chi Tiết Từng Bước

---

### PHASE 1: Tách Mock + Cơ Sở Hạ Tầng

**Time estimate:** 2-3h
**Risk:** Low
**Test:** Build firmware + chạy hardware

#### P1-1: Tách Mock_CAN2_Process_RX

**Tạo file mới:**
```
charger/App/Src/mock_maxwell_can.c
```
Di chuyển toàn bộ `Mock_CAN2_Process_RX` + 5 mock state vars (`mock_voltage`, `mock_current`, `mock_limit`, `mock_alarm`, `mock_temp`) vào file này.

**Thêm build guard:**
```c
#ifdef ENABLE_MAXWELL_MOCK
/* mock code here */
#endif
```
Hoặc tốt hơn: đơn giản là xóa hết. Nếu cần lại trong tương lai, lấy từ git history.

**Từ bsp_can.c:** Xóa dòng 103-193 (static mock vars + `Mock_CAN2_Process_RX` body).
**Từ bsp_can.h:** Xóa declaration `void Mock_CAN2_Process_RX(...)`.

#### P1-2: Tách BSP_CAN2_Start ra BMS

**Tạo file:**
```
charger/App/Src/bms_can.c
charger/App/Inc/bms_can.h
```

Di chuyển `BSP_CAN2_Start()` vào đây. Đổi tên thành `BMS_CAN_Init()` để rõ ràng hơn.

**Update:**
- `app_charger.c`: `#include "bms_can.h"` thay vì `bsp_can.h` cho CAN2
- `bsp_can.c`: Xóa toàn bộ section CAN2 MOCK (đã xử lý ở P1-1)
- `bsp_can.h`: Xóa `BSP_CAN2_Start` và `Mock_CAN2_Process_RX` declarations

**Sau P1-2, bsp_can.c chỉ còn:**
- `BSP_CAN_Start()` — CAN1 filter + start
- `BSP_CAN_Transmit()` — CAN1 TX
- ~40 dòng thay vì ~193 dòng

#### P1-3: Đặt tên thống nhất `g_rr_index`

| Driver | Hiện tại | Thành |
|--------|-----------|-------|
| driver_maxwell.c | `g_current_idx` | `g_rr_index` |
| driver_tonhe.c | `g_current_idx` | `g_rr_index` |
| driver_lianming.c | `g_rr_index` | `g_rr_index` (giữ nguyên) |

#### P1-4: Thêm `default:` vào switch state

Cả 3 driver đều có `switch(mod->view.state)` cho FSM nhưng không có `default:`.

Format chuẩn:
```c
switch (state) {
case CHG_STATE_IDLE:      ...; break;
case CHG_STATE_STARTING:  ...; break;
case CHG_STATE_RUNNING:   ...; break;
case CHG_STATE_OFFLINE:   ...; break;
case CHG_STATE_FAULT:     ...; break;
case CHG_STATE_RECOVERING: ...; break;
default: /* Should not happen */ break;
}
```

#### P1-5: Magic numbers → #define

**driver_maxwell.c:**
```c
// Hiện tại:
if (m->start_attempts > 3U)       // → #define MXR_START_MAX_ATTEMPTS  3U
if (m->retry_count == 1 && ... >= 50)  // → #define MXR_START_VOLTAGE_DELAY_MS  50U
if (m->retry_count == 2 && ... >= 50)   // → #define MXR_START_CURRENT_DELAY_MS  50U
if (m->retry_count >= 3 && ... >= 100)  // → #define MXR_START_CONFIRM_WAIT_MS  100U
```

**driver_tonhe.c:**
```c
// Hiện tại:
if ((now - mod->last_tx_tick) >= 1000) // → #define TONHE_HEARTBEAT_INTERVAL_MS  1000U
```

#### P1-6: TonHe `set_state()` đầy đủ

TonHe `set_state()` hiện chỉ set 2 trường. Sửa thành:
```c
static void set_state(TONHE_Internal_t *mod, CHG_ModuleState_t st, uint32_t now)
{
    if (mod->view.state != st) {
        mod->view.state = st;
        mod->retry_count = 0;
        // Thêm từ Maxwell/Lianming pattern:
        switch (st) {
        case CHG_STATE_IDLE:
        case CHG_STATE_FAULT:
        case CHG_STATE_OFFLINE:
        case CHG_STATE_RECOVERING:
            mod->view.online = false;
            mod->view.running = false;
            break;
        case CHG_STATE_STARTING:
        case CHG_STATE_RUNNING:
            mod->view.online = true;
            mod->view.running = (st == CHG_STATE_RUNNING);
            break;
        }
    }
    // (uint32_t now) param được dùng nếu cần track state_enter_tick
    (void)now;
}
```

#### P1-7: Bỏ direct `mod->view.state = ` ở TonHe

Có 4 chỗ set trực tiếp:
- driver_tonhe.c:268 — `mod->view.state = CHG_STATE_FAULT`
- driver_tonhe.c:303 — `mod->view.state = CHG_STATE_RUNNING`
- driver_tonhe.c:422 — `mod->view.state = CHG_STATE_RUNNING`
- driver_tonhe.c:436 — `mod->view.state = CHG_STATE_IDLE`

Tất cả 4 chỗ thay bằng `set_state(mod, STATE, now)`.

#### P1-8: TonHe `set_state()` unused param `now`

Đã xử lý trong P1-6 (`(void)now;`).

#### P1-9: Lianming unused param `dlc`

```c
static void lm_process_rx(uint32_t ext_id, const uint8_t *data, uint8_t dlc, uint32_t now)
```
Thêm `(void)dlc;` hoặc xóa nếu thực sự không dùng.

#### P1-10: Rebuild test_drivers.exe

Build với source mới nhất (driver_tonhe.c modified sau test exe). Rebuild:
```bash
gcc -I. -Icharger/App/Inc -Icharger/Core/Inc -ICore/Inc \
  -DSTM32F407xx -DENABLE_MAXWELL_MOCK=0 \
  test/test_drivers.c \
  charger/App/Src/driver_maxwell.c \
  charger/App/Src/driver_lianming.c \
  charger/App/Src/driver_tonhe.c \
  charger/App/Src/charger_core.c \
  charger/App/Src/bsp_can.c \
  charger/App/Src/debug_log.c \
  -o test/test_drivers.exe \
  -W -Wall -Wextra -Werror -Wno-unused-variable
```

Lưu ý: build standalone test cần mock thêm STM32 HAL headers. Xem lại cách project hiện tại build test.

---

### PHASE 2: Chuẩn Hóa FSM + State API

**Time estimate:** 3-4h
**Risk:** Medium (thay đổi logic FSM, cần test kỹ)
**Test:** Unit test + hardware

#### P2-1: Inject `now_tick()` helper

Tạo 1 helper duy nhất:
```c
// charger/App/Src/charger_time.c
static uint32_t g_sys_tick = 0;
uint32_t CHG_GetTick(void) {
    extern uint32_t HAL_GetTick(void);  // production
    return HAL_GetTick();
}
// Hoặc test override:
void CHG_SetTick(uint32_t t) { g_sys_tick = t; }
uint32_t CHG_GetTick(void) { return g_sys_tick; }
```

Mỗi driver gọi `CHG_GetTick()` thay vì extern `HAL_GetTick()`.

#### P2-2: Inject CAN backend

Tạo abstraction layer:
```c
// charger/App/Inc/chg_can_backend.h
typedef struct {
    bool (*transmit)(uint32_t ext_id, const uint8_t *data, uint8_t dlc);
    void (*on_frame)(uint32_t ext_id, const uint8_t *data, uint8_t dlc, uint32_t now);
} CHG_CanBackend_t;

void CHG_SetBackend(const CHG_CanBackend_t *backend);
```

**Production:** `bsp_can.c` cung cấp backend gọi `BSP_CAN_Transmit()`.
**Test:** Test suite cung cấp mock backend ghi nhận TX frames.

**Thay đổi driver:**
- Xóa `#include "bsp_can.h"`
- Gọi `CHG_CanTransmit(ext_id, data, dlc)` thay vì `BSP_CAN_Transmit()`

**Thay đổi app_charger.c:**
- Gọi `CHG_SetBackend(&g_bsp_backend)` sau `BSP_CAN_Start()`

#### P2-3: `find_by_addr()` helper chung

Trích thành helper trong `charger_core.c`:
```c
static int8_t chg_find_module_by_addr(uint8_t addr, const CHG_ModuleView_t *views, uint8_t count);
```
3 driver gọi helper này thay vì copy logic.

#### P2-4: Fix TonHe byte offset comments

Comment trong code nói "Byte 2-3" nhưng C array 0-indexed → sửa thành "Byte 1-2" để khớp C convention:
```c
/* Byte 1-2: Output voltage (0.1V/bit, LE) */  // was "Byte 2-3"
/* Byte 3-4: Output current (0.01A/bit, LE) */ // was "Byte 4-5"
```

---

### PHASE 3: CAN Backend Abstraction (Tùy chọn)

**Time estimate:** 4-5h
**Risk:** Medium-High
**Test:** Unit test + integration test

#### P3-1: CAN Backend như P2-2 đã mô tả

#### P3-2: Round-robin helper chung

Trích logic round-robin:
```c
// charger_core.c
static uint8_t g_rr_index = 0;
void CHG_RoundRobinAdvance(void) {
    g_rr_index = (g_rr_index + 1) % CHG_GetModuleCount();
}
```

---

### PHASE 4: Chuẩn Hóa Bộ Test

**Time estimate:** 2h
**Risk:** Low
**Test:** Chạy test mới

#### P4-1: Thêm test `set_current_limit` TX encoding

Hiện tại test chỉ gọi `set_current_limit` mà không verify frame content.
Thêm:
```c
bool test_maxwell_current_limit_encoding() {
    ops->start(idx);
    ops->process(g_tick); ops->process(50); ops->process(100);
    ops->feed_frame(/* running response */);
    ops->process(200);
    ops->set_current_limit(idx, 10.0f);
    // Verify TX frame contains IEEE 754 Float
    ASSERT(g_tx_frames[N].data[2] == 0x00);  // reg MSB
    ASSERT(g_tx_frames[N].data[3] == 0x22);   // reg LSB = 0x0022
    // Verify float value encoding
}
```

#### P4-2: Test `rx_count` behavior

Verify `rx_count` chỉ tăng khi có frame parse thành công:
```c
bool test_lianming_rx_count_only_on_valid_frame() {
    // Feed frame in IDLE state -> should NOT increment rx_count
    // Feed frame in RUNNING state -> should increment rx_count
}
```

---

## Thứ Tự Thực Hiện Đề Xuất

```
1. P1-1 + P1-2 (Mock + CAN2 tách)  ──► Tách xong, build verify
2. P1-3 (Naming)                   ──► Nhanh, 10 phút
3. P1-4 (default:)                 ──► Nhanh, 15 phút
4. P1-5 (Magic numbers)            ──► Nhanh, 20 phút
5. P1-6 + P1-7 (TonHe set_state)  ──► Cẩn thận FSM logic
6. P1-8 + P1-9 (unused params)    ──► Nhanh, 5 phút
7. P1-10 (Rebuild test)            ──► Verify không break gì
8. P2-1 (Inject now_tick)         ──► Backend
9. P2-2 (Inject CAN backend)       ──► Refactor lớn, cần test kỹ
10. P2-3 (find_by_addr helper)    ──► Optional
11. P2-4 (Fix comments)            ──► Optional
12. P4-1 + P4-2 (Test coverage)   ──► Sau khi code ổn định
```

---

## Files Thay Đổi

### Xóa hoặc tách

| File | Hành động | Lý do |
|------|-----------|--------|
| `charger/App/Src/bsp_can.c` | Sửa | Xóa Mock, xóa CAN2 |
| `charger/App/Inc/bsp_can.h` | Sửa | Xóa declarations |
| *(new)* `charger/App/Src/bms_can.c` | Tạo mới | CAN2 cho BMS |
| *(new)* `charger/App/Inc/bms_can.h` | Tạo mới | CAN2 cho BMS |

### Tạo mới

| File | Nội dung |
|------|----------|
| `charger/App/Inc/chg_can_backend.h` | CAN backend interface |
| `charger/App/Src/chg_can_backend.c` | Backend registration + default implementation |
| `charger/App/Src/chg_time.c` | `now_tick()` helper |

### Sửa

| File | Thay đổi chính |
|------|----------------|
| `charger/App/Src/driver_maxwell.c` | P1-3, P1-4, P1-5, P2-1, P2-2, P2-3 |
| `charger/App/Src/driver_lianming.c` | P1-3, P1-4, P1-5, P1-9, P2-1, P2-2, P2-3 |
| `charger/App/Src/driver_tonhe.c` | P1-3, P1-4, P1-5, P1-6, P1-7, P1-8, P2-1, P2-2, P2-3, P2-4 |
| `charger/App/Src/app_charger.c` | P1-2, P2-2 |
| `charger/App/Src/charger_core.c` | P2-2, P2-3 |
| `test/test_drivers.c` | P1-10, P4-1, P4-2 |

---

## Cần Confirm Trước Khi Bắt Đầu

### C-1: Maxwell `set_current_limit` behavior

**Câu hỏi:** Khi PC app gọi `CHG_SetCurrentLimit(module, 10.0f)` (10 Amps):
- Module Maxwell thực sự output 10A không?
- Hay bạn set ratio (0.0–1.0) và module dùng default rated?

**Lý do:** Maxwell protocol spec (Maxwell.md) nói:
- READ register 0x0003 → returns Amps
- WRITE register 0x0003 → uses ratio (0.0–1.0)

Code hiện tại gửi ratio = `current_a / rated_current` (dùng `view.current` làm rated heuristic).

**Action:** Nếu hardware đúng với 10A output → giữ nguyên. Nếu không → cần xác định rated đúng cách (config, hardcode, hoặc detect).

### C-2: `PC_CMD_SET_DRIVER` intent

**Câu hỏi:** Khi PC gửi lệnh đổi driver (Maxwell → Lianming):
- Có nên reset toàn bộ state không? (module list, setpoints mất)
- Hay chỉ switch protocol handler, giữ modules?

**Action:** Xác nhận intent để document hoặc sửa.

---

## Review Checklist (Sau Mỗi Phase)

- [ ] Build không warning với `-Werror`
- [ ] Unit tests pass
- [ ] Hardware test smoke test (module start/stop)
- [ ] No functional regression so với trước khi refactor
