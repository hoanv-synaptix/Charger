using System;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using Microsoft.Win32;
using ChargerDebugApp.Protocol;
using ChargerDebugApp.ViewModels;

namespace ChargerDebugApp;

/// <summary>
/// Interaction logic for MainWindow.xaml
/// </summary>
public partial class MainWindow : Window
{
    private readonly SerialService _serialService;
    public ChargeConfigViewModel ViewModel { get; } = new ChargeConfigViewModel();
    private TaskCompletionSource<ChargeCycleConfig>? _cfgReadTcs;

    public MainWindow()
    {
        InitializeComponent();
        DataContext = ViewModel;
        _serialService = SerialService.Instance;

        // Register protocol events
        _serialService.Parser.OnChargeConfigReceived += OnChargeConfigReceived;
        _serialService.Parser.OnErrorReceived += OnMcuErrorReceived;

        RefreshPorts();
        UpdateConnectionUi();
    }

    private void RefreshPorts()
    {
        if (cmbMainPort == null) return;
        cmbMainPort.Items.Clear();
        var ports = _serialService.GetAvailablePorts();
        foreach (var port in ports) cmbMainPort.Items.Add(port);
        if (ports.Length > 0) cmbMainPort.SelectedIndex = 0;
    }

    private void BtnRefresh_Click(object sender, RoutedEventArgs e)
    {
        RefreshPorts();
    }

    private void UpdateConnectionUi()
    {
        if (_serialService.IsConnected)
        {
            btnMainConnect.Content = "Disconnect";
            btnMainConnect.Background = new SolidColorBrush(Color.FromRgb(239, 68, 68)); // Red
            elpStatusDot.Fill = new SolidColorBrush(Color.FromRgb(16, 185, 129)); // Green
            lblConnectionStatus.Text = "Connected";
            lblConnectionStatus.Foreground = new SolidColorBrush(Color.FromRgb(16, 185, 129));
            lblStatusPrompt.Text = "Connected to MCU";
        }
        else
        {
            btnMainConnect.Content = "Connect";
            btnMainConnect.Background = new SolidColorBrush(Color.FromRgb(37, 99, 235)); // Blue
            elpStatusDot.Fill = new SolidColorBrush(Color.FromRgb(239, 68, 68)); // Red
            lblConnectionStatus.Text = "Disconnected";
            lblConnectionStatus.Foreground = new SolidColorBrush(Color.FromRgb(239, 68, 68));
            lblStatusPrompt.Text = "Ready";
        }
    }

