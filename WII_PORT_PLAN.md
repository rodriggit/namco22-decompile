# Wii Port Plan for Time Crisis

This document describes a focused porting strategy to adapt the Time Crisis code from this repository to the Nintendo Wii using devkitPro homebrew tooling.

## Goal

Port only the Time Crisis game from the Namco 2x decompilation project to the Wii, while keeping the core game logic and ROM handling intact and replacing only the host/platform layer that depends on desktop SDL/OpenGL APIs.

## Status

- [x] Scope narrowed to Time Crisis only
- [x] Wii branch created
- [x] Port plan created in English
- [x] Nominal platform abstraction created for the Wii backend
- [x] Time Crisis Wii backend files isolated under `timecris/wii`
- [x] Trigger mapping fixed to Wiimote B
- [x] Wiimote IR pointer path introduced into the host stub
- [x] Wii platform config contract added for display/audio setup
- [ ] Real GX renderer backend implementation
- [ ] Real AX audio backend implementation
- [ ] Full Wii devkitPro build target
- [ ] End-to-end gameplay validation on Wii hardware

## Scope

This plan intentionally ignores the other games in the repository. The target is a Wii-native build for Time Crisis only.

## High-level approach

The project is not a pure single-file game. It is a decompiled arcade emulator framework with:

- the actual game logic generated from the original arcade ROMs
- shared engine modules for rendering, audio, input, and ROM access
- a desktop host layer based on SDL2 and OpenGL

For Wii, the correct strategy is to preserve the game logic and replace the host layer with a Wii-native runtime built on:

- devkitPro
- libogc
- GRRLIB or GX directly
- AX for audio
- WPAD / PAD / Wii controller APIs
- SD card / FAT filesystem access for ROMs and save data

## Why this is the right approach

The Desktop build depends on APIs that do not exist in the same form on the Wii:

- SDL2 windowing and input
- OpenGL rendering context
- desktop file APIs and OS assumptions
- audio output via desktop sound APIs

The Wii port therefore needs a platform abstraction layer, not a rewrite of the game logic itself.

## Files relevant to Time Crisis

The main files to focus on are:

- `timecris/src/tc_game.c` – Time Crisis hardware definition, ROM list, inputs, and game config
- `timecris/build.sh` – shows the project’s Linux/Desktop build assumptions
- `engine/ss22_run.c` – main runtime loop and host integration
- `engine/ss22_host.c` – window/OpenGL host creation
- `engine/ss22_gl.c` – OpenGL rendering path
- `engine/audio_out.c` – audio output
- `engine/ss22_input.*` and related input headers – input mapping
- `engine/ss22_board.c` and other shared engine modules – platform-neutral emulation functions

## Porting strategy

### 1. Freeze the desktop logic path

Keep the current Linux/Windows build intact and do not modify the Time Crisis game logic while designing the Wii port.

The Wii port should act as a second platform backend that reuses the same game logic and ROM data, while replacing only the host runtime layer.

Status: [x] Done

### 2. Introduce a platform abstraction layer

Create a minimal platform interface for the engine, roughly along these lines:

- init platform
- shutdown platform
- poll input
- start frame
- present frame
- output audio samples
- read/write save data
- locate ROM files on SD

This layer should isolate the engine from desktop-specific dependencies, especially SDL and OpenGL.

Status: [x] Done

### 3. Replace SDL windowing and input

The current desktop host code expects an SDL window and keyboard/mouse/gamepad input model.

On Wii:

- use libogc for initialization and frame loop
- use GX for rendering
- use WPAD and PAD for controller access
- map Wii controls to the arcade inputs used by Time Crisis

For example:

- trigger -> Wiimote B button
- pedal / reload -> mapped to Wiimote 1 or Nunchuk Z
- coin / service / test -> mapped menu commands
- aim -> Wiimote IR pointer

Status: [x] Input mapping baseline defined

### 4. Replace the OpenGL renderer

This is the most substantial part of the port.

The desktop pipeline uses OpenGL fixed-function or compatibility-style calls. Wii cannot use that path directly.

The port must replace it with a Wii-native rendering backend based on GX/GRRLIB, which means:

- creating a framebuffer-compatible output path
- uploading textures for rendered layers
- blitting 2D sprite/quad output to the Wii screen
- preserving the 2D arcade presentation as close as possible

This is not a game logic rewrite; it is a video backend replacement.

