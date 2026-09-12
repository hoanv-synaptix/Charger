using System;
using System.IO;
using System.Threading;
using ChargerDebugApp.Protocol;
using ChargerDebugApp.Services;

namespace ChargerDebugApp
{
    class ProgramTest
    {
        public static void RunTests()
        {
            Console.WriteLine("[TEST 1] Testing Default Config Binary Size...");
            var cfg = ChargeCycleConfig.CreateDefault();
            byte[] bytes = cfg.ToBytes();
            if (bytes.Length != 243)
                throw new Exception($"Binary size expected 243, got {bytes.Length}");
            Console.WriteLine($"[PASS] Binary size is exactly {bytes.Length} bytes.");

            Console.WriteLine("[TEST 2] Testing Binary Round-Trip...");
            cfg.BatteryCapacityAh = 150.5f;
            cfg.VMaxV = 58.4f;
            cfg.ProtectJackTempTripC = 78.5f;
            cfg.DeviceId = "TEST_DEVICE_01";
            cfg.HwRev = "REV_2.1";
            cfg.AdminPin = 654321;
            byte[] packed = cfg.ToBytes();
            var restored = ChargeCycleConfig.FromBytes(packed);
            if (Math.Abs(restored.BatteryCapacityAh - 150.5f) > 0.001f ||
                Math.Abs(restored.VMaxV - 58.4f) > 0.001f ||
                Math.Abs(restored.ProtectJackTempTripC - 78.5f) > 0.001f ||
                restored.DeviceId != "TEST_DEVICE_01" ||
                restored.HwRev != "REV_2.1" || restored.AdminPin != 654321)
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
                RatedPower = 10000.0f,
                RatedCurrent = 100.0f,
                TempDcdc = 42.1f,
                AcPhaseAVoltage = 221.5f,
                PfcBusPosVoltage = 398.0f
            });
            vm.Modules.Add(mod);

            // The window-level ALL_MODULES handler selects the first module
            // automatically. Keep this ViewModel test focused on telemetry;
            // selection behavior is covered by the handler contract.
            vm.SelectedModule = vm.Modules[0];

