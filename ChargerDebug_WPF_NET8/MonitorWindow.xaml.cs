using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows;
using System.Windows.Data;
using System.Windows.Threading;
using Microsoft.Win32;
using OxyPlot;
using OxyPlot.Axes;
using OxyPlot.Series;
using ChargerDebugApp.Protocol;
using ChargerDebugApp.ViewModels;

namespace ChargerDebugApp
{
    public partial class MonitorWindow : Window
    {
        public MainViewModel ViewModel { get; } = new MainViewModel();
        
        private LineSeries _voltageSeries;
        private LineSeries _currentSeries;
        private LinearAxis _voltageAxis;
        private LinearAxis _currentAxis;
        private readonly SerialService _serialService;
        private readonly DebugProtocolParser _parser;
        
        private DispatcherTimer _plotTimer;
        private double _timeCounter = 0;
        private readonly ICollectionView? _trafficLogView;
        private readonly HashSet<string> _activeWarnings = new HashSet<string>();

        public MonitorWindow()
        {
            InitializeComponent();
            
            // OxyPlot Setup
            ViewModel.PlotModel = new PlotModel { Title = "Charging Profile" };
            ViewModel.PlotModel.Axes.Add(new LinearAxis { Position = AxisPosition.Bottom, Title = "Time (s)" });
            _voltageAxis = new LinearAxis { Position = AxisPosition.Left, Title = "Voltage (V)", Key = "VoltageAxis", Minimum = 0, Maximum = 1 };
            _currentAxis = new LinearAxis { Position = AxisPosition.Right, Title = "Current (A)", Key = "CurrentAxis", Minimum = 0, Maximum = 1 };
            ViewModel.PlotModel.Axes.Add(_voltageAxis);
            ViewModel.PlotModel.Axes.Add(_currentAxis);
            
            _voltageSeries = new LineSeries { Title = "Voltage", Color = OxyColors.Blue, YAxisKey = "VoltageAxis" };
            _currentSeries = new LineSeries { Title = "Current", Color = OxyColors.Red, YAxisKey = "CurrentAxis" };
            ViewModel.PlotModel.Series.Add(_voltageSeries);
            ViewModel.PlotModel.Series.Add(_currentSeries);

            DataContext = ViewModel;
            _trafficLogView = CollectionViewSource.GetDefaultCollectionView(ViewModel.TrafficLogs);
            _trafficLogView.Filter = TrafficLogFilter;
            
            // Backend Setup
            _serialService = SerialService.Instance;
            _parser = _serialService.Parser;
            
            Action<byte, byte[]> frameHandler = (cmd, payload) => 
            {
                Dispatcher.InvokeAsync(() => {
                    string data = BitConverter.ToString(payload).Replace("-", " ");
                    AddTrafficLog("RX", cmd.ToString("X2"), payload.Length.ToString(), data, "");
                    AddDecodedWarnings(cmd, payload, data);
                });
            };
            
            Action<string> logHandler = msg => Dispatcher.InvokeAsync(() => {
                AddTrafficLog("WARN", "-", "-", "-", msg);
            });
            
            Action<string> errorHandler = msg => Dispatcher.InvokeAsync(() => {
                AddTrafficLog("WARN", "-", "-", "-", msg);
            });
            
            Action<SystemInfo> sysHandler = sys => Dispatcher.InvokeAsync(() => ViewModel.UpdateSystem(sys));
            Action<BMSData> bmsHandler = bms => Dispatcher.InvokeAsync(() => ViewModel.UpdateBMS(bms));
            Action<System.Collections.Generic.List<ModuleData>> modsHandler = mods => Dispatcher.InvokeAsync(() => {
                foreach (var mod in mods)
                {
                    var vm = ViewModel.Modules.FirstOrDefault(m => m.ModuleIdx == mod.ModuleIdx);
                    if (vm == null)
                    {
                        vm = new ModuleViewModel();
                        ViewModel.Modules.Add(vm);
                    }
                    vm.Update(mod);

                    // Select the first real module once. Subsequent telemetry
                    // updates must not override a user's manual selection.
                    if (ViewModel.SelectedModule == null && ViewModel.Modules.Count > 0)
                        ViewModel.SelectedModule = ViewModel.Modules[0];
                }
            });

            _serialService.OnFrameReceived += frameHandler;
            _serialService.OnLog += logHandler;
            _serialService.OnError += errorHandler;
            _parser.OnSystemInfoReceived += sysHandler;
            _parser.OnBmsReceived += bmsHandler;
            _parser.OnAllModulesReceived += modsHandler;

            // Timer for plot
            _plotTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(1000) };
            _plotTimer.Tick += (s, e) =>
            {
                _voltageSeries.Points.Add(new DataPoint(_timeCounter, ViewModel.SysTotalVoltage));
                _currentSeries.Points.Add(new DataPoint(_timeCounter, ViewModel.SysTotalCurrent));
                if (_voltageSeries.Points.Count > 100)
                {
                    _voltageSeries.Points.RemoveAt(0);
                    _currentSeries.Points.RemoveAt(0);
                }
                UpdateYAxis(_voltageAxis, _voltageSeries);
                UpdateYAxis(_currentAxis, _currentSeries);
                ViewModel.PlotModel.InvalidatePlot(true);
                _timeCounter++;
            };
            _plotTimer.Start();