Status: [ ] Pending real GX implementation

### 5. Replace desktop audio

The project currently uses SDL audio functions in the engine.

Wii audio should be moved to AX with a small audio mixing layer. A practical first version is:

- generate audio in PCM samples from the engine
- feed them into AX audio streams or buffers
- keep sample format consistent with the existing emulated output

Status: [ ] Pending real AX implementation

### 6. Keep ROM loading and memory layout unchanged

Time Crisis depends on ROM files unpacked from the original MAME set and the generated C code output.

The Wii version should still use the same ROM data layout and game logic. Only the file discovery and loading path must be adapted to the Wii homebrew filesystem.

Status: [ ] Pending Wii filesystem integration

### 7. Create a Wii-only build target

Add a dedicated Wii build path with devkitPro toolchain settings. Keep the existing Linux/Windows build paths untouched.

The build target should compile only the Time Crisis subset and use Wii-specific libraries.

Status: [ ] Pending devkitPro target definition

## Recommended implementation order

### Phase 1: project isolation

- create a dedicated `wii` branch
- isolate Time Crisis from the rest of the repo
- confirm which engine files are required for `tc_game.c` and the main runtime

Status: [x] Done

### Phase 2: platform stub

- create a Wii backend skeleton with stubs for:
  - init
  - input polling
  - frame submission
  - audio
  - file access
- compile a minimal Wii binary that boots and reaches the engine entry point

Status: [x] Stub layer and backend contract in place

### Phase 3: input layer

- implement control mapping for Wii input
- support at least a basic playable control scheme
- keep the original emulator logic unchanged

Status: [x] Baseline mapping implemented: trigger is Wiimote B; IR pointer integrated

### Phase 4: graphics backend

- replace the OpenGL host path with GX-based rendering
- ensure the game displays correctly at 480p or native Wii resolution
- tune scaling and aspect ratio

Status: [ ] Pending real GX implementation

### Phase 5: audio backend

- route generated sound to AX
- verify basic audio timing and stream operation

Status: [ ] Pending real AX implementation

### Phase 6: validation

- confirm ROM loading works from SD
- verify menu flow and startup behavior
- test basic gameplay loop
- fix frame pacing and input latency

Status: [ ] Pending end-to-end Wii validation

## Risks and expected challenges

### Rendering differences

The decompiled engine assumes an OpenGL-style pipeline. Direct mapping to GX is not one-to-one. Some graphical effects may need approximation or simplification.

### Timing and frame pacing

Wii hardware differs from desktop timing. The emulator expects a specific frame cadence. That may require tuning to maintain stable gameplay.

### Input modeling

The original Time Crisis game uses a gun-style input model with trigger, pedal, and aim. Wii input must be mapped sensibly for homebrew playability.

### Audio pipeline complexity

The original project emits sound from the emulated machine. The Wii backend must produce audio without breaking timing.

## Next concrete work block

### GX render backend

- define the Wii frame buffer lifecycle in the backend
- replace the desktop OpenGL host path in the Wii branch with GX presentation
- maintain a 4:3 arcade presentation with Wii-native scaling
- handle screen clear, texture upload, and final blit path

Status: [ ] In progress

### AX audio backend

- initialize AX audio stream with a fixed rate and format
- queue PCM output generated by the engine
- keep the sample format aligned with 16-bit stereo output

Status: [ ] In progress

### devkitPro build target

- add the Wii toolchain definition for Time Crisis only
- isolate the build to the native backend and the Time Crisis subset
- keep desktop builds unchanged for Linux/Windows

Status: [ ] In progress

## Success criteria

The Wii port is successful when all of the following are true:

- the Time Crisis ROMs can be loaded from SD
- the game starts in a Wii-native binary
- the menu and game flow run
- controls are mapped and usable
- video renders through a non-OpenGL backend
- audio runs through libogc / AX
- the project remains isolated to the Time Crisis subset and does not require unrelated games

## Immediate next action

The next step is to create a small platform abstraction and begin replacing the SDL/OpenGL host initialization with a Wii stub. Once that compiles, the engine can be validated incrementally without rewriting the game logic.

## Recommended file targets for the first pass

- `engine/ss22_run.c`
- `engine/ss22_host.c`
- `engine/ss22_gl.c`
- `engine/audio_out.c`
- `timecris/src/tc_game.c`

These are the highest-value files to adapt first.
