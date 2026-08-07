# Draw, build, fly. One command for the canopy loop.
#
#   .\tools\canopy.ps1                 bake what changed, build, flash
#   .\tools\canopy.ps1 -Bake           bake only, no build
#   .\tools\canopy.ps1 -NoFlash        bake and build, do not touch the device
#   .\tools\canopy.ps1 -Port COM7      a different port
#
# TO ADD A COCKPIT FOR A SHIP, put the drawing in design\canopy\ and name it after
# the ship:
#
#   design\canopy\chariot.png
#
# Then run this with no arguments. The file name is the wiring: there is nothing to
# edit, no name to choose and no header path to keep in step. A ship with no drawing
# flies with no cockpit frame, which the game supports -- there is no default texture.
#
# The ship names are aegis, lance, chariot and ballista.
#
# Run it from the repository root.
#
# The drawing is SWIZZLED: red carries the frame and green carries the activation
# regions. In red, mid grey means leave the pixel alone, brighter adds light and darker
# takes it away. Left-right symmetry is detected, not required. Do not send a grey
# image -- that puts the same values in both channels, so the frame becomes its own
# mask. Full rules in tools\README.md.
#
# WHAT TO WATCH: the baker prints what the drawing costs the frame, from a cost model
# fitted to the device. After flashing, this asks the board itself:
#
#   python tools\canopy_cost.py COM6
#
# Coverage is the only thing that matters to the budget. 95% of the cost is the number
# of pixels the frame covers. If the game starts to miss 60 frames a second, make the
# shapes narrower. Levels, gradients and fine detail are all free.
#
# The full authoring rules, including the green channel, are in tools/README.md.

param(
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

if (-not (Test-Path "design\canopy")) { throw "no drawings folder at design\canopy" }

Write-Host ""
Write-Host "-- baking design\canopy" -ForegroundColor Cyan
python tools\canopy_set.py
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