            Closed += (s, e) =>
            {
                _plotTimer.Stop();
                _serialService.OnFrameReceived -= frameHandler;
                _serialService.OnLog -= logHandler;
                _serialService.OnError -= errorHandler;
                _parser.OnSystemInfoReceived -= sysHandler;
                _parser.OnBmsReceived -= bmsHandler;
                _parser.OnAllModulesReceived -= modsHandler;
            };
        }

        private static void UpdateYAxis(LinearAxis axis, LineSeries series)
        {
            double maxValue = 0.0;
            foreach (DataPoint point in series.Points)
            {
                if (point.Y > maxValue)
                    maxValue = point.Y;
            }

            /* Keep zero visible while allowing the usable upper range to
             * follow the values in the currently visible history window. */
            axis.Minimum = 0.0;
            axis.Maximum = maxValue > 0.0
                ? Math.Max(1.0, Math.Ceiling(maxValue * 1.2))
                : 1.0;
        }
        
        private void AddTrafficLog(string type, string id, string dlc, string data, string info)
        {
            var item = new TrafficLogItem
            {
                Time = DateTime.Now.ToString("HH:mm:ss.fff"),
                Type = type,
                Id = id,
                Dlc = dlc,
                Data = data,
                Info = info
            };
            ViewModel.TrafficLogs.Add(item);
            
            if (ViewModel.TrafficLogs.Count > 500)
                ViewModel.TrafficLogs.RemoveAt(0);
                
            if (TrafficLogGrid != null && TrafficLogGrid.Items.Contains(item))
                TrafficLogGrid.ScrollIntoView(ViewModel.TrafficLogs[^1]);
        }

        private bool TrafficLogFilter(object item)
        {
            if (item is not TrafficLogItem log)
                return false;

            return log.Type switch
            {
                "TX" => chkTx?.IsChecked == true,
                "RX" => chkRx?.IsChecked == true,
                "SYS" => chkSys?.IsChecked == true,
                "WARN" => chkWarn?.IsChecked == true,
                _ => chkWarn?.IsChecked == true
            };
        }

        private void TrafficFilterChanged(object sender, RoutedEventArgs e)
        {
            _trafficLogView?.Refresh();
        }

        private void AddDecodedWarnings(byte cmd, byte[] payload, string data)
        {
            if (cmd == (byte)DebugRsp.SYSTEM_INFO)
            {
                var sys = SystemInfo.FromBytes(payload);
                if (sys == null) return;
                UpdateWarning("SYS:stop", sys.ControllerStopReason != 0,
                    $"Stop reason: {DescribeStopReason(sys.ControllerStopReason)}", cmd, data);
                UpdateWarning("SYS:fault", sys.ControllerFaultFlags != 0,
                    $"Controller fault: {DescribeFaults(sys.ControllerFaultFlags)}", cmd, data);
            }
            else if (cmd == (byte)DebugRsp.BMS_DATA)
            {
                var bms = BMSData.FromBytes(payload);
                if (bms == null) return;
                UpdateWarning("BMS:alarm", bms.AlarmFlags != 0,
                    $"BMS alarm: {DescribeBmsAlarms(bms.AlarmFlags)}", cmd, data);
            }
            else if (cmd == (byte)DebugRsp.ALL_MODULES)
            {
                if (payload.Length < 2) return;
                byte count = payload[1];
                for (int i = 0; i < count; i++)
                {
                    var module = ModuleData.FromBytes(payload, 2 + (i * 123));
                    if (module == null) continue;
                    string key = $"MODULE:{module.ModuleIdx}:alarm";
                    bool active = module.AlarmFlags != 0;
                    UpdateWarning(key, active,
                        $"Module {module.ModuleIdx}: {DescribeModuleAlarms(module.AlarmFlags)}", cmd, data);
                }
            }
        }

