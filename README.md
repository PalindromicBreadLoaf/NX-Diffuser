<div align="center">

# G-Diffuser

**A native PC port of F-Zero X (N64), including 64DD Expansion Kit support.**

[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-blue)](#platforms)
[![Renderer](https://img.shields.io/badge/renderer-D3D11%20%7C%20OpenGL-8A2BE2)](#platforms)
[![Built on](https://img.shields.io/badge/built%20on-libultraship-informational)](https://github.com/Kenix3/libultraship)
[![Decomp](https://img.shields.io/badge/decomp-inspectredc%2Ffzerox-brightgreen)](https://github.com/inspectredc/fzerox)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

</div>

---

G-Diffuser runs F-Zero X natively on your PC — no emulator at runtime. It is a fully decompiled source port
built on top of the [inspectredc/fzerox](https://github.com/inspectredc/fzerox)
matching decompilation and the [Kenix3/libultraship](https://github.com/Kenix3/libultraship)
runtime (the same engine that powers Ship of Harkinian and Starship, alongside other PC Ports like Battleship). On top of the original
game it adds true widescreen rendering, an in-game enhancement menu, quality-of-life features,
texture-pack modding, and full support for the **64DD Expansion Kit** — Course Edit and the DD
cups included.

The repository ships **no** Nintendo ROM, disk, IPL, or extracted game assets. You bring your own
legally obtained dumps; game data is extracted or loaded locally from the files you supply.


## Features

- **True widescreen** — the game renders in 16:9 (`gEnhancements.Graphics.Widescreen`), with an
  optional **widescreen-anchored HUD** so on-screen elements sit at the screen edges instead of
  being stretched from 4:3 (`gEnhancements.Graphics.WidescreenUI`).
- **In-game enhancement menu** — a full ImGui menu opened with **F1** (also Escape or Gamepad
  Back). Graphics, audio, gameplay, practice, ghosts and workshop tabs, keyboard- and
  controller-navigable.
- **64DD Expansion Kit** — loads the translated EK disk image and the 64DD IPL ROM to unlock the
  **Course Edit** track editor and the DD cups.
- **Ghost library** — import/export ghost replays as `.gdg` files, a per-course host-side ghost
  library, and a **Ghost Browser** window, all alongside the game's own SRAM ghost slot.
- **Photo mode** — hide the HUD while paused in a race for clean captures
  (`gEnhancements.Practice.PhotoMode`).
- **Practice lap deltas** — your last lap versus your session best, drawn in Practice mode
  (`gEnhancements.Practice.ShowLapDeltas`).
- **Texture-pack modding** — drop `.o2r` packs in a `mods/` folder to override textures
  (`gEnhancements.Workshop.TexturePacks`), with hot reload from the menu.
- **Texture dumping** — dump every decoded texture while you play to build your own packs
  (`gEnhancements.Workshop.TextureDump`).
- **Durable 64DD save sidecar** — Course Edit saves and disk writes are journaled to a sidecar
  file next to the game; your original `.ndd` disk image is **never** modified.
- **Modern rendering knobs** — internal resolution scale, MSAA, texture filtering, VSync,
  z-fighting mitigation, draw distance, and frame pacing.
- **Input** — keyboard and SDL controller support (including DualSense), plus a draggable
  on-screen **N64 input viewer** overlay.
- **Developer tools** — live Stats, an optional top-right FPS/frame-time overlay, a command
  Console, and the Fast3D graphics debugger under **Dev Tools**.

## Quick Start

> **You must provide your own legally-obtained F-Zero X ROM. G-Diffuser does not, and will
> never, distribute copyrighted game files.**

1. **Get the game files you own.** G-Diffuser needs an F-Zero X **US rev0** ROM dump, named
   `baserom.us.rev0.z64` (the loader also accepts `fzerox.z64` / `f-zero-x.z64` as alternate
   names). The dump must be **big-endian (`.z64`) byte order** — the loader does not byte-swap,
   so a `.n64` or `.v64` dump must be converted to `.z64` first or it will be rejected.
2. **Download** a G-Diffuser release for your platform, or build it yourself (see
   [Building](#building)).
3. **Launch** the executable. On first boot the setup screen detects files already beside the
   executable and lets you review, replace, or choose the ROM, Expansion Kit disk image, and 64DD
   IPL ROM. Extraction begins only after you select **Build game data and continue**. Everything
   remains beside the executable so later launches boot directly into the game.
4. **Play.** Press **F1** at any time to open the enhancement menu.

### Expansion Kit files (required)

G-Diffuser is a port of the full Expansion Kit experience — Course Edit, the DD cups, and the
disk save system are core features, not add-ons — so these two files are required alongside the
ROM:

| File | What it is |
| --- | --- |
| `baserom.translated.ek.ndd` | The fan-translated Expansion Kit disk image (English translation by Zoinkity, adapted to the 64DD disk image by LuigiBlood). The loader also accepts `baserom.jp.ek.ndd` for the original Japanese disk. A retail 64DD image is ~64.9 MB. |
| `N64DDIPLROM.n64` | A 64DD IPL / drive ROM dump (~4 MB). Supplies the drive's built-in font. |

Place them next to your ROM (or feed them to the first-boot wizard). Setup will not complete
without them.

## Controls

Open or close the enhancement menu with **F1**, **Escape**, or **Gamepad Back**. Toggle
fullscreen with **F11**. Menu navigation from a controller is opt-in via the menu's
controller-navigation toggle.

## Platforms

| Platform | Renderer |
| --- | --- |
| **Windows** | Direct3D 11 |
| **Linux** | OpenGL (SDL2) |

Both targets are driven by libultraship's Fast3D renderer.

## Custom Assets

The release includes `gdiffuser.o2r`, which contains only the MIT-licensed Fast3D shaders needed
to initialize the renderer. On first boot, `gdx-extract` uses your ROM and the CC0-licensed
`decomp-recipes/` metadata to generate your local `fzerox.o2r`. That generated archive contains
game-derived data and must not be redistributed with G-Diffuser.

To mod textures, place additional `.o2r` packs in a `mods/` folder next to the game; enable
**texture packs** in the Workshop tab and reload. You can build packs from the game's own textures
using the built-in **texture dump** feature. A numeric filename prefix (for example,
`10-hifonts.o2r`) controls load order.

## Building

G-Diffuser is a CMake project. It pulls in three components: `libultraship/`
(Kenix3/libultraship — runtime + Fast3D renderer), `torch/` (HarbourMasters/Torch — build-time
asset extraction), and `decomp/` (inspectredc/fzerox — the F-Zero X C source). Clone with
submodules:

```sh
git clone --recursive https://github.com/Zorkats/G-Diffuser.git
```

### Windows

- **Visual Studio 2022** (MSVC toolset) with the C++ workload
- **CMake** and **Ninja**
- **vcpkg** for libultraship's C++ dependencies

> Build from a clean shell **without** MSYS2/MinGW on your `PATH`, or MSVC may pick up MinGW
> headers. Use a Developer Command Prompt (`vcvars64`).

```sh
# Install libultraship's dependencies via vcpkg
vcpkg install zlib bzip2 sdl2 glew libzip nlohmann-json tinyxml2 spdlog fmt --triplet x64-windows

# Configure + build
cmake -G Ninja -S . -B build/x64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build/x64
```

### Linux

Install the toolchain and libultraship's dependencies from your distribution (CMake, Ninja, a
C++20 compiler, SDL2, GLEW, zlib, bzip2, libzip, nlohmann-json, tinyxml2, spdlog, fmt), then:

```sh
cmake -G Ninja -S . -B build/x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build/x64
```

### Expansion Kit build

Expansion Kit support is a build option (`GDX_EXPANSION_KIT`). Enable it when you want the disk
loader, Course Edit, and DD cups compiled in.

## FAQ

**Do I need an emulator?** No. G-Diffuser is native code; there is no emulator at runtime.

**Where do I get the ROM / disk / IPL?** You must dump them yourself from hardware you own. This
project does not condone piracy and will not point you to copyrighted files.

**Which ROM region?** F-Zero X **US rev0**. Use a `.z64` (big-endian) dump. The loader does not
byte-swap — `.n64` / `.v64` images must be converted to big-endian `.z64` first, or they'll be
rejected at load time.

**Will my disk file get corrupted?** No. Disk writes go to a separate save sidecar; your original
`.ndd` image is never modified.

**Is the `docs/` folder part of the release?** No. `docs/` holds internal design notes and
investigation material used during development; it is not required to build, run, or use
G-Diffuser and is not part of the release.

## Troubleshooting

**The game runs too fast.** On a high-refresh display (e.g. a 120/144 Hz laptop or handheld) where
vsync isn't capping the frame rate, the game can run faster than 60 FPS. Open the enhancement menu
(**F1**), go to the graphics settings, and enable the **Frame Pacer** — it holds the game to its
intended 60 FPS. The setting persists across launches.

**Diagnostic logging.** Log output is opt-in. Set the environment variable `GDX_LOG=1` before
launching to write a `gdiffuser-run.log` file next to the executable; without it, no run log is
created.

## Credits

G-Diffuser stands entirely on the shoulders of the people who did the hard foundational work.

- **[inspectredc](https://github.com/inspectredc) and the [fzerox decompilation](https://github.com/inspectredc/fzerox) contributors** — without their matching decompilation of F-Zero X, none of this would exist. This port is built directly on their source, and I am deeply grateful to them and their team. F-Zero X is one of the most complex N64 games to reverse-engineer, and their work is the whole reason G-Diffuser can run at all.
- **Zoinkity** — for the original F-Zero X Expansion Kit English translation.
- **[LuigiBlood](https://github.com/LuigiBlood)** — for adapting and improving that translation into the 64DD disk image G-Diffuser loads, and for the extensive 64DD research and preservation work that makes the Course Edit / DD-cup support possible.
- **[Kenix3](https://github.com/Kenix3) and the [libultraship](https://github.com/Kenix3/libultraship) project** — the runtime and Fast3D renderer G-Diffuser is built on.
- **The [HarbourMasters](https://github.com/HarbourMasters) community** — [Ship of Harkinian](https://github.com/HarbourMasters/Shipwright) and [Starship](https://github.com/HarbourMasters/Starship) were both the inspiration for this project and the source of much shared technology and know-how.
- **The wider N64 decompilation and modding community** — for the tools, documentation, and years of accumulated knowledge that make projects like this achievable.

## License and legal

Original G-Diffuser code and documentation are available under the [MIT License](LICENSE).
Submodules, vendored components, tools, and fonts retain their own licenses; binary distributions
include their complete texts under `LICENSES/`. See [Third-party notices](THIRD_PARTY_NOTICES.md)
for the component and asset boundary.

This project is an unofficial compatibility project and is not affiliated with, endorsed by,
sponsored by, or associated with Nintendo. F-Zero and Nintendo are trademarks of Nintendo.
G-Diffuser distributions contain no Nintendo ROM, disk, IPL, texture, model, audio, or other game
payload. Users must supply legally obtained dumps. The locally generated `fzerox.o2r` and any
texture dumps remain game-derived and must not be distributed with the project.
