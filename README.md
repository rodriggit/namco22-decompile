# Time Crisis for Wii

A fork of [namco22-decompile](https://github.com/spacestate1/namco22-decompile), the project that rebuilds Namco's System 21 /
22 / 23 arcade games in C for PC. This fork ports **Time Crisis** (Namco, 1995, Super System 22) to the **Nintendo Wii**, as a
native PowerPC program built with devkitPro.

> **Status: playable, in progress.**
>
> | Part | Status |
> |---|---|
> | Boot, loading the ROMs from the SD card | ✅ Works |
> | 3D graphics (polygons) | ✅ Drawn on the Wii's GPU (GX) |
> | 2D graphics (text, sprites, HUD) | ✅ Drawn on the GPU, merged with the 3D by depth |
> | Controls (Wii Remote) | ✅ Work |
> | Sound | ⚠️ Plays (music, effects, voices), with gaps while the game runs below full speed |
> | Speed | ⚠️ In gameplay the game runs at about 40–42 fps and shows 20–21 frames a second (the arcade runs at 60) |
>
> Tested only in the **Dolphin** emulator. Not yet tried on a real Wii.

For the other games and the PC version, see the [original project's README](https://github.com/spacestate1/namco22-decompile#readme).

## What you need

- **Your own copy of the game**: MAME's `timecris.zip` set (Time Crisis, World, TS2 Ver.B; MAME 0.271 or later).
  **It is not included.** It is needed to build, not only to play. The checksums are in
  [docs/ROM_CHECKSUMS.md](docs/ROM_CHECKSUMS.md).
- **[devkitPro](https://devkitpro.org/wiki/Getting_Started)** with the `wii-dev` packages (devkitPPC, libogc, `elf2dol`)
  and `ppc-zlib`.
- **CMake** 3.16 or later, and **make**.
- **Python 3**.

On Windows, run everything from devkitPro's **MSYS2 terminal**, which comes with the devkitPro installer.

## Building the `.elf` and the `.dol`

Run every command from the top folder of the repository.

**1. Unpack the ROMs** (once). This puts the chips from the zip into `timecris/extracted/` and builds the main program
image:

```bash
python3 timecris/tools/setup_roms.py /path/to/timecris.zip
```

**2. Configure the Wii build** (once). Use the path of your devkitPro install: `/opt/devkitpro` on Linux and in
devkitPro's terminal, or `C:/devkitPro`, depending on how it was installed on Windows.

```bash
cmake -S timecris -B build-wii \
    -DTC_WII_PORT=ON \
    -DDEVKITPRO=/opt/devkitpro \
    -DCMAKE_TOOLCHAIN_FILE=timecris/wii/toolchain-devkitpro.cmake
```

**3. Build:**

```bash
cmake --build build-wii --target tc_wii -j2
```

The first build takes several minutes: the game's translated program is about 66 MB of C. It produces two files:

| File | What it is |
|---|---|
| `build-wii/tc_wii.elf` | The ELF executable (with symbols, for debugging; Dolphin can open it too) |
| `build-wii/tc_wii.dol` | The executable for the Wii / Dolphin |

The build also checks that the program fits in the Wii's MEM1 memory (`Checking tc_wii PT_LOAD ranges...`) and stops with
a message if it does not.

**To rebuild after changing the code**, repeat step 3 only. The `.dol` is remade only when the ELF is linked again. If you
deleted the `.dol` without changing anything, convert it by hand:

```bash
$DEVKITPRO/tools/bin/elf2dol build-wii/tc_wii.elf build-wii/tc_wii.dol
```

### Common build problems

- **`Cannot create temporary file in C:\WINDOWS\: Permission denied`** (Windows): the compiler has no temporary folder.
  Before building:
  ```bash
  export TMP=/tmp TEMP=/tmp TMPDIR=/tmp
  ```
- **`permission denied` with `Generator: build tool execution failed`**: the build folder is set up wrong, for example
  because it is somewhere else (`../build-wii`) or has no `Makefile`. Delete it and repeat step 2.
- **The `.dol` does not change**: Windows locks the file while Dolphin has the game open. Stop the emulation before
  building.

## The ROMs on the SD card

The game looks for the chips on the SD card, in this folder (or in `sd:/roms/`):

```
sd:/extracted/
    ts2verb.1  ts2verb.2  ts2verb.3  ts2verb.4  ts1data.8k
    ts1cg0.8d ... ts1cg6.18d   ts1ccrl.3d  ts1ccrh.1d
    ts1scg0.12f ... ts1scg5.3f
    ts1ptrl0.18k ... ts1ptru2.15f
    ts1wavea.2l  ts1waveb.1l
```

Copy **everything in `timecris/extracted/`** that step 1 made into it.

### In Dolphin

1. *Config → Wii → SD Card Settings*: turn on **Insert SD Card** and **Enable SD Card Sync Folder**.
2. Copy the ROMs into the sync folder, under `extracted/`. On Windows the folder is
   `Documents\Dolphin Emulator\Load\WiiSDSync\`. Dolphin copies it to the virtual SD card every time it starts a game.
3. *Graphics → Hacks*: turn off **Store XFB Copies to Texture Only**. The port writes the picture straight into the Wii's
   framebuffer, and with that option on Dolphin may not show it.
4. Open `build-wii/tc_wii.dol` with *File → Open*.

Without the SD card, the game cannot find the ROMs and quits right away.

### On a real Wii (untested)

With the Homebrew Channel: copy `tc_wii.dol` to `sd:/apps/timecrisis/boot.dol` and the ROMs to `sd:/extracted/`.

### Boot log

The game writes `sd:/wii_boot.log` with every step of the start-up: where it found the ROMs, whether the video started,
how much memory is left and, if something fails, what it was. In Dolphin it shows up in the sync folder **after you stop
the emulation**, as long as that folder is not open in Explorer. If it is open, Dolphin cannot sync and the file is lost on
the next start.

## Controls

The **Wii Remote is the gun**: aim with the IR pointer and shoot with the **B** trigger.

| Button | What it does |
|---|---|
| IR pointer | Aim |
| **B** (trigger) | Shoot. It also starts the game and picks menu items |
| **Nunchuk Z**, **1** or the GameCube controller's **B** | The foot pedal: step out of cover to attack; let go to take cover |
| **+** | Put in a coin (a game costs three) |
| **−** | Service |
| **2** | Test mode (the operator's menu) |
| **Home** | Leave the game: back to the Homebrew Channel (the settings are saved first) |
| Power (console or Wii Remote) | Save the settings and power off. Dolphin stops the game this way too |

**To reload**, aim **off the screen** and shoot, as on the arcade.

A red **crosshair** shows where the Wii Remote points (it is hidden while it points off the screen). The Wii Remote
**rumbles**: a short tap on each shot (fired by the gun's recoil output, as the arcade's solenoid; at most one every
150 ms, so a burst does not turn into one long buzz) and a strong one when the player is hit (the game's lives counter
going down).

**In Dolphin**, the Wii Remote's buttons are mapped to keyboard keys or a pad in *Controllers → Wii Remote 1 →
Configure*. The mouse can drive the IR pointer. The game needs **B and the pedal held together** (you shoot while out of
cover): if shooting stops working while the pedal is held, Dolphin is not passing both inputs at once. Map them to inputs
that can be pressed together, for example B on the left mouse button and 1 on a key.

## How the port works

All the Wii-only code is kept apart: what is new lives in `timecris/wii/`, and what changed in the shared engine is behind
`#ifdef __WII__`. The PC version is unchanged.

| File | What it does |
|---|---|
| `timecris/wii/port.c` / `port.h` | The Wii backend: video (VI/GX), Wii Remote, SD card (libfat), audio (ASND) |
| `timecris/wii/toolchain-devkitpro.cmake` | The CMake toolchain for devkitPPC |
| `timecris/wii/check_mem1_segments.cmake` | The memory check after the build |
| `engine/ss22_host.c`, `engine/ss22_input_wii.c` | The engine's host and controls, Wii versions |
| `engine/ss22_gx.c` | The Wii renderer, on the GPU (GX). The PC version keeps `engine/ss22_gl.c` |

**The renderer.** The polygons' textures are baked into CI8 (8-bit indexed) textures with one RGB565 palette per colour
group, cached between frames. The quads are sorted by depth as the arcade does (it has no depth buffer) and drawn with the
hardware's shading and depth fog in two TEV stages. Sprites and the text layer are converted to RGB5A3 textures: the text
only in the 16×16 cells that changed (the screen fade is applied by the GPU, so a fade does not redraw it), and a sprite
only when something it depends on changed since the last frame. The picture is copied from the GPU's framebuffer straight
to the screen.

**Sound.** The C352's output (85.3 kHz) is resampled to 48 kHz and played by one ASND stereo voice from a ring buffer, in
43 ms buffers, starting once 85 ms are queued. While the game runs below full speed the board makes less sound than the
Wii plays, so it runs dry now and then: the sound fades out for 5 ms instead of clicking, and comes back when the queue
refills. At full speed the gaps go away.

**Memory.** The Wii has 24 MB of MEM1 and 64 MB of MEM2, and the arcade uses large ROM regions. On the Wii the sound keeps
only its 6 MB of real chips instead of its 16 MB region, and the sprites 12 MB instead of 16. The result is the same: the
gaps read as zeros, or as the transparent colour for the sprites.

The ROM regions (textures, sprites, point data, sound: about 43 MB) are taken straight from the memory arenas
(`eng_alloc_big`), not from the heap. libogc's heap moves to MEM2 for good at the first request MEM1 cannot fit and leaves
the rest of MEM1 unused, so with the regions in it, how much memory the renderer had left depended on the program's size.
The renderer takes its own large buffers from what MEM1 has left, and the texture cache gets what remains (up to 6 MB).
If the cache is too small, textures are dropped and whole models flicker: the `tex:` part of `[WII-GX]` shows it
(`failed`, and why). The cache shares the heap with the rest of the renderer, so when something else finds no room (a
sprite's scratch buffer: after the attract demo had filled the cache, the menu's options went missing) it gives up the
textures the frame does not draw.

**Speed.** The Wii's CPU still runs the whole arcade board: the 68020's translated program, the master DSP's translated
program, the sound CPU and the C352 sound chip, plus the geometry stage that turns the DSP's display list into screen
polygons. Every 120 frames the game writes where the time went into `wii_boot.log`:

| Line | What it measures |
|---|---|
| `[WII-HOST]` | A frame's time: `emu` (the board), `draw` (the renderer), `present` (waiting for the screen), and the fps |
| `[WII-EMU]` | Inside `emu`: the DSP, and the sound (the C352's share in brackets). The 68K is what is left |
| `[WII-GX]` | Inside the renderer: polygons, textures, sprites, text, and `prep` (the geometry stage, inside `emu`) |

**Frameskip.** The board runs every frame, but while the game is behind the arcade's clock (59.906 Hz) one frame in two
at most is neither prepared nor drawn: no geometry stage, no draw, no wait for the screen. In gameplay that takes the game
from about 28 fps (and 28 on screen) to about 40–42 fps, with 20–21 on screen. When the game keeps up, every frame is
drawn. The Wii Remote is read on every frame, drawn or not.

In gameplay an emulated frame takes about 24–25 ms: DSP 7.5–8 ms, sound 7–7.5 ms (the C352 3.5–4), the 68K the rest, and on
the frames that are drawn geometry about 6.8 ms and drawing about 4 ms. What each part does on the Wii to get there:

- **68K**: it reads its registers and its ROM and work RAM in place (one load each, instead of a byte at a time through a
  call). Its main loop waits for the vblank interrupt by testing a word of work RAM over and over; nothing can change that
  word before the rest of the board runs, so the rest of its slice is spent at once (`RR_IDLE_POLL`, engine/lift_rt.h): the
  same emulation, about 2 ms a frame less.
- **Master DSP**: when no interrupt can be taken, `c71_run` retires an instruction itself and calls the translation of
  its 256-address page straight, through a table the translator emits (`tc_c25_exec_pages`).
- **Sound CPU**: the timers and the A-D converter are brought up to date only when their next event is due, not before
  every instruction.
- **Geometry**: the vertex transform runs in 32-bit arithmetic (each coordinate split in two, with the same result as the
  64-bit sum when the values are in range, and the 64-bit code when not), and the projection uses 32-bit integer divides.
  The PC build's 128-bit integers do not exist on a 32-bit CPU, where they fell back to software floating point.

**Profiling in Dolphin.** With *Show Debugging UI* on, *JIT → Enable JIT block profiling* (on before the game starts) and
*JIT → Write JIT block log dump* (with the game running, after playing a while) write
`Dump/Debug/JitBlocks/ID-tc_wii.txt` in Dolphin's user folder: every block of code with how often it ran and the cycles
it took, named after the function it belongs to. Run `build-wii/tc_wii.elf` rather than the `.dol` so Dolphin has the
names. Dolphin does not model the CPU's caches, so a real Wii may differ.

**Still to do:**
1. More speed: the master DSP (it runs one translated instruction per call), the sound CPU, the geometry stage, and the
   translated 68K itself.
2. A sky texture is missing in one of the cut-scenes.
3. The mixer's gamma, which is not applied yet.

The porting notes are in [WII_PORT_PLAN.md](WII_PORT_PLAN.md).

## License

The code in this repository is released under the [MIT License](LICENSE), like the original project.

- The game itself (its program, graphics, sound and music) belongs to Namco / Bandai Namco. This repository has no game
  files and the MIT License does not cover them; the files under `*/gen/` that are translated from the game's program are
  derived from Namco's code, and the license covers this project's own work, not theirs.
- Other people's code keeps its own license: [Nuklear](https://github.com/Immediate-Mode-UI/Nuklear) (`third_party/`, MIT /
  public domain). The Wii port links devkitPro's libogc, wiiuse, libfat and zlib, each under its own license.
- Credit for the original project: [spacestate1/namco22-decompile](https://github.com/spacestate1/namco22-decompile).
