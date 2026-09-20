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

        Loaded += async (_, _) => await CheckForUpdateAsync();
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
                MessageBox.Show("Vui lòng chọn cổng COM trước khi kết nối.", "Cảnh báo", MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }

            if (_serialService.Connect(port))
            {
                UpdateConnectionUi();

                // Sequence commands with proper pacing to avoid USB CDC queue overflow
                _ = Dispatcher.InvokeAsync(async () =>
                {
                    await Task.Delay(400);
                    if (!_serialService.IsConnected) return;

                    // Send ENTER command to wake up MCU debug stream
                    _serialService.SendFrame((byte)DebugCmd.ENTER);
                    await Task.Delay(150);

                    // Auto-sync real-time clock from PC to MCU
                    SyncRtcToMcu();
                    await Task.Delay(200);

                    // Auto-load charge configuration from MCU
                    await ReadMcuConfigInternalAsync(isAutoLoad: true);
                });
            }
            else
            {
                MessageBox.Show($"Không thể mở cổng {port}. Vui lòng kiểm tra xem cổng COM có đang bị phần mềm khác sử dụng không.", "Lỗi kết nối", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }
    }

    private bool SyncRtcToMcu()
    {
        try
        {
            uint epochUtc = (uint)DateTimeOffset.UtcNow.ToUnixTimeSeconds();
            return _serialService.SendFrame((byte)DebugCmd.SET_RTC, BitConverter.GetBytes(epochUtc));
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"[WARN] RTC sync error: {ex.Message}");
            return false;
        }
    }

    private async Task CheckForUpdateAsync()
    {
        try
        {
            var info = await Services.UpdateService.CheckForUpdateAsync();
            if (info is null) return;

            var dialog = new UpdateDialog(info)
            {
                Owner = this
            };
            dialog.ShowDialog();
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"[WARN] Update check error: {ex.Message}");
        }
    }

    private void OpenMonitor_Click(object sender, RoutedEventArgs e)
    {
        var monitorWindow = new MonitorWindow();
        monitorWindow.Show();
    }

    private void OpenMcuFirmwareUpdate_Click(object sender, RoutedEventArgs e)
    {
        var updateDialog = new McuFirmwareUpdateDialog(_serialService)
        {
            Owner = this
        };
        updateDialog.ShowDialog();
    }

    private async void BtnReadMcu_Click(object sender, RoutedEventArgs e)
    {
        await ReadMcuConfigInternalAsync(isAutoLoad: false);
    }

    private async Task ReadMcuConfigInternalAsync(bool isAutoLoad)
    {
        if (!_serialService.IsConnected)
        {
            if (!isAutoLoad)
            {
                MessageBox.Show("Vui lòng kết nối với MCU trước.", "Chưa kết nối", MessageBoxButton.OK, MessageBoxImage.Warning);
            }
            return;
        }

        btnReadMcu.IsEnabled = false;
        btnReadMcu.Content = "Đang đọc...";
        lblStatusPrompt.Text = isAutoLoad ? "Đang tự động tải cấu hình sạc từ MCU..." : "Đang yêu cầu cấu hình sạc từ MCU...";

        _cfgReadTcs = new TaskCompletionSource<ChargeCycleConfig>();

        // Send GET_CHARGE_CFG (0x19)
        bool sent = _serialService.SendFrame((byte)DebugCmd.GET_CHARGE_CFG);
        if (!sent)
        {
            btnReadMcu.IsEnabled = true;
            btnReadMcu.Content = "Read MCU";
            lblStatusPrompt.Text = "Không thể gửi lệnh đọc cấu hình";
            if (!isAutoLoad)
            {
                MessageBox.Show("Không thể truyền lệnh đọc tới MCU. Vui lòng kiểm tra cáp kết nối.", "Lỗi truyền thông", MessageBoxButton.OK, MessageBoxImage.Error);
            }
            return;
        }

        // Wait with 2.5-second timeout, retry once if needed
        var delayTask = Task.Delay(2500);
        var completedTask = await Task.WhenAny(_cfgReadTcs.Task, delayTask);

        if (completedTask != _cfgReadTcs.Task && _serialService.IsConnected)
        {
            // Auto-retry once in case first frame arrived during CDC line transition
            _serialService.SendFrame((byte)DebugCmd.GET_CHARGE_CFG);
            completedTask = await Task.WhenAny(_cfgReadTcs.Task, Task.Delay(2500));
        }

        btnReadMcu.IsEnabled = true;
        btnReadMcu.Content = "Read MCU";

        if (completedTask == _cfgReadTcs.Task)
        {
            var config = await _cfgReadTcs.Task;
            ViewModel.LoadConfig(config);
            ResetTextBoxBorders(this);
            lblStatusPrompt.Text = isAutoLoad ? "Cấu hình từ MCU đã được tự động đồng bộ!" : "Đã tải và đồng bộ cấu hình từ MCU thành công!";
            string msg = isAutoLoad 
                ? "Đã kết nối và tự động tải cấu hình từ MCU thành công!" 
                : "Đã đọc và tải cấu hình chu kỳ sạc từ MCU thành công!";
            MessageBox.Show(msg, "Đọc cấu hình MCU", MessageBoxButton.OK, MessageBoxImage.Information);
        }
        else
        {
            lblStatusPrompt.Text = isAutoLoad ? "Tự động tải cấu hình: MCU chưa phản hồi kịp" : "Hết thời gian chờ phản hồi từ MCU";
            if (!isAutoLoad)
            {
                MessageBox.Show("Không nhận được phản hồi từ MCU sau 5 giây. Vui lòng kiểm tra kết nối và trạng thái hoạt động của MCU.", "Hết thời gian chờ (Timeout)", MessageBoxButton.OK, MessageBoxImage.Warning);
            }
        }

        _cfgReadTcs = null;
    }

    private void OnChargeConfigReceived(ChargeCycleConfig config)
    {
        Dispatcher.Invoke(() =>
        {
            ViewModel.LoadConfig(config);
            ResetTextBoxBorders(this);
            lblStatusPrompt.Text = "Đã nhận và đồng bộ cấu hình từ MCU!";
            _cfgReadTcs?.TrySetResult(config);
        });
    }

    private void OnMcuErrorReceived(byte code, string msg)
    {
        Dispatcher.Invoke(() =>
        {
            lblStatusPrompt.Text = $"Lỗi MCU: {msg}";
        });
    }

    private void BtnWriteMcu_Click(object sender, RoutedEventArgs e)
    {
        if (!_serialService.IsConnected)
        {
            MessageBox.Show("Vui lòng kết nối với MCU trước.", "Chưa kết nối", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        try
        {
            byte[] payload = ViewModel.GetBytes();
            if (payload.Length != ChargeCycleConfig.EXPECTED_BINARY_SIZE && payload.Length != ChargeCycleConfig.V7_BINARY_SIZE)
            {
                MessageBox.Show($"Lỗi đóng gói cấu hình: Kích thước dữ liệu không hợp lệ (nhận {payload.Length} bytes).", "Lỗi dữ liệu", MessageBoxButton.OK, MessageBoxImage.Error);
                return;
            }

            lblStatusPrompt.Text = "Đang ghi cấu hình vào Flash MCU...";
            bool sent = _serialService.SendFrame((byte)DebugCmd.SET_CHARGE_CFG, payload);

            if (sent)
            {
                ResetTextBoxBorders(this);
                lblStatusPrompt.Text = "Đã lưu cấu hình vào Flash MCU thành công";
                MessageBox.Show("Cấu hình đã được gửi và lưu thành công vào bộ nhớ Flash của MCU!", "Ghi cấu hình MCU", MessageBoxButton.OK, MessageBoxImage.Information);
            }
            else
            {
                lblStatusPrompt.Text = "Gửi cấu hình tới MCU thất bại";
                MessageBox.Show("Không thể gửi gói cấu hình tới MCU.", "Ghi thất bại", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Lỗi đóng gói dữ liệu cấu hình: {ex.Message}", "Lỗi", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private void ImportConfig_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog
        {
            Filter = "Tệp JSON (*.json)|*.json|Tất cả tệp (*.*)|*.*",
            Title = "Nhập (Import) cấu hình sạc"
        };

        if (dlg.ShowDialog() == true)
        {
            try
            {
                string json = File.ReadAllText(dlg.FileName);
                ViewModel.LoadFromJson(json);
                ResetTextBoxBorders(this);
                lblStatusPrompt.Text = $"Đã nhập cấu hình từ {Path.GetFileName(dlg.FileName)}";
                MessageBox.Show($"Đã nhập (Import) cấu hình thành công từ tệp:\n{dlg.FileName}", "Nhập cấu hình", MessageBoxButton.OK, MessageBoxImage.Information);
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Lỗi đọc tệp cấu hình JSON:\n{ex.Message}", "Lỗi nhập cấu hình", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }
    }

    private void ExportConfig_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new SaveFileDialog
        {
            Filter = "Tệp JSON (*.json)|*.json|Tất cả tệp (*.*)|*.*",
            FileName = "charge_config.json",
            Title = "Xuất (Export) cấu hình sạc"
        };

        if (dlg.ShowDialog() == true)
        {
            try
            {
                string json = ViewModel.GetJson();
                File.WriteAllText(dlg.FileName, json);
                lblStatusPrompt.Text = $"Đã xuất cấu hình ra {Path.GetFileName(dlg.FileName)}";
                MessageBox.Show($"Đã xuất (Export) cấu hình thành công ra tệp:\n{dlg.FileName}", "Xuất cấu hình", MessageBoxButton.OK, MessageBoxImage.Information);
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Lỗi lưu tệp cấu hình:\n{ex.Message}", "Lỗi xuất cấu hình", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }
    }

    private void Defaults_Click(object sender, RoutedEventArgs e)
    {
        if (MessageBox.Show("Bạn có chắc chắn muốn khôi phục tất cả thông số về giá trị mặc định xuất xưởng?", "Xác nhận khôi phục mặc định", MessageBoxButton.YesNo, MessageBoxImage.Question) == MessageBoxResult.Yes)
        {
            ViewModel.ResetToDefaults();
            ResetTextBoxBorders(this);
            lblStatusPrompt.Text = "Đã tải cấu hình mặc định xuất xưởng";
            MessageBox.Show("Đã khôi phục các thông số cấu hình mặc định xuất xưởng thành công.", "Mặc định", MessageBoxButton.OK, MessageBoxImage.Information);
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
