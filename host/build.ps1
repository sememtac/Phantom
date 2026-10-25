# Build the desktop version of the game.
#
#   powershell -ExecutionPolicy Bypass -File host\build.ps1
#   powershell -ExecutionPolicy Bypass -File host\build.ps1 -Run
#
# The result is host\build\phantom.exe. It needs no DLLs and no installed
# libraries: the window is plain Win32 and the picture goes out through GDI.
#
# The game's own sources are compiled unchanged. Only two files are left out --
# the device port and its audio codec -- and one is added in their place.
param(
    [switch]$Run,
    [int]$Scale = 2,
    [switch]$Clean,
    [switch]$Package
)

$ErrorActionPreference = "Stop"
$root  = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$host_ = Join-Path $root "host"
$build = Join-Path $host_ "build"

if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
New-Item -ItemType Directory -Force $build | Out-Null

# --- find the compiler -------------------------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vcvars = $null
if (Test-Path $vswhere) {
    $install = & $vswhere -latest -products * `
                 -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                 -property installationPath
    if ($install) { $vcvars = Join-Path $install "VC\Auxiliary\Build\vcvars64.bat" }
}
if (-not $vcvars -or -not (Test-Path $vcvars)) {
    throw "no MSVC x64 toolchain found. Install the Visual Studio C++ build tools."
}

# --- what to compile ---------------------------------------------------------
#
# EVERY game source except the two that are device-only:
#   vg_port_co5300.cpp  the panel, touch, audio and storage for the real board
#   third_party/es8311  the audio codec driver that port talks to
#
$src = @()
$src += Get-ChildItem (Join-Path $root "src") -Filter *.cpp |
        ForEach-Object { $_.FullName }
$src += Get-ChildItem (Join-Path $root "src\vg") -Filter *.cpp |
        Where-Object { $_.Name -ne "vg_port_co5300.cpp" } |
        ForEach-Object { $_.FullName }
$src += Get-ChildItem (Join-Path $host_ "src") -Filter *.cpp |
        ForEach-Object { $_.FullName }

$inc = @(
    (Join-Path $host_ "compat"),     # Arduino.h and the esp_* headers, first
    (Join-Path $host_ "src"),
    (Join-Path $root  "src"),
    (Join-Path $root  "src\vg")
) | ForEach-Object { "/I`"$_`"" }

# /fp:fast matches the -ffast-math the firmware builds with. It will NOT produce
# the same bits as Xtensa -- see host\README.md -- but it keeps the same latitude
# so the code behaves the way it was written to.
$flags = @(
    "/nologo", "/std:c++17", "/EHsc", "/O2", "/fp:fast", "/GS-", "/MT",
    "/DNDEBUG", "/DVG_STEER_WIDGET=0", "/D_CRT_SECURE_NO_WARNINGS",
    "/DWIN32_LEAN_AND_MEAN",
    "/FI`"$host_\compat\host_prelude.h`"",
    "/wd4244", "/wd4305", "/wd4838", "/wd4996"   # float narrowing, all deliberate
)

$objdir = Join-Path $build "obj"
New-Item -ItemType Directory -Force $objdir | Out-Null

$args = @()
$args += $flags
$args += $inc
# Forward slash and a relative path on purpose: a trailing backslash inside
# quotes escapes the quote, which cl reports as one enormous invalid argument.
$args += '/Fo"obj/"'
$args += ($src | ForEach-Object { "`"$_`"" })
$args += "/link", "user32.lib", "gdi32.lib", "winmm.lib", "dwmapi.lib", "d3d11.lib", "dxgi.lib", "d3dcompiler.lib"
# Load the DLLs above from System32 only. Without this flag, Windows looks for
# winmm.dll, d3d11.dll and d3dcompiler_47.dll in the folder of the exe first.
# Then a DLL with one of those names, put beside a downloaded phantom.exe, runs
# inside the signed program. Windows 10 1607 and later obey the flag. Earlier
# versions ignore it.
$args += "/DEPENDENTLOADFLAG:0x800"
$args += "/OUT:`"$build\phantom.exe`""

$cmdline = "cl " + ($args -join " ")
$bat = Join-Path $build "_build.bat"
"@echo off`r`ncall `"$vcvars`" >nul`r`ncd /d `"$build`"`r`n$cmdline" |
    Out-File -Encoding ascii $bat

Write-Host "compiling $($src.Count) files..." -ForegroundColor Cyan
# Output goes to a log rather than the pipeline. vcvars64.bat writes a harmless
# complaint about vswhere to stderr, and PowerShell turns any stderr from a
# native command into a terminating error -- which would fail the build over a
# message that has nothing to do with it.
$log = Join-Path $build "build.log"
$prev = $ErrorActionPreference
$ErrorActionPreference = "Continue"
cmd /c "`"$bat`" > `"$log`" 2>&1"
$compileFailed = ($LASTEXITCODE -ne 0)
$ErrorActionPreference = $prev

Get-Content $log | Where-Object { $_ -match "error [A-Z]+[0-9]+|fatal error" } |
    Select-Object -First 30 | ForEach-Object { Write-Host $_ -ForegroundColor Red }

# THE EXIT CODE, NOT THE FILE. Testing for phantom.exe reported success off a
# STALE binary from the previous build while the compiler was failing, which is
# the worst possible way for a build to lie: it hands back something that runs.
if ($compileFailed -or -not (Test-Path "$build\phantom.exe")) {
    throw "build failed -- see $log"
}
Write-Host "built $build\phantom.exe" -ForegroundColor Green

if ($Run) { & "$build\phantom.exe" "--scale" $Scale }

# --- something to hand to somebody else --------------------------------------
#
# The exe is the whole game. It links the C runtime statically (/MT) and imports
# only USER32, GDI32, WINMM and KERNEL32, all of which are part of Windows, so
# there is nothing to install and nothing to sit beside it. The zip exists to
# carry the controls along with it, not because the exe needs company.
if ($Package) {
    $stage = Join-Path $build "package"
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force $stage | Out-Null
    Copy-Item "$build\phantom.exe" $stage

    @"
PHANTOM

Run phantom.exe. There is nothing to install.

Windows may warn you that it does not recognise the program. The file is not
signed. Choose "More info" and then "Run anyway".

CONTROLS

  Mouse                 Steer
  Left mouse button     Fire a missile
  Hold right button     Roll. The mouse rolls the ship instead of turning it.
  W and S               Throttle up and down
  Hold C                Centre the steering. Let go to steer again.
  Hold R                Look behind
  Esc                   Menu, and pause during a fight
  Mouse in a menu       Point at an item and click it

The window takes the mouse pointer while you fly. You get it back when you
pause, when you open a menu, and when you move to another window.

XBOX 360 CONTROLLER

  Right stick           Aim. Push up to point the nose up.
  Right trigger         Open the throttle. Let go to keep the setting.
  Left trigger          Close the throttle
  B                     Fire a missile
  Hold LB or RB         Roll left or roll right
  Hold Y                Look behind
  Start                 Menu, and pause during a fight
  In a menu             Right stick moves the pointer. A selects. B goes back.

The controller works together with the mouse and the keyboard. If you hold B
when a fight ends, release it before you press B in the menu. The left stick
does nothing in flight. To make forward on the right stick point the nose down,
as on the stick of an aeroplane, run:

    phantom.exe --pad-invert

The window is 960x960. For a larger window, run it from a command prompt:

    phantom.exe --scale 3

The ship turns too fast or too slow for some mice. To change it:

    phantom.exe --sens 0.05      slower
    phantom.exe --sens 0.20      faster

The game writes phantom_save.bin beside itself, so keep it in a folder you can
write to.
"@ | Out-File -Encoding ascii (Join-Path $stage "READ ME.txt")

    $zip = Join-Path $build "phantom-pc.zip"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    Compress-Archive -Path "$stage\*" -DestinationPath $zip
    Write-Host "packaged $zip" -ForegroundColor Green
}
