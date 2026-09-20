using System;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace ChargerDebugApp.Protocol
{
    public class ChargeCycleConfig
    {
        public const int EXPECTED_BINARY_SIZE = 253;
        public const int V7_BINARY_SIZE = 249;
        public const int V6_BINARY_SIZE = 243;
        public const int V5_BINARY_SIZE = 239;

        // 1. Version
        [JsonPropertyName("version")]
        public ushort Version { get; set; } = 8;

        // 2. General Limits (10 floats = 40 bytes)
        [JsonPropertyName("battery_capacity_ah")]
        public float BatteryCapacityAh { get; set; } = 100.0f;

        [JsonPropertyName("imin_c")]
        public float IMinC { get; set; } = 0.1f;

        [JsonPropertyName("imax_c")]
        public float IMaxC { get; set; } = 1.0f;

        [JsonPropertyName("ipre_c")]
        public float IPreC { get; set; } = 0.2f;

        [JsonPropertyName("ilow_c")]
        public float ILowC { get; set; } = 0.5f;

        [JsonPropertyName("vmin_v")]
        public float VMinV { get; set; } = 32.0f;

        [JsonPropertyName("vmax_v")]
        public float VMaxV { get; set; } = 53.0f;

        [JsonPropertyName("vpre_v")]
        public float VPreV { get; set; } = 48.0f;

        [JsonPropertyName("vlow_v")]
        public float VLowV { get; set; } = 52.0f;

        [JsonPropertyName("temp_limit_c")]
        public float TempLimitC { get; set; } = 55.0f;

        [JsonPropertyName("cell_volt_enabled")]
        public bool CellVoltEnabled { get; set; } = true;

        [JsonPropertyName("cell_volt_delta_t_s")]
        public float CellVoltDeltaTS { get; set; } = 3.0f;

        [JsonIgnore]
        public float CellVoltDeltaV
        {
            get => CellVoltDeltaTS;
            set => CellVoltDeltaTS = value;
        }

        [JsonPropertyName("cell_volt_1_v")]
        public float CellVolt1V { get; set; } = 3.2f;

        [JsonPropertyName("cell_volt_2_v")]
        public float CellVolt2V { get; set; } = 3.3f;

        [JsonPropertyName("cell_volt_3_v")]
        public float CellVolt3V { get; set; } = 3.4f;

        [JsonPropertyName("cell_volt_4_v")]
        public float CellVolt4V { get; set; } = 3.45f;

        [JsonPropertyName("cell_volt_5_v")]
        public float CellVolt5V { get; set; } = 3.53f;

        [JsonPropertyName("cell_curr_1_c")]
        public float CellCurr1C { get; set; } = 0.35f;

        [JsonPropertyName("cell_curr_2_c")]
        public float CellCurr2C { get; set; } = 0.35f;

        [JsonPropertyName("cell_curr_3_c")]
        public float CellCurr3C { get; set; } = 0.25f;

        [JsonPropertyName("cell_curr_4_c")]
        public float CellCurr4C { get; set; } = 0.15f;

        // 4. Temperature Stages (1 + 4 + 20 + 16 = 41 bytes)
        [JsonPropertyName("temp_enabled")]
        public bool TempEnabled { get; set; } = true;

        [JsonPropertyName("temp_delta_c")]
        public float TempDeltaC { get; set; } = 1.0f;

        [JsonPropertyName("temp_1_c")]
        public float Temp1C { get; set; } = 10.0f;

        [JsonPropertyName("temp_2_c")]
        public float Temp2C { get; set; } = 40.0f;

        [JsonPropertyName("temp_3_c")]
        public float Temp3C { get; set; } = 45.0f;

        [JsonPropertyName("temp_4_c")]
        public float Temp4C { get; set; } = 50.0f;

        [JsonPropertyName("temp_5_c")]
        public float Temp5C { get; set; } = 55.0f;

        [JsonPropertyName("temp_curr_1_c")]
        public float TempCurr1C { get; set; } = 0.35f;

        [JsonPropertyName("temp_curr_2_c")]
        public float TempCurr2C { get; set; } = 0.25f;

        [JsonPropertyName("temp_curr_3_c")]
        public float TempCurr3C { get; set; } = 0.15f;

        [JsonPropertyName("temp_curr_4_c")]
        public float TempCurr4C { get; set; } = 0.1f;

        // 5. SOC Stages (1 + 4 + 20 + 16 = 41 bytes)
        [JsonPropertyName("soc_enabled")]
        public bool SocEnabled { get; set; } = false;

        [JsonPropertyName("soc_delta_t_s")]
        public float SocDeltaTS { get; set; } = 2.0f;

        [JsonIgnore]
        public float SocDeltaPct
        {
            get => SocDeltaTS;
            set => SocDeltaTS = value;
        }

        [JsonPropertyName("soc_1_pct")]
        public float Soc1Pct { get; set; } = 20.0f;

        [JsonPropertyName("soc_2_pct")]
        public float Soc2Pct { get; set; } = 40.0f;

        [JsonPropertyName("soc_3_pct")]
        public float Soc3Pct { get; set; } = 60.0f;

        [JsonPropertyName("soc_4_pct")]
        public float Soc4Pct { get; set; } = 80.0f;

        [JsonPropertyName("soc_5_pct")]
        public float Soc5Pct { get; set; } = 95.0f;

        [JsonPropertyName("soc_curr_1_c")]
        public float SocCurr1C { get; set; } = 1.0f;

        [JsonPropertyName("soc_curr_2_c")]
        public float SocCurr2C { get; set; } = 0.8f;

        [JsonPropertyName("soc_curr_3_c")]
        public float SocCurr3C { get; set; } = 0.5f;

        [JsonPropertyName("soc_curr_4_c")]
        public float SocCurr4C { get; set; } = 0.3f;

        // 6. Charge Jack Protection (1 + 4 + 2 = 7 bytes)
        [JsonPropertyName("protect_jack_charge_enabled")]
        public bool ProtectJackChargeEnabled { get; set; } = true;

        [JsonPropertyName("protect_jack_charge_delta_v")]
        public float ProtectJackChargeDeltaV { get; set; } = 60.0f;

        [JsonPropertyName("protect_jack_charge_delay_s")]
        public ushort ProtectJackChargeDelayS { get; set; } = 60;

        // 7. Jack Temperature Protection (1 + 2 + 4 + 4 + 4 = 15 bytes)
        [JsonPropertyName("protect_jack_temp_enabled")]
        public bool ProtectJackTempEnabled { get; set; } = true;

        [JsonPropertyName("protect_jack_temp_delay_s")]
        public ushort ProtectJackTempDelayS { get; set; } = 5;

        [JsonPropertyName("protect_jack_temp_threshold_c")]
        public float ProtectJackTempThresholdC { get; set; } = 60.0f;

        [JsonPropertyName("protect_jack_temp_delta_c")]
        public float ProtectJackTempDeltaC { get; set; } = 5.0f;

        [JsonPropertyName("protect_jack_temp_power_limit_pct")]
        public float ProtectJackTempPowerLimitPct { get; set; } = 50.0f;

        [JsonPropertyName("protect_jack_temp_trip_c")]
        public float ProtectJackTempTripC { get; set; } = 75.0f;

        // 8. System & Module Hardware (1 + 1 + 1 + 1 + 4*4 = 20 bytes)
        [JsonPropertyName("charge_source_mode")]
        public byte ChargeSourceMode { get; set; } = 0; // 0=BMS, 1=No BMS

        [JsonPropertyName("can_battery_id")]
        public byte CanBatteryId { get; set; } = 1;

        [JsonPropertyName("source_module_count")]
        public byte SourceModuleCount { get; set; } = 1;

        [JsonPropertyName("module_type")]
        public byte ModuleType { get; set; } = 4; // 1=EVR, 2=Maxwell, 3=Lianming, 4=TonHe

        [JsonPropertyName("module_u_min_v")]
        public float ModuleUMinV { get; set; } = 30.0f;

        [JsonPropertyName("module_u_max_v")]
        public float ModuleUMaxV { get; set; } = 60.0f;

        [JsonPropertyName("module_i_min_a")]
        public float ModuleIMinA { get; set; } = 5.0f;

        [JsonPropertyName("module_i_max_a")]
        public float ModuleIMaxA { get; set; } = 100.0f;

        // 9. Identity v4 (16 + 12 = 28 bytes)
        [JsonPropertyName("device_id")]
        public string DeviceId { get; set; } = "PKG-0001";

        [JsonPropertyName("hw_rev")]
        public string HwRev { get; set; } = "V1.0.0";

        // 10. v6 pre-charge authorization PIN (app/Flash canonical config)
        [JsonPropertyName("admin_pin")]
        public uint AdminPin { get; set; } = 123456;

        // 11. v7 DWIN Charge Mode & Delay Start
        [JsonPropertyName("charge_mode")]
        public byte ChargeMode { get; set; } = 1; // 0=FAST, 1=NORMAL

        [JsonPropertyName("delay_enabled")]
        public bool DelayEnabled { get; set; } = false;

        [JsonPropertyName("delay_hours")]
        public ushort DelayHours { get; set; } = 2;

        [JsonPropertyName("delay_minutes")]
        public ushort DelayMinutes { get; set; } = 30;

        // 12. v8 Maximum Charge Current in Amperes
        [JsonPropertyName("imax_a")]
        public float IMaxA { get; set; } = 100.0f;

        public static ChargeCycleConfig CreateDefault()
        {
            return new ChargeCycleConfig();
        }

        public byte[] ToBytes()
        {
            using var ms = new MemoryStream(EXPECTED_BINARY_SIZE);
            using var writer = new BinaryWriter(ms, Encoding.ASCII);

            // Version (2)
            writer.Write(Version);

            // General Limits (40)
            writer.Write(BatteryCapacityAh);
            writer.Write(IMinC);
            writer.Write(IMaxC);
            writer.Write(IPreC);
            writer.Write(ILowC);
            writer.Write(VMinV);
            writer.Write(VMaxV);
            writer.Write(VPreV);
            writer.Write(VLowV);
            writer.Write(TempLimitC);

            // Cell Volt Stages (41)
            writer.Write((byte)(CellVoltEnabled ? 1 : 0));
            writer.Write(CellVoltDeltaTS);
            writer.Write(CellVolt1V);
            writer.Write(CellVolt2V);
            writer.Write(CellVolt3V);
            writer.Write(CellVolt4V);
            writer.Write(CellVolt5V);
            writer.Write(CellCurr1C);
            writer.Write(CellCurr2C);
            writer.Write(CellCurr3C);
            writer.Write(CellCurr4C);

            // Temperature Stages (41)
            writer.Write((byte)(TempEnabled ? 1 : 0));
            writer.Write(TempDeltaC);
            writer.Write(Temp1C);
            writer.Write(Temp2C);
            writer.Write(Temp3C);
            writer.Write(Temp4C);
            writer.Write(Temp5C);
            writer.Write(TempCurr1C);
            writer.Write(TempCurr2C);
            writer.Write(TempCurr3C);
            writer.Write(TempCurr4C);

            // SOC Stages (41)
            writer.Write((byte)(SocEnabled ? 1 : 0));
            writer.Write(SocDeltaTS);
            writer.Write(Soc1Pct);
            writer.Write(Soc2Pct);
            writer.Write(Soc3Pct);
            writer.Write(Soc4Pct);
            writer.Write(Soc5Pct);
            writer.Write(SocCurr1C);
            writer.Write(SocCurr2C);
            writer.Write(SocCurr3C);
            writer.Write(SocCurr4C);

            // Jack Charge Protect (7)
            writer.Write((byte)(ProtectJackChargeEnabled ? 1 : 0));
            writer.Write(ProtectJackChargeDeltaV);
            writer.Write(ProtectJackChargeDelayS);

            // Jack Temp Protect (19)
            writer.Write((byte)(ProtectJackTempEnabled ? 1 : 0));
            writer.Write(ProtectJackTempDelayS);
            writer.Write(ProtectJackTempThresholdC);
            writer.Write(ProtectJackTempDeltaC);
            writer.Write(ProtectJackTempPowerLimitPct);
            writer.Write(ProtectJackTempTripC);

            // Hardware / Module (20)
            writer.Write(ChargeSourceMode);
            writer.Write(CanBatteryId);
            writer.Write(SourceModuleCount);
            writer.Write(ModuleType);
            writer.Write(ModuleUMinV);
            writer.Write(ModuleUMaxV);
            writer.Write(ModuleIMinA);
            writer.Write(ModuleIMaxA);

            // Identity v4 (16 + 12 = 28)
            byte[] devBytes = new byte[16];
            if (!string.IsNullOrEmpty(DeviceId))
            {
                byte[] raw = Encoding.ASCII.GetBytes(DeviceId);
                Array.Copy(raw, devBytes, Math.Min(raw.Length, 16));
            }
            writer.Write(devBytes);

            byte[] hwBytes = new byte[12];
            if (!string.IsNullOrEmpty(HwRev))
            {
                byte[] raw = Encoding.ASCII.GetBytes(HwRev);
                Array.Copy(raw, hwBytes, Math.Min(raw.Length, 12));
            }
            writer.Write(hwBytes);

            // v6 admin PIN (u32, appended after the v5 payload)
            writer.Write(AdminPin);

            // v7 charge mode & delay (6 bytes)
            writer.Write(ChargeMode);
            writer.Write((byte)(DelayEnabled ? 1 : 0));
            writer.Write(DelayHours);
            writer.Write(DelayMinutes);

            // v8 imax_a (4 bytes)
            writer.Write(IMaxA);

            return ms.ToArray();
        }

        public static ChargeCycleConfig FromBytes(byte[] data)
        {
            if (data == null || data.Length < EXPECTED_BINARY_SIZE)
            {
                throw new ArgumentException($"Data length must be at least {EXPECTED_BINARY_SIZE} bytes, received {data?.Length ?? 0}");
            }

            using var ms = new MemoryStream(data);
            using var reader = new BinaryReader(ms, Encoding.ASCII);

            var cfg = new ChargeCycleConfig
            {
                Version = reader.ReadUInt16(),

                BatteryCapacityAh = reader.ReadSingle(),
                IMinC = reader.ReadSingle(),
                IMaxC = reader.ReadSingle(),
                IPreC = reader.ReadSingle(),
                ILowC = reader.ReadSingle(),
                VMinV = reader.ReadSingle(),
                VMaxV = reader.ReadSingle(),
                VPreV = reader.ReadSingle(),
                VLowV = reader.ReadSingle(),
                TempLimitC = reader.ReadSingle(),

                CellVoltEnabled = reader.ReadByte() != 0,
                CellVoltDeltaTS = reader.ReadSingle(),
                CellVolt1V = reader.ReadSingle(),
                CellVolt2V = reader.ReadSingle(),
                CellVolt3V = reader.ReadSingle(),
                CellVolt4V = reader.ReadSingle(),
                CellVolt5V = reader.ReadSingle(),
                CellCurr1C = reader.ReadSingle(),
                CellCurr2C = reader.ReadSingle(),
                CellCurr3C = reader.ReadSingle(),
                CellCurr4C = reader.ReadSingle(),

                TempEnabled = reader.ReadByte() != 0,
                TempDeltaC = reader.ReadSingle(),
                Temp1C = reader.ReadSingle(),
                Temp2C = reader.ReadSingle(),
                Temp3C = reader.ReadSingle(),
                Temp4C = reader.ReadSingle(),
                Temp5C = reader.ReadSingle(),
                TempCurr1C = reader.ReadSingle(),
                TempCurr2C = reader.ReadSingle(),
                TempCurr3C = reader.ReadSingle(),
                TempCurr4C = reader.ReadSingle(),

                SocEnabled = reader.ReadByte() != 0,
                SocDeltaTS = reader.ReadSingle(),
                Soc1Pct = reader.ReadSingle(),
                Soc2Pct = reader.ReadSingle(),
                Soc3Pct = reader.ReadSingle(),
                Soc4Pct = reader.ReadSingle(),
                Soc5Pct = reader.ReadSingle(),
                SocCurr1C = reader.ReadSingle(),
                SocCurr2C = reader.ReadSingle(),
                SocCurr3C = reader.ReadSingle(),
                SocCurr4C = reader.ReadSingle(),

                ProtectJackChargeEnabled = reader.ReadByte() != 0,
                ProtectJackChargeDeltaV = reader.ReadSingle(),
                ProtectJackChargeDelayS = reader.ReadUInt16(),

                ProtectJackTempEnabled = reader.ReadByte() != 0,
                ProtectJackTempDelayS = reader.ReadUInt16(),
                ProtectJackTempThresholdC = reader.ReadSingle(),
                ProtectJackTempDeltaC = reader.ReadSingle(),
                ProtectJackTempPowerLimitPct = reader.ReadSingle(),
                ProtectJackTempTripC = reader.ReadSingle(),

                ChargeSourceMode = reader.ReadByte(),
                CanBatteryId = reader.ReadByte(),
                SourceModuleCount = reader.ReadByte(),
                ModuleType = reader.ReadByte(),
                ModuleUMinV = reader.ReadSingle(),
                ModuleUMaxV = reader.ReadSingle(),
                ModuleIMinA = reader.ReadSingle(),
                ModuleIMaxA = reader.ReadSingle()
            };

            byte[] devRaw = reader.ReadBytes(16);
            int devLen = Array.IndexOf(devRaw, (byte)0);
            if (devLen < 0) devLen = 16;
            cfg.DeviceId = Encoding.ASCII.GetString(devRaw, 0, devLen);

            byte[] hwRaw = reader.ReadBytes(12);
            int hwLen = Array.IndexOf(hwRaw, (byte)0);
            if (hwLen < 0) hwLen = 12;
            cfg.HwRev = Encoding.ASCII.GetString(hwRaw, 0, hwLen);
            cfg.AdminPin = reader.ReadUInt32();

            // v7: if remaining bytes >= 6, read charge mode & delay fields
            if (ms.Length - ms.Position >= 6)
            {
                cfg.ChargeMode = reader.ReadByte();
                cfg.DelayEnabled = reader.ReadByte() != 0;
                cfg.DelayHours = reader.ReadUInt16();
                cfg.DelayMinutes = reader.ReadUInt16();
            }

            // v8: if remaining bytes >= 4, read imax_a
            if (ms.Length - ms.Position >= 4)
            {
                cfg.IMaxA = reader.ReadSingle();
            }

            return cfg;
        }

        public string ToJson()
        {
            var options = new JsonSerializerOptions { WriteIndented = true };
            return JsonSerializer.Serialize(this, options);
        }

        public static ChargeCycleConfig FromJson(string json)
        {
            var options = new JsonSerializerOptions { PropertyNameCaseInsensitive = true };
            return JsonSerializer.Deserialize<ChargeCycleConfig>(json, options) ?? new ChargeCycleConfig();
        }

        public ChargeCycleConfig Clone()
        {
            return FromJson(ToJson());
        }
    }
}
