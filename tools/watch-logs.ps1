#requires -Version 5.1
<#
  re2r_mp live log watcher.

  Tails the REFramework log in a terminal and highlights everything relevant to
  our mod (plugin load / loader errors / the re2r_mp bridge). Also prints the
  contents of the RAM-bridge file the net plugin publishes, so you can see the
  address + field offsets go live.

  Usage:
    powershell -ExecutionPolicy Bypass -File tools\watch-logs.ps1
    powershell -ExecutionPolicy Bypass -File tools\watch-logs.ps1 -All
    powershell -ExecutionPolicy Bypass -File tools\watch-logs.ps1 -Seconds 60

  -All     : show every log line (default: only plugin/loader/error lines).
  -Seconds : auto-exit after N seconds (0 = run until Ctrl+C).
#>
param(
    [string]$Game = 'D:\SteamLibrary\steamapps\common\RESIDENT EVIL 2  BIOHAZARD RE2',
    [switch]$All,
    [int]$Seconds = 0
)

$ErrorActionPreference = 'SilentlyContinue'

$log = Join-Path $Game 're2_framework_log.txt'
$net = Join-Path $Game 'reframework\data\re2r_mp_net.txt'
$pads = Join-Path $Game 'reframework\data\reunited_pads.txt'
$sfx = Join-Path $Game 'reframework\data\reunited_sfx.txt'

function Line($text, $color) { Write-Host $text -ForegroundColor $color }

function Show-Line([string]$line) {
    if ([string]::IsNullOrEmpty($line)) { return }

    $interesting = $script:All -or
        ($line -match 're2r_mp|PluginLoader|\[Plugin\]|\[error\]|\[warn\]|REFramework entry|Tag:|Build date|Renderer|initialized|initializ|requires version|D3D1[12]|hook')

    if (-not $interesting) { return }

    $color = 'Gray'
    if ($line -match 're2r_mp') { $color = 'Green' }
    if ($line -match '\[error\]|Requires a|Failed to (load|initialize)|Exception|Crash|different game') { $color = 'Red' }
    elseif ($line -match '\[warn\]|Desires a newer patch') { $color = 'Yellow' }
    elseif ($line -match 'PluginLoader|Renderer|D3D1[12]') { $color = 'Cyan' }

    Line $line $color
}

Line '================================================================' 'Cyan'
Line ' re2r_mp - live log watch' 'Cyan'
Line (" game  : {0}" -f $Game) 'DarkGray'
Line (" log   : {0}" -f $log) 'DarkGray'
Line (" filter: {0}" -f $(if ($All) { 'ALL lines' } else { 'plugin / loader / error lines only' })) 'DarkGray'
Line (" exit  : {0}" -f $(if ($Seconds -gt 0) { "after $Seconds s" } else { 'Ctrl+C' })) 'DarkGray'
Line '================================================================' 'Cyan'
Write-Host ''

$started = Get-Date
$pos = 0L
$lastBridge = ''

while ($true) {
    if ($Seconds -gt 0 -and ((Get-Date) - $started).TotalSeconds -ge $Seconds) { break }

    # ---- framework log (handles the file being recreated on each launch) ----
    if (Test-Path -LiteralPath $log) {
        $len = (Get-Item -LiteralPath $log).Length
        if ($len -lt $pos) {
            $pos = 0L
            Line '' 'White'
            Line '---- log recreated (new launch) ----' 'Yellow'
        }
        if ($len -gt $pos) {
            $fs = [System.IO.File]::Open($log, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
            try {
                [void]$fs.Seek($pos, [System.IO.SeekOrigin]::Begin)
                $sr = New-Object System.IO.StreamReader($fs)
                while (-not $sr.EndOfStream) {
                    Show-Line $sr.ReadLine()
                }
                $pos = $fs.Position
            } finally {
                if ($sr) { $sr.Dispose() }
                $fs.Close()
            }
        }
    }

    # ---- RAM bridge file published by re2r_mp.dll ----
    if (Test-Path -LiteralPath $net) {
        $txt = (Get-Content -LiteralPath $net -Raw)
        if ($txt -ne $lastBridge) {
            $lastBridge = $txt
            $addr = if ($txt -match 'addr\s+(\d+)') { $matches[1] } else { '?' }
            $ver = if ($txt -match 'version\s+(\d+)') { $matches[1] } else { '?' }
            Line ("[bridge] re2r_mp_net.txt  addr={0}  version={1}" -f $addr, $ver) 'Magenta'
        }
    }

    Start-Sleep -Milliseconds 250
}

Write-Host ''
Line '---- watcher stopped ----' 'Yellow'
