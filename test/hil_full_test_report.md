# BÁO CÁO TỔNG HỢP KIỂM THỬ TOÀN DIỆN PHẦN CỨNG HIL (ZCAN + STM32 + DWIN)

- **Thời gian**: 2026-09-17 23:04:49
- **Kết quả chung**: **10/14 bài PASS (71.4%)**

| STT | Nhóm chức năng | Mã test | Tên bài kiểm tra | Kết quả | Ghi chú kỹ thuật |
| :---: | :--- | :---: | :--- | :---: | :--- |
| 1 | Charging Logic | CL-01 | Soft-Start & Ramp dong 5A/s | **❌ FAIL** | Max ramp: 36.5A/s (chuan <= 5A/s), Dong dat: 83.5A |
| 2 | Charging Logic | CL-02 | Phan tang giam dong & Don dieu | **✅ PASS** | Band1: 100.0A -> Band2: 50.0A -> Band3: 30.0A -> Monotonic: 30.0A |
| 3 | Charging Logic | CL-03 | Nhiet do Pin Inhibit & Tu phuc hoi | **✅ PASS** | Inhibit target: 100.0A, Hoi phuc: 8.5A |
| 4 | Charging Logic | CL-05 | Dap dong ve 0 truoc khi nha Contactor | **✅ PASS** | Zero-current break verified: True, State ve IDLE (0) |
| 5 | Charging Logic | CL-06 | Dong bo Allow Charge CAN | **❌ FAIL** | IDLE allow=True, RUN allow=True, STOP allow=False |
| 6 | Charging Logic | CL-07 | Khoa cung FAULT qua nhiet lan 4 & Chong bypass | **✅ PASS** | State trip 4: 4 (chuan 4=FAULT), Chặn lệnh START: True |
| 7 | Protection | IC-01 | Bat dau sac binh thuong | **✅ PASS** | MCU State: 2 (RUNNING), I=52.0A |
| 8 | Protection | IC-02 | Dung sac nguoi dung (User Stop) | **✅ PASS** | MCU State ve IDLE (0), I=0.0A |
| 9 | Protection | IC-03 | Mat lien lac BMS (E021) & Tu khoi phuc | **❌ FAIL** | Phat hien mat CAN (fault=0x0008), Khoi phuc ve IDLE thanh cong |
| 10 | Protection | IC-04 | Bao ve qua ap Pack Pin | **✅ PASS** | MCU ngat sac vao FAULT=4, FaultFlags=0x0020 |
| 11 | Protection | IC-08 | Bao ve qua nhiet Jack sac | **✅ PASS** | An toan ngat dong |
| 12 | Protection | IC-09 | Dung khan cap E-Stop & Safe Reset | **✅ PASS** | E-Stop vao FAULT (4), Safe Reset thanh cong ve State=0 |
| 13 | Pre-charge | PC-01 | Kich hoat Pre-charge tu DWIN khi Pin kiet | **✅ PASS** | State=2, Fault=0x0000 (Sạch lỗi 100%, không còn E021) |
| 14 | Pre-charge | PC-06 | Dung Pre-charge thu cong ve IDLE | **❌ FAIL** | MCU State ve an toan: 2 (Running) |
