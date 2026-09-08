using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace ChargerDebugApp.Protocol
{
    public enum DebugCmd : byte
    {
        ENTER = 0x10,
        EXIT = 0x11,
        READ_ALL = 0x12,
        READ_ONE = 0x13,
        READ_STATS = 0x14,
        WRITE_REG = 0x15,
        SEND_RAW_CAN = 0x16,
        READ_BMS = 0x17,
        GET_SYSTEM = 0x18,
        GET_CHARGE_CFG = 0x19,
        SET_CHARGE_CFG = 0x1A,
        SET_RTC = 0x1D,
        GET_RTC = 0x1E,
        RESET_TOTALS = 0x1F
    }

    public enum DebugRsp : byte
    {
        MODULE_DATA = 0x90,
        ALL_MODULES = 0x91,
        COMM_STATS = 0x92,
        BMS_DATA = 0x93,
        SYSTEM_INFO = 0x94,
        RAW_CAN_TX = 0x95,
        ERROR = 0x96,
        CHARGE_CFG = 0x97,
        RTC = 0x9D
    }

    public class ModuleData
    {
        public byte ModuleIdx;
        public byte DriverId;
        public bool Enabled;
        public bool Online;
        public bool Running;
        public byte State;
        public float Voltage;
        public float Current;
        public float CurrentLimit;
        public float TempDcdc;
        public float TempAmbient;
        public float TempPfc;
        public float AcPhaseAVoltage;
        public float AcPhaseBVoltage;
        public float AcPhaseCVoltage;
        public float PfcBusPosVoltage;
        public float PfcBusNegVoltage;
        public uint InputPower;
        public float RatedPower;
        public float RatedCurrent;
        public uint AlarmStatus;
        public uint AlarmFlags;
        public byte PfcFault;
        public byte Addr;
        public byte Group;
        public uint LastRxTick;
        public uint LastTxTick;
        public uint TxCount;
        public uint RxCount;
        public uint ErrorCount;
        public uint TimeoutCount;
        public uint RecoveryCount;
        public byte VendorDataLen;
        public byte[] VendorData = new byte[21];

        public static ModuleData? FromBytes(byte[] data, int offset = 0)
        {
            if (data.Length - offset < 123) return null;
            using var ms = new MemoryStream(data, offset, 123);
            using var br = new BinaryReader(ms);
            
            var m = new ModuleData();
            m.ModuleIdx = br.ReadByte();
            m.DriverId = br.ReadByte();
            m.Enabled = br.ReadByte() != 0;
            m.Online = br.ReadByte() != 0;
            m.Running = br.ReadByte() != 0;
            m.State = br.ReadByte();
            m.Voltage = br.ReadSingle();
            m.Current = br.ReadSingle();
            m.CurrentLimit = br.ReadSingle();
            m.TempDcdc = br.ReadSingle();
            m.TempAmbient = br.ReadSingle();
            m.TempPfc = br.ReadSingle();
            m.AcPhaseAVoltage = br.ReadSingle();
            m.AcPhaseBVoltage = br.ReadSingle();
            m.AcPhaseCVoltage = br.ReadSingle();
            m.PfcBusPosVoltage = br.ReadSingle();
            m.PfcBusNegVoltage = br.ReadSingle();
            m.InputPower = br.ReadUInt32();
            m.RatedPower = br.ReadSingle();
            m.RatedCurrent = br.ReadSingle();
            m.AlarmStatus = br.ReadUInt32();
            m.AlarmFlags = br.ReadUInt32();
            m.PfcFault = br.ReadByte();
            m.Addr = br.ReadByte();
            m.Group = br.ReadByte();
            m.LastRxTick = br.ReadUInt32();
            m.LastTxTick = br.ReadUInt32();
            m.TxCount = br.ReadUInt32();
            m.RxCount = br.ReadUInt32();
            m.ErrorCount = br.ReadUInt32();
            m.TimeoutCount = br.ReadUInt32();
            m.RecoveryCount = br.ReadUInt32();
            m.VendorDataLen = br.ReadByte();
            m.VendorData = br.ReadBytes(21);
            return m;
        }
    }

    public class BMSData
    {
        public byte State;
        public bool Online;
        public bool ChargeRelayClosed;
        public bool DischargeRelayClosed;
        public float BattVoltage;
        public float BattCurrent;
        public float CapRemain;
        public float RateCap;
        public byte Soc;
        public byte Soh;
        public ushort MaxCellVolt;
        public ushort MinCellVolt;
        public float MaxCellTemp;
        public float MinCellTemp;
        public float ChgVoltRequest;
        public float ChgCurrRequest;
        public uint AlarmFlags;
        public uint LastRxTick;

        public static BMSData? FromBytes(byte[] data)
        {
            if (data.Length < 50) return null;
            using var ms = new MemoryStream(data);
            using var br = new BinaryReader(ms);

            var b = new BMSData();
            b.State = br.ReadByte();
            b.Online = br.ReadByte() != 0;
            b.ChargeRelayClosed = br.ReadByte() != 0;
            b.DischargeRelayClosed = br.ReadByte() != 0;
            b.BattVoltage = br.ReadSingle();
            b.BattCurrent = br.ReadSingle();
            b.CapRemain = br.ReadSingle();
            b.RateCap = br.ReadSingle();
            b.Soc = br.ReadByte();
            b.Soh = br.ReadByte();
            b.MaxCellVolt = br.ReadUInt16();
            b.MinCellVolt = br.ReadUInt16();
            b.MaxCellTemp = br.ReadSingle();
            b.MinCellTemp = br.ReadSingle();
            b.ChgVoltRequest = br.ReadSingle();
            b.ChgCurrRequest = br.ReadSingle();
            b.AlarmFlags = br.ReadUInt32();
            b.LastRxTick = br.ReadUInt32();
            return b;
        }
    }

    public class SystemInfo
    {
        public byte FwMajor;
        public byte FwMinor;
        public byte FwPatch;
        public byte DriverId;
        public byte ModulesTotal;
        public byte ModulesOnline;
        public byte ModulesFault;
        public bool Charging;
        public byte ControllerState;
        public bool ControllerDerating;
        public bool ControllerInhibit;
        public byte ChargeSourceMode;
        public byte ActiveLimitSource;
        public byte ActiveStageBand;
        public float TotalVoltage;
        public float TotalCurrent;
        public float TotalPowerIn;
        public float MaxTempDcdc;
        public float ControllerTargetVoltage;
        public float ControllerTargetCurrentTotal;
        public float ActiveLimitCurrentC;
        public uint UptimeTicks;
        // CAN stats skipped (5x uint32 = 20 bytes)
        public uint ControllerFaultFlags;
        public byte ControllerStopReason;
        public bool BmsStale;

        public static SystemInfo? FromBytes(byte[] data)
        {
            if (data.Length < 68) return null; // We only support format with diagnostics
            using var ms = new MemoryStream(data);
            using var br = new BinaryReader(ms);

            var s = new SystemInfo();
            s.FwMajor = br.ReadByte();
            s.FwMinor = br.ReadByte();
            s.FwPatch = br.ReadByte();
            s.DriverId = br.ReadByte();
            s.ModulesTotal = br.ReadByte();
            s.ModulesOnline = br.ReadByte();
            s.ModulesFault = br.ReadByte();
            s.Charging = br.ReadByte() != 0;
            s.ControllerState = br.ReadByte();
            s.ControllerDerating = br.ReadByte() != 0;
            s.ControllerInhibit = br.ReadByte() != 0;
            s.ChargeSourceMode = br.ReadByte();
            s.ActiveLimitSource = br.ReadByte();
            s.ActiveStageBand = br.ReadByte();
            s.TotalVoltage = br.ReadSingle();
            s.TotalCurrent = br.ReadSingle();
            s.TotalPowerIn = br.ReadSingle();
            s.MaxTempDcdc = br.ReadSingle();
            s.ControllerTargetVoltage = br.ReadSingle();
            s.ControllerTargetCurrentTotal = br.ReadSingle();
            s.ActiveLimitCurrentC = br.ReadSingle();
            s.UptimeTicks = br.ReadUInt32();
            if (data.Length >= 72)
            {
                br.ReadBytes(20); // skip 5x uint32 CAN stats
            }
            else
            {
                br.ReadBytes(16); // skip 4x uint32 CAN stats
            }
            s.ControllerFaultFlags = br.ReadUInt32();
            s.ControllerStopReason = br.ReadByte();
            s.BmsStale = br.ReadByte() != 0;
            return s;
        }
    }

    public class DebugProtocolParser
    {
        public event Action<List<ModuleData>>? OnAllModulesReceived;
        public event Action<ModuleData>? OnModuleReceived;
        public event Action<BMSData>? OnBmsReceived;
        public event Action<SystemInfo>? OnSystemInfoReceived;
        public event Action<ChargeCycleConfig>? OnChargeConfigReceived;
        public event Action<byte, string>? OnErrorReceived;

        public void ParseFrame(byte cmdByte, byte[] payload)
        {
            if (!Enum.IsDefined(typeof(DebugRsp), cmdByte)) return;
            DebugRsp cmd = (DebugRsp)cmdByte;

            try
            {
                switch (cmd)
                {
                    case DebugRsp.ALL_MODULES:
                        if (payload.Length < 2) return;
                        byte seq = payload[0];
                        byte count = payload[1];
                        var list = new List<ModuleData>();
                        int offset = 2;
                        for (int i = 0; i < count; i++)
                        {
                            var m = ModuleData.FromBytes(payload, offset);
                            if (m != null) list.Add(m);
                            offset += 123;
                        }
                        OnAllModulesReceived?.Invoke(list);
                        break;
                    case DebugRsp.MODULE_DATA:
                        var singleMod = ModuleData.FromBytes(payload, 0);
                        if (singleMod != null) OnModuleReceived?.Invoke(singleMod);
                        break;
                    case DebugRsp.BMS_DATA:
                        var bms = BMSData.FromBytes(payload);
                        if (bms != null) OnBmsReceived?.Invoke(bms);
                        break;
                    case DebugRsp.SYSTEM_INFO:
                        var sys = SystemInfo.FromBytes(payload);
                        if (sys != null) OnSystemInfoReceived?.Invoke(sys);
                        break;
                    case DebugRsp.CHARGE_CFG:
                        try
                        {
                            var cfg = ChargeCycleConfig.FromBytes(payload);
                            OnChargeConfigReceived?.Invoke(cfg);
                        }
                        catch (Exception ex)
                        {
                            OnErrorReceived?.Invoke(0xFF, $"Failed to parse ChargeConfig: {ex.Message}");
                        }
                        break;
                    case DebugRsp.ERROR:
                        byte errCode = payload.Length > 0 ? payload[0] : (byte)0;
                        string errMsg = errCode switch
                        {
                            0x01 => "BAD_PARAM (Tham số không hợp lệ)",
                            0x02 => "MODULE_OFFLINE (Mô-đun không phản hồi)",
                            0x03 => "NOT_SUPPORTED (Không hỗ trợ)",
                            0x04 => "FLASH_SAVE_FAIL (Lưu Flash thất bại)",
                            _ => $"Lỗi mã 0x{errCode:X2}"
                        };
                        OnErrorReceived?.Invoke(errCode, errMsg);
                        break;
                }
            }
            catch (Exception ex)
            {
                OnErrorReceived?.Invoke(0xFE, $"Frame parse error: {ex.Message}");
            }
        }
    }
}
