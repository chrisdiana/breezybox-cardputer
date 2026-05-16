# AGENTS

## Project Split

What’s still in `breezybox-firmware/`:

- Project/build files:
  - `breezybox-firmware/CMakeLists.txt`
  - `breezybox-firmware/Makefile`
  - `breezybox-firmware/partitions.csv`
  - `breezybox-firmware/sdkconfig`
  - `breezybox-firmware/sdkconfig.defaults`
  - `breezybox-firmware/dependencies.lock`
- App-specific firmware code in `breezybox-firmware/main`:
  - `breezybox-firmware/main/main.c`
  - Cardputer keyboard/input:
    - `breezybox-firmware/main/cardputer_keyboard.c`
    - `breezybox-firmware/main/cardputer_keyboard.h`
  - Console/display bridge:
    - `breezybox-firmware/main/my_console_io.c`
    - `breezybox-firmware/main/my_console_io.h`
  - Built-in app wrappers:
    - `breezybox-firmware/main/cmd_vi_builtin.c`
    - `breezybox-firmware/main/cmd_plasma_builtin.c`
    - `breezybox-firmware/main/cmd_termbench_builtin.c`
    - `breezybox-firmware/main/cmd_wget_builtin.c`
    - `breezybox-firmware/main/cmd_gzip_builtin.c`
    - `breezybox-firmware/main/cmd_gunzip_builtin.c`
    - `breezybox-firmware/main/cmd_testgfx.c`
    - `breezybox-firmware/main/cmd_app_compat.c`
  - Loader symbol table patch:
    - `breezybox-firmware/main/all_my_symbols.c`
- Local overridden components that are still Cardputer-specific or patched:
  - `breezybox-firmware/components/valdanylchuk__breezy_rgb_lcd`
  - `breezybox-firmware/components/valdanylchuk__breezy_term`
  - `breezybox-firmware/components/espressif__elf_loader`
- Managed dependencies still fetched normally:
  - `breezy_bt`
  - `littlefs`
  - `zlib`
  - cmake utilities
  - under `breezybox-firmware/managed_components`
- Runtime/build artifacts:
  - `breezybox-firmware/fs_image`
  - `breezybox-firmware/build`

What is no longer in `breezybox-firmware/` as the active shell core:

- `components/valdanylchuk__breezybox`
- that moved to `breezybox-cardputer`

So the split is now:

- `breezybox-cardputer/` = shell core
- `breezybox-firmware/` = Cardputer firmware app, board glue, built-in app wrappers, patched display/term/loader components

Built-in app source ownership:

- `breezybox-cardputer/apps/` now holds the local app sources that the firmware wrappers compile in:
  - `vi`
  - `plasma`
  - `termbench`
  - `wget`
  - `gzip`
  - `gunzip`

## Scripting Direction

The old dedicated MicroPython runtime path is no longer part of the active
product configuration.

The active replacement path is now:

- embedded Lua inside the main `breezybox-firmware/` image
- no second app partition
- no boot handoff between runtimes
- lower RAM and flash overhead than the old dual-runtime MicroPython model

Current active Lua surface:

- `lua`
- `lua shell`
- `lua -e <chunk>`
- `lua <script.lua> [args...]`
- a built-in `breezy` Lua module for shell/filesystem basics
- `breezy.gfx` for simple graphics-mode drawing from Lua

Treat `micropython-cardputer/`, `MicroPythonShell-main/`, and
`Cardputer-MicroHydra/` as inactive reference material unless a future task
explicitly revives that path.

## ELF Loader Note

Original Waveshare `breezydemo` could run external ELF apps because its memory layout and board assumptions were more favorable to the loader, including PSRAM-oriented expectations.

This Cardputer ADV port is different:

- `CONFIG_SPIRAM` is not enabled in the active firmware build
- runtime testing showed `EXEC free=0 largest=0` before ELF launch
- when the loader relocates into normal SRAM/DRAM instead, that code path is not executable here
- jumping to the relocated ELF entry causes `InstructionFetchError`

So external ELF apps are currently not a reliable execution model on this Cardputer build. Built-in apps are the supported path unless the loader/memory strategy is redesigned.


## Repo Layout

- `breezybox-firmware/` active Cardputer ADV firmware
- `breezybox-cardputer/apps/` built-in app sources and related assets




## Current Limits

Important current limits:

- terminal geometry is fixed at `40x16`
- runtime font scaling is not implemented
- external ELF app execution is not the primary supported path
- `ln` supports hard links only
- `sed` supports simple substitution form only:
  - `s/old/new/`
  - `s/old/new/g`
