using System;
using System.IO;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Media;
using Microsoft.Win32;
using ChargerDebugApp.Protocol;

namespace ChargerDebugApp;

public partial class McuFirmwareUpdateDialog : Window
{
    private readonly SerialService _serial;
    private byte[]? _fileBytes;
    private uint _fileCrc32;
    private TaskCompletionSource<bool>? _ackTcs;
    private byte _waitingAckCmd;
    private byte _lastNackCmd;
    private byte _lastNackErr;
    private CancellationTokenSource? _uploadCts;
    private bool _isUploading;
    private bool _isChecking4G;
    private OtaStatusView? _latestOtaStatus;
    private string _currentMcuVersionStr = "v2.0.0";

    public McuFirmwareUpdateDialog(SerialService serialService)
    {
        InitializeComponent();
        _serial = serialService;

        _serial.Parser.OnOtaStatusReceived += OnOtaStatusReceived;
        _serial.Parser.OnQuectelStatusReceived += OnQuectelStatusReceived;
        _serial.Parser.OnSystemInfoReceived += OnSystemInfoReceived;
        _serial.Parser.OnAckReceived += OnAckReceived;
        _serial.Parser.OnNackReceived += OnNackReceived;

        Loaded += McuFirmwareUpdateDialog_Loaded;
        Closed += McuFirmwareUpdateDialog_Closed;
    }

    private void McuFirmwareUpdateDialog_Loaded(object sender, RoutedEventArgs e)
    {
        if (!_serial.IsConnected)
        {
            SetStatus("Chưa kết nối cổng COM tới STM32!", true);
            BadgeSafe.Background = new SolidColorBrush(Color.FromRgb(254, 242, 242));
            BadgeSafe.BorderBrush = new SolidColorBrush(Color.FromRgb(254, 202, 202));
            TxtSafeStatus.Foreground = new SolidColorBrush(Color.FromRgb(185, 28, 28));
            TxtSafeStatus.Text = "🔴 CHƯA KẾT NỐI";
            return;
        }

        RequestStatus();
        Request4GStatus();
    }

    private void McuFirmwareUpdateDialog_Closed(object? sender, EventArgs e)
    {
        _uploadCts?.Cancel();
        _serial.Parser.OnOtaStatusReceived -= OnOtaStatusReceived;
        _serial.Parser.OnQuectelStatusReceived -= OnQuectelStatusReceived;
        _serial.Parser.OnSystemInfoReceived -= OnSystemInfoReceived;
        _serial.Parser.OnAckReceived -= OnAckReceived;
        _serial.Parser.OnNackReceived -= OnNackReceived;
    }

    private void RequestStatus()
    {
        _serial.SendFrame((byte)DebugCmd.GET_SYSTEM);
        _serial.SendFrame((byte)PcCmd.GET_OTA_STATUS);
        if (!_isUploading && !_isChecking4G)
        {
            SetStatus("Đang đọc phiên bản và trạng thái OTA từ MCU...");
        }
    }

    private void Request4GStatus()
    {
        _serial.SendFrame((byte)PcCmd.GET_4G_STATUS);
    }

    private void OnSystemInfoReceived(SystemInfo sys)
    {
        _currentMcuVersionStr = $"v{sys.FwMajor}.{sys.FwMinor}.{sys.FwPatch}";
        Dispatcher.Invoke(() =>
        {
            TxtCurrentVersion.Text = _currentMcuVersionStr;
            Txt4gCurrentFw.Text = _currentMcuVersionStr;
        });
    }

    private void TabControlMain_SelectionChanged(object sender, System.Windows.Controls.SelectionChangedEventArgs e)
    {
        if (e.Source != TabControlMain) return;
        if (_isUploading || _isChecking4G) return;

        if (TabControlMain.SelectedIndex == 0)
        {
            SetStatus("Sẵn sàng nạp firmware trực tiếp qua cáp USB.");
        }
        else if (TabControlMain.SelectedIndex == 1)
        {
            SetStatus("Sẵn sàng: Bấm 'Kiểm tra bản mới (4G)' để đối soát phiên bản với máy chủ Cloud.");
            if (_serial.IsConnected)
            {
                Request4GStatus();
                RequestStatus();
            }
        }
    }

