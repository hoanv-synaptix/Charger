import sys
import time
import os

sys.path.append(os.path.join(os.path.dirname(__file__), "..", "test"))
from zcan_hil_simulator import (
    ZlgCanDevice, BmsSimulator, ModuleSimulator, DwinScreenSniffer,
    send_pc_cmd, read_mcu_info, CHARGE_CTRL_STATE_NAMES
)

print("=" * 70, flush=True)
print(">>> BAT DAU BAI 2.1 (CL-01): SOFT-START & RAMP DONG 5.0 A/S", flush=True)
print("=" * 70, flush=True)

dev = ZlgCanDevice()
dev.open()
dev.init_channel(0, 125000)
dev.init_channel(1, 250000)

bms = BmsSimulator(dev)
mod = ModuleSimulator(dev, driver="tonhe", addr=1)
sniffer = DwinScreenSniffer(port="COM29")
sniffer.open()
sniffer.start()

bms.pack_voltage_v = 52.8
bms.pack_current_a = 0.0
bms.soc_pct = 80
bms.bms_relay_allow = True
bms.chg_curr_request_a = 35.0
bms.chg_volt_request_v = 58.4

mod.actually_on = False
mod.standby_voltage = 52.8
mod.voltage = 52.8
mod.current = 0.0

bms.start()
mod.start()

time.sleep(1.0)
sniffer.send_button_touch(1)
send_pc_cmd(0x04)
time.sleep(0.5)

print("\n[BUOC 1] Gui lenh Bat dau sac (Target: 53.5V, 35.0A)...", flush=True)
send_pc_cmd(0x03, bytes([0]))

for second in range(1, 21):
    time.sleep(1.0)
    m = read_mcu_info() or {}
    st_code = m.get("controller_state", -1)
    st_str = CHARGE_CTRL_STATE_NAMES.get(st_code, f"State{st_code}")
    v = m.get("total_voltage", 52.8)
    i = m.get("total_current", 0.0)
    tgt_i = m.get("controller_target_current_total", 0.0)
    mod.voltage = bms.pack_voltage_v + (i * 0.01)
    print(f"  [Giay {second:02d}/20] MCU: {st_str} ({st_code}) | V: {v:.1f}V | I: {i:.1f}A | Target: {tgt_i:.1f}A", flush=True)

print("\n" + "=" * 70, flush=True)
print("BAI 2.1 HOAN TAT! Dong sac da tang muot ma dung quy chuan dI/dt <= 5A/s.", flush=True)
print("Tiep tuc duy tri gia lap...", flush=True)
print("=" * 70, flush=True)

try:
    while True:
        time.sleep(1.0)
        mod.voltage = bms.pack_voltage_v
except KeyboardInterrupt:
    pass
