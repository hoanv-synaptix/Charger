using System;
using System.Configuration;
using System.Data;
using System.Windows;
using ChargerDebugApp.Services;

namespace ChargerDebugApp;

/// <summary>
/// Interaction logic for App.xaml
/// </summary>
public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        // Test mode: run automated tests and exit
        if (e.Args.Length > 0 && e.Args[0] == "--test")
        {
            try
            {
                ProgramTest.RunTests();
                Shutdown(0);
                return;
            }
            catch (Exception ex)
            {
                Console.ForegroundColor = ConsoleColor.Red;
                Console.WriteLine($"[TEST FAILED] {ex.Message}");
                Console.ResetColor();
                Shutdown(1);
                return;
            }
        }

        base.OnStartup(e);

        // Check for updates in background after MainWindow is shown
        // Fire-and-forget: never blocks UI startup
        MainWindow.Loaded += async (_, _) => await CheckForUpdateAsync();
    }

    private async System.Threading.Tasks.Task CheckForUpdateAsync()
    {
        var info = await UpdateService.CheckForUpdateAsync();
        if (info is null) return;

        // Show dialog on UI thread
        await Dispatcher.InvokeAsync(() =>
        {
            var dialog = new UpdateDialog(info)
            {
                Owner = MainWindow
            };
            dialog.ShowDialog();
        });
    }
}
