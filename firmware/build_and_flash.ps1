# ============================================================
# Claude Status Bar - one-click build & flash
# for LilyGo T-Display S3 Long (ESP32-S3)
#
# Usage:  powershell -ExecutionPolicy Bypass -File build_and_flash.ps1 [-Port COM5] [-CompileOnly]
#
# What it does (all self-contained in .\build, nothing installed system-wide):
#   1. Downloads arduino-cli
#   2. Installs the ESP32 board package (pinned 2.0.17 - big download, first run only)
#   3. Downloads LilyGo's official T-Display-S3-Long repo and copies the
#      AXS15231B display driver + pin config into the sketch
#   4. Installs Adafruit GFX + ArduinoJson libraries
#   5. Compiles the firmware
#   6. Flashes it to the device over USB
# ============================================================
param(
    [string]$Port = "",
    [switch]$CompileOnly
)
$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Root   = Split-Path -Parent $MyInvocation.MyCommand.Path
# Build tools live in a SHORT path: deep paths overflow Windows' command-line
# length limit when compiling ESP32 projects.
$Build  = "C:\ClaudeBarBuild"
# migrate an older build folder from inside the project, if present
$OldBuild = Join-Path $Root "build"
if ((Test-Path (Join-Path $OldBuild "data")) -and -not (Test-Path $Build)) {
    Write-Host "Moving existing build tools to $Build (no re-download needed)..."
    Move-Item $OldBuild $Build
}
$Sketch = Join-Path $Root "claude_statusbar"
$Cli    = Join-Path $Build "arduino-cli.exe"
$Cfg    = Join-Path $Build "arduino-cli.yaml"
$Out    = Join-Path $Build "out"
New-Item -ItemType Directory -Force -Path $Build | Out-Null

function Step($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }

# ---------- 1. arduino-cli ----------
if (-not (Test-Path $Cli)) {
    Step "Downloading arduino-cli..."
    $zip = Join-Path $Build "arduino-cli.zip"
    Invoke-WebRequest -Uri "https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Windows_64bit.zip" -OutFile $zip
    Expand-Archive -Path $zip -DestinationPath $Build -Force
    Remove-Item $zip
}

# isolated config so we don't touch any existing Arduino setup
@"
board_manager:
  additional_urls:
    - https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
directories:
  data: $Build\data
  downloads: $Build\downloads
  user: $Build\user
"@ | Set-Content -Path $Cfg -Encoding UTF8

# ---------- 2. ESP32 core ----------
Step "Updating board index..."
& $Cli --config-file $Cfg core update-index
$installed = & $Cli --config-file $Cfg core list 2>$null | Select-String "esp32:esp32"
if (-not $installed) {
    Step "Installing ESP32 board package 2.0.17 (about 1.5 GB, first run only - grab a coffee)..."
    & $Cli --config-file $Cfg core install esp32:esp32@2.0.17
    if ($LASTEXITCODE -ne 0) { throw "ESP32 core install failed" }
}

# ---------- 3. LilyGo display driver ----------
$needDriver = -not (Test-Path (Join-Path $Sketch "AXS15231B.h"))
if ($needDriver) {
    Step "Downloading LilyGo T-Display-S3-Long repo (official display driver)..."
    $repoZip = Join-Path $Build "lilygo.zip"
    $ok = $false
    foreach ($branch in @("master", "main")) {
        try {
            Invoke-WebRequest -Uri "https://github.com/Xinyuan-LilyGO/T-Display-S3-Long/archive/refs/heads/$branch.zip" -OutFile $repoZip
            $ok = $true; break
        } catch { Write-Host "  branch '$branch' not found, trying next..." }
    }
    if (-not $ok) { throw "Could not download the LilyGo repo from GitHub. Check your internet connection, or tell Claude." }
    $repoDir = Join-Path $Build "lilygo"
    if (Test-Path $repoDir) { Remove-Item $repoDir -Recurse -Force }
    Expand-Archive -Path $repoZip -DestinationPath $repoDir -Force
    Remove-Item $repoZip

    $drvCpp = Get-ChildItem -Path $repoDir -Recurse -Filter "AXS15231B.cpp" | Select-Object -First 1
    if (-not $drvCpp) { throw "Could not find AXS15231B.cpp in the LilyGo repo - repo layout may have changed. Tell Claude!" }
    $drvDir = $drvCpp.DirectoryName
    Step "Copying driver files from $drvDir"
    Copy-Item (Join-Path $drvDir "AXS15231B.cpp") $Sketch -Force
    Copy-Item (Join-Path $drvDir "AXS15231B.h")   $Sketch -Force
    # pins_config.h may live next to the driver or one level up
    $pins = Get-ChildItem -Path $repoDir -Recurse -Filter "pins_config.h" | Select-Object -First 1
    if ($pins) { Copy-Item $pins.FullName $Sketch -Force }
    # copy any other headers the driver depends on from the same folder
    Get-ChildItem -Path $drvDir -Filter "*.h" | Where-Object { $_.Name -ne "AXS15231B.h" } |
        ForEach-Object { Copy-Item $_.FullName $Sketch -Force }
}

