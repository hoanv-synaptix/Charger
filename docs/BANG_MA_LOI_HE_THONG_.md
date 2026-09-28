# BẢNG DANH MỤC MÃ LỖI VÀ CẢNH BÁO HỆ THỐNG SẠC PIN (BATTERY CHARGER SYSTEM FAULT SPECIFICATION)

## BẢNG TRA CỨU MÃ LỖI HỆ THỐNG CHI TIẾT

### Quy ước severity Pin (BMS)

Trong `ALM_INFO`: severity `0` là không cảnh báo; mọi severity `1..3` đều là trạng thái lỗi cần hành động trong firmware. Firmware đưa severity `>= 1` vào `alarm_flags`; `warning_flags` chỉ được giữ để tương thích telemetry PC và không được dùng để quyết định an toàn. Action cụ thể của từng mã do bảng Alarm và controller sở hữu quyết định.

### Quy ước nguồn gốc alarm

- **CAN**: Khối pin/bộ sạc tự phát hiện, gửi raw field qua CAN; firmware parse và mirror.
- **Connection-derived**: áp dụng cho mất kết nối do timeout (`W010`, `E021`).
- **Controller-derived**: quyết định an toàn từ telemetry CAN hợp lệ (`E022`).
- **Data-status**: `BMS_IsDataStale()` là chất lượng dữ liệu nội bộ, không phải alarm và không tạo DWIN code.
- **DWIN khi IDLE/chưa có CAN**: controller giữ trạng thái `IDLE/READY`, không tạo alarm chỉ vì pin/bộ sạc chưa online. Các trường đo lường chưa có dữ liệu (DC, AC, nhiệt độ và pin) hiển thị dạng Text Display `---`; SOC offline hiển thị `--%`, SOC online hiển thị dạng text, ví dụ `50%`. `0` không được dùng để biểu diễn dữ liệu chưa tồn tại.
- **Màu SOC trên DWIN**: SOC Text Display dùng SP `0x8000`; Text Color nằm tại `0x8003` (offset +3 WORD). RGB565 (chuẩn công nghiệp Dark Mode, không dùng màu neon): unavailable `0x9516` (slate 400), critical `0xF38E` (soft coral red, 0–10%), low `0xFD20` (orange, 11–30%), medium `0xD520` (muted amber, 31–60%), normal `0x262B` (green 500, 61–100%). Màu chỉ phục vụ hiển thị, không tạo alarm.

