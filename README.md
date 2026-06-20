# G-Diffuser

A native PC source port of **F-Zero X** (N64) and its **Expansion Kit** (64DD), built on the
[`inspectredc/fzerox`](https://github.com/inspectredc/fzerox) matching decompilation and the
[`Kenix3/libultraship`](https://github.com/Kenix3/libultraship) runtime.

No emulator at runtime. No copyrighted assets in the repository — everything is extracted at build
time from a dump you supply from your own copy of the game.

> **Status:** Pre-alpha scaffold.

## Targets (v1)

- Windows
- Linux

## Repository layout

```
G-Diffuser/
├── port/            Game-specific glue (context, resource factories, OS shims, entry point)
├── libultraship/    submodule → Kenix3/libultraship   (runtime + Fast3D renderer)
├── torch/           submodule → HarbourMasters/Torch    (build-time asset extraction)
├── decomp/          submodule → inspectredc/fzerox      (the F-Zero X C source)
├── assets/yamls/    Torch extraction configs
└── CMakeLists.txt   Top-level build
```

## Building (Windows)

> M0 status: libultraship and Torch build; assets extract. The bootable executable
> (`port/`) is in progress (M1).

### Prerequisites

- Visual Studio 2022/2026 (MSVC toolset), CMake ≥ 3.20, Ninja
- vcpkg (for libultraship's C++ dependencies)
- A legally obtained F-Zero X ROM dump (US rev 1.0)

> Build from a clean shell **without** MSYS2/MinGW on `PATH`, or MSVC may pick up MinGW
> headers. Run the commands from a Developer Command Prompt (`vcvars64`).

### Steps

```sh
# 1. Clone with submodules
git clone --recursive https://github.com/Zorkats/G-Diffuser.git

# 2. Install libultraship's dependencies via vcpkg
vcpkg install zlib bzip2 sdl2 glew libzip nlohmann-json tinyxml2 spdlog --triplet x64-windows

# 3. Configure + build (libultraship)
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-windows -S . -B build/x64
cmake --build build/x64

# 4. Build the Torch asset tool
cmake -G Ninja -S torch -B torch/build/x64
cmake --build torch/build/x64

# 5. Extract assets from your ROM into an .o2r archive
torch/build/x64/torch o2r <your_rom.z64> -s decomp -d assets/extracted -u 1.0.0
```

## Legal

This project contains **no** Nintendo assets or code. A legally obtained copy of the game is
required to extract the assets at build time. The decompilation is licensed CC0.
