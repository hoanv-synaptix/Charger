using System;
using System.Diagnostics;
using System.IO;
using System.Net.Http;
using System.Net.Http.Json;
using System.Reflection;
using System.Text.Json.Serialization;
using System.Threading;
using System.Threading.Tasks;

namespace ChargerDebugApp.Services;

/// <summary>
/// Info về bản update mới nhất từ GitHub Releases.
/// </summary>
public record UpdateInfo(
    Version LatestVersion,
    string TagName,
    string DownloadUrl,
    string ReleaseNotes
);

/// <summary>
/// Kiểm tra và tải update từ GitHub Releases.
/// Repo: https://github.com/hoanv-synaptix/Charger
/// </summary>
public class UpdateService
{
    private const string GitHubOwner = "hoanv-synaptix";
    private const string GitHubRepo  = "Charger";
    private const string AssetName   = "ChargerDebugApp.exe";

    private static readonly string ApiUrl =
        $"https://api.github.com/repos/{GitHubOwner}/{GitHubRepo}/releases/latest";

    private static readonly HttpClient _http = new()
    {
        DefaultRequestHeaders =
        {
            { "User-Agent", "ChargerDebugApp-Updater" },
            { "Accept", "application/vnd.github+json" }
        },
        Timeout = TimeSpan.FromSeconds(10)
    };

    /// <summary>
    /// Lấy version hiện tại của assembly.
    /// </summary>
    public static Version CurrentVersion =>
        Assembly.GetExecutingAssembly().GetName().Version ?? new Version(1, 0, 0);

    /// <summary>
    /// Kiểm tra xem có bản mới hơn trên GitHub không.
    /// Trả về null nếu không có update hoặc có lỗi mạng.
    /// </summary>
    public static async Task<UpdateInfo?> CheckForUpdateAsync(CancellationToken ct = default)
    {
        try
        {
            var release = await _http.GetFromJsonAsync<GitHubRelease>(ApiUrl, ct);
            if (release is null || release.Prerelease || release.Draft)
                return null;

            // Parse version từ tag (vd: "v1.2.0" → 1.2.0)
            var tagVersion = release.TagName?.TrimStart('v', 'V');
            if (!Version.TryParse(tagVersion, out var latestVersion))
                return null;

            if (latestVersion <= CurrentVersion)
                return null;

            // Tìm asset .exe trong release
            var asset = Array.Find(release.Assets ?? [],
                a => a.Name?.Equals(AssetName, StringComparison.OrdinalIgnoreCase) == true
                  || a.Name?.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) == true);

            if (asset?.BrowserDownloadUrl is null)
                return null;

            return new UpdateInfo(
                LatestVersion: latestVersion,
                TagName:       release.TagName ?? $"v{latestVersion}",
                DownloadUrl:   asset.BrowserDownloadUrl,
                ReleaseNotes:  release.Body ?? string.Empty
            );
        }
        catch (OperationCanceledException)
        {
            return null; // Timeout hoặc cancelled — bỏ qua im lặng
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"[UpdateService] Check failed: {ex.Message}");
            return null; // Không có mạng — bỏ qua im lặng
        }
    }

    /// <summary>
    /// Download file update về thư mục temp.
    /// </summary>
    public static async Task<string> DownloadUpdateAsync(
        string downloadUrl,
        IProgress<double>? progress = null,
        CancellationToken ct = default)
    {
        var tempPath = Path.Combine(Path.GetTempPath(), "ChargerDebugApp_update.exe");

        using var response = await _http.GetAsync(downloadUrl, HttpCompletionOption.ResponseHeadersRead, ct);
        response.EnsureSuccessStatusCode();

        var totalBytes = response.Content.Headers.ContentLength ?? 0L;
        await using var contentStream = await response.Content.ReadAsStreamAsync(ct);
        await using var fileStream = new FileStream(tempPath, FileMode.Create, FileAccess.Write, FileShare.None, 8192, true);

        var buffer = new byte[8192];
        long downloadedBytes = 0;
        int bytesRead;

        while ((bytesRead = await contentStream.ReadAsync(buffer, ct)) > 0)
        {
            await fileStream.WriteAsync(buffer.AsMemory(0, bytesRead), ct);
            downloadedBytes += bytesRead;
            if (totalBytes > 0)
                progress?.Report((double)downloadedBytes / totalBytes);
        }

        return tempPath;
    }

    /// <summary>
    /// Áp dụng update: tạo batch script thay thế file exe rồi restart app.
    /// </summary>
    public static void ApplyUpdate(string newExePath)
    {
        var currentExe = Environment.ProcessPath
                      ?? Process.GetCurrentProcess().MainModule?.FileName
                      ?? throw new InvalidOperationException("Cannot determine current exe path.");

        // Batch script: chờ process cũ thoát → copy file mới → chạy lại
        var batchPath = Path.Combine(Path.GetTempPath(), "charger_update.bat");
        var batchContent = $"""
            @echo off
            echo Applying PKG Battery Charger update...
            :waitloop
            timeout /t 1 /nobreak >nul
            tasklist /FI "PID eq {Environment.ProcessId}" 2>nul | find "{Environment.ProcessId}" >nul
            if not errorlevel 1 goto waitloop
            copy /Y "{newExePath}" "{currentExe}"
            if errorlevel 1 (
                echo Update failed! Could not replace file.
                pause
                exit /b 1
            )
            del "{newExePath}"
            start "" "{currentExe}"
            del "%~f0"
            """;

        File.WriteAllText(batchPath, batchContent);

        Process.Start(new ProcessStartInfo
        {
            FileName        = batchPath,
            UseShellExecute = true,
            WindowStyle     = ProcessWindowStyle.Hidden,
            CreateNoWindow  = true
        });

        // Thoát app hiện tại để batch script có thể thay thế file
        System.Windows.Application.Current.Shutdown();
    }

    // ── GitHub API DTOs ──────────────────────────────────────────────────────

    private sealed class GitHubRelease
    {
        [JsonPropertyName("tag_name")]   public string?        TagName    { get; init; }
        [JsonPropertyName("body")]       public string?        Body       { get; init; }
        [JsonPropertyName("prerelease")] public bool           Prerelease { get; init; }
        [JsonPropertyName("draft")]      public bool           Draft      { get; init; }
        [JsonPropertyName("assets")]     public GitHubAsset[]? Assets     { get; init; }
    }

    private sealed class GitHubAsset
    {
        [JsonPropertyName("name")]                  public string? Name               { get; init; }
        [JsonPropertyName("browser_download_url")] public string? BrowserDownloadUrl { get; init; }
    }
}