| Nguồn gốc | Tên lỗi kỹ thuật | Mã lỗi | Hiển thị DWIN | Hành vi | Nguyên nhân | Logic phát hiện trong Code (Không suy diễn) |
| **Pin** | Điện áp pack pin thấp | **E001** | **Điện áp pin thấp** | **Dừng sạc an toàn (STOP)** | Điện áp pack pin nằm trong khoảng $[0.5 \times V_{\max}, V_{\min})$ ở chế độ sạc thường (dưới ngưỡng sạc an toàn $V_{\min}$), HOẶC do BMS gửi cờ cảnh báo điện áp pack thấp. Được bypass ở chế độ PRECHARGE. | Nhận bản tin `ALM_INFO` (`0x07F4`), trường `low_pack_volt` với `severity >= 1`, HOẶC ở chế độ `BMS_CONTROLLED` (ngoài trạng thái PRECHARGE) đo được $0.5 \times V_{\max} \le V_{\text{batt}} < V_{\min}$. |
| **Pin** | Điện áp cell pin thấp | **E002** | **Điện áp cell pin thấp** | **Cảnh báo (INFO)** | Cell pin bị phóng điện quá sâu (thường < 2.5V đối với LFP, < 3.0V đối với NMC). | Nhận bản tin `ALM_INFO` (`0x07F4`), trường `low_cell_volt` với `severity >= 1`. `ev_bms()` kiểm tra `(in->bms.alarm_flags & BMS_ALARM_LOW_CELL_VOLT) != 0`. |
| **Pin** | Quá áp pack pin | **E003** | **Điện áp pin cao** | **Dừng sạc an toàn (STOP)** | Tổng điện áp của bộ pin đo được vượt quá ngưỡng điện áp sạc tối đa do pin quy định. | Nhận bản tin `ALM_INFO` (`0x07F4`), trường `high_pack_volt` với `severity >= 1`. `ev_bms()` kiểm tra `(in->bms.alarm_flags & BMS_ALARM_HIGH_PACK_VOLT) != 0`. |
| **Pin** | Quá áp cell pin | **E004** | **Điện áp cell pin cao** | **Dừng sạc an toàn (STOP)** | Có ít nhất một cell đơn lẻ trong pack đạt ngưỡng điện áp bảo vệ cắt sạc, pin bị mất cân bằng. | Nhận bản tin `ALM_INFO` (`0x07F4`), trường `high_cell_volt` với `severity >= 1`. `ev_bms()` kiểm tra `(in->bms.alarm_flags & BMS_ALARM_HIGH_CELL_VOLT) != 0`. |
| **Pin** | Quá nhiệt khi sạc | **E005** | **Pin quá nóng khi sạc** | **Tự phục hồi tối đa 3 lần, lần 4 Dừng sạc (STOP)** | Nhiệt độ tại một hoặc nhiều cảm biến nhiệt độ bên trong pack vượt ngưỡng cho phép khi nạp (từ BMS hoặc ngưỡng cấu hình trạm `Temp Limit` / tầng 5). | Nhận bản tin `ALM_INFO` (`0x07F4`), trường `temp_cell_high_chg` với `severity >= 1` HOẶC nhiệt độ cell đo được `max_cell_temp >= temp_5_c` HOẶC `max_cell_temp >= temp_limit_c`. Kẹp dòng về 0A (`inhibit = 1`), tự phục hồi tối đa 3 lần sau 3s ổn định khi nhiệt độ nguội dưới ngưỡng hysteresis. Quá nhiệt lần 4 chuyển `FAULT` ngắt hoàn toàn. |
| **Pin** | Nhiệt độ sạc thấp | **E006** | **Pin quá lạnh khi sạc** | **Dừng sạc an toàn (STOP)** | Nhiệt độ khối pin quá lạnh (dưới 0°C), từ chối hoặc ngắt sạc để tránh mạ lithium. | Nhận bản tin `ALM_INFO` (`0x07F4`), trường `temp_cell_low_chg` với `severity >= 1`. `ev_bms()` kiểm tra `(in->bms.alarm_flags & BMS_ALARM_TEMP_LOW_CHG) != 0`. |
| **Pin** | Quá dòng sạc pin | **E007** | **Quá dòng sạc pin** | **Dừng sạc an toàn (STOP)** | Dòng sạc thực tế đo được từ pin vượt quá giới hạn dòng sạc cho phép tối đa. | Nhận bản tin `ALM_INFO` (`0x07F4`), trường `over_chg_curr` với `severity >= 1`. `ev_bms()` kiểm tra `(in->bms.alarm_flags & BMS_ALARM_OVER_CHG_CURR) != 0`. |
| **Pin** | Quá nhiệt khi xả | **W001** | **Pin quá nóng khi xả** | **Cảnh báo (INFO)** | Pin gửi cờ cảnh báo nhiệt độ trong quá trình xả điện vượt ngưỡng. | Nhận bản tin `ALM_INFO` (`0x07F4`), trường `temp_cell_high_dchg`. `ev_bms()` kiểm tra `(in->bms.alarm_flags & BMS_ALARM_TEMP_HIGH_DCHG) != 0`. |
| **Pin** | Nhiệt độ xả thấp | **W002** | **Pin quá lạnh khi xả** | **Cảnh báo (INFO)** | Nhiệt độ làm việc của khối pin dưới ngưỡng xả tối ưu. | Nhận bản tin `ALM_INFO` (`0x07F4`), trường `temp_cell_low_dchg`. `ev_bms()` kiểm tra `(in->bms.alarm_flags & BMS_ALARM_TEMP_LOW_DCHG) != 0`. |
| **Pin** | Quá nhiệt MOS/Rơ-le | **W003** | **Nhiệt độ MOS cao** | **Cảnh báo (INFO)** | Cảm biến nhiệt độ gắn tại tầng công suất đóng cắt MOSFET/rơ-le của khối pin báo nhiệt độ cao. | Nhận bản tin `ALM_INFO` (`0x07F4`), trường `temp_relay_high`. `ev_bms()` kiểm tra `(in->bms.alarm_flags & BMS_ALARM_TEMP_RELAY_HIGH) != 0`. |
| **Pin** | Quá dòng xả | **W004** | **Quá dòng xả pin** | **Cảnh báo (INFO)** | Tải tiêu thụ xả từ pin vượt quá mức định mức an toàn. | Nhận bản tin `ALM_INFO` (`0x07F4`), trường `over_dchg_curr`. `ev_bms()` kiểm tra `(in->bms.alarm_flags & BMS_ALARM_OVER_DCHG_CURR) != 0`. |
| **Pin** | Mất giao tiếp CAN pin | **E021** | **Mất giao tiếp CAN pin** | **Dừng sạc an toàn (STOP)** | Máy sạc không nhận được bản tin định kỳ CAN từ mạch quản lý pin quá 5000ms. | `bms_core.c` đếm: `now - last_rx_tick > BMS_OFFLINE_TIMEOUT_MS = 5000ms`. Gán `bms.online = false` -> `ev_bms_comm_lost()` kích hoạt. |
| **Pin** | Không có điện áp pin | **E022** | **Không có PIN** | **Dừng sạc an toàn (STOP)** | Mạch pin online nhưng điện áp đo được tại cực pin < 50% điện áp định mức (chưa cắm chắc hoặc tiếp điểm pin mở). | `bms.batt_voltage < cfg_vmax_v * 0.5f`. Hàm `ev_bms_no_pack_voltage()` kích hoạt sau debounce 500ms. |
| **Bộ sạc** | Lỗi phần cứng bộ sạc | **E010** | **Lỗi phần cứng bộ sạc** | **Dừng sạc an toàn (STOP)** | Bộ sạc tự phát hiện hỏng hóc linh kiện công suất, cảm biến, nguồn nuôi phụ cấp hoặc EEPROM. | Nhận cờ CAN từ bộ sạc: `(in->mod_alarm_or & CHG_LIB_ALARM_HW_FAULT) != 0`. Lọc debounce động theo trạng thái trạm: IDLE = 10.000ms (10s) để lọc sạch quá trình xả tụ/sụt áp khi ngắt Aptomat AC; ACTIVE/RUNNING = 1.000ms (1s) ngắt sạc nhanh. Tự động suppress khi có sụt áp AC, lỗi PFC, mất pha lưới hoặc khi module offline trong IDLE. |
| **Bộ sạc** | Mất giao tiếp bộ sạc | **W010** | **Mất giao tiếp bộ sạc** | **Cảnh báo (INFO)** | Cáp CAN nối bộ điều khiển với bộ sạc bị đứt, lỏng giắc cắm hoặc bộ sạc mất nguồn nuôi (> 2000ms) trong một active session. | Driver cập nhật `online = false` khi quá timeout; Alarm suy ra W010 từ module enabled/offline trong `RUNNING`, `READY` hoặc `PRECHARGE`. Khi toàn bộ module mất, controller xử lý thêm fault mismatch/no-module. |
| **Bộ sạc** | Quá nhiệt bộ sạc | **E011** | **Nhiệt độ bộ sạc cao** | **Dừng sạc an toàn (STOP)** | Nhiệt độ khối tản nhiệt (Heatsink) hoặc biến áp bên trong bộ sạc vượt quá ngưỡng an toàn. | Nhận cờ CAN từ bộ sạc: `(in->mod_alarm_or & CHG_LIB_ALARM_OVER_TEMP) != 0`. |
| **Bộ sạc** | Quá áp đầu ra bộ sạc | **E012** | **Điện áp DC đầu ra sạc cao** | **Dừng khẩn cấp (E-STOP)** | Điện áp DC thực tế ngõ ra của bộ sạc vượt quá ngưỡng bảo vệ ngắt cắt. | Nhận cờ CAN bảo vệ quá áp từ bộ sạc: `(in->mod_alarm_or & CHG_LIB_ALARM_OVER_VOLTAGE_OUT) != 0`. |
| **Bộ sạc** | Ngắn mạch đầu ra sạc | **E013** | **Ngắn mạch đầu ra sạc** | **Dừng khẩn cấp (E-STOP)** | Ngắn mạch hoặc chập đường tải DC đầu ra bộ sạc. | Nhận cờ CAN ngắn mạch: `(in->mod_alarm_or & CHG_LIB_ALARM_SHORT_CIRCUIT) != 0`. |
| **Bộ sạc** | Quá dòng đầu ra bộ sạc | **E014** | **Quá dòng đầu ra bộ sạc** | **Dừng sạc an toàn (STOP)** | Dòng ngõ ra thực tế vượt quá ngưỡng dòng định mức tối đa của bộ sạc. | Nhận cờ CAN quá dòng: `(in->mod_alarm_or & CHG_LIB_ALARM_OVER_CURR_OUT) != 0`. |
| **Bộ sạc** | Lỗi khối nguồn bộ sạc | **E015** | **Lỗi khối nguồn bộ sạc** | **Dừng sạc an toàn (STOP)** | Khối mạch chỉnh lưu nguồn đầu vào (PFC) của bộ sạc bị sự cố. | Nhận cờ CAN lỗi PFC/mất pha từ bộ sạc: `ev_mod_pfc` kích hoạt. Tự động bypass ở IDLE (chống báo ảo khi ngắt AC), suppress khi có cờ thấp áp AC. |
| **Bộ sạc** | Lỗi quạt tản nhiệt bộ sạc | **E016** | **Lỗi quạt tản nhiệt bộ sạc** | **Dừng sạc an toàn (STOP)** | Một hoặc nhiều quạt làm mát bên trong bộ sạc bị kẹt cơ học hoặc đứt tín hiệu hồi tiếp tốc độ. | Nhận cờ CAN lỗi quạt: `(in->mod_alarm_or & CHG_LIB_ALARM_FAN_FAULT) != 0`. |
| **Bộ sạc** | Quá áp AC đầu vào sạc | **E017** | **Điện áp AC đầu vào sạc cao** | **Dừng sạc an toàn (STOP)** | Điện áp lưới điện xoay chiều AC cấp vào bộ sạc vượt ngưỡng bảo vệ tối đa. | Nhận cờ CAN quá áp AC: `(in->mod_alarm_or & CHG_LIB_ALARM_AC_OVER_VOLT) != 0`. |
| **Bộ sạc** | Điện áp AC đầu vào sạc thấp | **W011** | **Điện lưới AC bị yếu** | **Cảnh báo (INFO)** | Điện áp lưới AC đầu vào bị sụt giảm xuống dưới ngưỡng danh định cho phép. | Nhận cờ CAN thấp áp AC: `(in->mod_alarm_or & CHG_LIB_ALARM_AC_UNDER_VOLT) != 0`. Tự động bypass trong IDLE (Clean Shutdown); trong active session debounce set 1000ms, clear 3000ms. |
| **Hệ thống** | Mất tải đầu ra đột ngột | **E023** | **Mất tải đột ngột** | **Dừng sạc an toàn (STOP)** | Dòng sạc DC đột ngột sụt giảm về 0A (< 15% dòng đặt) khi đang sạc áp cao (tuột giắc sạc hoặc ngắt pin bất ngờ). | `ev_dc_load_lost()`: Đang ở `RUNNING`, dòng sụt `< 15%` trong khi áp vẫn `> 98%`. Debounce: 800ms. Latching = true. |
| **Hệ thống** | Chưa thiết lập đầu ra DC | **E024** | **Chưa thiết lập đầu ra DC** | **Dừng sạc an toàn (STOP)** | Đã phát lệnh đóng rơ-le sạc quá 12000ms (12s) nhưng tổng dòng sạc thực tế của các module online vẫn `< 2.0A`. | `ev_dc_out_not_established()`: `now - relay_close_since >= 12000ms`, không inhibit, dòng đặt đủ lớn và `CHG_LIB_GetSystemSummary().total_current < 2.0A`. Latching = true. |
| **Hệ thống** | Không tìm thấy bộ sạc | **E027** | **Lỗi bộ sạc** | **Từ chối sạc (Inhibit)** | Bộ điều khiển trung tâm không phát hiện được bất kỳ bộ sạc nào phản hồi online khi khởi tạo. | `mod_online_count == 0` hoặc driver chưa chọn -> cờ `CHARGE_CTRL_FAULT_NO_MODULE`. |
| **Hệ thống** | Số bộ sạc không khớp | **E028** | **Số bộ sạc không khớp** | **Từ chối sạc (Inhibit)** | Số bộ sạc thực tế online khác số bộ sạc được cài đặt trong cấu hình. | `actual_module_count != configured_module_count` -> cờ `CHARGE_CTRL_FAULT_MODULE_COUNT_MISMATCH`. |
| **Hệ thống** | Cấu hình sạc không hợp lệ | **E029** | **Lỗi cấu hình sạc** | **Từ chối sạc (Inhibit)** | Thông số giới hạn điện áp, dòng điện cài đặt vượt quá dải an toàn của hệ thống. | `ChargeCycleConfig_Set()` từ chối cấu hình -> cờ `CHARGE_CTRL_FAULT_INVALID_CONFIG`. |
| **Kết nối** | Sụt áp jack sạc | **E030** | **Sụt áp jack sạc** | **Dừng sạc an toàn (STOP)** | Chênh lệch điện áp tiếp xúc giữa đầu ra bộ sạc và cực pin vượt quá ngưỡng bảo vệ khi có dòng sạc chạy qua ($\Delta V = I \cdot R$). | Điều kiện: `total_current >= 2.0A`, relay đã chốt đóng (`relay_latched_closed`), không trong trạng thái ngắt nhiệt (`!bms_temp_paused`), và `V_module - V_bms > protect_jack_charge_delta_v` duy trì liên tục đủ `protect_jack_charge_delay_s`. Gián đoạn bất kỳ tick nào sẽ tự động reset timer về 0. |
| **Kết nối** | Nhiệt độ jack sạc cao | **E031** | **Nhiệt độ jack sạc cao** | **Derating / STOP** | Nhiệt độ đo được tại đầu cắm jack sạc pin vượt ngưỡng an toàn cho phép. | Cảm biến NTC jack sạc vượt ngưỡng cấu hình `CHARGE_CTRL_FAULT_PROTECT_JACK_TEMP`. |
| **Hệ thống** | Cắm nhầm loại pin / sạc | **E032** | **Lỗi nhầm sạc** | **Dừng sạc an toàn (STOP)** | Khách hàng cắm nhầm chủng loại pin vào máy sạc (ví dụ cắm pin 48V vào sạc 72V). | So sánh `\|Volt_target - Volt_max\| > 2.0V`, với `Volt_target` từ bản tin CAN BMS `0x1806E5F4` và `Volt_max` từ cấu hình sạc. Debounce: 1000ms. |

