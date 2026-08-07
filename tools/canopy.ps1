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
# Run it from any directory. Use the path to it:
#
#   .\tools\canopy.ps1        from the repository root
#   .\canopy.ps1              from this folder
#
# PowerShell does not run a script from the current directory without a path, so the
# leading .\ is required. "canopy.ps1" alone fails with CommandNotFoundException.
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

# THE SCRIPT FINDS THE PROJECT, the project does not have to find the script.
#
# Everything below uses a path relative to the repository root: design\canopy, the two
# Python tools, and pio itself, which reads platformio.ini from the current directory. So
# the root has to BE the current directory.
#
# It used to say "run it from the repository root" in a comment and leave it there. That
# put the one directory the script works from one level above the directory the script
# lives in, so opening the folder and running .\canopy.ps1 -- the most natural thing to
# try -- got "no drawings folder at design\canopy" and named a path that is really there.
#
# $PSScriptRoot is this file's folder, so its parent is the root. Push and pop rather than
# Set-Location, and in a finally, so a failed bake does not leave the caller's shell in a
# directory it did not ask for.
Push-Location (Split-Path -Parent $PSScriptRoot)
try {

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
# `return`, not `exit`. exit leaves the caller's shell in the repository root, because it
# skips the finally that pops it. A script that changes directory has to leave by the door.
if ($Bake) { Write-Host "`nbaked. build skipped." -ForegroundColor Green; return }

Write-Host ""
Write-Host "-- building" -ForegroundColor Cyan
& $pio run | Select-String -Pattern "error|Error|RAM:|Flash:|SUCCESS|FAILED"
if ($LASTEXITCODE -ne 0) { throw "build failed" }
if ($NoFlash) { Write-Host "`nbuilt. device untouched." -ForegroundColor Green; return }

# IS THE PORT EVEN THERE? Asked before the flash, because the answer changes the advice
# completely and the flash cannot tell the two cases apart.
#
# A second board arrives on its own port number, and plugging one in while the other is out
# does not reuse the old number -- COM6 became COM7 that way. esptool then prints the port
# it was given and fails, and the message below used to blame another program for holding a
# port that was not there at all. That sent the reader to look for the process holding it.
#
# Naming the ports that ARE present is the whole value here: the next step is almost always
# to pass one of them to -Port.
$present = [System.IO.Ports.SerialPort]::GetPortNames()
if ($present -notcontains $Port) {
    $seen = if ($present) { ($present | Sort-Object) -join ", " } else { "none at all" }
    # The parentheses around the concatenation are load-bearing: -f binds to the string
    # immediately left of it, so without them it formats only "or plug the board in.",
    # which has no placeholders, and {0} and {1} reach the reader verbatim.
    throw (("{0} is not present. Ports found: {1}. Pass one with -Port, " +
            "or plug the board in.") -f $Port, $seen)
}

Write-Host ""
Write-Host "-- flashing $Port  (do not interrupt this)" -ForegroundColor Yellow
& $pio run -t upload --upload-port $Port | Select-Object -Last 3
# ${Port} AND NOT $Port. PowerShell allows ? inside a bare variable name -- that is why $?
# works -- so "$Port?" reads as a variable called Port?, which nothing ever set. The message
# lost the port name silently and came out as "is another program holding" with a trailing
# space, which is the one detail the reader needed.
if ($LASTEXITCODE -ne 0) {
    throw ("flash failed on ${Port}. Another program may be holding it -- the Clawdmeter " +
           "tray daemon is the usual one. Hold BOOT while plugging it in if esptool " +
           "cannot reach the bootloader.")
}

Write-Host ""
Write-Host "-- asking the board what it costs" -ForegroundColor Cyan
Start-Sleep -Seconds 3            # the board reboots after a flash
python tools\canopy_cost.py $Port

Write-Host ""
Write-Host "on the device. Fly a match to see it." -ForegroundColor Green
Write-Host "The replay baseline will now report DIFFERENT on any frame with the" -ForegroundColor DarkGray
Write-Host "canopy in it, which is correct: the picture changed on purpose." -ForegroundColor DarkGray

} finally { Pop-Location }
