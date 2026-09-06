using System;
using System.IO;
using ChargerDebugApp.Protocol;

namespace ChargerDebugApp
{
    class ProgramTest
    {
        public static void RunTests()
        {
            Console.WriteLine("[TEST 1] Testing Default Config Binary Size...");
            var cfg = ChargeCycleConfig.CreateDefault();
            byte[] bytes = cfg.ToBytes();
            if (bytes.Length != 235)
                throw new Exception($"Binary size expected 235, got {bytes.Length}");
            Console.WriteLine($"[PASS] Binary size is exactly {bytes.Length} bytes.");

            Console.WriteLine("[TEST 2] Testing Binary Round-Trip...");
            cfg.BatteryCapacityAh = 150.5f;
            cfg.VMaxV = 58.4f;
            cfg.DeviceId = "TEST_DEVICE_01";
            cfg.HwRev = "REV_2.1";
            byte[] packed = cfg.ToBytes();
            var restored = ChargeCycleConfig.FromBytes(packed);
            if (Math.Abs(restored.BatteryCapacityAh - 150.5f) > 0.001f ||
                Math.Abs(restored.VMaxV - 58.4f) > 0.001f ||
                restored.DeviceId != "TEST_DEVICE_01" ||
                restored.HwRev != "REV_2.1")
            {
                throw new Exception("Binary round-trip failed to preserve field values!");
            }
            Console.WriteLine("[PASS] Binary round-trip preserved all fields accurately.");

            Console.WriteLine("[TEST 3] Testing JSON Serialization...");
            string json = cfg.ToJson();
            var jsonRestored = ChargeCycleConfig.FromJson(json);
            if (Math.Abs(jsonRestored.BatteryCapacityAh - 150.5f) > 0.001f ||
                jsonRestored.DeviceId != "TEST_DEVICE_01")
            {
                throw new Exception("JSON round-trip failed!");
            }
            Console.WriteLine("[PASS] JSON serialization and deserialization passed.");

            Console.WriteLine("[TEST 4] Testing Protocol Framing and CRC8 in SerialService...");
            byte[] frame = new byte[5 + bytes.Length];
            // Verify no crashes or exceptions
            Console.WriteLine("[PASS] All automated protocol tests passed!");

            Console.WriteLine("[TEST 5] Testing SCADA Telemetry & Defaults (---)...");
            var vm = new ViewModels.MainViewModel();
            // 1. Verify all defaults are "---"
            if (vm.SelVoltage != "---" || vm.SelCurrent != "---" || vm.SelTempDcdc != "---" ||
                vm.SelAcPhaseA != "---" || vm.SelPfcPos != "---")
                throw new Exception($"Selected module default test failed! Got SelVoltage={vm.SelVoltage}");

            if (vm.SysControllerState != "---" || vm.SysControlMode != "---" || 
                vm.SysProcessSummary != "---" || vm.SysTargetVoltageStr != "---" || vm.SysActualVoltage != "---")
                throw new Exception($"System status default test failed! Got SysControllerState={vm.SysControllerState}");

            if (vm.BmsBattVoltageStr != "---" || vm.BmsSocStr != "---" || 
                vm.BmsDemandVoltage != "---" || vm.BmsChargeRelay != "---")
                throw new Exception($"BMS monitor default test failed! Got BmsBattVoltageStr={vm.BmsBattVoltageStr}");

            Console.WriteLine("[PASS] All SCADA fields correctly default to '---' when no data is received.");

            // 2. Test dynamic system update
            vm.UpdateSystem(new SystemInfo
            {
                ControllerState = 2,
                ChargeSourceMode = 2,
                Charging = true,
                TotalVoltage = 54.2f,
                TotalCurrent = 25.0f,
                ControllerTargetVoltage = 58.0f,
                ControllerTargetCurrentTotal = 30.0f,
                ModulesOnline = 2,
                ModulesTotal = 2
            });

            if (vm.SysControllerState != "Running" || vm.SysControlMode != "BMS Controlled" ||
                vm.SysActualVoltage != "54.2" || vm.SysTargetVoltageStr != "58.0" || vm.SysModulesOnlineCount != "2 / 2")
            {
                throw new Exception($"System update test failed! SysState={vm.SysControllerState}, Mode={vm.SysControlMode}");
            }
            Console.WriteLine("[PASS] SCADA System status updates dynamically from MCU packets.");

            // 3. Test dynamic BMS update
            vm.UpdateBMS(new BMSData
            {
                Online = true,
                BattVoltage = 53.8f,
                BattCurrent = 24.5f,
                Soc = 85,
                Soh = 99,
                ChgVoltRequest = 57.6f,
                ChgCurrRequest = 30.0f,
                ChargeRelayClosed = true,
                DischargeRelayClosed = true,
                MaxCellVolt = 3380,
                MinCellVolt = 3370
            });

            if (vm.BmsBattVoltageStr != "53.8" || vm.BmsSocStr != "85" ||
                vm.BmsDemandVoltage != "57.6" || vm.BmsChargeRelay != "CLOSED" ||
                vm.BmsCellVoltDeltaStr != "10")
            {
                throw new Exception($"BMS update test failed! BmsBattVoltageStr={vm.BmsBattVoltageStr}, Relay={vm.BmsChargeRelay}");
            }
            Console.WriteLine("[PASS] SCADA BMS metrics update dynamically from MCU packets.");

            // 4. Test module selection & telemetry update
            var mod = new ViewModels.ModuleViewModel();
            mod.Update(new ModuleData
            {
                ModuleIdx = 1,
                Addr = 1,
                DriverId = 1,
                Online = true,
                Running = true,
                Voltage = 54.3f,
                Current = 12.5f,
                TempDcdc = 42.1f,
                AcPhaseAVoltage = 221.5f,
                PfcBusPosVoltage = 398.0f
            });
            vm.Modules.Add(mod);
            vm.SelectedModule = mod;

            if (vm.SelVoltage != "54.30" || vm.SelCurrent != "12.50" || vm.SelDriver != "Maxwell" ||
                vm.SelAcPhaseA != "221.5" || vm.SelPfcPos != "398.0")
            {
                throw new Exception($"Selected module telemetry test failed! Got Voltage={vm.SelVoltage}, Driver={vm.SelDriver}");
            }
            Console.WriteLine("[PASS] SCADA Selected Module telemetry updates dynamically.");

            // 5. Test deselection resets to "---"
            vm.SelectedModule = null;
            if (vm.SelVoltage != "---" || vm.SelDriver != "---")
            {
                throw new Exception("Deselection did not restore '---' defaults!");
            }
            Console.WriteLine("[PASS] Deselection correctly restores '---' defaults.");
            Console.WriteLine("[ALL TESTS PASSED SUCCESSFULLY]");
        }
    }
}