---

## 4. MA TRẬN PHÂN ĐỊNH VÀ CHỐNG XUNG ĐỘT MÃ LỖI (FAULT CONFLICT RESOLUTION MATRIX)

Hệ thống cảnh báo và bảo vệ dùng một không gian mã lỗi thống nhất, các action được gom theo thứ tự ưu tiên. Các điều kiện loại trừ bên dưới là contract đã được kiểm thử trên host; các ngưỡng phần cứng vẫn cần xác nhận HIL.

### 4.1 Thứ bậc hành vi an toàn (Action Priority Hierarchy)
Khi xuất hiện nhiều lỗi đồng thời, hệ thống phân cấp hành vi theo độ ưu tiên:
1. **`ALARM_ACT_ESTOP` (Cấp 1 - Cao nhất)**: Ngắt relay tức thì không chờ dòng xả, hạ lệnh module về 0 ngay lập tức (Áp dụng cho: `E012` Quá áp DC đầu ra, `E013` Ngắn mạch đầu ra).
2. **`ALARM_ACT_STOP` (Cấp 2)**: Giảm lệnh dòng về 0A, chờ dòng thực tế xả an toàn dưới 1.0A hoặc hết timeout 3000ms rồi mở relay dập hồ quang (Áp dụng cho: Các lỗi nghiêm trọng `E001`, `E003`, `E004`, `E006`, `E007`, `E010`, `E011`, `E014`..`E017`, `E021`..`E024`, `E030`, `E032`).
3. **`ALARM_ACT_INFO` (Cấp 3)**: Cảnh báo hiển thị và ghi log, không ngắt sạc (Áp dụng cho: `E002`, `W001`..`W004`, `W010`, `W011`, và `E005` trong 3 lần ngắt nhiệt tự phục hồi đầu tiên).

