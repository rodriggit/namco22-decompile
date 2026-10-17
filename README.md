# Time Crisis for Wii

A fork of [namco22-decompile](https://github.com/spacestate1/namco22-decompile), the project that rebuilds Namco's System 21 /
22 / 23 arcade games in C for PC. This fork ports **Time Crisis** (Namco, 1995, Super System 22) to the **Nintendo Wii**, as a
native PowerPC program built with devkitPro.

> **Status: first working version, in progress.**
>
> | Part | Status |
> |---|---|
> | Boot, loading the ROMs from the SD card | ✅ Works |
> | 2D graphics (text, sprites, HUD) | ✅ Shown |
> | 3D graphics (polygons) | ❌ Not drawn yet: the GPU (GX) renderer is still to be written |
> | Controls (Wii Remote) | ✅ Work |
> | Sound | ❌ Not audible yet |
> | Speed | ⚠️ Well below full speed |
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
| `build-wii/tc_wii` | The ELF executable (with symbols, for debugging) |
| `build-wii/tc_wii.dol` | The executable for the Wii / Dolphin |

The build also checks that the program fits in the Wii's MEM1 memory (`Checking tc_wii PT_LOAD ranges...`) and stops with
a message if it does not.

**To rebuild after changing the code**, repeat step 3 only. The `.dol` is remade only when the ELF is linked again. If you
deleted the `.dol` without changing anything, convert it by hand:

```bash
$DEVKITPRO/tools/bin/elf2dol build-wii/tc_wii build-wii/tc_wii.dol
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
| **Home** | Test mode (the operator's menu) |

**To reload**, aim **off the screen** and shoot, as on the arcade.

**In Dolphin**, the Wii Remote's buttons are mapped to keyboard keys or a pad in *Controllers → Wii Remote 1 →
Configure*. The mouse can drive the IR pointer.

## How the port works

All the Wii-only code is kept apart: what is new lives in `timecris/wii/`, and what changed in the shared engine is behind
`#ifdef __WII__`. The PC version is unchanged.

| File | What it does |
|---|---|
| `timecris/wii/port.c` / `port.h` | The Wii backend: video (VI/GX), Wii Remote, SD card (libfat), audio (ASND) |
| `timecris/wii/toolchain-devkitpro.cmake` | The CMake toolchain for devkitPPC |
| `timecris/wii/check_mem1_segments.cmake` | The memory check after the build |
| `engine/ss22_host.c`, `engine/ss22_input_wii.c` | The engine's host and controls, Wii versions |
| `engine/ss22_gl.c` | The renderer. On the Wii it draws the 2D in software today; the 3D is still to come |

**Memory.** The Wii has 24 MB of MEM1 and 64 MB of MEM2, and the arcade uses large ROM regions. On the Wii the sound keeps
only its 6 MB of real chips instead of its 16 MB region, and the sprites 12 MB instead of 16. The result is the same: the
gaps read as zeros, or as the transparent colour for the sprites.

**Still to do**, in order:
1. Draw the 3D on the Wii's GPU (GX). This is also what should help the speed most.
2. Show the picture through GX instead of converting it to YCbCr on the CPU.
3. Play the sound through ASND.

The porting notes are in [WII_PORT_PLAN.md](WII_PORT_PLAN.md).

## License

The code in this repository is released under the [MIT License](LICENSE), like the original project.

- The game itself (its program, graphics, sound and music) belongs to Namco / Bandai Namco. This repository has no game
  files and the MIT License does not cover them; the files under `*/gen/` that are translated from the game's program are
  derived from Namco's code, and the license covers this project's own work, not theirs.
- Other people's code keeps its own license: [Nuklear](https://github.com/Immediate-Mode-UI/Nuklear) (`third_party/`, MIT /
  public domain). The Wii port links devkitPro's libogc, wiiuse, libfat and zlib, each under its own license.
- Credit for the original project: [spacestate1/namco22-decompile](https://github.com/spacestate1/namco22-decompile).
