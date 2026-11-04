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

### Video: the VFX driver

The game draws everything into its own 640×480 off-screen buffer and talks to
the screen only through a Miles Design **VFX 3** driver, `VESA480.DLL` (an LX
module loaded from the COB archive at run time). The driver is a table of 13
cdecl functions, copied by `VFX_register_driver` (`0x571F1`) to `0x86A78`:

| # | Function | Args |
|---|---|---|
| 0 | describe | → `{640, 480, 1, 256, 1, 1}` |
| 1/2 | init / shutdown | |
| 3 | area_wipe | x0, y0, x1, y1, colour |
| 4/5 | wait_vblank_leading / wait_vblank | |
| 6 | window_refresh | WINDOW*, x0, y0, x1, y1 (scales if sizes differ) |
| 7 | window_read | WINDOW*, x0, y0, x1, y1 |
| 8/9 | DAC_read / DAC_write | index, rgb[3] (6-bit) |
| 10 | bank_reset | |
| 11 | pane_refresh | PANE*, x0, y0, x1, y1 |
| 12 | line_address | x, y, &ptr, &count |

`runtime/vfx_driver.cpp` implements these on a linear framebuffer;
`runtime/video.cpp` shows it with SDL2. The game's VESA mode check
(`0x4F112`) is hooked to report mode 0x101 available, so UniVBE is never used.

### Render thread and mouse pointer

The window runs on its own render thread: it shows the framebuffer at the
monitor refresh rate (vsync), independent of the game's ~33 fps main loop,
and handles window/input events. The game thread only writes framebuffer
and palette (like video memory on a VGA card).

The game draws its pointer in software, two ways: the pointer module
(`0x5FCAC` draw / `0x5FF2E` erase, via windows `0x1220D0` composite and
`0x1220B8` background) and a "pane refresh with pointer" wrapper (`0x60BF2`)
that composites the pointer into the off-screen image before refreshing a
region under it. Both caused flicker and lag once the screen is presented
asynchronously. The port keeps the pointer out of the image entirely:
refreshes of the two pointer windows are dropped, `0x60BF2` becomes a plain
pane refresh, and the pointer shape (rendered with the game's own VFX shape
routine `0x57EFC`, twice over different fills for an exact mask) is drawn by
the render thread at the live mouse position. Visibility follows the game's
show/hide counter (`0x1220AC`) and off-screen flag (`0x1220E4`).

### Star map camera

`windows.txt` defines the camera buttons (ROTLEFT … ZOOMIN, messages 50–56
to COSMOSWnd). They only toggle continuous motion (flags at window+0xC0);
each frame message 59 calls `0x23834` rotate (EDX −1 spin / 0 tilt, float
degrees), `0x23878` zoom (±1, distance 20–60), `0x2390C` update view and the
window's redraw method. The port captures the COSMOSWnd instance through
its vtable entry (0x85A54) and applies wheel/drag input with the same
functions once per frame, from a hook on the main loop's call to the frame
function (0x1CC03, and the same call in the modal loop at 0x50A49) — always
on the game thread at a defined point.

### List scrolling

All list windows (windows.txt TYPE 10: diplomacy phrases, species, save
games, ship components, status lists) are one class (`gwindow.cpp`, vtable
0x85C70). Its message handler (0x2DAB4) scrolls one line per message
0x191/0x192 — the auto-repeat messages of its arrow buttons — and redraws.
Wheel notches outside the camera areas are passed to the frame hook, which
finds the shown list window under the pointer in the window manager's table
(0xFA368) and sends it those messages (3 lines per notch for long lists).

### Input

- Mouse: the game registers an 18-byte real-mode callback with the mouse
  driver that stores buttons/X/Y at `0000:04F0..04F5`, and its 60 Hz mouse poll
  (`0x6053C`, an AIL timer callback) reads them. The installer (`0x5FFC1`) is
  replaced by "return success", and the eight address constants are redirected
  to a low-memory shadow that the port fills from SDL (X/Y × 8, as the game
  sets an 8× mickey range).
- Keyboard: Watcom `_bios_keybrd` → `int 16h` 01/00/02, fed from SDL key
  events translated to PC scan codes + ASCII.

### Timer interrupts

The game installs a Miles AIL IRQ0 handler (`0x6F5CE`) driving 16 software
timers (frame counter 100 Hz, mouse poll 60 Hz, …). A ticker thread runs at
the PIT rate the game programs (ports 40h/43h) and sets the game thread's trap
flag; at the next instruction boundary the exception handler pushes an
interrupt frame (EFLAGS, CS, EIP) and enters the handler — only while the game
has interrupts (virtually) enabled. Ticks are also delivered after every
emulated instruction. Two patches were needed: the handler's private stacks
are moved to 256 KB stacks (Windows exception frames overflowed the 2.5 KB
DOS-era stack into the timer table), and chaining to the BIOS IRQ0 handler is
skipped.

### Status of the runner

Logo → main menu → new game setup → species briefing → star map → planet
screens, diplomacy, battles run natively (tested under Wine and on Windows),
with sound and music.

### Sound

The game's sound module (`ssystem.cpp`, object at 0xF8638) is unchanged; the
Miles AIL 3.02 digital functions under it are replaced (`audio.cpp`, cdecl
hooks at the AIL entry points 0x633B8 …0x6459A): `AIL_install_DIG_INI`
returns a token instead of loading a Sound Blaster driver, sample handles are
voices of an SDL audio mixer (48 kHz float, Hermite resampling, Miles
volume/pan, soft clipping). Effects: `soundfx.txt` names, loaded from
`data\NAME.voc` with the 42-byte header skipped, 22050 Hz, handles 1–7.
Music: handle 0, `music.txt` tracks (8-bit 22050 Hz RAW) streamed through two
0xC800-byte buffers (`AIL_sample_buffer_ready` / `AIL_load_sample_buffer`)
that the game refills from its frame limiter (0x5013C → 0x4AEA4), so the
music keeps up as long as the game loop runs; tracks resume where they left
off. Clicks: at `AIL_start_sample` the sample address is looked up in the
game's effect cache (0xBB4, 6 × 0x16 bytes: +8 address, +0x14 effect number);
effect 0 (`button`) gets the selected click variation (default *Soft*: the
game's click through a 2 kHz low-pass, 5 ms fade-in, about -10 dB).
The game polls `AIL_sample_buffer_ready` tens of millions of times per second
from its idle loop; that call has a lock-free fast path (a per-voice "buffer
may be free" flag), otherwise the mixer thread starves on the lock
(crackling, dropouts).

### Research tree

RESWIN (screen 20, handler 0x42A64, vtable entry 0x85EB8): its buttons set
motion flags applied by the per-frame message 0x3A: rotate =
`0x4E954(window+0x11F, float degrees)`, move = float at window+0xE3 (the
drawing clamps it; Home sets −100). The port feeds wheel and middle-drag
into the same two operations from the frame hook.

## Tools

- `ascx` — COB archives, palettes, SHP sprites → PNG
- `ledump` — LE parser; `--elf` export for Ghidra; `--fixups` list
- `re/ghidra_scripts/` — headless Ghidra pipeline:
  `Prepare.java` (parameter recovery, functions at vtable/callback targets),
  `ExportAll.java` (labels functions by source module, exports function list,
  strings and decompiled C). Requires adding a `__watcall` prototype to
  Ghidra's `x86gcc.cspec` — see `re/README.md`.
