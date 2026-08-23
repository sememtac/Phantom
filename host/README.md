# Phantom on the desktop

This directory holds the desktop build of the game. Use it to play the game on a
PC when the board is not convenient.

This is not an emulator. The build compiles the same game source that the board
runs. Only the hardware layer changes. The files in `src/` are the same files in
both builds.

## What you need

- Windows.
- Visual Studio 2022 with the C++ build tools for x64.

You do not need any other library. The window uses Win32 and GDI.

## How to build

1. Open a PowerShell prompt at the top of the repository.
2. Run this command:

       powershell -ExecutionPolicy Bypass -File host\build.ps1

The build writes `host\build\phantom.exe`.

In a bash shell, use forward slashes. Bash removes the backslash, and
PowerShell then looks for a file called `hostbuild.ps1`:

    powershell -ExecutionPolicy Bypass -File host/build.ps1

Add `-Run` to start the game after the build. Add `-Clean` to remove the build
output first.

If the build fails, read `host\build\build.log`. It holds the full compiler
output.

## How to run

    host\build\phantom.exe
    host\build\phantom.exe --scale 3

| Option | What it does |
|---|---|
| `--scale N` | Sets the window size to N times the 480x480 panel. N is 1 to 4. The default is 2. |
| `--frames N` | Runs N frames and then stops. Use this to test the build. |
| `--sens F` | Sets the mouse scale. The default is 0.10. A lower number is slower. |
| `--help` | Shows the options. |

The game keeps its progress in `phantom_save.bin`. The game writes the file
beside the executable.

You can resize the window. The window keeps the shape of the panel while you
drag it, so the picture cannot stretch. If the window is not the shape of the
panel, for example when you make it full screen, the game draws in the middle
and fills the rest with black.

## Controls

The mouse is a finger on the glass. The game reads it through the same steering
code that the board uses.

| Control | Key |
|---|---|
| Steer | Move the mouse |
| Centre the stick | Hold `C` or the right mouse button |
| Throttle up | `W` or the up arrow |
| Throttle down | `S` or the down arrow |
| Fire a missile | The left mouse button, or space |
| Roll | Hold `Shift`, then steer left or right |
| Look behind | Hold `R` |
| Menu key | `Esc` or `Enter` |
| Select a menu item | Move the mouse, then click |

The window holds the mouse pointer only while you fly the ship. It lets the
pointer go on every screen, in a cutscene, when you pause the game, and when you
move to another window. You always get the pointer back when the ship stops
flying.

Hold `Shift` to roll. The mouse then rolls the ship instead of turning it, which
is what the roll button does on the board. Pitch still works while you roll.

In a menu the window does not hold the pointer. Move the mouse to the item, then
click it.

A click is a tap. The game sees a finger while you hold the button, and it sees
the finger lift when you let go. The game reads the position where the button
went down. If you move the mouse more than 16 pixels before you let go, the game
reads a drag and not a tap.

### Mouse scale

The mouse scale is the number of game pixels for each count that the mouse
reports. The game needs 115 pixels for a full turn. At the default of 0.10, a
full turn needs about 1150 counts. That is about 3 cm of hand movement on a
1000 DPI mouse.

Mouse hardware differs by a factor of ten, so change the number to suit your
mouse:

    hostuild\phantom.exe --sens 0.05     slower
    hostuild\phantom.exe --sens 0.20     faster

The game prints the scale and the count for a full turn when it starts.

Windows pointer speed does not change the scale. The game reads the mouse
device, not the pointer, so pointer acceleration cannot affect the ship.

The mouse does not centre itself. The board centres the stick when the finger
lifts, but a mouse stays where you put it. Hold `C` to lift the finger. The
stick centres, and the game starts a new stick position when you release `C`.

## What this build is not for

The picture and the timing are close to the board, but they are not the same.
Do not use this build for these three tasks.

- **Do not measure speed.** A PC draws a frame about 85 times faster than the
  board. The frame budget, the band window and the wire floor have no meaning
  here.
- **Do not compare pixels.** The x86 and Xtensa compilers round floating point
  differently, so some pixels differ. The two-core split is also off, and that
  split moves some pixels by one. Use `tools/phantom_regress.py` on the board.
- **Do not record or replay a session.** The link is not connected. The game
  reads no commands from it, so a capture cannot start.

Use this build to judge how the game feels and how the ships balance. Use the
board for everything else.

## What is in this directory

| Path | What it holds |
|---|---|
| `build.ps1` | The build script. |
| `compat/` | Headers that stand in for the Arduino and ESP-IDF headers. |
| `compat/host_prelude.h` | Definitions that every file needs. The build forces this header into each file. |
| `src/host_main.cpp` | The program entry point. It calls `setup` once and `loop` in a cycle. |
| `src/host_window.cpp` | The window, the picture and the raw input. |
| `src/vg_port_win32.cpp` | The hardware layer. It answers the same calls as `src/vg/vg_port_co5300.cpp`. |
| `build/` | The build output. Git ignores this directory. |

The build leaves out two game files, because both talk to the board:
`src/vg/vg_port_co5300.cpp` and `src/third_party/es8311.c`. It also leaves out
the two `.S` files, because they hold Xtensa assembly code. The build sets
`CANOPY_PIE=0`, so the canopy uses the C code that those files make faster. The
self test in the game proves that the two paths give the same pixels.

## How to share the game

Run this command:

    powershell -ExecutionPolicy Bypass -File hostuild.ps1 -Package

It writes `hostuild\phantom-pc.zip`. The zip holds the executable and a text
file with the controls. Send the zip. The other person unpacks it and runs
`phantom.exe`.

You can also send `phantom.exe` on its own. The executable is the whole game:

- It links the C runtime into itself, so nobody needs a Visual C++ package.
- It uses only Win32 and GDI, which are part of Windows.
- It holds the art and the models. It makes the sky when it starts.

Three things to tell the person you send it to:

- Windows shows a warning for a program it does not recognise, because the file
  is not signed. They must choose **More info** and then **Run anyway**.
- The game needs 64-bit Windows.
- The game writes `phantom_save.bin` beside itself, so it must sit in a folder
  they can write to. It cannot run from `C:\Program Files`.

## Sound

The game plays sound through the default Windows output device. It uses waveOut,
which is part of Windows, so there is nothing to install.

The game makes one channel of sound at 22050 Hz. The board makes the same sound
and copies it to two channels, because the board's codec needs a pair. A PC
sound device accepts one channel, so this build sends one.

If the game cannot open the sound device, it prints a warning and runs silently.
Nothing else changes.

Use the game's own volume setting to change the level.
