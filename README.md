# BreezyBox for Cardputer

Cardputer ADV and StickS3 port of BreezyBox, a BusyBox-inspired shell and virtual terminal system for ESP32 with Wi-Fi, Bluetooth keyboard support, bundled built-in apps, and an embedded Lua runtime.

## Features
* Unix-like Shell: Familiar commands like `ls`, `cat`, `echo`, `cd`, `pwd`, `cp`, `mv`, `rm`, `mkdir`, `sed`, `grep`
* Virtual Terminals: Multiple independent terminals with hotkey switching (F1-F4)
* ANSI Colors: Full 16-color support with SGR escape sequences
* I/O Redirection: Support for >, >>, <, and pipes |
* Script Execution: Run shell scripts from files with sh
* Tab/Hint Completion: Command completion via linenoise and hint autocomplete for some command options
* History: Arrow key navigation through command history
* WiFi Commands: wifi scan, wifi connect, wifi status, saved credentials
* Scrollback: Ctrl+arrow to view paged scrollback
* HTTP Server: Built-in file server with httpd
* SSH: SSH client with known-host persistence and saved host aliases
* Bluetooth keyboard support: Bluetooth keyboard scanning, pairing, reconnect, and saved target storage
* File storage: LittleFS and SD card support
* Lua: Embedded Lua runtime for scripting and lightweight GUI/TUI apps
* Built-in apps: `vi`, `plasma`, `termbench`, `wget`, `gzip`, `gunzip`, `ping`
- Lots of extra shell utilities: [See commands.md](docs/commands.md)

## Examples and commands
Check out more examples and commands here

* [Commands](docs/commands.md)
* [Examples](docs/examples.md)
* [Lua](docs/lua.md)

## Filesystems

The shell exposes these roots:

- `/root` for internal LittleFS
- `/sd` for the SD card, when mounted

## Build And Flash

Activate ESP-IDF first:

```sh
source ~/esp/esp-idf/export.sh
```

Cardputer ADV:

```sh
make build
make flash PORT=/dev/cu.usbmodem1101
make monitor PORT=/dev/cu.usbmodem1101
```

StickS3:

```sh
make build BOARD=sticks3
make flash BOARD=sticks3 PORT=/dev/cu.usbmodemXXXX
make monitor BOARD=sticks3 PORT=/dev/cu.usbmodemXXXX
```

Useful targets:

```sh
make build
make flash
make monitor
make erase
make rebuild
```

## Serial Monitor

From the repo root:

```sh
make monitor PORT=/dev/cu.usbmodem1101
```

If `idf.py monitor` is running, exit with:

```text
Ctrl-]
```

## Keyboard Notes

Cardputer modifier behavior:

- `Shift`: hold for uppercase
- `Ctrl`: control modifier
- `Alt`: ESC-prefix/meta
- `Opt`: ESC-prefix/meta

Fn navigation:

- `Fn + ;` = Up
- `Fn + .` = Down
- `Fn + ,` = Left
- `Fn + /` = Right
- `Fn + Backspace` = Delete
- `Fn + \`` = Escape

Scrollback:

- `Ctrl + ;` = scroll up
- `Ctrl + .` = scroll down

Virtual terminals:

- `Ctrl + 1` = VT0
- `Ctrl + 2` = VT1
- `Ctrl + 3` = VT2
- `Ctrl + 4` = VT3
- `vt` shows the active VT
- `vt 0` to `vt 3` switches directly

Pager controls in `help` and `more`:

- `Space` = next page
- `Enter` = next line
- `q` = quit

## StickS3 Input

StickS3 uses an on-screen keyboard for text entry.

- first button press opens the keyboard
- short `A` = previous key
- short `B` = next key
- short `A+B` = select key
- long `A` = previous row
- long `B` = next row
- long `A+B` = accept/send line

Special keys in the on-screen keyboard:

- `^` toggle caps
- `_` space
- `<` backspace
- `>` cancel/close
- `!` accept/send

### Thanks
Thank you [valdanylchuk](https://github.com/valdanylchuk) for the great work on BreezyBox!

* [BreezyBox](https://github.com/valdanylchuk/breezybox/tree/main)
* Original inspiration [BreezyDemo](https://github.com/valdanylchuk/breezydemo)