    private void BtnMainConnect_Click(object sender, RoutedEventArgs e)
    {
        if (_serialService.IsConnected)
        {
            _serialService.Disconnect();
            UpdateConnectionUi();
        }
        else
        {
            string? port = cmbMainPort.SelectedItem as string;
            if (string.IsNullOrEmpty(port))
            {
                MessageBox.Show("Please select a COM port.", "Warning", MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }

            if (_serialService.Connect(port))
            {
                UpdateConnectionUi();
                // Send ENTER command to wake up MCU debug stream
                _serialService.SendFrame((byte)DebugCmd.ENTER);
            }
            else
            {
                MessageBox.Show($"Failed to open port {port}. Please check if another app is using it.", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }
    }

    private void OpenMonitor_Click(object sender, RoutedEventArgs e)
    {
        var monitorWindow = new MonitorWindow();
        monitorWindow.Show();
    }

    private async void BtnReadMcu_Click(object sender, RoutedEventArgs e)
    {
        if (!_serialService.IsConnected)
        {
            MessageBox.Show("Please connect to MCU first.", "Not Connected", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        btnReadMcu.IsEnabled = false;
        btnReadMcu.Content = "Reading...";
        lblStatusPrompt.Text = "Requesting charge configuration from MCU...";

        _cfgReadTcs = new TaskCompletionSource<ChargeCycleConfig>();

        // Send GET_CHARGE_CFG (0x19)
        bool sent = _serialService.SendFrame((byte)DebugCmd.GET_CHARGE_CFG);
        if (!sent)
        {
            btnReadMcu.IsEnabled = true;
            btnReadMcu.Content = "Read MCU";
            lblStatusPrompt.Text = "Failed to send read command";
            MessageBox.Show("Failed to transmit command to MCU.", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            return;
        }

        // Wait with 3-second timeout
        var delayTask = Task.Delay(3000);
        var completedTask = await Task.WhenAny(_cfgReadTcs.Task, delayTask);

        btnReadMcu.IsEnabled = true;
        btnReadMcu.Content = "Read MCU";

        if (completedTask == _cfgReadTcs.Task)
        {
            var config = await _cfgReadTcs.Task;
            ViewModel.LoadConfig(config);
            ResetTextBoxBorders(this);
            lblStatusPrompt.Text = "Config loaded from MCU successfully";
            MessageBox.Show("Charge cycle config read from MCU successfully!", "Read MCU", MessageBoxButton.OK, MessageBoxImage.Information);
        }
        else
        {
            lblStatusPrompt.Text = "Timeout waiting for MCU response";
            MessageBox.Show("No response from MCU within 3 seconds. Check connection and firmware status.", "Timeout", MessageBoxButton.OK, MessageBoxImage.Warning);
        }

        _cfgReadTcs = null;
    }

    private void OnChargeConfigReceived(ChargeCycleConfig config)
    {
        Dispatcher.Invoke(() =>
        {
            _cfgReadTcs?.TrySetResult(config);
        });
    }

    private void OnMcuErrorReceived(byte code, string msg)
    {
        Dispatcher.Invoke(() =>
        {
            lblStatusPrompt.Text = $"MCU Error: {msg}";
        });
    }

    private void BtnWriteMcu_Click(object sender, RoutedEventArgs e)
    {
        if (!_serialService.IsConnected)
        {
            MessageBox.Show("Please connect to MCU first.", "Not Connected", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        try
        {
            byte[] payload = ViewModel.GetBytes();
            if (payload.Length != ChargeCycleConfig.EXPECTED_BINARY_SIZE)
            {
                MessageBox.Show($"Config serialization error: expected {ChargeCycleConfig.EXPECTED_BINARY_SIZE} bytes, got {payload.Length}", "Validation Error", MessageBoxButton.OK, MessageBoxImage.Error);
                return;
            }

            lblStatusPrompt.Text = "Writing configuration to MCU flash...";
            bool sent = _serialService.SendFrame((byte)DebugCmd.SET_CHARGE_CFG, payload);

            if (sent)
            {
                ResetTextBoxBorders(this);
                lblStatusPrompt.Text = "Configuration saved to MCU flash successfully";
                MessageBox.Show("Configuration successfully sent and written to MCU Flash!", "Write MCU", MessageBoxButton.OK, MessageBoxImage.Information);
            }
            else
            {
                lblStatusPrompt.Text = "Failed to transmit config to MCU";
                MessageBox.Show("Failed to transmit configuration packet to MCU.", "Write Failed", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Error packing configuration: {ex.Message}", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private void ImportConfig_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog
        {
            Filter = "JSON Files (*.json)|*.json|All Files (*.*)|*.*",
            Title = "Import Charge Configuration"
        };

        if (dlg.ShowDialog() == true)
        {
            try
            {
                string json = File.ReadAllText(dlg.FileName);
                ViewModel.LoadFromJson(json);
                ResetTextBoxBorders(this);
                lblStatusPrompt.Text = $"Config imported from {Path.GetFileName(dlg.FileName)}";
                MessageBox.Show($"Configuration imported successfully from:\n{dlg.FileName}", "Import Config", MessageBoxButton.OK, MessageBoxImage.Information);
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Failed to parse config file:\n{ex.Message}", "Import Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }
    }

    private void ExportConfig_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new SaveFileDialog
        {
            Filter = "JSON Files (*.json)|*.json|All Files (*.*)|*.*",
            FileName = "charge_config.json",
            Title = "Export Charge Configuration"
        };

        if (dlg.ShowDialog() == true)
        {
            try
            {
                string json = ViewModel.GetJson();
                File.WriteAllText(dlg.FileName, json);
                lblStatusPrompt.Text = $"Config exported to {Path.GetFileName(dlg.FileName)}";
                MessageBox.Show($"Configuration exported successfully to:\n{dlg.FileName}", "Export Config", MessageBoxButton.OK, MessageBoxImage.Information);
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Failed to save config file:\n{ex.Message}", "Export Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }
    }

    private void Defaults_Click(object sender, RoutedEventArgs e)
    {
        if (MessageBox.Show("Reset all parameters to factory defaults?", "Confirm Defaults", MessageBoxButton.YesNo, MessageBoxImage.Question) == MessageBoxResult.Yes)
        {
            ViewModel.ResetToDefaults();
            ResetTextBoxBorders(this);
            lblStatusPrompt.Text = "Factory default configuration loaded";
            MessageBox.Show("Default configuration parameters loaded.", "Defaults", MessageBoxButton.OK, MessageBoxImage.Information);
        }
    }

    private void ConfigTextBox_TextChanged(object sender, TextChangedEventArgs e)
    {
        if (sender is TextBox tb && tb.IsLoaded)
        {
            // Set orange border to indicate unsaved changes
            tb.BorderBrush = new SolidColorBrush(Color.FromRgb(249, 115, 22)); // #F97316
            tb.BorderThickness = new Thickness(2);
        }
    }

    private void ResetTextBoxBorders(DependencyObject parent)
    {
        int count = VisualTreeHelper.GetChildrenCount(parent);
        for (int i = 0; i < count; i++)
        {
            var child = VisualTreeHelper.GetChild(parent, i);
            if (child is TextBox tb)
            {
                tb.BorderBrush = new SolidColorBrush(Color.FromRgb(100, 116, 139)); // #64748B
                tb.BorderThickness = new Thickness(1);
            }
            else
            {
                ResetTextBoxBorders(child);
            }
        }
    }
}