Mã lỗi hiển thị trên DWIN luôn là **mã lỗi có cấp hành vi cao nhất** (`highest_action`), sau đó là mã lỗi có thứ tự ưu tiên đầu tiên trong bảng (`worst_code`). Cảnh báo cấp thấp (INFO) tuyệt đối không che lấp lỗi cấp cao (STOP/ESTOP).

### 4.2 Bảng phân tích phân định giữa các cặp lỗi có liên quan (Conflict Resolution Analysis)

| Cặp mã lỗi liên quan | Hiện tượng vật lý dễ gây nhầm lẫn | Cơ chế cô lập & phân định trong Firmware | Kết luận xung đột |
| :--- | :--- | :--- | :--- |
| **E005 (Quá nhiệt sạc)** vs **E030 (Sụt áp jack sạc)** | Khi BMS quá nhiệt, dòng ngắt về 0A, điện áp tụ ngõ ra sạc giữ nguyên, tạo chênh lệch áp $\Delta V = V_{\text{cap}} - V_{\text{batt}} > 2.0\text{V}$. | 1) Khóa theo trạng thái: Nếu `bms_temp_paused == true` $\rightarrow$ Vô hiệu hóa tính toán `E030`.<br>2) Khóa theo dòng: Chỉ đánh giá `E030` khi `total_current >= 2.0A`.<br>3) Khi dừng FAULT và relay mở, xóa điều kiện so sánh $\Delta V$ khi reset lỗi. | **Đã phân định theo điều kiện runtime; cần HIL để xác nhận ngưỡng thực tế.** |
| **E005 (Quá nhiệt sạc)** vs **E023 (Mất tải đột ngột)** | Khi BMS ngắt nhiệt, MOSFET nội bộ ngắt khiến dòng sạc sụt từ 50A về 0A tức thì trong khi áp vẫn cao. | Hàm `ev_dc_load_lost()` chủ động kiểm tra `if (in->cc.inhibit != 0U) return false;` và kiểm tra cờ nhiệt BMS `bms_thermal_alarm`. Khi đang dừng nhiệt có chủ đích, cấm kích hoạt `E023`. | **Được phân định theo snapshot hiện tại; cần HIL để xác nhận phối hợp thực tế.** |
| **E005 (Quá nhiệt sạc)** vs **E024 (Chưa thiết lập đầu ra DC)** | Khi bắt đầu sạc hoặc đang sạc mà nhiệt độ cell vượt ngưỡng khiến dòng không dâng được quá 2.0A. | Hàm `ev_dc_out_not_established()` kiểm tra `if (in->cc.inhibit != 0U) return false;`. | **Được phân định theo trạng thái inhibit; cần HIL để xác nhận phối hợp thực tế.** |
| **E021 (Mất CAN pin)** vs **E022 (Không pin)** vs **E023 (Mất tải đột ngột / Hot Unplug)** | Rút giắc sạc hoặc tắt BMS xe đột ngột khi đang phát dòng công suất lớn khiến dòng sụt về 0A, sau đó tín hiệu CAN và điện áp pin mất theo dây chuyền. | 1) Khi dòng sụt đột ngột trong active session, `ev_dc_load_lost()` lập tức bắt lỗi gốc **E023**.<br>2) Hàm `ev_bms_comm_lost()` và `ev_bms_no_pack_voltage()` kiểm tra `is_alarm_active_or_latched(ALARM_DC_LOAD_LOST)`: Nếu đã ngắt bởi E023 thì **chế áp hoàn toàn E021 và E022**, không cho ghi log hay hiển thị đè.<br>3) Khi không có E023, `E022` chỉ kích hoạt khi CAN online (`in->bms.online == true`) nhưng $V_{\text{batt}} < 0.5 \times V_{\max}$; khi mất CAN chỉ duy nhất `E021` kích hoạt. | **Cô lập nguyên nhân gốc (Root Cause Isolation): Chỉ duy nhất E023 xuất hiện khi hot-unplug, triệt tiêu hoàn toàn báo động phái sinh.** |
| **W011 (Lưới AC yếu / Mất pha)** vs **E010 (Lỗi phần cứng)** vs **E015 (Lỗi PFC)** vs **W010 (Mất CAN module)** | Khi tắt CB/Aptomat nguồn trạm sạc hoặc mất pha lưới: Tụ DC Bus xả điện trong 200ms–2s khiến module LianMing/TonHe phát xung cờ HW Fault (Byte 7 Bit 1 = 1) trước khi đo được sụt áp AC hoặc mất CAN. | 1) **Lọc Debounce động (Dynamic State-Dependent Debounce)**: Lỗi phần cứng `E010` áp dụng debounce **10.000ms (10 giây)** ở trạng thái `IDLE` (`ALARM_DB_HW_FAULT_IDLE_SET_MS`) và **1.000ms (1 giây)** ở trạng thái đang sạc (`ALARM_DB_HW_FAULT_ACTIVE_SET_MS`). Xung cờ lỗi do xả tụ chỉ tồn tại < 1.5s nên bị nuốt trọn hoàn toàn, 0 Alarm ghi log khi tắt CB lúc IDLE.<br>2) **Bảo vệ chế độ IDLE (Clean Shutdown)**: Khi trạm ở trạng thái chờ (`CHARGE_CTRL_STATE_IDLE` - màn hình READY), `ev_mod_ac_undervolt()`, `ev_mod_pfc()` và `ev_mod_comm_lost()` tự động bypass. Tắt CB nguồn lúc IDLE tuyệt đối không phát sinh cảnh báo, không ghi log rác.<br>3) **Khóa Module Offline trong IDLE**: Nếu module mất nguồn hoặc đi vào offline lúc IDLE (`in->mod_offline_count != 0U`), `ev_mod_hw_fault()` tự động trả về false, loại trừ hoàn toàn báo ảo khi tắt điện trạm.<br>4) **Chế áp dây chuyền khi đang chạy (Active Session)**: Khi mất AC/mất pha trong lúc sạc, hệ thống kích hoạt duy nhất **W011** (hoặc dừng an toàn). `ev_mod_hw_fault()`, `ev_mod_pfc()`, `ev_mod_comm_lost()` đều kiểm tra cờ `AC_UNDER_VOLT`: Khi có sụt áp AC, **chế áp hoàn toàn E010, E015 và W010**.<br>5) **Cách ly cờ HW Fault chung E010**: `ev_mod_hw_fault()` chỉ kích hoạt khi có cờ HW Fault mà KHÔNG có sụt áp AC/PFC/mất pha và KHÔNG có các lỗi bảo vệ cụ thể (`E011` Quá nhiệt, `E012` Quá áp, `E013` Ngắn mạch, `E014` Quá dòng, `E016` Quạt, `E017` Quá áp AC). | **Triệt tiêu 100% chuỗi lỗi domino (Cascade Suppression). Tương thích đồng nhất cho cả 3 module Maxwell, TonHe, LianMing.** |
| **E032 (Lỗi nhầm sạc)** vs **E003 (Quá áp pin)** | Cắm nhầm pin có điện áp danh định thấp hơn sạc (ví dụ cắm pin 48V vào sạc 72V). | `E032` debounce sai lệch điện áp yêu cầu trong 1000ms; không giới hạn chỉ ở 1000ms đầu phiên. `E003` là bảo vệ điện áp thực tế dự phòng tầng 2. | **Hai lớp bảo vệ, mã có thể cùng tồn tại; action STOP vẫn được aggregate.** |
| **Chế độ PRECHARGE** vs **Các lỗi pin BMS (E001..E007, E021, E022)** | Pin cạn kiệt sập nguồn, BMS đang tắt/ngủ, điện áp cực pin bằng 0V. | Trong trạng thái `PRECHARGE`, controller chủ động cho phép BMS offline. `ev_bms()` và `ev_bms_no_pack_voltage()` tự động suppress các cảnh báo này cho đến khi pin được kích hoạt và BMS gửi bản tin online hợp lệ. | **Tự động thích ứng theo chế độ hoạt động, không báo lỗi ảo.** |
| **Chế độ STANDALONE (Không BMS)** vs **Các lỗi pin BMS** | Chạy sạc thủ công hoặc không có giao tiếp BMS (`CHARGE_SOURCE_STANDALONE_NO_BMS`). | Tất cả các bộ đánh giá lỗi BMS đều kiểm tra `if (in->cfg_source_mode != CHARGE_SOURCE_BMS_CONTROLLED) return false;`. Toàn bộ lỗi pin tự động vô hiệu hóa, chỉ giám sát bảo vệ phần cứng module và trạm sạc. | **Cách ly hoàn toàn giữa các chế độ sạc.** |

