# Addendum — ChgRequest_INFO (0x1806E5F4) is big-endian

**Why this file exists:** `docs/CAN BMS_BB_PKG V1.0.pdf` §3 (Physical interface)
states a blanket rule — *"unless otherwise specified, the low byte will come
first and the high byte will come last (little endian)"* — and its own §5.7
table for ChgRequest_INFO does not call out an exception. Taken at face
value, that PDF alone implies ChgRequest_INFO should be little-endian, which
contradicts what `Modules/bms/bms_protocol.c`'s `parse_chg_request()`
actually implements (`get_u16_be()`). This *looked* like a real firmware bug
during review on 2026-08-28 (see the reclassification note this addendum
backs, in `docs/SRS_Charger_Controller.md` and `docs/AUDIT_Findings.md`).

The user then supplied the actual authoritative source for this specific
frame — a different/newer document (title fragment: *"极空 BMS-CAN 协议
V2.0"*, publisher 成都极空科技有限公司 / Chengdu Jikong Technology Co.,
Ltd — only this one section was available, not the full PDF), §7.2
"充电请求（BMSChgINFO）ID：0x1806E5F4". Transcribed in full below since only
a screenshot of this section exists, not a stored file.

## §7.2 充电请求（BMSChgINFO）ID：0x1806E5F4

充电信息为事件触发式发送，当充电器插入或接收到充电机的报文（0x18FF50E5）时 BMS
以 500ms 的周期发送充电信息，当充电器没有插入时则不发送。

**注：本帧通信过程中数据采用大端，格式如下：**
(*"Note: data in this frame's communication uses big-endian, format as
follows:"* — an explicit, frame-specific override of whatever the general
document's default might be, unlike `CAN BMS_BB_PKG V1.0.pdf` which has no
such override noted for this frame.)

| 序号 (No) | 参数 (Param) | 起始位 (start bit) | 位长度 (bit len) | 范围 (range) | 分辨率 (resolution) | 偏移量 (offset) | 单位 (unit) | 备注 (remark) |
|---|---|---|---|---|---|---|---|---|
| 1 | ChgVol | 0 | 16 | 0~2000 | 0.1 | 0 | V | 充电电压 (charging voltage) |
| 2 | ChgCur | 16 | 16 | 0~2000 | 0.1 | 0 | A | 充电电流 (charging current) |
| 3 | ChgDevSw | 32 | 8 | 0~1 | — | — | — | 充电器开关，0：开启 1：关闭 (charger switch, 0=on 1=off) |
| 4 | ChgAndHeat | 40 | 8 | 0~1 | — | — | — | 充电加热模式，0：充电 1：加热 (charge/heat mode, 0=charging 1=heating) |

**Worked example from the source document (confirms big-endian):**
```
0x1806E5F4  03 48 00 C8 00 00 XX XX
03 48 → 充电电压 84V   (bytes [0..2) as big-endian u16: 0x0348 = 840, ×0.1 = 84.0V)
00 C8 → 充电电流 20A   (bytes [2..4) as big-endian u16: 0x00C8 = 200,  ×0.1 = 20.0A)
00     → 充电器开启 (charger on)
00     → 充电模式   (charging mode)
```
Reading the same bytes as little-endian gives `0x4803` = 1843.5V — inconsistent
with the document's own stated "84V" example. Big-endian is the only reading
consistent with the source's worked example, confirming
`Modules/bms/bms_protocol.c`'s existing `get_u16_be()` implementation is
correct, not a bug.

## Scope of this exception

Only ChgRequest_INFO (0x1806E5F4) is confirmed big-endian. Every other frame
this firmware parses (BATT_ST1, CELL_VOLT, CELL_TEMP, ALM_INFO, BATT_ST2,
BmsSwSta, CELL_VOLT_FULL, CELL_TEMP_FULL) uses `get_u16_le()`/`get_u16_be()`
per `docs/CAN BMS_BB_PKG V1.0.pdf`'s default little-endian rule (no
per-frame exception documented for those) — verify against that PDF, not
this addendum, for anything other than ChgRequest_INFO.

## Known gap

Only this one section of the "极空/Jikong BMS-CAN 协议 V2.0" document has
been seen (a screenshot, not a file) — the rest of that document (if it
covers the other 8 frame types too) has not been cross-checked against
`CAN BMS_BB_PKG V1.0.pdf`. If a fuller copy becomes available, re-verify
every frame against it, not just this one.
