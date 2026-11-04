# Ascendancy — native Windows port

A native Windows 10/11 port of **Ascendancy** (The Logic Factory, 1995), the
DOS 4X space strategy game — no DOSBox required.

> **You need your own copy of the game.** This repository contains only new
> code. It never includes or distributes the original game files
> (`ASCEND.EXE`, `*.COB`, …); you point the port at your installation.

## Status

| Phase | What | State |
|---|---|---|
| 0 | Tooling: archive/sprite readers, LE executable loader core, Ghidra pipeline | **in progress** |
| 1 | Native runner: original game code runs directly on the CPU, DOS/VGA/sound replaced by Windows/SDL | **in progress** — the whole game runs natively, with sound and music |
| 1.5 | Smarter AI (not harder — no extra cheats), borderless fullscreen, filters, 16:9 | planned |
| 2 | Full C++ source port, original executable no longer needed | long-term |

See [docs/ROADMAP.md](docs/ROADMAP.md) for the full plan and
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for how it works.

## What works today

- `Ascendancy.exe` — the Phase 1 runner. Put it in your Ascendancy folder and
  run it. Logo, main menu, new-game setup, species briefing, star map and
  planet screens work with mouse and keyboard, borderless fullscreen by
  default, with sound effects and music. Not yet verified: the intro movie.

  Star map and system view: **mouse wheel** zooms, **hold the middle button
  and drag** to rotate (left/right; star map also tilts up/down), **middle
  click** re-centres. Elsewhere the **mouse wheel** scrolls the list under
  the pointer (diplomacy phrases, species, save games, ship components, ...).

  Right-click anything for the game's built-in help popup (the original
  "help-click", Shift+click, still works too); click anywhere to close it.

  **Options panel:** the *Options* button on the main menu, or **F12** at any
  time: fullscreen/window, picture filter (Crisp, Pixel perfect, Soft, Smooth
  art, CRT monitor), right-click behaviour, music and effects volume, and the
  button click (*Original*, *Varied* - subtly different every time - or
  *Mixed* - takes turns with the game's other click sounds). Saved to
  `ascendancy.ini`. `--no-sound` starts without sound.

  Command line (overrides the ini for one session): `--windowed`,
  `--fullscreen`, `--filter pixel|crisp|soft|smooth|crt`, `--no-right-help`.
  `Alt+Enter` toggles fullscreen. Debug: `--trace`, `--trace-irq`, `--trace-vfx`, `--headless`,
  `--shots <ms>`, `--script "<ms>:click:<x>:<y>,<ms>:key:<k>,<ms>:shot,<ms>:winshot"`
  (with `SDL_VIDEODRIVER=offscreen` the full window can be tested without a display).

- `ascx` — lists/extracts the COB archives and converts every sprite to PNG.
- `ledump` — parses `ASCEND.EXE` (DOS/4GW LE format), applies its relocations
  and exports an ELF file for analysis in Ghidra.

```
ascx list    ASCEND01.COB
ascx extract ASCEND01.COB out\
ascx dumpall C:\Games\Ascendancy sprites\
ledump ASCEND.EXE --elf ascend.elf
```

## Building

The game runtime is a **32-bit** Windows program (in Phase 1 the original
32-bit game code runs inside it). Windows 10/11 on x64 and ARM64 run 32-bit
programs natively.

Cross-compiling from Linux with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw):

```
cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.cmake
cmake --build build-win
```

Native Linux build (tools only): `cmake -B build && cmake --build build`.

## Legal

Ascendancy is © The Logic Factory. This project is an independent,
non-commercial effort to keep the game playable on modern systems; it ships
no copyrighted game content. Supply the data files from your own copy.
