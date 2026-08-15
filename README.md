<div align="center">

<img src="assets/branding/gdiffuser-logo.png" alt="G-Diffuser" width="640">

**A native PC port of F-Zero X (N64), including 64DD Expansion Kit support, now on Nintendo Switch!**

[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

</div>

---
NX-Diffuser is a port of [G-Diffuser](https://github.com/Zorkats/G-Diffuser) to the Nintendo Switch.
G-Diffuser itself is a fully decompiled source port built on top of the [inspectredc/fzerox](https://github.com/inspectredc/fzerox)
matching decompilation and the [Kenix3/libultraship](https://github.com/Kenix3/libultraship)
runtime.

You must provide your own copies of the U.S. Rev. 1.0. ROM, the N64DD BIOS, and F-Zero X EK Data.

## Features

Currently, all features are the same as upstream v1.0.1, with four exceptions

1. fzerox.o2r must be built on your PC and copied to your SD card.
2. Asset dumping only works on PC.
3. Discord Rich Presence is disabled.
4. An in-game updater is now present for updates that don't require a new .o2r to be generated.

Everything else should be the same.

## Quick Start

1. G-Diffuser needs an F-Zero X US rev0 ROM dump, N64DD IPL (either US or JP), and F-Zero X EK data.
   The ROM must be in .z64 format and named `baserom.us.rev0.z64`. The EK data should be in a .ndd format
   and named `baserom.jp.ek.ndd`. Optionally, you can get the English fan-translation and name the patched
   file `baserom.translated.ek.ndd`. The N64DD IPL should be named either `N64DDIPLROM.n64` for the JPN IPL
   or `64DD_IPL_US_MJR.n64` for the US IPL. Both are compatible across either the US or JPN EK data.
2. Download a G-Diffuser release for your platform
3. Run the executable. On first boot the setup screen detects files already beside the
   executable and lets you review, replace, or choose the ROM, Expansion Kit disk image, and 64DD
   IPL ROM. Extraction begins only after you select 'Build game data and continue'.
4. Download the corresponding NX-Diffuser release zip from [releases](https://github.com/PalindromicBreadLoaf/NX-Diffuser/releases)
5. Extract everything from the release zip to /switch/G-Diffuser on your sd card. Also copy `fzerox.o2r`, `fzerox-disk.o2r`, and 
   `n64ddipl.o2r` to the same folder.

After this, you should have a directory looking like so:
```
sdmc:/switch/G-Diffuser/
├── G-Diffuser.nro          from release zip
├── gdiffuser.o2r           from release zip
├── fzerox.o2r              from PC install
├── fzerox-disk.o2r         from PC install
├── n64ddipl.o2r            from PC install
├── gamecontrollerdb.txt    from release zip
├── fonts/                  from release zip
└── mods/                   optional texture packs
```

Saves, ghosts, the log and `gdiffuser.cfg.json` are written to this directory on first start.

### Updating

Settings -> Updates checks GitHub releases for new updates, and if one is found, installs it on the
next start. However, since .o2r files cannot be made on Switch, you'll have to remake those on any 
update that requires new .o2r files be created.

The menu can be accessed via `-`.

## Building

```sh
git clone --recursive https://github.com/PalindromicBreadLoaf/NX-Diffuser.git
```

Python 3 is required, with PyYAML and Pillow. Also, building requires [devkitPro](https://devkitpro.org/wiki/Getting_Started)
with the `switch-dev` group.

```sh
cmake -S . -B build/switch -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/switch
```

## Reporting a bug

Open an issue on the [issues tracker](https://github.com/PalindromicBreadLoaf/NX-Diffuser/issues).

Please attach your log that can be found next to your .nro on the SD card.

## Credits

NX-Diffuser wouldn't be possible without these other projects:

- **[G-Diffuser](https://github.com/Zorkats/G-Diffuser)** - the initial port of this project that allowed for this one to even exist in the first place. Please go give them some love if you like this project.
- **[inspectredc](https://github.com/inspectredc) and the [fzerox decompilation](https://github.com/inspectredc/fzerox) contributors** — without their matching decompilation of F-Zero X, none of this would exist. This port is built directly on their source, and I am deeply grateful to them and their team. F-Zero X is one of the most complex N64 games to reverse-engineer, and their work is the whole reason G-Diffuser can run at all.
- **Zoinkity** — for the original F-Zero X Expansion Kit English translation.
- **[LuigiBlood](https://github.com/LuigiBlood)** — for adapting and improving that translation into the 64DD disk image G-Diffuser loads, and for the extensive 64DD research and preservation work that makes the Course Edit / DD-cup support possible.
- **[Kenix3](https://github.com/Kenix3) and the [libultraship](https://github.com/Kenix3/libultraship) project** — the runtime and Fast3D renderer G-Diffuser is built on.
- **The [HarbourMasters](https://github.com/HarbourMasters) community** — [Ship of Harkinian](https://github.com/HarbourMasters/Shipwright) and [Starship](https://github.com/HarbourMasters/Starship) were both the inspiration for this project and the source of much shared technology and know-how.
- **[Kiziio](https://github.com/Kiziio1)** — for the G-Diffuser logo and icon artwork.
- **The wider N64 decompilation and modding community** — for the tools, documentation, and years of accumulated knowledge that make projects like this achievable.

## License and legal

G-Diffuser (and by extension NX-Diffuser) code and documentation are available under the [MIT License](LICENSE).
Submodules, vendored components, tools, and fonts retain their own licenses; binary distributions
include their complete texts under `LICENSES/`. See [Third-party notices](THIRD_PARTY_NOTICES.md)
for the component and asset boundary.

This project is an unofficial compatibility project and is not affiliated with, endorsed by,
sponsored by, or associated with Nintendo. F-Zero and Nintendo are trademarks of Nintendo.
G-Diffuser distributions contain no Nintendo IP.
