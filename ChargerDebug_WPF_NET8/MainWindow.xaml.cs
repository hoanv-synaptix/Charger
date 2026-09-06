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
        _serialService = new SerialService();
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

    private void SyncMCU_Click(object sender, RoutedEventArgs e)
    {
        // Recursively reset borders for all TextBoxes to simulate a successful "Save" or "Read"
        ResetTextBoxBorders(this);
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