        private void UpdateWarning(string key, bool active, string message, byte cmd, string data)
        {
            if (active)
            {
                if (_activeWarnings.Add(key))
                    AddTrafficLog("WARN", cmd.ToString("X2"), "-", data, message);
            }
            else
            {
                _activeWarnings.Remove(key);
            }
        }

        private static string DescribeStopReason(byte reason) => reason switch
        {
            1 => "User command", 2 => "Charge condition blocked", 3 => "BMS disconnected",
            4 => "BMS alarm", 5 => "Protection active", 6 => "Module timeout",
            7 => "Module fault", 8 => "Module count mismatch", 9 => "Emergency stop",
            10 => "Start precondition failed", 11 => "Output voltage reached limit",
            12 => "Cell voltage reached limit", 13 => "SOC target reached",
            _ => $"Unknown reason ({reason})"
        };

        private static string DescribeFaults(uint flags)
        {
            var names = new List<string>();
            if ((flags & (1U << 0)) != 0) names.Add("No charger driver");
            if ((flags & (1U << 1)) != 0) names.Add("No online module");
            if ((flags & (1U << 2)) != 0) names.Add("Module count mismatch");
            if ((flags & (1U << 3)) != 0) names.Add("BMS disconnected");
            if ((flags & (1U << 4)) != 0) names.Add("BMS data stale");
            if ((flags & (1U << 5)) != 0) names.Add("BMS alarm active");
            if ((flags & (1U << 6)) != 0) names.Add("Invalid configuration");
            if ((flags & (1U << 8)) != 0) names.Add("Jack voltage protection");
            if ((flags & (1U << 9)) != 0) names.Add("Jack temperature protection");
            if ((flags & (1U << 11)) != 0) names.Add("Emergency stop");
            return names.Count > 0 ? string.Join("; ", names) : $"Unknown fault (0x{flags:X8})";
        }

        private static string DescribeBmsAlarms(uint flags)
        {
            string[] names = { "Low pack voltage", "Low cell voltage", "High pack voltage",
                "High cell voltage", "Cell temp high (charge)", "Cell temp high (discharge)",
                "Cell temp low (charge)", "Cell temp low (discharge)", "Relay temperature high",
                "Overcharge current", "Overdischarge current", "Cell voltage difference", "Low SOC" };
            var active = new List<string>();
            for (int bit = 0; bit < names.Length; bit++)
                if ((flags & (1U << bit)) != 0) active.Add(names[bit]);
            return active.Count > 0 ? string.Join("; ", active) : $"Unknown BMS alarm (0x{flags:X8})";
        }

        private static string DescribeModuleAlarms(uint flags)
        {
            var names = new Dictionary<int, string>
            {
                [0] = "Hardware fault", [1] = "Communication fault", [2] = "Overtemperature",
                [3] = "Output overvoltage", [4] = "Short circuit", [5] = "AC undervoltage / phase loss",
                [6] = "Output overcurrent", [16] = "PFC bus overvoltage", [17] = "PFC input overcurrent",
                [18] = "PFC bus/phase imbalance", [19] = "Mains frequency fault"
            };
            var active = new List<string>();
            foreach (var pair in names)
                if ((flags & (1U << pair.Key)) != 0) active.Add(pair.Value);
            return active.Count > 0 ? string.Join("; ", active) : $"Unknown module alarm (0x{flags:X8})";
        }

        private void ClearLogs_Click(object sender, RoutedEventArgs e)
        {
            ViewModel.TrafficLogs.Clear();
            _activeWarnings.Clear();
        }

        private void SaveLogs_Click(object sender, RoutedEventArgs e)
        {
            var dlg = new SaveFileDialog
            {
                Filter = "CSV Files (*.csv)|*.csv|All Files (*.*)|*.*",
                FileName = $"traffic_log_{DateTime.Now:yyyyMMdd_HHmmss}.csv",
                Title = "Save Traffic Log"
            };

            if (dlg.ShowDialog() == true)
            {
                try
                {
                    using var sw = new StreamWriter(dlg.FileName, false, Encoding.UTF8);
                    sw.WriteLine("Time,Type,ID,DLC,Data,Info");
                    foreach (var log in ViewModel.TrafficLogs)
                    {
                        sw.WriteLine($"\"{log.Time}\",\"{log.Type}\",\"{log.Id}\",\"{log.Dlc}\",\"{log.Data}\",\"{log.Info}\"");
                    }
                    MessageBox.Show($"Traffic log saved successfully to:\n{dlg.FileName}", "Saved", MessageBoxButton.OK, MessageBoxImage.Information);
                }
                catch (Exception ex)
                {
                    MessageBox.Show($"Failed to save log:\n{ex.Message}", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
                }
            }
        }
    }
}
