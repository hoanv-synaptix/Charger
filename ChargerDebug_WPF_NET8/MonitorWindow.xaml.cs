using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows;
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
        private readonly SerialService _serialService;
        private readonly DebugProtocolParser _parser;
        
        private DispatcherTimer _plotTimer;
        private double _timeCounter = 0;

        public MonitorWindow()
        {
            InitializeComponent();
            
            // OxyPlot Setup
            ViewModel.PlotModel = new PlotModel { Title = "Charging Profile" };
            ViewModel.PlotModel.Axes.Add(new LinearAxis { Position = AxisPosition.Bottom, Title = "Time (s)" });
            ViewModel.PlotModel.Axes.Add(new LinearAxis { Position = AxisPosition.Left, Title = "Voltage (V)", Key = "VoltageAxis", Minimum = 0, Maximum = 70 });
            ViewModel.PlotModel.Axes.Add(new LinearAxis { Position = AxisPosition.Right, Title = "Current (A)", Key = "CurrentAxis", Minimum = 0, Maximum = 100 });
            
            _voltageSeries = new LineSeries { Title = "Voltage", Color = OxyColors.Blue, YAxisKey = "VoltageAxis" };
            _currentSeries = new LineSeries { Title = "Current", Color = OxyColors.Red, YAxisKey = "CurrentAxis" };
            ViewModel.PlotModel.Series.Add(_voltageSeries);
            ViewModel.PlotModel.Series.Add(_currentSeries);

            DataContext = ViewModel;
            
            // Backend Setup
            _serialService = SerialService.Instance;
            _parser = _serialService.Parser;
            
            Action<byte, byte[]> frameHandler = (cmd, payload) => 
            {
                Dispatcher.InvokeAsync(() => {
                    if (chkRx != null && chkRx.IsChecked == true) {
                        AddTrafficLog("RX", cmd.ToString("X2"), payload.Length.ToString(), BitConverter.ToString(payload).Replace("-", " "), "");
                    }
                });
            };
            
            Action<string> logHandler = msg => Dispatcher.InvokeAsync(() => {
                if (chkWarn != null && chkWarn.IsChecked == true) AddTrafficLog("WARN", "-", "-", "-", msg);
            });
            
            Action<string> errorHandler = msg => Dispatcher.InvokeAsync(() => {
                if (chkWarn != null && chkWarn.IsChecked == true) AddTrafficLog("ERROR", "-", "-", "-", msg);
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
                
            if (TrafficLogGrid != null && ViewModel.TrafficLogs.Count > 0)
                TrafficLogGrid.ScrollIntoView(ViewModel.TrafficLogs[^1]);
        }

        private void ClearLogs_Click(object sender, RoutedEventArgs e)
        {
            ViewModel.TrafficLogs.Clear();
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