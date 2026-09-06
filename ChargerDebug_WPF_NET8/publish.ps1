C:\Users\bongd\dotnet\dotnet.exe publish -c Release -r win-x64 `
  --self-contained true `
  -p:PublishSingleFile=true `
  -p:IncludeNativeLibrariesForSelfExtract=true `
  -p:DebugType=embedded `
  -p:EnableCompressionInSingleFile=true

Write-Host ""
Write-Host "=== Publish Complete ===" -ForegroundColor Green
$exe = "bin\Release\net8.0-windows\win-x64\publish\ChargerDebugApp.exe"
if (Test-Path $exe) {
    $size = [math]::Round((Get-Item $exe).Length / 1MB, 1)
    Write-Host "Output: $exe" -ForegroundColor Cyan
    Write-Host "Size:   $size MB" -ForegroundColor Cyan
} else {
    Write-Host "ERROR: Output file not found!" -ForegroundColor Red
}
