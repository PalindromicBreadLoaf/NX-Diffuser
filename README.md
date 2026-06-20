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

## Building

Requires a user-supplied F-Zero X dump. Build instructions land with milestone M0.

## Legal

This project contains **no** Nintendo assets or code. A legally obtained copy of the game is
required to extract the assets at build time. The decompilation is licensed CC0.