    private void OnOtaStatusReceived(OtaStatusView status)
    {
        _latestOtaStatus = status;
        Dispatcher.Invoke(() =>
        {
            string statusStr = status.Status switch
            {
                OtaStatusCode.IDLE => "IDLE (Rảnh rỗi)",
                OtaStatusCode.DOWNLOADING => "Đang tải bản mới...",
                OtaStatusCode.DOWNLOADED => "Đã tải xong",
                OtaStatusCode.VERIFIED => "Đã kiểm tra tính toàn vẹn (Sẵn sàng nạp)",
                OtaStatusCode.APPLIED => "Đã nạp và đang chạy",
                OtaStatusCode.ERROR_SIZE => "Lỗi kích thước file",
                OtaStatusCode.ERROR_CRC => "Lỗi CRC / Sai lệch mã",
                OtaStatusCode.ERROR_FLASH => "Lỗi bộ nhớ Flash",
                OtaStatusCode.ERROR_NETWORK => "Lỗi mạng 4G / HTTP",
                OtaStatusCode.ERROR_TIMEOUT => "Quá thời gian tải (Timeout)",
                OtaStatusCode.ERROR_ROLLBACK => "Đã tự khôi phục về FW cũ",
                OtaStatusCode.BOOT_TEST => "Đang kiểm tra boot bản mới",
                _ => $"Mã trạng thái {status.Status}"
            };

            TxtOtaStatus.Text = statusStr;
            TxtCurrentVersion.Text = _currentMcuVersionStr;
            Txt4gCurrentFw.Text = _currentMcuVersionStr;

            ChkAutoPolicy.IsChecked = status.PolicyEnabled != 0;

            if (status.Status == OtaStatusCode.VERIFIED)
            {
                uint v = status.Version;
                string newVerStr = $"v{(v >> 16) & 0xFF}.{(v >> 8) & 0xFF}.{v & 0xFF}";
                Txt4gCloudFw.Text = $"{newVerStr} (Có bản mới)";
                Txt4gCloudFw.Foreground = new SolidColorBrush(Color.FromRgb(22, 101, 52));
                TxtFwCheckSummary.Text = "🔥 CÓ BẢN MỚI SẴN SÀNG!";
                TxtFwCheckSummary.Foreground = new SolidColorBrush(Color.FromRgb(22, 101, 52));
                BorderFwCheckResult.Background = new SolidColorBrush(Color.FromRgb(220, 252, 231));
                BorderFwCheckResult.BorderBrush = new SolidColorBrush(Color.FromRgb(134, 239, 172));

                BtnApply4G.IsEnabled = true;
                BadgeSafe.Background = new SolidColorBrush(Color.FromRgb(220, 252, 231));
                BadgeSafe.BorderBrush = new SolidColorBrush(Color.FromRgb(134, 239, 172));
                TxtSafeStatus.Foreground = new SolidColorBrush(Color.FromRgb(22, 101, 52));
                TxtSafeStatus.Text = "🟢 FW MỚI SẴN SÀNG";

                if (!_isUploading && !_isChecking4G)
                {
                    ProgressBarUpload.Value = 100;
                    TxtProgressPercent.Text = "100%";
                    SetStatus($"Đã có bản firmware mới {newVerStr} trong Flash đệm! Bấm 'Áp dụng & Khởi động lại' để nạp.");
                }
            }
            else
            {
                BadgeSafe.Background = new SolidColorBrush(Color.FromRgb(220, 252, 231));
                BadgeSafe.BorderBrush = new SolidColorBrush(Color.FromRgb(134, 239, 172));
                TxtSafeStatus.Foreground = new SolidColorBrush(Color.FromRgb(22, 101, 52));
                TxtSafeStatus.Text = "🟢 IDLE - SẴN SÀNG";
            }
        });
    }

    private void OnAckReceived(byte cmd)
    {
        if (_waitingAckCmd == cmd && _ackTcs != null && !_ackTcs.Task.IsCompleted)
        {
            _ackTcs.TrySetResult(true);
        }
    }

    private void OnNackReceived(byte cmd, byte err)
    {
        _lastNackCmd = cmd;
        _lastNackErr = err;

        if (_waitingAckCmd == cmd && _ackTcs != null && !_ackTcs.Task.IsCompleted)
        {
            _ackTcs.TrySetResult(false);
        }

        Dispatcher.Invoke(() =>
        {
            string errStr = err switch
            {
                0x01 => "BAD_CRC (Sai mã kiểm tra CRC)",
                0x02 => "UNKNOWN_CMD (Lệnh không hỗ trợ)",
                0x03 => "BAD_LENGTH (Độ dài gói tin sai)",
                0x04 => "CAN_FAIL (Truyền CAN thất bại)",
                0x05 => "BAD_PARAM (Tham số không hợp lệ)",
                0x20 => "OTA_BUSY (Tiến trình OTA đang bận xử lý đợt kiểm tra khác)",
                0x21 => "OTA_NOT_SAFE (Không an toàn: Trạm sạc đang chạy, dừng khẩn cấp hoặc đang có cờ FAULT)",
                0x22 => "OTA_NET_NOT_READY (Module SIM 4G chưa sẵn sàng kết nối Data)",
                0x23 => "OTA_FLASH_BUSY (Bộ nhớ Flash SPI ngoài đang bận)",
                0x24 => "OTA_POLICY_DISABLED (Cấu hình Auto-OTA đang tắt hoặc URL trống)",
                _ => $"Mã lỗi 0x{err:X2}"
            };
            SetStatus($"[THÔNG BÁO] {errStr}", true);
        });
    }

    private void BtnRefreshStatus_Click(object sender, RoutedEventArgs e)
    {
        RequestStatus();
    }

    private void BtnQuery4G_Click(object sender, RoutedEventArgs e)
    {
        if (!_serial.IsConnected)
        {
            SetStatus("Chưa kết nối cổng COM tới STM32!", true);
            return;
        }
        Request4GStatus();
        SetStatus("Đang truy vấn thông tin module 4G từ MCU...");
    }

