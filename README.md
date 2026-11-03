# LaunchpadLEDs

A C-based project for controlling a Novation Launchpad through MIDI SysEx messages.  
It's a CLI-based shell for lighting and text effects, monitoring input, and sending raw SysEx commands.

<img src="https://github.com/user-attachments/assets/23cb3209-a391-47eb-8edc-27e8066a91f8" width="50%">

## Features

- **Interactive Shell:** Live control of lights, text, effects, and modes ([sysex_shell.c](sysex_shell.c)).
- **Lighting Effects:** Set solid colors, pulse, flash, and clear pads.
- **Text Display:** Scroll custom text across the Launchpad.
- **Tempo Control:** Send MIDI clock messages to adjust effect tempo.
- **Raw SysEx:** Send custom SysEx messages.
- **Mode Switching:** Switch between different built in modes/button layouts.
- **Input Listener:** Monitor and display incoming MIDI messages.

And the nice thing is- It *should* work with both Windows and Linux systems :P

## Requirements

- A compiler like GCC or Clang
- A Launchpad Mk2
- Windows: WinMM support (`midi_windows.c`)
- Linux: ALSA development headers (`midi_linux.c`):
  - ```sh
    sudo apt-get install build-essential libasound2-dev
    ```

## Building

On Windows, assuming you have GCC installed and in your path, run:
```bat
build.bat
```
to build to a ``.EXE`` file.

On linux, run:
```sh
./build.sh
```
to build to an executable file (no extension)

Actions should automatically build Windows x64, Linux x64 and Linux ARMv7 binaries.  
These will be available as artifacts for successful workflow runs.

---

But both of the provided scripts are basically just a fancy version of this command:
```sh
gcc -std=c11 -Wall -Wextra -Wpedantic sysex_shell.c midi_linux.c -o sysex_shell $(pkg-config --cflags --libs alsa) -pthread
```

## Usage

Run the shell from the project root:

```bat
lp.exe
```

or

```sh
./lp
```

Type `help` in the shell to view the supported commands.

## Commands

| Command | Description |
| --- | --- |
| `solid <r> <g> <b> [notes]` | Set individual or all pads to an RGB value |
| `solid <palette> [notes]` | Set pads to a Launchpad palette color |
| `pulse <color>` | Pulse all pads with a palette color |
| `flash <color>` | Flash all pads with a palette color |
| `text <color> <speed> <message>` | Scroll text across the grid |
| `clear` | Turn off all pads |
| `tempo <bpm> [count]` | Send MIDI clock messages |
| `send <hex bytes...>` | Send a Launchpad SysEx payload |
| `sendraw <hex bytes...>` | Send a raw SysEx payload or short MIDI message |
| `mode <session\|user1\|user2\|mixer>` | Switch the Launchpad layout |
| `reconnect` | Reconnect to the MIDI device |
| `listenon` or `listenoff` | Toggle MIDI input reporting |
| `help` | Display command help |
| `exit` | Quit the shell |

---

## SysEx basics

Launchpad SysEx messages typically begin with a predefined header:

```c
0xF0, 0x00, 0x20, 0x29, 0x02, 0x18
```

and end with

```c
0xF7
```

Common commands include:

```c
0x0A, LED, Colour                // Palette color (0-127)
0x0B, LED, R, G, B               // RGB color (0-63)
0x0E, Colour                     // Change all LEDs (palette color)
0x23, 0, LED, Colour             // Flash effect
0x28, 0, LED, Colour             // Pulse effect
0x14, Colour, Loop, Speed, Text  // Scrolls text
```

See the Novation Launchpad MK2 Programmer's Reference Manual for more information.

## Notes

This project is basically my earlier [LaunchpadLEDs-Python](https://github.com/Yuri010/LaunchpadLEDs-Python) but ported to C.  
As expected, this C version is a lot more- unreadable.. But hey it works...

## Screenshots
<img src="https://github.com/user-attachments/assets/23cb3209-a391-47eb-8edc-27e8066a91f8" width="45%">
<img src="https://github.com/user-attachments/assets/b4f537e0-218a-4b10-93c6-7ef2da24d937" width="45%">

## License

This project is provided as-is for hobby and educational use.

## Credits

- [Novation Launchpad MK2 Programmer's Reference Manual](https://fael-downloads-prod.focusrite.com/customer/prod/s3fs-public/downloads/Launchpad%20MK2%20Programmers%20Reference%20Manual%20v1.03.pdf)
- Inspired by the Ableton workflow and Launchpad community.
