using System;
using System.Threading;
using System.Windows;
using ChargerDebugApp.Services;

namespace ChargerDebugApp;

public partial class UpdateDialog : Window
{
    private readonly UpdateInfo _info;
    private CancellationTokenSource? _cts;

    public UpdateDialog(UpdateInfo info)
    {
        InitializeComponent();
        _info = info;

        CurrentVersionText.Text = $"v{UpdateService.CurrentVersion.Major}.{UpdateService.CurrentVersion.Minor}.{UpdateService.CurrentVersion.Build}";
        NewVersionText.Text     = _info.TagName.StartsWith('v') ? _info.TagName : $"v{_info.TagName}";
        ReleaseNotesText.Text   = string.IsNullOrWhiteSpace(_info.ReleaseNotes)
                                    ? "Không có thông tin release notes."
                                    : _info.ReleaseNotes;
    }

    private async void UpdateButton_Click(object sender, RoutedEventArgs e)
    {
        _cts = new CancellationTokenSource();

        // Disable buttons, show progress
        UpdateButton.IsEnabled = false;
        SkipButton.IsEnabled   = false;
        ProgressPanel.Visibility = Visibility.Visible;
        ProgressLabel.Text = "Đang kết nối...";

        try
        {
            var progress = new Progress<double>(pct =>
            {
                DownloadProgress.Value = pct * 100;
                ProgressLabel.Text = $"Đang tải... {pct:P0}";
            });

            var tempPath = await UpdateService.DownloadUpdateAsync(
                _info.DownloadUrl, progress, _cts.Token);

            ProgressLabel.Text = "Hoàn tất! Đang áp dụng cập nhật...";
            DownloadProgress.Value = 100;

            await System.Threading.Tasks.Task.Delay(800); // Brief pause so user sees 100%

            UpdateService.ApplyUpdate(tempPath);
        }
        catch (OperationCanceledException)
        {
            ResetUI();
        }
        catch (Exception ex)
        {
            MessageBox.Show(
                $"Tải update thất bại:\n{ex.Message}",
                "Lỗi cập nhật",
                MessageBoxButton.OK, MessageBoxImage.Error);
            ResetUI();
        }
    }

    private void SkipButton_Click(object sender, RoutedEventArgs e)
    {
        _cts?.Cancel();
        Close();
    }

    private void ResetUI()
    {
        UpdateButton.IsEnabled   = true;
        SkipButton.IsEnabled     = true;
        ProgressPanel.Visibility = Visibility.Collapsed;
        DownloadProgress.Value   = 0;
    }

    protected override void OnClosed(EventArgs e)
    {
        _cts?.Cancel();
        base.OnClosed(e);
    }
}
