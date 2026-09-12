using System;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using OxyPlot;
using ChargerDebugApp.Protocol;

namespace ChargerDebugApp.ViewModels
{
    public class ViewModelBase : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler? PropertyChanged;
        protected void OnPropertyChanged([CallerMemberName] string? propertyName = null)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }
        protected bool SetProperty<T>(ref T storage, T value, [CallerMemberName] string? propertyName = null)
        {
            if (Equals(storage, value)) return false;
            storage = value;
            OnPropertyChanged(propertyName);
            return true;
        }
    }

    public class ModuleViewModel : ViewModelBase
    {
        private ModuleData _data = new ModuleData();
        
        public void Update(ModuleData d)
        {
            _data = d;
            OnPropertyChanged(string.Empty);
        }
        
        public int ModuleIdx => _data.ModuleIdx;
        public int Addr => _data.Addr;
        public string DriverName => _data.DriverId == 1 ? "Maxwell" : _data.DriverId == 2 ? "Lianming" : _data.DriverId == 3 ? "TonHe" : "Unknown";
        public string Online => _data.Online ? "YES" : "NO";
        public string Running => _data.Running ? "YES" : "NO";
        public string State => _data.State switch {
            0 => "IDLE", 1 => "STARTING", 2 => "RUNNING", 3 => "WARNING",
            4 => "OFFLINE", 5 => "FAULT", 6 => "RECOVER", 7 => "STOPPING", _ => "???"
        };
        public float Voltage => _data.Voltage;
        public float Current => _data.Current;
        public float CurrentLimit => _data.CurrentLimit;
        public string RatedPower => _data.RatedPower > 0.0f ? $"{_data.RatedPower:F0}" : "---";
        public string RatedCurrent => _data.RatedCurrent > 0.0f ? $"{_data.RatedCurrent:F1}" : "---";
        public float TempDcdc => _data.TempDcdc;
        public float TempAmbient => _data.TempAmbient;
        public float TempPfc => _data.TempPfc;
        public float AcPhaseAVoltage => _data.AcPhaseAVoltage;
        public float AcPhaseBVoltage => _data.AcPhaseBVoltage;
        public float AcPhaseCVoltage => _data.AcPhaseCVoltage;
        public float PfcBusPosVoltage => _data.PfcBusPosVoltage;
        public float PfcBusNegVoltage => _data.PfcBusNegVoltage;
        public uint InputPower => _data.InputPower;
        public string Fault => _data.AlarmStatus > 0 ? $"0x{_data.AlarmStatus:X2}" : "None";
    }

    public class TrafficLogItem
    {
        public string Time { get; set; } = "";
        public string Type { get; set; } = "";
        public string Id { get; set; } = "";
        public string Dlc { get; set; } = "";
        public string Data { get; set; } = "";
        public string Info { get; set; } = "";
    }

    public class MainViewModel : ViewModelBase
    {
        public ObservableCollection<ModuleViewModel> Modules { get; } = new ObservableCollection<ModuleViewModel>();
        public ObservableCollection<TrafficLogItem> TrafficLogs { get; } = new ObservableCollection<TrafficLogItem>();
        public PlotModel PlotModel { get; set; } = new PlotModel();

        /// <summary>True when at least one module has been received from MCU.</summary>
        public bool HasModules => Modules.Count > 0;

        public MainViewModel()
        {
            Modules.CollectionChanged += (_, __) => OnPropertyChanged(nameof(HasModules));
        }

        private ModuleViewModel? _selectedModule;
        public ModuleViewModel? SelectedModule
        {
            get => _selectedModule;
            set
            {
                if (_selectedModule != value)
                {
                    _selectedModule = value;
                    OnPropertyChanged();
                    OnPropertyChanged(string.Empty); // Refresh all Sel* properties
                }
            }
        }

        private BMSData? _bms;
        private SystemInfo? _sys;

        public void UpdateSystem(SystemInfo sys)
        {
            _sys = sys;
            OnPropertyChanged(string.Empty);
        }

        public void UpdateBMS(BMSData bms)
        {
            _bms = bms;
            OnPropertyChanged(string.Empty);
        }

        // ==========================================
        // 1. SELECTED MODULE TELEMETRY (Default "---")
        // ==========================================
        public string SelDriver => SelectedModule?.DriverName ?? "---";
        public string SelState => SelectedModule?.State ?? "---";
        public string SelOnline => SelectedModule?.Online ?? "---";
        public string SelRunning => SelectedModule?.Running ?? "---";
        public string SelVoltage => SelectedModule != null ? $"{SelectedModule.Voltage:F2}" : "---";
        public string SelCurrent => SelectedModule != null ? $"{SelectedModule.Current:F2}" : "---";
        public string SelCurrentLimit => SelectedModule != null ? $"{SelectedModule.CurrentLimit:F2}" : "---";
        public string SelRatedPower => SelectedModule?.RatedPower ?? "---";
        public string SelRatedCurrent => SelectedModule?.RatedCurrent ?? "---";
        public string SelTempDcdc => SelectedModule != null ? $"{SelectedModule.TempDcdc:F1}" : "---";
        public string SelTempAmbient => SelectedModule != null ? $"{SelectedModule.TempAmbient:F1}" : "---";
        public string SelTempPfc => SelectedModule != null ? $"{SelectedModule.TempPfc:F1}" : "---";
        public string SelInputPower => SelectedModule != null ? $"{SelectedModule.InputPower}" : "---";
        public string SelAcPhaseA => SelectedModule != null ? $"{SelectedModule.AcPhaseAVoltage:F1}" : "---";
        public string SelAcPhaseB => SelectedModule != null ? $"{SelectedModule.AcPhaseBVoltage:F1}" : "---";
        public string SelAcPhaseC => SelectedModule != null ? $"{SelectedModule.AcPhaseCVoltage:F1}" : "---";
        public string SelPfcPos => SelectedModule != null ? $"{SelectedModule.PfcBusPosVoltage:F1}" : "---";
        public string SelPfcNeg => SelectedModule != null ? $"{SelectedModule.PfcBusNegVoltage:F1}" : "---";

        // ==========================================
        // 2. CHARGE PROCESS & CONTROLLER (Default "---")
        // ==========================================
        public string SysControllerState => _sys != null ? (_sys.ControllerState switch {
            0 => "Idle", 1 => "Ready", 2 => "Running", 3 => "Stopping",
            4 => "Fault", 5 => "Pre-charge", _ => $"State {_sys.ControllerState}"
        }) : "---";

        public string SysControlMode => _sys != null ? (_sys.ChargeSourceMode == 1 ? "Standalone (No BMS)" : "BMS Controlled") : "---";

        public string SysProcessSummary => _sys != null ? (!_sys.Charging ? "Stopped" : _sys.ControllerInhibit ? "Inhibited" : _sys.ControllerDerating ? "Derating" : "Active Charging") : "---";

        public string SysModulesOnlineCount => _sys != null ? $"{_sys.ModulesOnline} / {_sys.ModulesTotal}" : "0";

        // BMS Limits (Demand from BMS)
        public string BmsDemandStatus => _bms != null ? (_bms.Online ? "Active" : "Offline") : "---";
        public string BmsDemandVoltage => _bms != null ? $"{_bms.ChgVoltRequest:F1}" : "---";
        public string BmsDemandCurrent => _bms != null ? $"{_bms.ChgCurrRequest:F1}" : "---";
        public string BmsDemandMaxTemp => _bms != null ? $"{_bms.MaxCellTemp:F1}" : "---";

        // Controller Target (Logic)
        public string SysTargetVoltageStr => _sys != null ? $"{_sys.ControllerTargetVoltage:F1}" : "---";
        public string SysTargetCurrentStr => _sys != null ? $"{_sys.ControllerTargetCurrentTotal:F1}" : "---";
        public string SysActiveLogic => _sys != null ? (_sys.ChargeSourceMode == 1 ? "No BMS" : _sys.ActiveLimitSource switch {
            1 => "Cell Voltage", 2 => "Temperature", 3 => "SOC", _ => "None"
        }) : "---";
        public string SysChargeLevel => _sys != null ? (_sys.ActiveStageBand switch {
            2 => "Stage 1", 3 => "Stage 2", 4 => "Stage 3", 5 => "Stage 4", _ => "Standard"
        }) : "---";
        public string SysStatusText => _sys != null ? (!_sys.Charging ? "Standby" : "Charging") : "---";
        public string SysCurrentLimitC => _sys != null ? $"{_sys.ActiveLimitCurrentC:F2}" : "---";
        public string SysStopReasonStr => _sys == null ? "---" :
            _sys.Charging ? "Not stopped" : _sys.ControllerStopReason switch
            {
                0 => "No stop reason",
                1 => "User command",
                2 => "Charge condition blocked",
                3 => "BMS disconnected",
                4 => "BMS alarm",
                5 => "Protection active",
                6 => "Module timeout",
                7 => "Module fault",
                8 => "Module count mismatch",
                9 => "Emergency stop",
                10 => "Start precondition failed",
                11 => "Output voltage reached limit",
                12 => "Cell voltage reached limit",
                13 => "SOC target reached",
                _ => $"Unknown reason ({_sys.ControllerStopReason})"
            };
        public string SysStopReasonColor => _sys == null || _sys.Charging || _sys.ControllerStopReason == 0
            ? "#64748B" : "#DC2626";
        private static string DescribeControllerFaults(uint flags)
        {
            if (flags == 0U)
                return "Normal";

            string description = "";
            void Add(string text)
            {
                if (description.Length > 0)
                    description += "; ";
                description += text;
            }

            if ((flags & (1U << 0)) != 0U) Add("No charger driver");
            if ((flags & (1U << 1)) != 0U) Add("No online module");
            if ((flags & (1U << 2)) != 0U) Add("Module count mismatch");
            if ((flags & (1U << 3)) != 0U) Add("BMS disconnected");
            if ((flags & (1U << 4)) != 0U) Add("BMS data stale");
            if ((flags & (1U << 5)) != 0U) Add("BMS alarm active");
            if ((flags & (1U << 6)) != 0U) Add("Invalid configuration");
            if ((flags & (1U << 8)) != 0U) Add("Jack voltage protection");
            if ((flags & (1U << 9)) != 0U) Add("Jack temperature protection");
            if ((flags & (1U << 11)) != 0U) Add("Emergency stop");

            uint knownFlags = 0x00000F7FU;
            uint unknownFlags = flags & ~knownFlags;
            if (unknownFlags != 0U)
                Add($"Unknown fault (0x{unknownFlags:X8})");

            return description;
        }

        public string SysFaultStr => _sys == null ? "---" : DescribeControllerFaults(_sys.ControllerFaultFlags);
        public string SysFaultColor => _sys == null || _sys.ControllerFaultFlags == 0U
            ? "#64748B" : "#DC2626";

        // Actual Charger Output
        public string SysActualVoltage => _sys != null ? $"{_sys.TotalVoltage:F1}" : "---";
        public string SysActualCurrent => _sys != null ? $"{_sys.TotalCurrent:F1}" : "---";
        public string SysActualMaxTemp => _sys != null ? $"{_sys.MaxTempDcdc:F1}" : "---";

        public double SysTotalVoltage => _sys?.TotalVoltage ?? 0;
        public double SysTotalCurrent => _sys?.TotalCurrent ?? 0;

        // ==========================================
        // 3. BMS MONITOR PANEL (Default "---")
        // ==========================================
        public string BmsStateText => _bms == null ? "---" : _bms.State switch
        {
            0 => "OFFLINE",
            1 => "ONLINE",
            2 => "FAULT",
            _ => $"UNKNOWN ({_bms.State})"
        };
        public string BmsOnlineText => _bms == null ? "---" : (_bms.Online ? "ONLINE" : "OFFLINE");
        public string BmsStateColor => _bms == null ? "#64748B" : _bms.State switch
        {
            1 => "#10B981",
            2 => "#EF4444",
            _ => "#64748B"
        };
        public string BmsOnlineColor => _bms == null ? "#64748B" : (_bms.Online ? "#10B981" : "#EF4444");
        public string BmsBattVoltageStr => _bms != null ? $"{_bms.BattVoltage:F1}" : "---";
        public string BmsBattCurrentStr => _bms != null ? $"{_bms.BattCurrent:F1}" : "---";
        public string BmsSocStr => _bms != null ? $"{_bms.Soc}" : "---";
        public string BmsSohStr => _bms != null ? $"{_bms.Soh}" : "---";
        public string BmsRemainCapStr => _bms != null ? $"{_bms.CapRemain:F1}" : "---";
        public string BmsRatedCapStr => _bms != null ? $"{_bms.RateCap:F1}" : "---";

        public string BmsMaxCellVoltStr => _bms != null ? $"{_bms.MaxCellVolt}" : "---";
        public string BmsMinCellVoltStr => _bms != null ? $"{_bms.MinCellVolt}" : "---";
        public string BmsMaxCellTempStr => _bms != null ? $"{_bms.MaxCellTemp:F1}" : "---";
        public string BmsMinCellTempStr => _bms != null ? $"{_bms.MinCellTemp:F1}" : "---";
        public string BmsCellVoltDeltaStr => _bms != null ? $"{Math.Abs((int)_bms.MaxCellVolt - (int)_bms.MinCellVolt)}" : "---";
        public string BmsCellTempDeltaStr => _bms != null ? $"{Math.Abs(_bms.MaxCellTemp - _bms.MinCellTemp):F1}" : "---";

        public string BmsChargeRelay => _bms != null ? (_bms.ChargeRelayClosed ? "CLOSED" : "OPEN") : "---";
        public string BmsDischargeRelay => _bms != null ? (_bms.DischargeRelayClosed ? "CLOSED" : "OPEN") : "---";
    }
}
