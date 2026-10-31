import sys, time
sys.path.append('Charger/test')
from zcan_hil_simulator import *

def main():
    print("=" * 80)
    print("  CHUONG TRINH KIEM THU LIVE HIL TOAN DIEN TREN PHAN CUNG THAT")
    print("  MCU STM32G0 (COM26) <-> ZLG USBCAN2 (CAN1 TonHe 125k, CAN2 BMS 250k)")
    print("=" * 80)

    dev = ZlgCanDevice()
    dev.open()
    dev.init_channel(0, 125000)
    dev.init_channel(1, 250000)
    print("[1/2] Da ket noi va khoi tao ZLG USBCAN2 thanh cong.")

    bms = BmsSimulator(dev)
    mod = ModuleSimulator(dev, driver="tonhe", addr=1, bms=bms)

    bms.pack_voltage_v = 52.8
    bms.max_cell_mv = 3300
    bms.min_cell_mv = 3290
    bms.soc_pct = 50
    bms.bms_relay_allow = True
    bms.chg_curr_request_a = 30.0
    bms.chg_volt_request_v = 58.4

    bms.transmitting = True
    mod.transmitting = True
    bms.start()
    mod.start()

    time.sleep(1.0)
    send_pc_cmd(0x04, port="COM26") # STOP
    send_pc_cmd(0x0A, port="COM26") # RESET_FAULT
    time.sleep(0.5)

    inf = read_mcu_info("COM26")
    print(f"[2/2] MCU Telemetry: FW={inf.get('fw_major')}.{inf.get('fw_minor')}.{inf.get('fw_patch')} | "
          f"State={inf.get('controller_state')} ({CHARGE_CTRL_STATE_NAMES.get(inf.get('controller_state'))}) | "
          f"Modules Online={inf.get('modules_online')}/1")

    results = []

    # =========================================================================
    # SUITE 1: CHARGING LOGIC & RAMP UP
    # =========================================================================
    print("\n" + "=" * 80)
    print(">>> [SUITE 1] CHARGING FLOW & RAMP-UP TEST")
    print("=" * 80)
    send_pc_cmd(0x03, bytes([0]), port="COM26") # Start
    time.sleep(0.5)

    currents = []
    for s in range(1, 6):
        time.sleep(1.0)
        m = read_mcu_info("COM26")
        act_i = m.get("total_current", 0.0)
        currents.append(act_i)
        print(f"  t={s}s | State={m.get('controller_state')} | V={m.get('total_voltage'):.1f}V | I={act_i:.1f}A | Fault={hex(m.get('controller_fault_flags', 0))}")

    p_suite1 = (currents[-1] >= 20.0)
    print(f"  ==> KET QUA SUITE 1: {'[PASS]' if p_suite1 else '[FAIL]'} (Dong max dat: {currents[-1]:.1f}A)")
    results.append(("Suite 1: Charging Flow & Current Ramp-up", p_suite1))

    # =========================================================================
    # SUITE 2: E004 HIGH CELL VOLTAGE & CHATTERING PREVENTION (LOCKOUT TEST)
    # =========================================================================
    print("\n" + "=" * 80)
    print(">>> [SUITE 2] E004 HIGH CELL VOLTAGE FAULT LOCKOUT & ANTI-CHATTERING TEST")
    print("=" * 80)
    print("  [1] Pin nạp gần đầy (Cell vọt lên 3.52V) -> BMS phát cờ E004 (fault_high_cell_volt = 1)...")
    bms.fault_high_cell_volt = 1
    time.sleep(1.0)

    m = read_mcu_info("COM26")
    st_trip = m.get("controller_state")
    fault_trip = m.get("controller_fault_flags", 0)
    print(f"  [2] MCU phan ung: State={st_trip} ({CHARGE_CTRL_STATE_NAMES.get(st_trip)}) | Fault={hex(fault_trip)} | StopReason={m.get('controller_stop_reason')}")

    print("  [3] Dòng sạc ngắt về 0A -> Điện áp cell sụt về an toàn (BMS xóa cờ fault_high_cell_volt = 0)...")
    bms.fault_high_cell_volt = 0
    time.sleep(0.5)

    print("  [4] Theo dõi trong 5 giây: Kiểm tra xem AutoStart có bị kích hoạt sạc lại bậy hay không...")
    restarted = False
    for s in range(1, 6):
        time.sleep(1.0)
        m = read_mcu_info("COM26")
        if m.get("controller_state") in (1, 2, 6):
            restarted = True
            print(f"  [CANH BAO] AutoStart da tu dong bat sac lai tai t={s}s!")
            break
        print(f"  t={s}s: State={m.get('controller_state')} (Khong sac lai - Khoa Lockout hoat dong tot)")

    p_suite2 = (st_trip in (0, 3, 4)) and (not restarted)
    print(f"  ==> KET QUA SUITE 2: {'[PASS]' if p_suite2 else '[FAIL]'} (Triet tieu hoan toan vong lap E004 Chattering)")
    results.append(("Suite 2: E004 Fault Lockout & Anti-Chattering", p_suite2))

    # =========================================================================
    # SUITE 3: UNPLUGGING RESET & RE-START NEW SESSION
    # =========================================================================
    print("\n" + "=" * 80)
    print(">>> [SUITE 3] UNPLUG CABLE RESET & MANUAL RE-START TEST")
    print("=" * 80)
    print("  [1] Rút súng sạc ra khỏi xe (BMS CAN offline 6.0s)...")
    bms.transmitting = False
    time.sleep(6.0)

    print("  [2] Cắm súng sạc vào xe mới (BMS CAN online trở lại)...")
    bms.transmitting = True
    time.sleep(2.0)

    # Clear fault on MCU so it settles to IDLE
    send_pc_cmd(0x0A, port="COM26")
    time.sleep(0.5)

    m = read_mcu_info("COM26")
    print(f"  [3] Trạng thái sẵn sàng cho xe mới: State={m.get('controller_state')} ({CHARGE_CTRL_STATE_NAMES.get(m.get('controller_state'))})")

    # Start new session
    send_pc_cmd(0x03, bytes([0]), port="COM26")
    time.sleep(2.0)
    m = read_mcu_info("COM26")
    p_suite3 = (m.get("controller_state") == 2)
    print(f"  [4] Xe mới bắt đầu sạc thành công: State={m.get('controller_state')} | I={m.get('total_current'):.1f}A")
    print(f"  ==> KET QUA SUITE 3: {'[PASS]' if p_suite3 else '[FAIL]'}")
    results.append(("Suite 3: Cable Unplug Reset & Re-Start", p_suite3))

    # Clean up
    send_pc_cmd(0x04, port="COM26")
    time.sleep(0.5)
    bms.transmitting = False
    mod.transmitting = False
    dev.close()

    print("\n" + "=" * 80)
    print("  TONG HOP KET QUA KIEM THU LIVE HARDWARE HIL (ZCAN SIMULATOR)")
    print("=" * 80)
    all_pass = True
    for name, p in results:
        status_str = "[PASS]" if p else "[FAIL]"
        print(f"  {status_str} | {name}")
        if not p: all_pass = False
    print("=" * 80)
    if all_pass:
        print("  >>> TOAN BO CAC TEST SUITE HIL DA PASS 100%! HE THONG HOAN TOAN ON DINH! <<<")
    else:
        print("  >>> CO TEST SUITE CHUA DAT! <<<")
    print("=" * 80)

if __name__ == "__main__":
    main()
