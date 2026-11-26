# MDK for the Nintendo 3DS (C++)

A port of [MDK](https://en.wikipedia.org/wiki/MDK_(video_game)) (Shiny Entertainment, 1997) to
the Nintendo 3DS, written in C++. The same game code also builds for Linux (SDL3 and Vulkan),
which is how it is developed and tested on a PC.

It is a C++ port of the C# and SDL3 port
[mdk-sdl](https://github.com/nautilusbits/mdk-sdl) by Marek Draškaba, which in turn reuses the
reverse engineering of the Godot port [mdk-godot](https://github.com/nemo22/mdk-godot). The C#
source is kept in `src/` as the reference the C++ code is checked against; it is not built here.

> [!IMPORTANT]
> **You need your own copy of the original game.** This repository contains no data from MDK:
> no levels, textures, models, sounds or videos. The build reads them from your installed copy
> (for example the GOG release) and packs them into the 3DS executable it makes on your computer.
> Do not share that executable: it contains the game's data.

- **3DS**: a `.3dsx` for the Homebrew Launcher, with the game data inside it. The original 3DS
  is the performance target. The SELECT button opens a menu of optional effects (lighting,
  filtering, glow, haze, HD textures and others).
- **Linux**: a desktop build of the same game code (SDL3, Vulkan).

The port is a work in progress.

## Prerequisites

Linux, x86-64. The steps below were checked from a fresh clone on Pop!_OS 24.04 (an Ubuntu
24.04 derivative) with g++ 13, CMake 3.28, Python 3.13, SDL 3.5 (built from source) and
devkitARM r67. Nothing here needs root except installing packages.

**For both builds**

| What | Why | Where from |
| --- | --- | --- |
| git, GNU make | | your distribution (`git`, `make`) |
| A C++17 compiler (g++) and zlib headers | the Linux build; host tools the 3DS build compiles | `g++`, `zlib1g-dev` |
| Python 3 with numpy and Pillow | the 3DS build's texture conversion; the Linux build's shader step | `python3`, `pip install numpy Pillow` |
| Your MDK folder | the game data | the folder MDK is installed in, with `TRAVERSE`, `MISC`, `FALL3D` and `STREAM` in it. Copy it from Windows if you installed it there; the case of the file names does not matter |

**For the 3DS build**: [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the 3DS
packages. With devkitPro's pacman:

```
sudo dkp-pacman -S 3ds-dev 3ds-zlib
```

`3ds-dev` brings devkitARM, libctru, citro3d, 3dstools (`3dsxtool`, `smdhtool`), picasso, tex3ds
and general-tools (`bin2s`). The build expects `DEVKITPRO` and `DEVKITARM` to be set (below).

**For the Linux build**: CMake 3.16 or newer, SDL 3 development files (`libsdl3-dev` where your
distribution has it; otherwise [build SDL 3 from source](https://wiki.libsdl.org/SDL3/README-cmake)
and install it), `glslangValidator` (`glslang-tools`), `pkg-config`, and a GPU with a Vulkan
driver to run it. The Vulkan headers and loader (volk) are in the repository.

**Check them**: `tools/check_prereqs.sh 3ds`, `tools/check_prereqs.sh linux` and (for the
optional HD textures) `tools/check_prereqs.sh hd` print what is missing. The 3DS build runs the first one itself before it compiles anything.

## Building for the 3DS

```
git clone https://github.com/colbyshores/mdk-3ds.git
cd mdk-3ds
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
export MDK_DATA_DIR="/path/to/MDK"
make -C mdk_3ds -j"$(nproc)"
```

Instead of `MDK_DATA_DIR` you can copy or link your MDK folder to `MDK` in the clone
(`ln -s /path/to/MDK MDK`).

The result is `mdk_3ds/mdk.3dsx` (about 235 MB with the committed music). The first build takes
10 to 15 minutes; most of it is converting the game's textures for the 3DS (into `build-arena-etc1/` and `build-kurt-etc1/`,
which `make clean` keeps, so later builds only convert what changed).

The build stops with an error, instead of making a broken `.3dsx`, when a prerequisite or the
game data is missing or a texture conversion fails. Two switches leave a conversion out on
purpose; the game then expands those textures on the console, which is slower:
`MDK_ARENA_ETC1=0` (the level textures) and `MDK_KURT_ETC1=0` (Kurt's sprite).

**The music.** `mdk_3ds/music/` holds the levels' and the menus' music, already converted for the
3DS (by `mdk_3ds/tools/music_dsp_convert.py`, from the original game's music and a fan-made
high-quality soundtrack; `mdk_3ds/music/CONVERT_REPORT.txt` lists the source of each track).
If that folder is absent, the build converts the music from your game data instead (into
`build-music/`; about 15 s, and it also needs `ffmpeg` built with libsoxr and a C compiler).
That conversion gives the original game's music; three tracks that only the committed set has
(level 3's AMB_DRIP and H9WATER, level 7's TRACK12) are left out, so those places are silent,
as they are in the original game.

**Installing.** Copy `mdk.3dsx` to the SD card's `/3ds/` folder and start it from the Homebrew
Launcher. The game keeps its files on the SD card in `/3ds/mdk/`: `config.ini` (the SELECT
menu's effects), `controls.ini` (the buttons; written with the defaults on the first start),
`saves/` and `mdk.log`.

**Controls** (the defaults, changeable in `controls.ini`): circle pad look; X forward, B back,
Y and A strafe; L jump; R fire (ZR too on a New 3DS); d-pad left and right pick the item, up
sniper mode, down use the item; in sniper mode X and B zoom. START pauses, SELECT opens the
effects menu.

**A .cia** (optional): `make -C mdk_3ds cia` also needs
[makerom](https://github.com/3DSGuy/Project_CTR), which is not part of devkitPro, on the `PATH`
(or `MAKEROM=/path/to/makerom`).

### HD textures (3DS, optional)

The 3DS can draw textures upscaled 2x by the AI upscaler
[Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN), made on your computer from your copy of
the game. Nothing upscaled is distributed. `mdk_3ds/tools/build_hd_pack.sh` makes the pack in
three steps: a C++ tool reads the textures out of your game data, Real-ESRGAN upscales them,
and they are converted to the 3DS's compressed formats. Besides the 3DS build's prerequisites
it needs:

- a GPU with a Vulkan driver and the Vulkan loader (`libvulkan1`); there is no CPU fallback,
- network access once, to download the pinned Real-ESRGAN release (about 45 MB, SHA-256
  checked, into `build-hd-tools/`).

```
export MDK_DATA_DIR="/path/to/MDK"
tools/check_prereqs.sh hd
mdk_3ds/tools/build_hd_pack.sh                # all levels; --levels 3,7 for some
```

For one level expect several minutes of upscaling on a desktop GPU (level 3 took 217 s on an
RX 6600) and several more of encoding on the CPU. The pack is stored uncompressed (about 85 MB
for levels 3-8), because the 3DS reads it faster than it can unpack it. A pack made before
2026-11-26 is compressed; convert it without upscaling again with
`mdk_3ds/tools/hd_pack_convert.py --from-pack build-hd-pack build-hd-pack-raw` and copy the new
folder instead. A second run only redoes what changed. It
writes the upscaled textures to `build-hd-mod/` and the pack to `build-hd-pack/`. Copy the
pack's contents to the SD card as `/3ds/mdk/hd/` (so that
`/3ds/mdk/hd/manifest.txt` exists), then turn on HD TEXTURES in the SELECT menu (or set
`enableHdTextures=1` in `/3ds/mdk/config.ini`); it takes effect from the next level loaded.
The pack is read from the SD card, not from the `.3dsx`.

## Building for Linux

```
cmake -S mdk_linux -B build-linux
cmake --build build-linux -j"$(nproc)"
MDK_DATA_DIR="/path/to/MDK" build-linux/mdk
```

Instead of `MDK_DATA_DIR`, a file `mdk_paths.cfg` next to the program can name the game's folder:

```
[paths]
mdk="/path/to/MDK"
```

## Tests

```
make -C tests/formats        # the format parsers (the ones that read game data need MDK_DATA_DIR)
make -C tests/scripts test   # the game logic; build the Linux binary first
tools/check.sh               # the code rules (AGENTS.md)
```

## Layout

- `mdk_core/src/`: the game, shared by both builds: formats, game logic, the script VM,
  collisions, audio mixer, the renderer's interface.
- `mdk_3ds/`: the 3DS build: `src/` (Citro3D renderer, DSP audio, input, the SD card), `shaders/`
  (PICA200), `tools/` (data staging, texture and music conversion, HD pack), `music/`, `Makefile`.
- `mdk_linux/`: the Linux build: `src/` (SDL3, Vulkan renderer), `shaders/`, `third_party/`
  (Vulkan headers, volk), `CMakeLists.txt`.
- `tests/`: `formats/` and `scripts/` test the C++ code; the `*.sh` scripts at the top test the
  C# port.
- `src/`, `shaders/`: the C# port this one is checked against (not built here).
- `docs/`: notes on the original game, its formats and this port. `AGENTS.md` and `SPEC.md` are
  the development rules and plan.

## Legal

This is an unofficial fan project, not affiliated with Shiny Entertainment or the current rights
holders of MDK. MDK and its data are the property of their respective owners.

Apart from the converted music in `mdk_3ds/music/` (see [the music](#building-for-the-3ds)), this
repository contains only original code, documentation and tools and doesn't include or
distribute data from the game. To run the port you need a legally obtained copy of the
original MDK, whose installed data files the port loads.

## Contributors

- [agrofubris](https://github.com/agrofubris): stereo 3D (C# port).

## Licence

Copyright © 2026 Marek Draškaba.

This program is free software: you can redistribute it and/or modify it
under the terms of the **GNU General Public License, version 3** as
published by the Free Software Foundation. It is distributed in the hope
that it will be useful, but WITHOUT ANY WARRANTY — without even the
implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See [LICENSE](LICENSE) for the full text.

The licence covers **this port's own code**. It says nothing about the
original game's data, which belongs to its rights holders and is not
distributed here.
