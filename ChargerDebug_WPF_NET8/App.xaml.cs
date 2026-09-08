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

        var mainWindow = new MainWindow();
        MainWindow = mainWindow;
        mainWindow.Show();
    }
}
