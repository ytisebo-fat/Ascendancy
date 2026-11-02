# Architecture

## The original program

| | |
|---|---|
| Executable | `ASCEND.EXE`, Dec 24 1996, 587,475 bytes |
| Format | MZ stub + LE (Linear Executable), DOS/4GW extender |
| Compiler | Watcom C/C++32 (C++; asserts name ~30 source modules) |
| Code object | preferred base `0x10000`, 0x6DAF6 bytes (~440 KB) |
| Data object | preferred base `0x80000`, 0xA7710 bytes (BSS + stack included) |
| Relocations | 10,187, all 32-bit absolute offsets |
| Graphics | VESA 640×480×256 via SciTech UniVBE/Lite + `VESA480.DLL` |
| Sound | Miles AIL 3.02 digital (`*.DIG` drivers), VOC effects, RAW music |
| Calling convention | `__watcall`: EAX, EDX, EBX, ECX, then stack; callee pops; all registers except EAX preserved |

Source modules seen in assert strings: `winmgr`, `cosmos`, `race`, `ssystem`,
`raceneg`, `flic`, `eventwnd`, `batmode`, `gwindow`, `ship`, `planet`,
`coswnd`, `batcon`, `cache`, `status`, `corn` (contains `main`), `batfxa`,
`batfx`, `gsystem`, `helpwin`, `shipwin`, `negwnd`, `shipgiz`, `planwin`,
`cfile`, `font`, `research`, `flicwin`.

### Where DOS and hardware are touched

Almost all of it is in the Watcom C runtime and libraries at the end of the
code object (`0x6F000`–`0x7D000`):

| Kind | Count | Notes |
|---|---|---|
| `int 21h` (DOS) | 74 | file I/O, time, memory, exit |
| `int 31h` (DPMI) | 37 | memory, selectors, real-mode calls (VESA) |
| `int 2Fh` | 9 | multiplex (Windows detection, time slice) |
| `int 16h` | 5 | keyboard BIOS |
| `int 33h` | 3 | mouse |
| `int 10h` | 2 | video BIOS |
| port I/O | ~95 | clusters at `0x5AB6B`–`0x5B84F` and `0x6D7A9`–`0x6D8E9` in game code (to identify), rest in libraries |
| segment loads | ~150 | nearly all in the runtime; harmless except FS/GS (patched) |

## Key addresses (original)

| Address | What |
|---|---|
| `0x73774` | `_cstart_` (entry; **not run** by the port) |
| `0x76C76` | `__InitRtns(max_priority)` — runs the 19 init routines at `0x8A050` |
| `0x76C27` | `__CMain()` — builds argv, calls `main`, then `exit` |
| `0x1C424` | `main(argc, argv)` (corn.cpp) |
| `0x25878` | assert handler `(?, "..\\file.cpp", line)` |
| `0x897C4`… | runtime globals set by `_cstart_` (see `runtime/game_image.h`) |
| `0x73A49` | DS slot used by `__interrupt` handlers |

## Phase 1 runner (`src/runtime/`)

```
Ascendancy.exe (32-bit Windows)
 ├─ game_image  map ASCEND.EXE: code at 0x10000 (1:1 with disassembly when
 │              possible), data at original+0x10000000; apply relocations
 ├─ patches     verified byte patches (FS/GS reloads → harmless equivalents)
 ├─ dos         vectored exception handler: INT nn / IN / OUT / CLI / STI /
 │              segment loads fault in user mode → decoded and emulated:
 │                INT 21h  file I/O → Win32, time, memory, exit
 │                INT 31h  DPMI memory → VirtualAlloc, selectors (pseudo)
 │                INT 33h/16h/10h  mouse/keyboard/video → (SDL, next step)
 │                ports    VGA DAC palette, retrace, PIT, keyboard
 └─ main        set the runtime globals _cstart_ would have set,
                call __InitRtns(0xFF), then __CMain()
```

Why skip `_cstart_`: it reaches the PSP and environment through DOS
selectors, which a Windows process cannot create. Its effects are a dozen
globals, which `main.cpp` sets directly.

Why exceptions instead of rewriting every `INT` site: the faulting
instruction tells us exactly what the game wanted, with no need to understand
the surrounding code first. Hot paths (file reads, palette, blits, timers) are
being moved to direct hooks — the original function's entry is replaced by a
jump to C++ — which is also how game logic (e.g. the AI) gets replaced later.

### Status of the runner

Runs the full C runtime start-up and enters `main()`: reads `COB.CFG`, opens
and indexes all three archives, allocates its 32 MB heap. Stops at the first
VESA BIOS call (`int 10h AX=4F01` through DPMI `0300h`) — the next step is to
replace the UniVBE/VESA layer with an SDL2 window.

## Tools

- `ascx` — COB archives, palettes, SHP sprites → PNG
- `ledump` — LE parser; `--elf` export for Ghidra; `--fixups` list
- `re/ghidra_scripts/` — headless Ghidra pipeline:
  `Prepare.java` (parameter recovery, functions at vtable/callback targets),
  `ExportAll.java` (labels functions by source module, exports function list,
  strings and decompiled C). Requires adding a `__watcall` prototype to
  Ghidra's `x86gcc.cspec` — see `re/README.md`.
