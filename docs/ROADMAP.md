# Roadmap

## Goals (from the project owner)

- Native Windows 10/11 executable, no DOSBox.
- **Borderless fullscreen**.
- Two display styles: **original 4:3** and a **new 16:9** layout.
- **Image filters** to soften the 640×480 pixel art (smooth scaling, pixel-art
  upscalers, optional CRT look).
- **A smarter AI — not a harder one.** The AI should play better (sensible
  colony development, research, ship design, fleet use, diplomacy), *without*
  extra production/research bonuses or diplomatic bias against the player.

## Phase 0 — groundwork *(in progress)*

- [x] COB archive reader (`src/assets/cob.*`)
- [x] Palette + SHP/TMP sprite decoder (`src/assets/*`), verified on all 218
      sprite files / 1541 frames
- [x] LE (DOS/4GW) executable parser with fixups (`src/le/*`) — 2 objects,
      10,187 relocations, all 32-bit offsets
- [x] ELF export → Ghidra; Watcom `__watcall` calling convention
      (EAX, EDX, EBX, ECX, then stack; callee pops)
- [x] Map DOS/hardware touchpoints (see ARCHITECTURE.md)
- [ ] Identify Watcom C runtime functions (file I/O, memory, time, …)
- [ ] Name game functions from the source-file strings left in asserts
      (`winmgr.cpp`, `cosmos.cpp`, `race.cpp`, `batmode.cpp`, …)
- [ ] Remaining formats: FNT fonts, VOC sound, RAW music, FLC video, BIN/TSV
      tables, HAZ, save games

## Phase 1 — native runner

The original 32-bit game code is loaded into a 32-bit Windows process and
runs directly on the CPU. Everything that touched DOS or PC hardware is
redirected:

- [x] Loader: map code/data, apply relocations, set up runtime globals, call init + main
- [~] Watcom C runtime: file I/O, memory, time → Win32 (emulated at the
      interrupt level; hot paths move to direct function hooks)
- [x] Video: the game's VFX driver (VESA480.DLL) replaced by a native one → SDL2 window, palette, borderless fullscreen
- [x] Mouse (int 33h + real-mode handler at 0000:04F0) and keyboard (int 16h) → SDL events
- [x] Timer (PIT/IRQ0) → ticker thread, interrupts delivered at instruction boundaries
- [ ] Sound: Miles AIL 3.02 digital → SDL audio (VOC effects, RAW music)
- [ ] FLC intro/cutscenes
- [x] Safety net: unexpected `int`/port I/O caught and logged (ascendancy.log)
- [x] Watcom start-up + heap + COB archive loading run natively (tested under Wine)
- [ ] Borderless fullscreen, windowed, integer/fit scaling, 4:3 pillarbox
- **Milestone:** double-click `Ascendancy.exe`, play a full game, save/load
  compatible with the DOS version.

## Phase 1.5 — the improvements

Because each original function can be swapped for a C++ one individually,
these can land before the full source port:

- **Options panel** (done): main-menu button + F12, in the game's style and
  font; settings in `ascendancy.ini`.
- **Filters** (done): Crisp (sharp-bilinear, default), Sharp pixels, Soft,
  Smooth art (Scale2x on palette indices), CRT scanlines. Possible later:
  GPU shader filters (xBR/ScaleFX, curved CRT) if a shader backend is added.
- **16:9:** first, a 16:9 frame around the 4:3 game (ambient star-field side
  panels instead of black bars). Then real widescreen layouts for the main
  screens (star map, solar system, planet view) once their drawing code is
  reverse-engineered — these views have the most to gain from the extra width.
- **Smarter AI:**
  1. Reverse-engineer and document the existing AI (colony build queues,
     research picks, ship design, fleet orders, diplomacy, star-lane use).
  2. Write a test harness that runs AI-vs-AI games headless and measures
     outcomes (colonies, tech, fleet strength over time).
  3. Replace the weakest decisions with better heuristics, one area at a
     time, keeping the same rules and no cheats. Compare against the original
     AI with the harness.
  4. Option in the menu: *Original AI* / *Improved AI*.
- **Help:** right-click = the game's help-click (done). Later: hover
  tooltips — a short one-line summary from the same help texts after a
  short hover delay, once the window manager's hit-testing is mapped.
- **Camera controls** (done for the star map): wheel zoom, middle-drag
  rotate/tilt, middle-click re-centre, using the game's own camera functions.
  Next: the same for the battle view (BatModeWnd) and the research tree.
- **Sound variety:** the original plays the same click sound for every
  button; once audio is in, optionally vary it (per-button sounds or slight
  pitch/volume variation).
- The Antagonizer (`ANTAG.EXE`) can be supported by the same runner if wanted.

## Phase 2 — full source port

Replace the remaining original machine code with readable C++, function by
function, each verified against the original. When done the original
executable is no longer needed (only the data files), enabling a 64-bit build,
bug fixes, modding and other platforms.