# ---------- 3b. LVGL (the LilyGo driver #includes it) ----------
$lvglDir = Join-Path $Build "user\libraries\lvgl"
if (-not (Test-Path $lvglDir)) {
    Step "Installing LVGL 8.3.11 (required by LilyGo's display driver)..."
    & $Cli --config-file $Cfg lib install "lvgl@8.3.11"
}
$lvConf = Join-Path $Build "user\libraries\lv_conf.h"
if (-not (Test-Path $lvConf)) {
    Step "Setting up lv_conf.h..."
    $repoDir = Join-Path $Build "lilygo"
    $src = $null
    if (Test-Path $repoDir) {
        $src = Get-ChildItem -Path $repoDir -Recurse -Filter "lv_conf.h" | Select-Object -First 1
    }
    if ($src) {
        Copy-Item $src.FullName $lvConf -Force
    } else {
        # enable the template that ships with lvgl
        $tpl = Join-Path $lvglDir "lv_conf_template.h"
        $c = Get-Content -Raw $tpl
        $c = $c -replace '#if 0', '#if 1'
        Set-Content -Path $lvConf -Value $c -Encoding UTF8
    }
}

# ---------- 4. libraries ----------
Step "Installing libraries (Adafruit GFX, ArduinoJson)..."
& $Cli --config-file $Cfg lib install "Adafruit GFX Library" "Adafruit BusIO" "ArduinoJson"

# ---------- 5. compile ----------
$FQBN = "esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,UploadSpeed=115200"
Step "Compiling ($FQBN)..."
& $Cli --config-file $Cfg compile --fqbn $FQBN --output-dir $Out $Sketch
if ($LASTEXITCODE -ne 0) {
    Write-Host "`nCOMPILE FAILED. Copy the error above and paste it to Claude to fix." -ForegroundColor Red
    Read-Host "Press Enter to exit"
    exit 1
}
Write-Host "Compiled OK. Binaries in $Out" -ForegroundColor Green
if ($CompileOnly) { exit 0 }

# ---------- 6. flash ----------
if (-not $Port) {
    Step "Looking for the device..."
    $list = & $Cli --config-file $Cfg board list
    Write-Host ($list | Out-String)
    # prefer ports flagged as USB (the display's native USB), not motherboard serial ports
    $match = $list | Select-String "COM\d+.*USB" | Select-Object -First 1
    if (-not $match) { $match = $list | Select-String "COM\d+" | Select-Object -First 1 }
    if ($match) { $Port = ($match -replace '.*?(COM\d+).*', '$1') }
    if (-not $Port) {
        Write-Host "No serial port found. Plug in the display via USB-C." -ForegroundColor Yellow
        Write-Host "If it still doesn't show up: hold the BOOT button, plug in USB, release BOOT." -ForegroundColor Yellow
        $Port = Read-Host "Enter port manually (e.g. COM5)"
    }
}
Step "Flashing to $Port ..."
& $Cli --config-file $Cfg upload -p $Port --fqbn $FQBN $Sketch
if ($LASTEXITCODE -ne 0) {
    Write-Host "`nUPLOAD FAILED. Try: hold BOOT, tap RESET (or replug USB while holding BOOT), release BOOT, run again." -ForegroundColor Red
    Read-Host "Press Enter to exit"
    exit 1
}
Write-Host "`nDONE! Unplug and replug the USB cable. The display should show 'Claude Status Bar'." -ForegroundColor Green
Write-Host "Next: run the bridge  ->  bridge\run_bridge.bat" -ForegroundColor Green
Read-Host "Press Enter to exit"
