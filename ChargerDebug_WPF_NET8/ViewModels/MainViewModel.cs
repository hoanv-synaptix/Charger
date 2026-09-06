using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;
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
            OnPropertyChanged(string.Empty); // Update all properties
        }
        
        public int ModuleIdx => _data.ModuleIdx;
        public int Addr => _data.Addr;
        public string DriverName => _data.DriverId == 1 ? "Maxwell" : _data.DriverId == 2 ? "Lianming" : _data.DriverId == 3 ? "TonHe" : "Unknown";
        public string Online => _data.Online ? "YES" : "NO";
        public string State => _data.State == 0 ? "IDLE" : _data.State == 1 ? "STARTING" : _data.State == 2 ? "RUNNING" : _data.State == 3 ? "WARNING" : _data.State == 4 ? "OFFLINE" : _data.State == 5 ? "FAULT" : _data.State == 6 ? "RECOVER" : _data.State == 7 ? "STOPPING" : "???";
        public float Voltage => _data.Voltage;
        public float Current => _data.Current;
        public float TempDcdc => _data.TempDcdc;
        public float TempPfc => _data.TempPfc;
        public uint InputPower => _data.InputPower;
        public string Fault => _data.AlarmStatus > 0 ? $"0x{_data.AlarmStatus:X2}" : "-";
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

        // BMS Panel Properties
        public string BmsState => _bms?.State == 1 ? "ONLINE" : _bms?.State == 2 ? "FAULT" : "OFFLINE";
        public string BmsStateColor => _bms?.State == 1 ? "#10B981" : "#EF4444";
        public float BmsVoltage => _bms?.BattVoltage ?? 0;
        public float BmsCurrent => _bms?.BattCurrent ?? 0;
        public float BmsSoc => _bms?.Soc ?? 0;
        public float BmsSoh => _bms?.Soh ?? 0;
        public float BmsMaxCellVolt => (_bms?.MaxCellVolt ?? 0) / 1000f; // mV to V
        public float BmsMinCellVolt => (_bms?.MinCellVolt ?? 0) / 1000f;
        public float BmsMaxCellTemp => _bms?.MaxCellTemp ?? 0;
        public float BmsMinCellTemp => _bms?.MinCellTemp ?? 0;
        public float BmsReqVoltage => _bms?.ChgVoltRequest ?? 0;
        public float BmsReqCurrent => _bms?.ChgCurrRequest ?? 0;
        public string BmsChargeRelay => _bms != null && _bms.ChargeRelayClosed ? "CLOSED" : "OPEN";
        public string BmsDischargeRelay => _bms != null && _bms.DischargeRelayClosed ? "CLOSED" : "OPEN";

        // System Info Panel
        public string SysState => _sys?.ControllerState == 0 ? "Idle" : _sys?.ControllerState == 1 ? "Ready" : _sys?.ControllerState == 2 ? "Running" : _sys?.ControllerState == 3 ? "Derating" : _sys?.ControllerState == 4 ? "Stopping" : _sys?.ControllerState == 5 ? "Fault" : "Unknown";
        public string SysStatus => _sys != null ? (!_sys.Charging ? "Stopped" : _sys.ControllerInhibit ? "Blocked" : _sys.ControllerDerating ? "Derating" : "Normal") : "-";
        public string SysLogic => _sys?.ChargeSourceMode == 1 ? "No BMS" : _sys?.ActiveLimitSource == 1 ? "Cell Voltage" : _sys?.ActiveLimitSource == 2 ? "Temperature" : _sys?.ActiveLimitSource == 3 ? "SOC" : "Unknown";
        public string SysStage => _sys?.ActiveStageBand == 2 ? "Level 1 (M1-M2)" : _sys?.ActiveStageBand == 3 ? "Level 2 (M2-M3)" : _sys?.ActiveStageBand == 4 ? "Level 3 (M3-M4)" : _sys?.ActiveStageBand == 5 ? "Level 4 (M4-M5)" : "None";
        public string SysStopReason => _sys?.ControllerStopReason == 0 ? "None" : $"Reason Code: {_sys?.ControllerStopReason}";
        public float SysTargetVoltage => _sys?.ControllerTargetVoltage ?? 0;
        public float SysTargetCurrent => _sys?.ControllerTargetCurrentTotal ?? 0;
        public float SysTotalVoltage => _sys?.TotalVoltage ?? 0;
        public float SysTotalCurrent => _sys?.TotalCurrent ?? 0;
        public float SysTotalPower => _sys?.TotalPowerIn ?? 0;
        public float SysMaxTemp => _sys?.MaxTempDcdc ?? 0;
        public float SysLimitC => _sys?.ActiveLimitCurrentC ?? 0;
    }
}