    private void OnQuectelStatusReceived(QuectelNetStatus s)
    {
        Dispatcher.Invoke(() =>
        {
            Txt4gPower.Text = s.Powered ? "BẬT" : "TẮT";
            Txt4gPower.Foreground = s.Powered 
                ? new SolidColorBrush(Color.FromRgb(22, 163, 74)) 
                : new SolidColorBrush(Color.FromRgb(220, 38, 38));

            Txt4gSim.Text = s.SimReady ? "ĐÃ NHẬN (READY)" : "CHƯA CÓ SIM / LỖI";
            Txt4gSim.Foreground = s.SimReady 
                ? new SolidColorBrush(Color.FromRgb(22, 163, 74)) 
                : new SolidColorBrush(Color.FromRgb(220, 38, 38));

            if (s.CsqRssi == 99 || s.CsqRssi == 0)
            {
                Txt4gSignal.Text = "Không có sóng (0/31)";
                Txt4gSignal.Foreground = new SolidColorBrush(Color.FromRgb(220, 38, 38));
            }
            else
            {
                Txt4gSignal.Text = $"{s.CsqRssi}/31";
                Txt4gSignal.Foreground = new SolidColorBrush(Color.FromRgb(15, 23, 42));
            }

            if (s.NetRegistered)
            {
                Txt4gNet.Text = s.PdpActive ? "ĐÃ KẾT NỐI DATA" : "ĐÃ ĐĂNG KÝ MẠNG";
                Txt4gNet.Foreground = new SolidColorBrush(Color.FromRgb(22, 163, 74));
            }
            else
            {
                Txt4gNet.Text = "CHƯA KẾT NỐI";
                Txt4gNet.Foreground = new SolidColorBrush(Color.FromRgb(220, 38, 38));
            }

            Txt4gIp.Text = !string.IsNullOrWhiteSpace(s.IpAddr) ? s.IpAddr : "--";
            Txt4gModel.Text = !string.IsNullOrWhiteSpace(s.Model) ? s.Model : "--";

            SetStatus($"Cập nhật trạng thái module 4G: Nguồn={(s.Powered ? "ON" : "OFF")}, SIM={(s.SimReady ? "OK" : "Lỗi/Trống")}, Mạng={(s.NetRegistered ? "OK" : "Chưa")}, CSQ={s.CsqRssi}.");
        });
    }

