using System;
using System.Windows;
using OxyPlot;
using OxyPlot.Series;
using OxyPlot.Axes;

namespace ChargerDebugApp
{
    public partial class MonitorWindow : Window
    {
        public PlotModel PlotModel { get; private set; }
        private LineSeries _voltageSeries;
        private LineSeries _currentSeries;

        public MonitorWindow()
        {
            InitializeComponent();
            
            PlotModel = new PlotModel { Title = "Charging Profile" };
            
            // X Axis (Time)
            PlotModel.Axes.Add(new LinearAxis { Position = AxisPosition.Bottom, Title = "Time (s)", Minimum = 0, Maximum = 100 });
            
            // Y Axis (Voltage)
            PlotModel.Axes.Add(new LinearAxis { Position = AxisPosition.Left, Title = "Voltage (V)", Key = "VoltageAxis", Minimum = 40, Maximum = 65, MajorGridlineStyle = LineStyle.Solid, MajorGridlineColor = OxyColors.LightGray });
            
            // Y Axis (Current)
            PlotModel.Axes.Add(new LinearAxis { Position = AxisPosition.Right, Title = "Current (A)", Key = "CurrentAxis", Minimum = 0, Maximum = 60 });
            
            _voltageSeries = new LineSeries
            {
                Title = "Voltage",
                Color = OxyColors.Blue,
                YAxisKey = "VoltageAxis"
            };
            
            _currentSeries = new LineSeries
            {
                Title = "Current",
                Color = OxyColors.Red,
                YAxisKey = "CurrentAxis"
            };
            
            PlotModel.Series.Add(_voltageSeries);
            PlotModel.Series.Add(_currentSeries);
            
            DataContext = this;
            
            // Demo data
            for (int i = 0; i < 100; i++)
            {
                _voltageSeries.Points.Add(new DataPoint(i, 45 + Math.Sin(i * 0.1) * 5));
                _currentSeries.Points.Add(new DataPoint(i, 20 + Math.Cos(i * 0.1) * 10));
            }
        }
    }
}
