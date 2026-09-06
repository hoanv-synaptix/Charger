using System;
using System.Windows;
using System.Windows.Threading;
using System.Linq;
using OxyPlot;
using OxyPlot.Series;
using OxyPlot.Axes;
using ChargerDebugApp.ViewModels;
using ChargerDebugApp.Protocol;
using System.Collections.Generic;

namespace ChargerDebugApp
{
    public partial class MonitorWindow : Window
    {
        public MainViewModel ViewModel { get; } = new MainViewModel();
        public PlotModel PlotModel { get; private set; }
        
        private LineSeries _voltageSeries;
        private LineSeries _currentSeries;
        private SerialService _serialService;
        private DebugProtocolParser _parser;
        
        private DispatcherTimer _plotTimer;
        private double _timeCounter = 0;

        public MonitorWindow()
        {
            InitializeComponent();
            
            // OxyPlot Setup
            PlotModel = new PlotModel { Title = "Charging Profile" };
            PlotModel.Axes.Add(new LinearAxis { Position = AxisPosition.Bottom, Title = "Time (s)" });
            PlotModel.Axes.Add(new LinearAxis { Position = AxisPosition.Left, Title = "Voltage (V)", Key = "VoltageAxis", Minimum = 0, Maximum = 70 });
            PlotModel.Axes.Add(new LinearAxis { Position = AxisPosition.Right, Title = "Current (A)", Key = "CurrentAxis", Minimum = 0, Maximum = 100 });
            
            _voltageSeries = new LineSeries { Title = "Voltage", Color = OxyColors.Blue, YAxisKey = "VoltageAxis" };
            _currentSeries = new LineSeries { Title = "Current", Color = OxyColors.Red, YAxisKey = "CurrentAxis" };
            PlotModel.Series.Add(_voltageSeries);
            PlotModel.Series.Add(_currentSeries);

            DataContext = this;
            
            // Backend Setup
            _serialService = SerialService.Instance;
            _parser = new DebugProtocolParser();
            
            _serialService.OnFrameReceived += (cmd, payload) => 
            {
                // Parse it
                _parser.ParseFrame(cmd, payload);
                
                // Add to traffic log
                Dispatcher.InvokeAsync(() => {
                    if (chkRx != null && chkRx.IsChecked == true) {
                        AddTrafficLog("RX", cmd.ToString("X2"), payload.Length.ToString(), BitConverter.ToString(payload).Replace("-", " "), "");
                    }
                });
            };
            
            _serialService.OnLog += msg => Dispatcher.InvokeAsync(() => {
                if (chkWarn != null && chkWarn.IsChecked == true) AddTrafficLog("WARN", "-", "-", "-", msg);
            });
            
            _serialService.OnError += msg => Dispatcher.InvokeAsync(() => {
                if (chkWarn != null && chkWarn.IsChecked == true) AddTrafficLog("ERROR", "-", "-", "-", msg);
            });
            
            _parser.OnSystemInfoReceived += sys => Dispatcher.InvokeAsync(() => ViewModel.UpdateSystem(sys));
            _parser.OnBmsReceived += bms => Dispatcher.InvokeAsync(() => ViewModel.UpdateBMS(bms));
            _parser.OnAllModulesReceived += mods => Dispatcher.InvokeAsync(() => {
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

            // Demo timer for plot
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
                PlotModel.InvalidatePlot(true);
                _timeCounter++;
            };
            _plotTimer.Start();
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
}



}