---

## 5. KIẾN TRÚC VÀ LOGIC GHI NHẬT KÝ LỖI (EVENT LOGGING ARCHITECTURE & RULES)

Hệ thống quản trị và ghi nhật ký lỗi (Alarm Logging Subsystem) được thiết kế theo tiêu chuẩn công nghiệp nhằm đảm bảo: **không bỏ sót sự cố thật, không ghi log rác, chống tràn Flash và đồng bộ hiển thị HMI DWIN / PC App**.

### 5.1 Luồng Xử lý Ghi Log (Logging Pipeline)

```
[ Dữ liệu CAN / ADC / Trạng thái ]
              │
              ▼
   [ 1. Thu thập & Đánh giá Raw (eval) ]  <─── Cascade Suppression (Chặn lỗi lan truyền)
              │
              ▼
   [ 2. Lọc Debounce động (run_debounce) ] <─── IDLE (10s) vs ACTIVE (1s)
              │
              ▼
   [ 3. Phát hiện Sườn (Edge Detection) ]  <─── Sườn lên (RAISED=1) / Sườn xuống (CLEARED=0)
              │
        ┌─────┴─────────────────────────┐
        ▼                               ▼
[ 4. Ghi RAM Ring Buffer ]     [ 5. Ghi SPI Flash Ngoại ]
(Độ sâu: 16 bản ghi mới nhất)     (Vùng nhớ Flash W25Qxx lưu vĩnh viễn)
        │                               │
        ▼                               ▼
[ Hiển thị DWIN Trang 10 ]     [ Đọc dữ liệu lên PC App ]
(Bảng lịch sử lỗi thực tế)     (Truy xuất toàn bộ lịch sử)
```

