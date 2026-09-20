param(
    [string]$Port = "COM26",
    [int]$TimeoutSeconds = 15
)

$ErrorActionPreference = "Stop"
$CommandTestSd = [byte]0x20
$ResponseSdTest = [byte]0x88

function Get-Crc8([byte[]]$Data) {
    [byte]$crc = 0
    foreach ($value in $Data) {
        $crc = [byte]($crc -bxor $value)
        for ($bit = 0; $bit -lt 8; $bit++) {
            if (($crc -band 0x80) -ne 0) {
                $crc = [byte]((($crc -shl 1) -bxor 0x07) -band 0xFF)
            } else {
                $crc = [byte](($crc -shl 1) -band 0xFF)
            }
        }
    }
    return $crc
}

function Read-SdResponse($SerialPort, [int]$TimeoutSeconds) {
    $buffer = New-Object System.Collections.Generic.List[byte]
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)

    while ([DateTime]::UtcNow -lt $deadline) {
        if ($SerialPort.BytesToRead -gt 0) {
            $chunk = New-Object byte[] $SerialPort.BytesToRead
            [void]$SerialPort.Read($chunk, 0, $chunk.Length)
            foreach ($value in $chunk) { [void]$buffer.Add($value) }
        }

        while ($buffer.Count -ge 5) {
            $start = -1
            for ($i = 0; $i -lt ($buffer.Count - 1); $i++) {
                if ($buffer[$i] -eq 0xAA -and $buffer[$i + 1] -eq 0x55) {
                    $start = $i
                    break
                }
            }
            if ($start -lt 0) {
                $buffer.Clear()
                break
            }
            if ($start -gt 0) { $buffer.RemoveRange(0, $start) }
            if ($buffer.Count -lt 5) { break }

            $length = [int]$buffer[3]
            $total = 5 + $length
            if ($buffer.Count -lt $total) { break }

            $frame = [byte[]]$buffer.GetRange(0, $total).ToArray()
            $buffer.RemoveRange(0, $total)
            $crcData = [byte[]]$frame[2..(3 + $length)]
            if ((Get-Crc8 $crcData) -ne $frame[$total - 1]) { continue }
            if ($frame[2] -ne $ResponseSdTest) { continue }
            return [byte[]]$frame[4..(3 + $length)]
        }
        Start-Sleep -Milliseconds 20
    }
    return $null
}

$serial = New-Object System.IO.Ports.SerialPort $Port, 115200, None, 8, One
$serial.ReadTimeout = 100
$serial.DtrEnable = $true
$serial.RtsEnable = $true

try {
    $serial.Open()
    Start-Sleep -Milliseconds 1200
    $serial.DiscardInBuffer()

    [byte[]]$commandData = @($CommandTestSd, 0)
    [byte]$crc = Get-Crc8 $commandData
    [byte[]]$frame = @(0xAA, 0x55, $CommandTestSd, 0, $crc)
    $serial.Write($frame, 0, $frame.Length)

    [byte[]]$payload = Read-SdResponse $serial $TimeoutSeconds
    if ($null -eq $payload -or $payload.Length -ne 16) {
        Write-Error "No valid SD test response received from $Port."
        exit 2
    }

    $blockCount = [BitConverter]::ToUInt32($payload, 10)
    $summary = ("card_present={0} card_ready={1} mounted={2} sector0_read={3} " +
        "mbr_signature={4} file_write={5} file_read={6} file_match={7} " +
        "file_removed={8} fatfs_result={9} block_count={10} read_stage={11} read_response=0x{12:X2}")
    Write-Output ($summary -f
        $payload[0], $payload[1], $payload[2], $payload[3], $payload[4],
        $payload[5], $payload[6], $payload[7], $payload[8], $payload[9],
        $blockCount, $payload[14], $payload[15])

    $pass = $payload[0] -and $payload[1] -and $payload[2] -and
        $payload[3] -and $payload[4] -and $payload[5] -and $payload[6] -and
        $payload[7] -and $payload[8]
    if (-not $pass) { exit 1 }
    exit 0
} finally {
    if ($serial.IsOpen) { $serial.Close() }
}