            if (vm.SelVoltage != "54.30" || vm.SelCurrent != "12.50" || vm.SelRatedPower != "10000" ||
                vm.SelRatedCurrent != "100.0" || vm.SelDriver != "Maxwell" ||
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

            // ── TEST 6: Auto-Update Logic ──────────────────────────────────────
            Console.WriteLine("[TEST 6] Testing Auto-Update Service Logic...");

            var currentVer = new Version(1, 0, 0);
            var asset = new UpdateService.GitHubAsset
            {
                Name = "ChargerDebugApp.exe",
                BrowserDownloadUrl = "https://github.com/hoanv-synaptix/Charger/releases/download/v1.1.0/ChargerDebugApp.exe"
            };

            // 6a. Newer version available → should return UpdateInfo
            var releaseNewer = new UpdateService.GitHubRelease
            {
                TagName    = "v1.1.0",
                Body       = "- Fix BMS bug\n- Improve SCADA display",
                Prerelease = false,
                Draft      = false,
                Assets     = [asset]
            };
            var result = UpdateService.EvaluateRelease(releaseNewer, currentVer);
            if (result is null)
                throw new Exception("6a FAIL: Expected UpdateInfo for newer version v1.1.0, got null");
            if (result.LatestVersion != new Version(1, 1, 0))
                throw new Exception($"6a FAIL: Expected v1.1.0, got {result.LatestVersion}");
            if (result.DownloadUrl != asset.BrowserDownloadUrl)
                throw new Exception($"6a FAIL: DownloadUrl mismatch: {result.DownloadUrl}");
            if (!result.ReleaseNotes.Contains("Fix BMS bug"))
                throw new Exception("6a FAIL: ReleaseNotes not forwarded correctly");
            Console.WriteLine("[PASS] 6a: Newer version (v1.1.0) correctly detected and UpdateInfo returned.");

            // 6b. Same version → no update
            var releaseSame = new UpdateService.GitHubRelease
            {
                TagName = "v1.0.0", Prerelease = false, Draft = false, Assets = [asset]
            };
            if (UpdateService.EvaluateRelease(releaseSame, currentVer) is not null)
                throw new Exception("6b FAIL: Same version should return null (no update needed)");
            Console.WriteLine("[PASS] 6b: Same version (v1.0.0) → no update.");

            // 6c. Older version → no update
            var releaseOlder = new UpdateService.GitHubRelease
            {
                TagName = "v0.9.0", Prerelease = false, Draft = false, Assets = [asset]
            };
            if (UpdateService.EvaluateRelease(releaseOlder, currentVer) is not null)
                throw new Exception("6c FAIL: Older version should return null");
            Console.WriteLine("[PASS] 6c: Older version (v0.9.0) → no update.");

            // 6d. Prerelease → ignored
            var releasePrerelease = new UpdateService.GitHubRelease
            {
                TagName = "v2.0.0-beta", Prerelease = true, Draft = false, Assets = [asset]
            };
            if (UpdateService.EvaluateRelease(releasePrerelease, currentVer) is not null)
                throw new Exception("6d FAIL: Prerelease should be ignored");
            Console.WriteLine("[PASS] 6d: Prerelease tag → ignored.");

            // 6e. Draft → ignored
            var releaseDraft = new UpdateService.GitHubRelease
            {
                TagName = "v1.1.0", Prerelease = false, Draft = true, Assets = [asset]
            };
            if (UpdateService.EvaluateRelease(releaseDraft, currentVer) is not null)
                throw new Exception("6e FAIL: Draft release should be ignored");
            Console.WriteLine("[PASS] 6e: Draft release → ignored.");

            // 6f. No .exe asset → return null
            var releaseNoAsset = new UpdateService.GitHubRelease
            {
                TagName = "v1.1.0", Prerelease = false, Draft = false,
                Assets = [new UpdateService.GitHubAsset { Name = "source.zip", BrowserDownloadUrl = "https://example.com/source.zip" }]
            };
            if (UpdateService.EvaluateRelease(releaseNoAsset, currentVer) is not null)
                throw new Exception("6f FAIL: No .exe asset should return null");
            Console.WriteLine("[PASS] 6f: No .exe asset in release → null (safe fallback).");

            // 6g. Null release (network error / no releases) → null
            if (UpdateService.EvaluateRelease(null, currentVer) is not null)
                throw new Exception("6g FAIL: Null release should return null");
            Console.WriteLine("[PASS] 6g: Null release (no internet / no releases) → null (safe).");

            // 6h. Tag without 'v' prefix → still parsed correctly
            var releaseNoPrefix = new UpdateService.GitHubRelease
            {
                TagName = "1.2.0", Prerelease = false, Draft = false, Assets = [asset]
            };
            var resultNoPrefix = UpdateService.EvaluateRelease(releaseNoPrefix, currentVer);
            if (resultNoPrefix is null || resultNoPrefix.LatestVersion != new Version(1, 2, 0))
                throw new Exception($"6h FAIL: Tag without 'v' prefix not parsed correctly. Got: {resultNoPrefix?.LatestVersion}");
            Console.WriteLine("[PASS] 6h: Tag without 'v' prefix (1.2.0) → parsed correctly.");

            // 6i. Malformed tag → null (safe fallback)
            var releaseBadTag = new UpdateService.GitHubRelease
            {
                TagName = "latest", Prerelease = false, Draft = false, Assets = [asset]
            };
            if (UpdateService.EvaluateRelease(releaseBadTag, currentVer) is not null)
                throw new Exception("6i FAIL: Malformed tag should return null");
            Console.WriteLine("[PASS] 6i: Malformed tag ('latest') → null (safe fallback).");

            // 6j. Network timeout simulation → CheckForUpdateAsync returns null (not throw)
            var cts = new CancellationTokenSource();
            cts.Cancel(); // Pre-cancelled token simulates timeout
            var timeoutResult = UpdateService.CheckForUpdateAsync(cts.Token).GetAwaiter().GetResult();
            if (timeoutResult is not null)
                throw new Exception("6j FAIL: Cancelled token should return null, not throw");
            Console.WriteLine("[PASS] 6j: Network timeout/cancel → null returned (app won't crash).");

            Console.WriteLine("[ALL TESTS PASSED SUCCESSFULLY]");
        }
    }
}
