using System;
using System.Collections.Generic;

namespace ChargerDebug_WPF.Protocol
{
    public class SystemInfo
    {
        public int FwMajor { get; set; }
        public int FwMinor { get; set; }
        public int FwPatch { get; set; }
        public int DriverId { get; set; }
        public int ModulesTotal { get; set; }
        public int ModulesOnline { get; set; }
        public int ModulesFault { get; set; }
        public bool Charging { get; set; }
        public int ControllerState { get; set; }
        public bool ControllerDerating { get; set; }
        public bool ControllerInhibit { get; set; }
        public int ChargeSourceMode { get; set; }
        public int ActiveLimitSource { get; set; }
        public int ActiveStageBand { get; set; }
        
        public float TargetVoltage { get; set; }
        public float TargetCurrent { get; set; }
        
        public int ChargeStatus { get; set; }
        public int ActiveLogic { get; set; }
        public int ChargeLevel { get; set; }
        public int StopReason { get; set; }
        public uint ControllerFaultFlags { get; set; }
        public float ActiveLimitCurrentC { get; set; }
        
        public float TotalVoltage { get; set; }
        public float TotalCurrent { get; set; }
        public float MaxTempDcdc { get; set; }

        public string GetControllerStateName()
        {
            return ControllerState switch
            {
                0 => "Idle",
                1 => "Precharge",
                2 => "Charging",
                3 => "Stop Delay",
                4 => "Fault",
                5 => "Offline",
                _ => $"State {ControllerState}"
            };
        }

        public string GetChargeStatusName()
        {
            return ChargeStatus switch
            {
                0 => "Normal",
                1 => "Stopping",
                2 => "Fault",
                3 => "Completed",
                _ => "Unknown"
            };
        }

        public string GetChargeSourceModeName()
        {
            return ChargeSourceMode switch
            {
                0 => "CAN Override",
                1 => "Manual Override",
                2 => "Auto (BMS)",
                _ => "Unknown"
            };
        }
    }

    public class BMSData
    {
        public bool Online { get; set; }
        public int State { get; set; }
        public float Soc { get; set; }
        public float Soh { get; set; }
        public float BattVoltage { get; set; }
        public float BattCurrent { get; set; }
        public float CapRemain { get; set; }
        public float RateCap { get; set; }
        public float ChgVoltRequest { get; set; }
        public float ChgCurrRequest { get; set; }
        public float MaxCellVolt { get; set; }
        public float MinCellVolt { get; set; }
        public float MaxCellTemp { get; set; }
        public float MinCellTemp { get; set; }
        public bool ChargeRelayClosed { get; set; }
        public bool DischargeRelayClosed { get; set; }
        public uint AlarmFlags { get; set; }
        
        public string GetStateName()
        {
            return State switch
            {
                0 => "Idle",
                1 => "Precharge",
                2 => "Charging",
                3 => "Stop Delay",
                4 => "Fault",
                5 => "Offline",
                _ => $"State {State}"
            };
        }
    }

    public class ModuleData
    {
        public int Address { get; set; }
        public int Driver { get; set; }
        public bool Online { get; set; }
        public int State { get; set; }
        public uint StatusFlags { get; set; }
        public float Voltage { get; set; }
        public float Current { get; set; }
        public float TempDcdc { get; set; }
        public float TempPfc { get; set; }
        public float VAcA { get; set; }
        public float VAcB { get; set; }
        public float VAcC { get; set; }
    }
}