### 5.2 Cấu trúc Bản ghi Nhật ký (`AlarmLogEntry_t`)

Mỗi sự kiện cảnh báo được nén thành một cấu trúc nhị phân 8 bytes lưu trữ:

| Trường dữ liệu | Kiểu dữ liệu | Kích thước | Mô tả chi tiết |
| :--- | :--- | :---: | :--- |
| `uptime_ms` | `uint32_t` | 4 bytes | Thời gian hoạt động của MCU tính từ lúc khởi động (đơn vị: mili-giây). |
| `code` | `uint16_t` | 2 bytes | Mã định danh lỗi (Enum `AlarmCode_t`), ánh xạ trực tiếp sang `E001`..`E032`, `W001`..`W011`. |
| `action` | `uint8_t` | 1 byte | Cấp hành vi xử lý sự cố (`0`: NONE, `1`: INFO, `2`: STOP, `3`: ESTOP). |
| `event` | `uint8_t` | 1 byte | Trạng thái chuyển đổi: `1` = **XUẤT HIỆN LỖI (RAISED)**, `0` = **XÓA/HẾT LỖI (CLEARED)**. |
| `timestamp` *(phụ)* | `uint32_t` | 4 bytes | Thời gian thực Epoch (giây kể từ 1970) lấy từ chip RTC ngoại (DS3231/STM32 RTC). Nếu RTC chưa sync, giá trị là 0. |

