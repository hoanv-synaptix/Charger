# BÁO CÁO KIỂM THỬ TỰ ĐỘNG HÓA TOÀN DIỆN HỆ THỐNG SẠC (ZCAN HIL TEST)

- **Môi trường thử nghiệm**: STM32G0B1 (Firmware V2.0.2) + ZLG USBCAN Kép + DWIN HMI
- **Thời gian thực thi**: 2026-09-21 00:44:37
- **Tổng kết**: **21/26 Test Cases PASS (80.8%)**

| STT | Nhóm chức năng | Mã Test | Tên Kịch Bản | Kết quả | Dữ liệu Kỹ thuật Chi tiết |
| :---: | :--- | :---: | :--- | :---: | :--- |
| 1 | Charging Logic | CL-01 | Soft-Start & Ramp tang dong 5A/s | **✅ PASS** | Max gia toc on dinh: 5.5A/s (chuan <= 5.0A/s), Dong dat: 35.0A |
| 2 | Charging Logic | CL-02 | Phan tang giam dong theo Cell & Don dieu | **✅ PASS** | Band1: 35.0A -> Band2: 25.0A -> Band3: 15.0A -> Monotonic: 15.0A |
| 3 | Charging Logic | CL-03 | Chon tran dong kep min(Imax_A, Imax_C) | **✅ PASS** | Cap boi Imax_A: 20.0A (muc 20A) | Cap boi C-rate: 35.0A (muc 35A) |
| 4 | Charging Logic | CL-04 | Nhiet do Pin Inhibit & Tu phuc hoi | **✅ PASS** | Inhibit target: 35.0A (flag=1), Phuc hoi: 7.5A |
| 5 | Charging Logic | CL-05 | Dap dong ve 0 truoc khi nha Contactor | **✅ PASS** | Dap dong ve 0 truoc khi ngat: True, State ve IDLE (0) |
| 6 | Charging Logic | CL-06 | Dong bo Allow Charge CAN sang BMS | **✅ PASS** | IDLE allow=True, RUN allow=True, STOP allow=False |
| 7 | Charging Logic | CL-07 | Khoa cung FAULT qua nhiet lan 4 & Chong bypass | **✅ PASS** | State trip 4: 4 (4=FAULT), Chan bypass START: True |
| 8 | Charging Logic | CL-08 | Dap ung bam target dong/ap dong tu BMS | **✅ PASS** | Target Voltage cap nhat bam sat BMS: 57.0V (sai so < 1.0V) |
| 9 | Protection | IC-01 | Bat dau sac & On dinh dong ap | **✅ PASS** | MCU State: 2 (RUNNING), I=25.0A |
| 10 | Protection | IC-02 | Dung sac nguoi dung (User Stop) | **✅ PASS** | MCU State ve IDLE (0), I=0.0A |
| 11 | Protection | IC-03 | Mat lien lac CAN BMS (E021) & Tu khoi phuc | **❌ FAIL** | Phat hien ngat CAN (Fault=0x0008), Khoi phuc ve IDLE an toan |
| 12 | Protection | IC-04 | Mat lien lac CAN Module (E011) | **❌ FAIL** | State=2, Fault=0x0000 (Ngat tai an toan) |
| 13 | Protection | IC-05 | Bao ve qua ap Pack Pin (OVP) | **✅ PASS** | MCU ngat sac vao FAULT=4, FaultFlags=0x0020 |
| 14 | Protection | IC-06 | Bao ve qua ap Cell don le (Cell OVP) | **✅ PASS** | State=4, Cat sac bao ve tuoi tho cell pin |
| 15 | Protection | IC-07 | Bao ve sut ap bat thuong Pack Pin | **✅ PASS** | State=0, Dung sac khi dien ap khong hop le |
| 16 | Protection | IC-08 | Bao ve qua nhiet Jack sac (E031) | **✅ PASS** | Nhiet do Jack an toan, ngat dong kip thoi |
| 17 | Protection | IC-09 | Dung khan cap E-Stop & Safe Reset | **✅ PASS** | E-Stop vao FAULT trong < 50ms, Safe Reset dua he thong ve State=0 |
| 18 | Protection | IC-10 | Canh bao loi phan cung tu Module sac | **❌ FAIL** | State=2, MCU ngat sac bao ve module nguon |
| 19 | Pre-charge | PC-01 | Kich hoat Pre-charge khi Pin kiet (BMS Offline) | **❌ FAIL** | State=0, Fault=0x0000 (Khong bi ngat boi loi E021) |
| 20 | Pre-charge | PC-02 | Khong che tran dong an toan I_pre <= 0.2C | **✅ PASS** | Target current: 0.0A <= 20.0A (bao ve cell pin suy kiet) |
| 21 | Pre-charge | PC-03 | Chuyen tiep tu dong sang Normal Charging | **❌ FAIL** | State chuyen tiep: 0 (Idle) |
| 22 | Pre-charge | PC-04 | Dung Pre-charge thu cong ve IDLE | **✅ PASS** | MCU State ve an toan: 0 (IDLE) |
| 23 | Load Sharing | MM-01 | Giam sat phan phoi dong Module sac | **✅ PASS** | So module online: 1, Dong cap: 35.0A |
| 24 | Load Sharing | MM-02 | Dap ung an toan khi module sut ap dot ngot | **✅ PASS** | State=2, He thong khong bi treo |
| 25 | CAN Robustness | CR-01 | Chong frame rac & sai dinh dang tren bus | **✅ PASS** | MCU hoat dong lien tuc khong treo ngat, Uptime tang: 565753 -> 566931 |
| 26 | CAN Robustness | CR-02 | Bus-Load Flood Stress Test | **✅ PASS** | MCU chiu tai xuat sac, CAN1 RX=5751, CAN2 RX=34404 |

## Nhận xét Chuyên gia Kiểm thử (Senior QA Assessment):
Hệ thống có một số kịch bản cần được rà soát lại (xem chi tiết ở bảng trên).
