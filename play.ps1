# Build and play the latest, on the PC or on the board.
#
#   .\play.ps1                 build the PC version and run it
#   .\play.ps1 device          build the firmware and flash the board
#   .\play.ps1 both            build both, flash the board, then run the PC version
#   .\play.ps1 check           build both and stop. Nothing runs and nothing flashes.
#
# Options:
#   -Scale N     PC window size, N times the 480x480 panel (default 3)
#   -Port COMn   which board to flash. Found automatically if only one is attached.
#
# WHY THIS EXISTS RATHER THAN TWO COMMANDS.
#
# The two builds share every line of src/ and differ only in one port file, so a
# change can compile for one and fail for the other. That has happened: MSVC
# rejects things the Xtensa compiler accepts, and the failures are not obvious
# from the source. `check` builds both, which is the cheapest way to know the
# tree is still whole.
#
# It also carries the three things that have each cost an afternoon on this
# project: the upload needs PYTHONIOENCODING or it hangs for ever, the COM port
# moves between sessions and must never be assumed, and a tray daemon on this
# machine resets the board mid-session in a way that looks exactly like the game
# crashing.
param(
    [Parameter(Position = 0)]
    [ValidateSet('pc', 'device', 'both', 'check')]
    [string]$Target = 'pc',

    [int]$Scale = 3,
    [string]$Port = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

function Build-Pc {
    Write-Host "building the PC version..." -ForegroundColor Cyan
    & powershell -ExecutionPolicy Bypass -File (Join-Path $root 'host\build.ps1')
    if ($LASTEXITCODE -ne 0) { throw "the PC build failed" }
}

function Build-Firmware {
    Write-Host "building the firmware..." -ForegroundColor Cyan
    $pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
    if (-not (Test-Path $pio)) { throw "PlatformIO is not installed at $pio" }
    # UTF-8 OR IT HANGS. PlatformIO writes a character the default Windows code
    # page cannot encode, and the upload stops for ever rather than reporting it.
    $env:PYTHONIOENCODING = 'utf-8'
    & $pio run
    if ($LASTEXITCODE -ne 0) { throw "the firmware build failed" }
}

# Where the chosen board is remembered. Two identical boards live on this desk
# and the port they land on moves between sessions, so the choice has to be made
# once by a human and then kept. Named with a leading dot and ignored by git: it
# is a fact about this desk, not about the project.
$portFile = Join-Path $root '.phantom-port'

function Find-Port {
    if ($Port) {
        Set-Content -Path $portFile -Value $Port -Encoding ascii
        Write-Host "remembering $Port for next time" -ForegroundColor DarkGray
        return $Port
    }

    # Espressif's USB vendor ID. Both boards use it, so this narrows the list
    # and does not identify a board.
    $ports = Get-CimInstance Win32_PnPEntity |
             Where-Object { $_.DeviceID -match 'VID_303A' -and $_.Name -match '\(COM\d+\)' } |
             ForEach-Object { if ($_.Name -match '\((COM\d+)\)') { $Matches[1] } }

    if (-not $ports) { throw "no Espressif board found. Attach one, or pass -Port COMn." }
    if (@($ports).Count -eq 1) { return @($ports)[0] }

    # More than one. Use the remembered choice if it is still attached.
    if (Test-Path $portFile) {
        $saved = (Get-Content $portFile -Raw).Trim()
        if ($ports -contains $saved) {
            Write-Host "using the remembered board on $saved" -ForegroundColor DarkGray
            return $saved
        }
    }

    throw ("more than one board is attached: " + ($ports -join ', ') + ".`n" +
           "  They are the same hardware, so the port cannot say which is which.`n" +
           "  Choose once with -Port COMn and it will be remembered.`n" +
           "  To find out which is which, read the MAC without writing anything:`n" +
           "    esptool --port COMn --no-stub read-mac")
}

function Warn-Daemon {
    # It opens every Espressif port looking for its own device, and opening a
    # port restarts an ESP32-S3. The board then reboots to the title screen
    # mid-flight, which reads as the game crashing.
    $d = Get-CimInstance Win32_Process |
         Where-Object { $_.Name -eq 'pythonw.exe' -and $_.CommandLine -match 'tray_windows' }
    if ($d) {
        Write-Host ""
        Write-Host "WARNING: the Clawdmeter tray daemon is running." -ForegroundColor Yellow
        Write-Host "  It opens serial ports to look for its device, and opening a port" -ForegroundColor Yellow
        Write-Host "  restarts the board. The game will reboot to the title screen about" -ForegroundColor Yellow
        Write-Host "  every 45 seconds while you play." -ForegroundColor Yellow
        Write-Host "  Stop it with:  Stop-Process -Id $($d.ProcessId)" -ForegroundColor Yellow
        Write-Host ""
    }
}

function Flash-Board {
    $p = Find-Port
    Warn-Daemon
    Write-Host "flashing $p..." -ForegroundColor Cyan
    $pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
    $env:PYTHONIOENCODING = 'utf-8'
    & $pio run -t upload --upload-port $p
    if ($LASTEXITCODE -ne 0) { throw "the upload failed" }
    Write-Host "flashed. The board is running the latest." -ForegroundColor Green
}

switch ($Target) {
    'pc' {
        Build-Pc
        & (Join-Path $root 'host\build\phantom.exe') --scale $Scale
    }
    'device' {
        Build-Firmware
        Flash-Board
    }
    'both' {
        Build-Firmware
        Build-Pc
        Flash-Board
        & (Join-Path $root 'host\build\phantom.exe') --scale $Scale
    }
    'check' {
        Build-Firmware
        Build-Pc
        Write-Host "both targets build from this tree." -ForegroundColor Green
    }
}
