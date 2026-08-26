# Hardware-in-the-loop (HIL) & Fuzzing Test Plan

## 1. M?c dích
- Ðánh giá kh? nang t? ph?c h?i c?a Charger Firmware (STM32G0) du?i các di?u ki?n l?i kh?t khe nh?t.
- Ð?m b?o FSM không b? k?t khi m?t k?t n?i d?t ng?t ho?c nh?n d? li?u rác.

## 2. K?ch b?n Soak Test (72 gi?)
**Setup:**
- K?t n?i Firmware v?i PC qua RS485 (DWIN mock) và CAN1 (PC-CAN interface mô ph?ng Module s?c).
- K?t n?i CAN2 v?i BMS Simulator.

**Th?c hi?n:**
1. Kh?i d?ng s?c t? d?ng l?p l?i liên t?c.
2. M?i 1 gi?, script ch?y t? d?ng s?:
   - Ng?t ng?u nhiên CAN_H/CAN_L trong 5 giây.
   - Inject l?i BMS_ALARM_OVER_CHG_CURR.
3. **Tiêu chí Pass:** H? th?ng t? d?ng tr? v? tr?ng thái Fault/Offline và T? Ð?NG ph?c h?i ch?y l?i khi l?i bi?n m?t, liên t?c trong 72 gi? không treo vòng l?p.

## 3. PC Fuzzing
**Setup:**
- Ch?y `python fuzz_pc.py` qua c?ng USB.

**Th?c hi?n:**
- Script g?i ng?u nhiên các khung d? li?u NaN, Inf, và frame h?ng d?nh d?ng.
- **Tiêu chí Pass:** Firmware ph?n h?i NACK ho?c Ignore, không phát sinh HardFault (do qua th?i gian uptime).