    private async void BtnSavePolicy_Click(object sender, RoutedEventArgs e)
    {
        if (!_serial.IsConnected)
        {
            MessageBox.Show("Chưa kết nối cổng COM!", "Cảnh báo", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        bool enabled = ChkAutoPolicy.IsChecked == true;
        if (!uint.TryParse(TxtIntervalHours.Text.Trim(), out uint hours) || hours == 0)
        {
            hours = 6;
            TxtIntervalHours.Text = "6";
        }

        uint intervalMs = hours * 3600U * 1000U;
        string url = TxtManifestUrl.Text.Trim();

        if (enabled && (string.IsNullOrEmpty(url) || !url.StartsWith("https://", StringComparison.OrdinalIgnoreCase)))
        {
            MessageBox.Show("Manifest URL phải bắt đầu bằng https://", "Cảnh báo", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        byte[] urlBytes = Encoding.ASCII.GetBytes(url);
        byte[] payload = new byte[1 + 4 + urlBytes.Length];
        payload[0] = (byte)(enabled ? 1 : 0);
        Array.Copy(BitConverter.GetBytes(intervalMs), 0, payload, 1, 4);
        Array.Copy(urlBytes, 0, payload, 5, urlBytes.Length);

        SetStatus("Đang lưu chính sách Auto-OTA xuống Flash MCU...");
        using var cts = new CancellationTokenSource(3000);
        bool ok = await SendCommandWithAckAsync((byte)PcCmd.SET_OTA_POLICY, payload, 3000, cts.Token);
        if (ok)
        {
            SetStatus($"Đã lưu chính sách Auto-OTA (Enabled: {enabled}, Chu kỳ: {hours} giờ).");
            MessageBox.Show("Đã lưu chính sách Auto-OTA vào Flash MCU thành công!", "Thành công", MessageBoxButton.OK, MessageBoxImage.Information);
        }
        else
        {
            string reason = _lastNackErr switch
            {
                0x21 => "Điều kiện an toàn không đạt (đang sạc hoặc có lỗi)",
                0x23 => "Flash SPI ngoài bị bận hoặc lỗi ghi",
                _ => $"Mã lỗi 0x{_lastNackErr:X2} hoặc MCU không phản hồi"
            };
            SetStatus($"Lưu chính sách Auto-OTA thất bại ({reason})!", true);
            MessageBox.Show($"MCU không phản hồi hoặc từ chối lưu chính sách OTA.\nChi tiết: {reason}", "Lỗi lưu cấu hình", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private async void BtnCheckNow4G_Click(object sender, RoutedEventArgs e)
    {
        if (!_serial.IsConnected)
        {
            MessageBox.Show("Chưa kết nối cổng COM tới STM32!", "Cảnh báo", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        if (_isUploading || _isChecking4G)
        {
            MessageBox.Show("Một tiến trình khác đang thực hiện, vui lòng chờ...", "Thông báo", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }

        string url = TxtManifestUrl.Text.Trim();
        if (string.IsNullOrEmpty(url) || !url.StartsWith("https://", StringComparison.OrdinalIgnoreCase))
        {
            MessageBox.Show("Manifest URL phải bắt đầu bằng https://", "Cảnh báo", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        _isChecking4G = true;
        SetUiUploading(true);
        Txt4gCloudFw.Text = "Đang kiểm tra...";
        Txt4gCloudFw.Foreground = new SolidColorBrush(Color.FromRgb(37, 99, 235));
        TxtFwCheckSummary.Text = "Đang kết nối 4G...";
        TxtFwCheckSummary.Foreground = new SolidColorBrush(Color.FromRgb(37, 99, 235));
        BorderFwCheckResult.Background = new SolidColorBrush(Color.FromRgb(239, 246, 255));
        BorderFwCheckResult.BorderBrush = new SolidColorBrush(Color.FromRgb(147, 197, 253));
        TxtFwCheckTimestamp.Text = DateTime.Now.ToString("HH:mm:ss");

        UpdateProgress(5, "Đang chuẩn bị gửi cấu hình và yêu cầu MCU kiểm tra bản mới qua 4G...");

        try
        {
            // Bước 1: Gửi SET_OTA_POLICY để cập nhật URL và chờ ACK
            SetStatus("Đang đồng bộ URL và chính sách OTA xuống MCU...");
            byte[] urlBytes = Encoding.ASCII.GetBytes(url);
            byte[] payload = new byte[1 + 4 + urlBytes.Length];
            payload[0] = 1; // Cho phép OTA
            Array.Copy(BitConverter.GetBytes(6U * 3600U * 1000U), 0, payload, 1, 4);
            Array.Copy(urlBytes, 0, payload, 5, urlBytes.Length);

            using var policyCts = new CancellationTokenSource(3000);
            bool policyOk = await SendCommandWithAckAsync((byte)PcCmd.SET_OTA_POLICY, payload, 3000, policyCts.Token);
            if (!policyOk)
            {
                Txt4gCloudFw.Text = "Lỗi lưu URL";
                Txt4gCloudFw.Foreground = new SolidColorBrush(Color.FromRgb(220, 38, 38));
                TxtFwCheckSummary.Text = "❌ LỖI ĐỒNG BỘ URL";
                TxtFwCheckSummary.Foreground = new SolidColorBrush(Color.FromRgb(220, 38, 38));
                BorderFwCheckResult.Background = new SolidColorBrush(Color.FromRgb(254, 226, 226));
                BorderFwCheckResult.BorderBrush = new SolidColorBrush(Color.FromRgb(252, 165, 165));

                UpdateProgress(0, "Không thể lưu URL và cấu hình OTA vào Flash SPI của MCU.");
                SetStatus("MCU từ chối cấu hình OTA (SET_OTA_POLICY thất bại)!", true);

                MessageBox.Show(
                    "Không thể đồng bộ cấu hình OTA xuống MCU!\n\n" +
                    "MCU không phản hồi hoặc từ chối cấu hình URL mới vào Flash SPI.\n" +
                    "Khuyến nghị: Thử lại hoặc chuyển sang tab 'Nạp qua cáp USB'.",
                    "Lỗi cấu hình OTA",
                    MessageBoxButton.OK,
                    MessageBoxImage.Warning);
                return;
            }

            // Bước 2: Gửi lệnh OTA_CHECK_NOW và chờ MCU phản hồi
            SetStatus("Đang gửi lệnh yêu cầu MCU kích hoạt kết nối 4G...");
            using var ackCts = new CancellationTokenSource(5000);
            bool accepted = await SendCommandWithAckAsync((byte)PcCmd.OTA_CHECK_NOW, null, 5000, ackCts.Token);

            if (!accepted)
            {
                string reason = _lastNackErr switch
                {
                    0x20 => "MCU đang thực hiện một tiến trình OTA khác (Busy).",
                    0x21 => "Điều kiện an toàn không đạt (Hệ thống đang sạc hoặc đang có lỗi / E-stop).",
                    0x22 => "Module SIM 4G chưa sẵn sàng kết nối mạng (chưa có Data PDP / chưa gắn SIM).",
                    0x23 => "Bộ nhớ Flash SPI ngoài đang bận hoặc bị lỗi.",
                    0x24 => "Chính sách OTA trên MCU đang bị tắt.",
                    _ => "Module 4G chưa có Data, Flash bận hoặc MCU đang bận tác vụ khác."
                };

                Txt4gCloudFw.Text = "Từ chối";
                Txt4gCloudFw.Foreground = new SolidColorBrush(Color.FromRgb(220, 38, 38));
                TxtFwCheckSummary.Text = "❌ MCU TỪ CHỐI";
                TxtFwCheckSummary.Foreground = new SolidColorBrush(Color.FromRgb(220, 38, 38));
                BorderFwCheckResult.Background = new SolidColorBrush(Color.FromRgb(254, 226, 226));
                BorderFwCheckResult.BorderBrush = new SolidColorBrush(Color.FromRgb(252, 165, 165));

                UpdateProgress(0, $"MCU từ chối lệnh kiểm tra 4G: {reason}");
                SetStatus($"MCU từ chối lệnh kiểm tra 4G (Mã lỗi: 0x{_lastNackErr:X2}). Khuyến nghị nạp qua tab 'Nạp qua cáp USB'.", true);

                MessageBox.Show(
                    $"MCU từ chối kích hoạt kiểm tra 4G!\n\n" +
                    $"Chi tiết nguyên nhân từ MCU (Mã 0x{_lastNackErr:X2}):\n" +
                    $"• {reason}\n\n" +
                    $"Khuyến nghị: Bạn hãy kiểm tra lại trạng thái sạc/SIM hoặc chuyển sang Tab 'Nạp qua cáp USB' để nạp trực tiếp.",
                    "Kiểm tra 4G",
                    MessageBoxButton.OK,
                    MessageBoxImage.Warning);
                return;
            }

            // Reset status cũ trước khi bắt đầu chu kỳ polling
            _latestOtaStatus = null;

            // Bước 3: MCU đã bắt đầu kiểm tra qua 4G LTE -> Polling GET_OTA_STATUS định kỳ
            UpdateProgress(15, "MCU đã kích hoạt module 4G và đang kết nối Cloud...");
            SetStatus("Đang tải và kiểm tra phiên bản mới qua mạng 4G...");

            const int maxWaitSeconds = 35;
            bool completed = false;

            for (int sec = 1; sec <= maxWaitSeconds; sec++)
            {
                await Task.Delay(1000);

                // Gửi lệnh đọc trạng thái OTA từ MCU
                _serial.SendFrame((byte)PcCmd.GET_OTA_STATUS);

                int simulatedPercent = Math.Min(90, 15 + (sec * 75 / maxWaitSeconds));
                UpdateProgress(simulatedPercent, $"Đang kết nối Cloud qua module 4G... ({sec}s/{maxWaitSeconds}s)");

                if (_latestOtaStatus != null)
                {
                    if (_latestOtaStatus.Status == OtaStatusCode.VERIFIED)
                    {
                        uint v = _latestOtaStatus.Version;
                        string verStr = $"v{(v >> 16) & 0xFF}.{(v >> 8) & 0xFF}.{v & 0xFF}";
                        Txt4gCloudFw.Text = $"{verStr} (Có bản mới!)";
                        Txt4gCloudFw.Foreground = new SolidColorBrush(Color.FromRgb(22, 101, 52));
                        TxtFwCheckSummary.Text = "🔥 CÓ BẢN MỚI SẴN SÀNG!";
                        TxtFwCheckSummary.Foreground = new SolidColorBrush(Color.FromRgb(22, 101, 52));
                        BorderFwCheckResult.Background = new SolidColorBrush(Color.FromRgb(220, 252, 231));
                        BorderFwCheckResult.BorderBrush = new SolidColorBrush(Color.FromRgb(134, 239, 172));

                        UpdateProgress(100, $"🎉 PHÁT HIỆN BẢN MỚI {verStr}! Bấm nút 'Áp dụng & Khởi động lại' để nạp vào MCU.");
                        SetStatus($"Kiểm tra 4G thành công! Đã tìm thấy bản mới {verStr}. Sẵn sàng nạp.");
                        BtnApply4G.IsEnabled = true;
                        completed = true;

                        MessageBox.Show(
                            $"🎉 Đã tìm thấy bản firmware mới {verStr} trên Cloud!\n\n" +
                            "Bản mới đã được tải và xác thực toàn vẹn CRC32 thành công.\n" +
                            "Bạn có thể bấm nút 'Áp dụng & Khởi động lại' để MCU boot bản mới.",
                            "Phát hiện Firmware Mới",
                            MessageBoxButton.OK,
                            MessageBoxImage.Information);
                        break;
                    }
                    else if (_latestOtaStatus.Status == OtaStatusCode.DOWNLOADED)
                    {
                        UpdateProgress(95, "Đã tải xong gói firmware, đang xác thực toàn vẹn mã CRC32...");
                    }
                    else if (_latestOtaStatus.Status >= OtaStatusCode.ERROR_SIZE)
                    {
                        string errDesc = _latestOtaStatus.Status switch
                        {
                            OtaStatusCode.ERROR_NETWORK => "Lỗi kết nối mạng 4G hoặc máy chủ manifest không phản hồi",
                            OtaStatusCode.ERROR_TIMEOUT => "Hết thời gian chờ phản hồi mạng từ module 4G",
                            OtaStatusCode.ERROR_CRC => "Lỗi kiểm tra tính toàn vẹn mã CRC32 của firmware tải về",
                            OtaStatusCode.ERROR_SIZE => "Kích thước tệp firmware trên máy chủ không hợp lệ",
                            OtaStatusCode.ERROR_FLASH => "Lỗi ghi bộ nhớ Flash đệm SPI trên MCU",
                            _ => $"Mã lỗi {_latestOtaStatus.Status}"
                        };

                        Txt4gCloudFw.Text = "Lỗi kết nối";
                        Txt4gCloudFw.Foreground = new SolidColorBrush(Color.FromRgb(220, 38, 38));
                        TxtFwCheckSummary.Text = "❌ KIỂM TRA THẤT BẠI";
                        TxtFwCheckSummary.Foreground = new SolidColorBrush(Color.FromRgb(220, 38, 38));
                        BorderFwCheckResult.Background = new SolidColorBrush(Color.FromRgb(254, 226, 226));
                        BorderFwCheckResult.BorderBrush = new SolidColorBrush(Color.FromRgb(252, 165, 165));

                        UpdateProgress(0, $"❌ Kiểm tra 4G thất bại: {errDesc}. Khuyến nghị nạp qua cáp USB ở Tab 1.");
                        SetStatus($"[THÔNG BÁO] {errDesc}", true);
                        completed = true;

                        MessageBox.Show(
                            $"Kiểm tra cập nhật qua 4G không thành công!\n\n" +
                            $"Chi tiết lỗi: {errDesc}\n" +
                            $"URL máy chủ: {url}\n\n" +
                            "Khuyến nghị: Nếu máy chủ Cloud chưa triển khai hoặc SIM chưa có Data, bạn hãy sử dụng Tab 'Nạp qua cáp USB' để nạp trực tiếp an toàn.",
                            "Kết quả Kiểm tra 4G",
                            MessageBoxButton.OK,
                            MessageBoxImage.Warning);
                        break;
                    }
                    else if (sec >= 4 && _latestOtaStatus.Status == OtaStatusCode.IDLE)
                    {
                        // MCU hoàn tất kiểm tra manifest và thấy FW hiện tại đã là bản mới nhất
                        Txt4gCloudFw.Text = $"{_currentMcuVersionStr} (Đã là mới nhất)";
                        Txt4gCloudFw.Foreground = new SolidColorBrush(Color.FromRgb(22, 101, 52));
                        TxtFwCheckSummary.Text = "✅ ĐÃ LÀ BẢN MỚI NHẤT";
                        TxtFwCheckSummary.Foreground = new SolidColorBrush(Color.FromRgb(22, 101, 52));
                        BorderFwCheckResult.Background = new SolidColorBrush(Color.FromRgb(220, 252, 231));
                        BorderFwCheckResult.BorderBrush = new SolidColorBrush(Color.FromRgb(134, 239, 172));

                        UpdateProgress(100, $"✅ HOÀN TẤT: Firmware trên MCU ({_currentMcuVersionStr}) hiện tại đã là phiên bản mới nhất! Không có bản cập nhật mới nào trên Cloud.");
                        SetStatus($"Kiểm tra hoàn tất: Firmware trên MCU ({_currentMcuVersionStr}) đã là phiên bản mới nhất.");
                        completed = true;

                        MessageBox.Show(
                            $"Firmware trên MCU ({_currentMcuVersionStr}) hiện tại đã là phiên bản mới nhất!\n\n" +
                            "Không có bản cập nhật mới nào trên máy chủ Cloud.",
                            "Kết quả Kiểm tra 4G",
                            MessageBoxButton.OK,
                            MessageBoxImage.Information);
                        break;
                    }
                }
            }

            if (!completed)
            {
                Txt4gCloudFw.Text = "Hết thời gian";
                Txt4gCloudFw.Foreground = new SolidColorBrush(Color.FromRgb(220, 38, 38));
                TxtFwCheckSummary.Text = "⚠️ TIMEOUT (35S)";
                TxtFwCheckSummary.Foreground = new SolidColorBrush(Color.FromRgb(220, 38, 38));
                BorderFwCheckResult.Background = new SolidColorBrush(Color.FromRgb(254, 226, 226));
                BorderFwCheckResult.BorderBrush = new SolidColorBrush(Color.FromRgb(252, 165, 165));

                UpdateProgress(0, "Hết thời gian chờ (Timeout sau 35s). Máy chủ hoặc mạng 4G không phản hồi kịp thời.");
                SetStatus("Kiểm tra 4G quá thời gian chờ (Timeout). Vui lòng thử lại hoặc sử dụng cáp USB.", true);

                MessageBox.Show(
                    "Quá thời gian chờ (Timeout sau 35 giây) khi kết nối mạng 4G Cloud!\n\n" +
                    "Khuyến nghị: Hãy kiểm tra sóng 4G hoặc sử dụng Tab 'Nạp qua cáp USB'.",
                    "Hết thời gian chờ",
                    MessageBoxButton.OK,
                    MessageBoxImage.Warning);
            }
        }
        catch (Exception ex)
        {
            UpdateProgress(0, $"Lỗi trong quá trình kiểm tra: {ex.Message}");
            SetStatus($"Lỗi: {ex.Message}", true);
        }
        finally
        {
            _isChecking4G = false;
            SetUiUploading(false);
        }
    }

    private void BtnApply4G_Click(object sender, RoutedEventArgs e)
    {
        if (!_serial.IsConnected)
        {
            MessageBox.Show("Chưa kết nối cổng COM!", "Cảnh báo", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        var result = MessageBox.Show(
            "Bạn có chắc chắn muốn kích hoạt cập nhật và khởi động lại MCU ngay bây giờ?",
            "Xác nhận Áp dụng Firmware",
            MessageBoxButton.YesNo,
            MessageBoxImage.Question);

        if (result == MessageBoxResult.Yes)
        {
            _serial.SendFrame((byte)PcCmd.OTA_APPLY);
            SetStatus("Đã gửi lệnh khởi động lại để nạp firmware mới...");
        }
    }

    private void BtnBrowseBin_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog
        {
            Filter = "Firmware Binary (*.bin)|*.bin|Tất cả tệp (*.*)|*.*",
            Title = "Chọn file Firmware Charger (.bin)"
        };

        if (dlg.ShowDialog(this) == true)
        {
            try
            {
                byte[] bytes = File.ReadAllBytes(dlg.FileName);
                if (bytes.Length == 0)
                {
                    MessageBox.Show("File được chọn rỗng!", "Lỗi", MessageBoxButton.OK, MessageBoxImage.Error);
                    return;
                }
                if (bytes.Length > 120 * 1024)
                {
                    MessageBox.Show($"File quá lớn ({bytes.Length / 1024} KB). Giới hạn tối đa là 120 KB!", "Lỗi", MessageBoxButton.OK, MessageBoxImage.Error);
                    return;
                }

                _fileBytes = bytes;
                _fileCrc32 = ComputeCrc32(bytes);

                TxtBinFilePath.Text = dlg.FileName;
                TxtSelectedFileSize.Text = $"{bytes.Length / 1024.0:F1} KB ({bytes.Length} bytes)";
                TxtSelectedFileCrc.Text = $"0x{_fileCrc32:X8}";

                SetStatus($"Đã chọn file {Path.GetFileName(dlg.FileName)} ({bytes.Length} bytes). Sẵn sàng nạp!");
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Không thể đọc file: {ex.Message}", "Lỗi", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }
    }

    private async void BtnStartUsbUpload_Click(object sender, RoutedEventArgs e)
    {
        if (!_serial.IsConnected)
        {
            MessageBox.Show("Chưa kết nối cổng COM tới STM32!", "Cảnh báo", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        if (_fileBytes == null || _fileBytes.Length == 0)
        {
            MessageBox.Show("Vui lòng chọn file firmware (.bin) trước khi bắt đầu nạp!", "Cảnh báo", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        if (_isUploading)
        {
            MessageBox.Show("Tiến trình nạp đang chạy, vui lòng chờ...", "Thông báo", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }

        var confirm = MessageBox.Show(
            $"Bắt đầu nạp firmware vào STM32 qua cáp USB?\n\n" +
            $"Tên file: {Path.GetFileName(TxtBinFilePath.Text)}\n" +
            $"Kích thước: {_fileBytes.Length} bytes\n" +
            $"CRC32: 0x{_fileCrc32:X8}\n\n" +
            $"Lưu ý: Đảm bảo trạm sạc đang dừng hoạt động.",
            "Xác nhận nạp Firmware",
            MessageBoxButton.YesNo,
            MessageBoxImage.Question);

        if (confirm != MessageBoxResult.Yes) return;

        _isUploading = true;
        _uploadCts = new CancellationTokenSource();
        SetUiUploading(true);

        try
        {
            await RunUploadAsync(_fileBytes, _fileCrc32, _uploadCts.Token);
        }
        catch (OperationCanceledException)
        {
            SetStatus("Tiến trình nạp đã bị hủy!", true);
        }
        catch (Exception ex)
        {
            SetStatus($"Lỗi trong quá trình nạp: {ex.Message}", true);
        }
        finally
        {
            _isUploading = false;
            SetUiUploading(false);
        }
    }

    private async Task RunUploadAsync(byte[] data, uint crc32, CancellationToken ct)
    {
        uint totalSize = (uint)data.Length;
        uint version = 0x010000; // Default 1.0.0

        // Bước 1: Gửi lệnh UPLOAD_START
        SetStatus("Đang xóa bộ nhớ Flash đệm (Staging) trên STM32...");
        UpdateProgress(0, "Chuẩn bị bộ nhớ Flash...");

        byte[] startPayload = new byte[12];
        Array.Copy(BitConverter.GetBytes(totalSize), 0, startPayload, 0, 4);
        Array.Copy(BitConverter.GetBytes(crc32), 0, startPayload, 4, 4);
        Array.Copy(BitConverter.GetBytes(version), 0, startPayload, 8, 4);

        bool startOk = await SendCommandWithAckAsync((byte)PcCmd.OTA_UPLOAD_START, startPayload, 10000, ct);
        if (!startOk)
        {
            throw new Exception("MCU từ chối lệnh khởi tạo nạp (Kiểm tra lại: Máy có đang sạc không?)");
        }

        // Bước 2: Gửi từng khối dữ liệu (128 bytes/khối)
        const int chunkSize = 128;
        int totalChunks = (data.Length + chunkSize - 1) / chunkSize;

        for (int i = 0; i < totalChunks; i++)
        {
            ct.ThrowIfCancellationRequested();

            int offset = i * chunkSize;
            int length = Math.Min(chunkSize, data.Length - offset);

            byte[] chunkPayload = new byte[4 + length];
            Array.Copy(BitConverter.GetBytes((uint)offset), 0, chunkPayload, 0, 4);
            Array.Copy(data, offset, chunkPayload, 4, length);

            bool chunkOk = await SendCommandWithAckAsync((byte)PcCmd.OTA_UPLOAD_CHUNK, chunkPayload, 3000, ct);
            if (!chunkOk)
            {
                throw new Exception($"MCU từ chối gói dữ liệu tại vị trí {offset} bytes!");
            }

            int percent = (int)((offset + length) * 100L / totalSize);
            UpdateProgress(percent, $"Đang nạp gói {i + 1}/{totalChunks} ({offset + length}/{totalSize} bytes)...");
        }

        // Bước 3: Gửi lệnh UPLOAD_FINISH
        UpdateProgress(100, "Đang xác thực kiểm tra mã CRC32 trên Flash ngoài...");
        SetStatus("Đang kiểm tra toàn vẹn dữ liệu...");

        bool finishOk = await SendCommandWithAckAsync((byte)PcCmd.OTA_UPLOAD_FINISH, null, 10000, ct);
        if (!finishOk)
        {
            throw new Exception("Xác thực CRC32 thất bại trên Flash ngoài STM32!");
        }

        SetStatus("NẠP FIRMWARE THÀNH CÔNG 100%!");
        UpdateProgress(100, "Đã nạp và xác thực thành công!");

        var askReboot = MessageBox.Show(
            "Nạp firmware vào bộ nhớ Flash thành công và đã vượt qua kiểm tra toàn vẹn!\n\n" +
            "Bạn có muốn khởi động lại MCU ngay bây giờ để Bootloader nạp vào chip không?",
            "Hoàn tất nạp Firmware",
            MessageBoxButton.YesNo,
            MessageBoxImage.Information);

        if (askReboot == MessageBoxResult.Yes)
        {
            _serial.SendFrame((byte)PcCmd.OTA_APPLY);
            SetStatus("Đã gửi lệnh khởi động lại. MCU đang boot bản mới...");
        }
    }

    private async Task<bool> SendCommandWithAckAsync(byte cmd, byte[]? payload, int timeoutMs, CancellationToken ct)
    {
        _waitingAckCmd = cmd;
        _ackTcs = new TaskCompletionSource<bool>();

        bool sent = _serial.SendFrame(cmd, payload);
        if (!sent) return false;

        using var timeoutCts = new CancellationTokenSource(timeoutMs);
        using var linkedCts = CancellationTokenSource.CreateLinkedTokenSource(ct, timeoutCts.Token);

        linkedCts.Token.Register(() => _ackTcs.TrySetCanceled());

        try
        {
            return await _ackTcs.Task;
        }
        catch (TaskCanceledException)
        {
            if (ct.IsCancellationRequested) throw new OperationCanceledException(ct);
            return false;
        }
        finally
        {
            _ackTcs = null;
        }
    }

    private void UpdateProgress(int percent, string message)
    {
        Dispatcher.Invoke(() =>
        {
            ProgressBarUpload.Value = percent;
            TxtProgressPercent.Text = $"{percent}%";
            TxtLogMessage.Text = message;
        });
    }

    private void SetStatus(string msg, bool isError = false)
    {
        Dispatcher.Invoke(() =>
        {
            TxtLogMessage.Text = msg;
            TxtLogMessage.Foreground = isError 
                ? new SolidColorBrush(Color.FromRgb(220, 38, 38)) 
                : new SolidColorBrush(Color.FromRgb(71, 85, 105));
        });
    }

    private void SetUiUploading(bool uploading)
    {
        BtnStartUsbUpload.IsEnabled = !uploading;
        BtnBrowseBin.IsEnabled = !uploading;
        BtnRefreshStatus.IsEnabled = !uploading;
        BtnQuery4G.IsEnabled = !uploading;
        BtnSavePolicy.IsEnabled = !uploading;
        BtnCheckNow4G.IsEnabled = !uploading;
        BtnApply4G.IsEnabled = !uploading && (_latestOtaStatus?.Status == OtaStatusCode.VERIFIED);
    }

    private static uint ComputeCrc32(byte[] data)
    {
        uint crc = 0xFFFFFFFF;
        foreach (byte b in data)
        {
            crc ^= b;
            for (int bit = 0; bit < 8; bit++)
            {
                crc = (crc >> 1) ^ (0xEDB88320U & (uint)-(int)(crc & 1));
            }
        }
        return crc ^ 0xFFFFFFFF;
    }

    private void BtnClose_Click(object sender, RoutedEventArgs e)
    {
        if (_isUploading)
        {
            var res = MessageBox.Show(
                "Tiến trình nạp firmware đang diễn ra. Bạn có thực sự muốn hủy và thoát không?",
                "Cảnh báo",
                MessageBoxButton.YesNo,
                MessageBoxImage.Warning);
            if (res != MessageBoxResult.Yes) return;
            _uploadCts?.Cancel();
        }
        Close();
    }
}