### 5.3 Nguyên tắc Ghi Log theo Sườn (Edge-Triggered Logging)

1. **Chỉ ghi log khi có chuyển đổi trạng thái (State Transition Edge)**:
   - Khi lỗi xuất hiện và duy trì đủ thời gian `held >= set_ms`: Hệ thống ghi duy nhất **1 bản ghi RAISED (`event = 1`)**.
   - Khi lỗi tự hết hoặc được xóa và ổn định đủ `held >= clear_ms`: Hệ thống ghi duy nhất **1 bản ghi CLEARED (`event = 0`)**.
   - Tuyệt đối không ghi lặp lại theo chu kỳ định kỳ, ngăn ngừa tràn bộ nhớ và bảo vệ tuổi thọ chip Flash.
2. **Phân biệt Lỗi Tự xóa (Non-latching) và Lỗi Chốt (Latching)**:
   - **Lỗi tự xóa (Non-latching)**: Ví dụ `E010` (HW Fault), `E011` (Quá nhiệt), `W011` (AC yếu)... Khi điều kiện vật lý an toàn trở lại trong `clear_ms` (mặc định 200ms), cờ `rt->active` tự hạ xuống false và ghi sự kiện `CLEARED`. Màn hình DWIN tự động quay về `CODE: 0000`.
   - **Lỗi chốt (Latching)**: Ví dụ `E023` (Mất tải DC đột ngột), `E024` (Chưa thiết lập đầu ra DC)... Dù điều kiện raw biến mất, hệ thống chuyển sang `rt->latched = true`. Cảnh báo vẫn duy trì và màn hình giữ nguyên mã lỗi cho đến khi người vận hành nhấn nút **Acknowledge / Reset** trên màn hình hoặc PC App.

### 5.4 Logic Debounce Động theo Trạng Thái (Dynamic State-Dependent Debounce)

Để giải quyết mâu thuẫn giữa "bảo vệ tốc độ cao khi sạc" và "chống báo ảo khi bật/tắt nguồn trạm":

