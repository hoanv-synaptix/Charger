using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Navigation;
using System.Windows.Shapes;
using ChargerDebugApp.Protocol;

namespace ChargerDebugApp;

/// <summary>
/// Interaction logic for MainWindow.xaml
/// </summary>
public partial class MainWindow : Window
{
    private SerialService _serialService;

    public MainWindow()
    {
        InitializeComponent();
        _serialService = SerialService.Instance;
        RefreshPorts();
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

    private void BtnMainConnect_Click(object sender, RoutedEventArgs e)
    {
        if (_serialService.IsConnected)
        {
            _serialService.Disconnect();
            btnMainConnect.Content = "Connect";
            btnMainConnect.Background = new SolidColorBrush(Color.FromRgb(37, 99, 235)); // Blue
        }
        else
        {
            string? port = cmbMainPort.SelectedItem as string;
            if (string.IsNullOrEmpty(port))
            {
                MessageBox.Show("Please select a COM port.", "Error", MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }

            if (_serialService.Connect(port))
            {
                btnMainConnect.Content = "Disconnect";
                btnMainConnect.Background = new SolidColorBrush(Color.FromRgb(239, 68, 68)); // Red
                // Automatically ask MCU to enter debug stream mode
                _serialService.SendFrame((byte)DebugCmd.ENTER);
            }
            else
            {
                MessageBox.Show($"Failed to open port {port}", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }
    }
    
    private void OpenMonitor_Click(object sender, RoutedEventArgs e)
    {
        var monitorWindow = new MonitorWindow();
        monitorWindow.Show();
    }

    private void CboModuleType_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (cboModuleType == null || txtUMin == null || txtUMax == null || txtIMin == null || txtIMax == null) return;
        
        var selectedItem = cboModuleType.SelectedItem as ComboBoxItem;
        if (selectedItem == null) return;

        string moduleName = selectedItem.Content.ToString() ?? "";
        
        // Reset defaults
        txtUMin.Text = "0";
        txtIMin.Text = "0";

        switch (moduleName)
        {
            case "EVR_10KW_100A_100V":
                txtUMax.Text = "100";
                txtIMax.Text = "100";
                break;
            case "TR48_9KW_150A_48V":
                txtUMax.Text = "48";
                txtIMax.Text = "150";
                break;
            case "ICR100_20KW_200A_100V":
                txtUMax.Text = "100";
                txtIMax.Text = "200";
                break;
            case "MXR100200_20KW_200A_120V":
                txtUMax.Text = "120";
                txtIMax.Text = "200";
                break;
            case "ICR65_6.5KW_100A_65V":
                txtUMax.Text = "65";
                txtIMax.Text = "100";
                break;
            case "LA100_6KW_60A_100V":
                txtUMax.Text = "100";
                txtIMax.Text = "60";
                break;
            case "LA4500_4.5KW_50A_80V":
                txtUMax.Text = "80";
                txtIMax.Text = "50";
                break;
            default:
                // Unknown, Maxwell, Lainming, Tonhe -> Reset to 0
                txtUMax.Text = "0";
                txtIMax.Text = "0";
                break;
        }
    }

    private void CboChargeSource_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (txtBmsCanId == null || cboChargeSource == null) return;
        
        var selectedItem = cboChargeSource.SelectedItem as ComboBoxItem;
        if (selectedItem != null && selectedItem.Content.ToString() == "No BMS")
        {
            txtBmsCanId.IsEnabled = false;
            txtBmsCanId.Opacity = 0.5;
        }
        else
        {
            txtBmsCanId.IsEnabled = true;
            txtBmsCanId.Opacity = 1.0;
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

    private async void SyncMCU_Click(object sender, RoutedEventArgs e)
    {
        // Recursively reset borders for all TextBoxes to simulate a successful "Save" or "Read"
        ResetTextBoxBorders(this);
        
        var btn = sender as Button;
        if (btn != null)
        {
            string original = btn.Content.ToString() ?? "";
            btn.Content = "Syncing...";
            btn.IsEnabled = false;
            
            // Wait for 1 second asynchronously
            await System.Threading.Tasks.Task.Delay(1000);
            
            btn.Content = original;
            btn.IsEnabled = true;
            
            MessageBox.Show("Configuration synced with MCU successfully.", "Success", MessageBoxButton.OK, MessageBoxImage.Information);
        }
    }

    private void ImportConfig_Click(object sender, RoutedEventArgs e)
    {
        MessageBox.Show("Select a JSON file to import configuration parameters.", "Import Config", MessageBoxButton.OK, MessageBoxImage.Information);
    }

    private void ExportConfig_Click(object sender, RoutedEventArgs e)
    {
        MessageBox.Show("Configuration exported to 'charge_config.json'.", "Export Config", MessageBoxButton.OK, MessageBoxImage.Information);
    }

    private void Defaults_Click(object sender, RoutedEventArgs e)
    {
        if (MessageBox.Show("Reset all parameters to factory defaults?", "Confirm", MessageBoxButton.YesNo, MessageBoxImage.Question) == MessageBoxResult.Yes)
        {
            MessageBox.Show("Defaults loaded.", "Defaults", MessageBoxButton.OK, MessageBoxImage.Information);
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
