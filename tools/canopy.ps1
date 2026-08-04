# Draw, build, fly. One command for the canopy loop.
#
#   .\tools\canopy.ps1                 bake design\Test.png, build, flash
#   .\tools\canopy.ps1 -Bake           bake only, no build
#   .\tools\canopy.ps1 -NoFlash        bake and build, do not touch the device
#   .\tools\canopy.ps1 -Png other.png  use another drawing
#   .\tools\canopy.ps1 -Port COM7      a different port
#
# Run it from the repository root.
#
# The drawing is a grey field with the frame on it. Mid grey means leave the pixel
# alone, brighter adds light, darker takes it away. Left-right symmetry is detected,
# not required: a symmetric drawing is stored as a half, and a lopsided one is stored
# whole and costs a little more flash.
#
# WHAT TO WATCH: the baker prints what the drawing costs the frame, from a cost model
# fitted to the device. After flashing, this asks the board itself:
#
#   python tools\canopy_cost.py COM6
#
# Coverage is the only thing that matters to the budget. 95% of the cost is the number
# of pixels covered, so if the game starts missing 60 frames a second, narrow the
# shapes -- levels, gradients and fine detail are all free.

param(
    [string]$Png  = "design\Test.png",
    [string]$Port = "COM6",
    [switch]$Bake,
    [switch]$NoFlash
)

$ErrorActionPreference = "Stop"

# PlatformIO is not on the PATH.
$pio = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe"
if (-not (Test-Path $pio)) { throw "no PlatformIO at $pio" }

# The uploader hangs forever without this. Do not leave it out.
$env:PYTHONIOENCODING = "utf-8"

if (-not (Test-Path $Png)) { throw "no drawing at $Png" }

Write-Host ""
Write-Host "-- baking $Png" -ForegroundColor Cyan
python tools\canopy_bake.py $Png src\vg\vg_canopy_data.h
if ($LASTEXITCODE -ne 0) { throw "bake failed" }
if ($Bake) { Write-Host "`nbaked. build skipped." -ForegroundColor Green; exit 0 }

Write-Host ""
Write-Host "-- building" -ForegroundColor Cyan
& $pio run | Select-String -Pattern "error|Error|RAM:|Flash:|SUCCESS|FAILED"
if ($LASTEXITCODE -ne 0) { throw "build failed" }
if ($NoFlash) { Write-Host "`nbuilt. device untouched." -ForegroundColor Green; exit 0 }

Write-Host ""
Write-Host "-- flashing $Port  (do not interrupt this)" -ForegroundColor Yellow
& $pio run -t upload --upload-port $Port | Select-Object -Last 3
if ($LASTEXITCODE -ne 0) { throw "flash failed -- is another program holding $Port?" }

Write-Host ""
Write-Host "-- asking the board what it costs" -ForegroundColor Cyan
Start-Sleep -Seconds 3            # the board reboots after a flash
python tools\canopy_cost.py $Port

Write-Host ""
Write-Host "on the device. Fly a match to see it." -ForegroundColor Green
Write-Host "The replay baseline will now report DIFFERENT on any frame with the" -ForegroundColor DarkGray
Write-Host "canopy in it, which is correct: the picture changed on purpose." -ForegroundColor DarkGray
