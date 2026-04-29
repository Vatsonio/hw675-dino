# build.ps1 — compile + auto-detect COM + upload HW-675 Dino sketch
#
# Usage:
#   .\build.ps1                    # compile + auto-detect port + upload
#   .\build.ps1 -Port COM5         # force a specific port
#   .\build.ps1 -NoUpload          # compile only
#   .\build.ps1 -Monitor           # open serial monitor after upload
#   .\build.ps1 -Setup             # bootstrap arduino-cli + ESP32 core + U8g2 (one-time)

[CmdletBinding()]
param(
    [string]$Port,
    [switch]$Monitor,
    [switch]$NoUpload,
    [switch]$Setup
)

$ErrorActionPreference = 'Stop'
$root   = $PSScriptRoot
$cli    = Join-Path $root 'tools\arduino-cli.exe'
$sketch = Join-Path $root 'dino'
$fqbn   = 'esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashMode=qio,FlashSize=4M,UploadSpeed=921600'

function Run-Setup {
    Write-Host "=== Setup ===" -ForegroundColor Cyan
    if (-not (Test-Path $cli)) {
        Write-Host "Downloading arduino-cli..." -ForegroundColor Yellow
        $zip = Join-Path $env:TEMP 'arduino-cli.zip'
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri 'https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Windows_64bit.zip' -OutFile $zip
        $tools = Join-Path $root 'tools'
        New-Item -ItemType Directory -Force -Path $tools | Out-Null
        Expand-Archive -Path $zip -DestinationPath $tools -Force
        Remove-Item $zip
    }
    Write-Host "arduino-cli: $(& $cli version)" -ForegroundColor Green
    & $cli config init --overwrite | Out-Null
    & $cli config add board_manager.additional_urls "https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json" | Out-Null
    & $cli core update-index
    & $cli core install esp32:esp32
    & $cli lib install "U8g2"
    Write-Host "Setup complete." -ForegroundColor Green
}

if ($Setup) { Run-Setup; return }

if (-not (Test-Path $cli))                       { throw "arduino-cli not found. Run: .\build.ps1 -Setup" }
if (-not (Test-Path (Join-Path $sketch 'dino.ino'))) { throw "Sketch dino\dino.ino missing." }

Write-Host "[1/3] Compiling..." -ForegroundColor Cyan
& $cli compile --fqbn $fqbn $sketch
if ($LASTEXITCODE -ne 0) { throw "Compile failed." }
Write-Host "  Compile OK" -ForegroundColor Green

if ($NoUpload) { Write-Host "Compile-only run complete." -ForegroundColor Green; return }

if (-not $Port) {
    Write-Host "[2/3] Detecting board..." -ForegroundColor Cyan
    $jsonRaw = & $cli board list --format json
    $json = $jsonRaw | ConvertFrom-Json
    # arduino-cli >=0.31 wraps results under .detected_ports
    $entries = if ($json.detected_ports) { $json.detected_ports } else { $json }
    $candidates = @()
    foreach ($e in $entries) {
        $addr = $null
        if ($e.port -and $e.port.address) { $addr = $e.port.address }
        elseif ($e.address)                { $addr = $e.address }
        if ($addr -and $addr -ne 'COM1') {
            $proto = if ($e.port -and $e.port.protocol) { $e.port.protocol } else { $e.protocol }
            if (-not $proto -or $proto -eq 'serial') { $candidates += $addr }
        }
    }
    $candidates = @($candidates | Sort-Object -Unique)
    if ($candidates.Count -eq 0) {
        throw "No serial port found (COM1 excluded). Cable data-capable? Try: hold BOOT (GPIO9), tap RESET, release, retry."
    }
    if ($candidates.Count -gt 1) {
        throw "Multiple ports: $($candidates -join ', '). Pass -Port COMx."
    }
    $Port = $candidates[0]
    Write-Host "  Detected port: $Port" -ForegroundColor Green
}

Write-Host "[3/3] Uploading to $Port..." -ForegroundColor Cyan
& $cli upload -p $Port --fqbn $fqbn $sketch
if ($LASTEXITCODE -ne 0) {
    throw "Upload failed. Hold BOOT (GPIO9), tap RESET, release BOOT, then rerun."
}

Write-Host "Done. OLED should display the READY screen with a dino." -ForegroundColor Green
if ($Monitor) {
    Write-Host "Opening serial monitor (Ctrl-C to exit)..." -ForegroundColor Cyan
    & $cli monitor -p $Port -c baudrate=115200
}