```c
uint32_t set_ms = sp->set_ms;
if (sp->code == ALARM_MOD_HW_FAULT) {
    set_ms = (in->cc.state == CHARGE_CTRL_STATE_IDLE)
                 ? ALARM_DB_HW_FAULT_IDLE_SET_MS       /* 10.000 ms (10 giây) */
                 : ALARM_DB_HW_FAULT_ACTIVE_SET_MS;     /* 1.000 ms (1 giây)   */
}
```

* **Trạng thái IDLE (Standby / Chờ sạc):**
  * `set_ms` nâng lên **10 giây**. Quá trình ngắt Aptomat AC gây sụt áp DC-bus trong 200ms–1500ms được hấp thụ 100%, không ghi log, không báo giả `E010`.
  * Nếu module thực sự hỏng hóc trong lúc cắm điện chờ ở IDLE (lỗi giữ liên tục > 10s), hệ thống ghi log và báo lỗi bình thường.
* **Trạng thái ACTIVE (Precharge, Running, Paused...):**
  * `set_ms` hạ xuống **1 giây**. Ngay khi có lỗi phần cứng lúc phát dòng sạc, hệ thống ngắt sạc và ghi log `E010` sau đúng 1s.
* **Chuyển tiếp trạng thái an toàn:**
  * Nếu module đã phát cờ lỗi ở IDLE được 2 giây mà người dùng bấm sạc: Trạng thái rời IDLE $\rightarrow$ `set_ms` lập tức giảm về 1s $\rightarrow$ Điều kiện `held (2s) >= set_ms (1s)` thỏa mãn tức thì $\rightarrow$ Báo lỗi `E010` và hủy phiên sạc ngay lập tức trước khi đóng relay.

### 5.5 Cơ chế Chế Áp Lỗi Dây Chuyền (Cascade Suppression Rules)

Để tránh hiện tượng 1 sự cố gốc làm phát sinh hàng loạt bản ghi log phái sinh ("bão log"):

1. **Sụt áp / Mất điện lưới AC**:
   - Khi cờ `CHG_LIB_ALARM_AC_UNDER_VOLT` được bật hoặc alarm `W011` đang active/latched:
   - Tự động **CHẾ ÁP HOÀN TOÀN** các lỗi: `E010` (HW Fault), `E015` (PFC Fault), `W010` (Module Comms Lost).
   - Chỉ duy nhất sự cố nguồn AC được ghi nhận.
2. **Rút giắc sạc đột ngột (Hot Unplug)**:
   - Khi dòng điện sụt đột ngột trong active session, `ev_dc_load_lost()` kích hoạt lỗi gốc `E023`.
   - Cờ `E023` tự động **CHẾ ÁP HOÀN TOÀN** các lỗi phái sinh theo sau: `E021` (Mất CAN pin do rút giắc) và `E022` (Không có điện áp pin).
3. **Quá nhiệt sạc (Thermal Protection)**:
   - Khi hệ thống đang chủ động kẹp dòng về 0A do quá nhiệt (`in->cc.inhibit != 0` hoặc `bms_temp_paused`):
   - Tự động **CHẾ ÁP** `E023` (Mất tải DC) và `E024` (Chưa thiết lập đầu ra DC) và `E030` (Sụt áp giắc sạc).
4. **Trạm sạc ở trạng thái IDLE (Standby Shutdown)**:
   - Các cảnh báo `W011` (AC yếu), `E015` (PFC fault), `W010` (Module comm lost) tự động vô hiệu hóa (`return false`).
   - Nếu có module offline khi đang IDLE: `ev_mod_hw_fault()` tự động vô hiệu hóa (`return false`).

### 5.6 Đồng bộ Hiển thị DWIN và Truyền thông PC App

1. **Hiển thị trên Màn hình DWIN:**
   * **Mã lỗi tức thời (Trang chủ / Trang sạc / Trang lỗi):** Hiển thị mã lỗi có cấp hành vi cao nhất (`highest_action`), ưu tiên ESTOP $\rightarrow$ STOP $\rightarrow$ INFO.
   * **Trang Nhật ký Lỗi (Trang 10):** Đọc trực tiếp từ bộ đệm `g_alarm.log` (16 bản ghi gần nhất). Hiển thị danh sách gồm:
     - Cột Thời gian: Định dạng `HH:MM:SS` (nếu RTC hợp lệ) hoặc Uptime.
     - Cột Mã lỗi: `E001`..`E032`, `W001`..`W011`.
     - Cột Trạng thái: Biểu tượng/Màu phân biệt rõ sự kiện xuất hiện (Đỏ) hay đã phục hồi (Xanh).
2. **Truy xuất qua PC Debug App:**
   * Lệnh nhị phân `CMD_GET_ALARM_LOG` cho phép PC Debug App tải toàn bộ danh sách sự kiện từ chip nhớ SPI Flash ngoại ra file CSV hoặc hiển thị bảng đồ thị phục vụ công tác giám sát, bảo trì trạm sạc.

