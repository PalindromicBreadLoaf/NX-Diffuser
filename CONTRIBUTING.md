# Contributing to G-Diffuser

Thanks for wanting to help. G-Diffuser is a native PC port of F-Zero X built on the
[inspectredc/fzerox](https://github.com/inspectredc/fzerox) matching decompilation and
[libultraship](https://github.com/Kenix3/libultraship).

Two things about this project are unusual, and both will cost you time if you find them out the hard
way:

- **`decomp/` is a matching decompilation.** Your change has to work on PC *and* leave the retail
  build byte-identical. The PC build cannot tell you when you have broken the second one.
- **There is no CI on this repository.** Your local build is the only check that runs. (`decomp/`
  has its own CI, and it checks something different.)

Read [Things that will bite you](#things-that-will-bite-you) before you touch `decomp/`.

## No game files. Ever.

**Never put a ROM, disk image, IPL dump, extracted asset, or generated `fzerox.o2r` in the
repository, in an issue, or in a pull request.** Not as an attachment, not in a test fixture, not
base64'd in a comment. This is not negotiable and there is no exception for "just a small texture".

Contributors supply their own legally obtained dumps. If a bug can only be shown with game data,
describe it, attach a log, and say which file reproduces it — do not attach the file.

## Building

Full prerequisites and commands are in the [README](README.md#building). The short version:

| | |
| --- | --- |
| **Clone** | `git clone --recursive` — the build needs the submodules; a plain clone will not configure |
| **Windows** | Visual Studio 2022, CMake 3.20+, Python 3 |
| **Linux** | CMake 3.20+, Ninja, a C++20 compiler, Python 3, and libultraship's dependencies from your distribution |

**Python 3 is required, not optional.** `port/CMakeLists.txt:9` is
`find_package(Python3 COMPONENTS Interpreter REQUIRED)` — configure fails outright without an
interpreter, because build-time generators run during the build.

Two things routinely trip people up on Windows:

- CMake defaults to the **Visual Studio generator**, which is multi-config. It ignores
  `-DCMAKE_BUILD_TYPE`, so you must pass `--config Release` at build time. **Ninja** is
  single-config and works the other way around: `-DCMAKE_BUILD_TYPE` at configure time, and
  `--config` does nothing. Both paths work; the README has the exact commands for each.
- Build from a shell **without** MSYS2/MinGW on `PATH`, or MSVC may pick up MinGW headers.

## Checking your change

Build the game, then build and run the console tests. They need no ROM, no window and no assets,
and each one exits **0** on success. They catch the failure modes specific to this port — pointer
truncation, byte order, DSP maths.

```sh
# Windows (Visual Studio generator)
cmake --build build/x64 --config Release --target gdx_dsp_tests

# Linux (Ninja)
cmake --build build/x64-linux --target gdx_dsp_tests
```

`--target` takes several names at once. `ALL_BUILD` (Visual Studio) or the default target (Ninja)
builds the whole set along with everything else.

| Target | Run it when you touched | Covers |
| --- | --- | --- |
| `gdx_dsp_tests` | Audio | VADPCM, resample, FIR, envelope kernels |
| `gdx_pcm_capture_tests` | Audio capture | Streamed `.pcm` layout and its SHA-256 sidecar |
| `gdx_vi_fallback_tests` | VI / scanout | RGBA5551 → RGBA8888 conversion |
| `gdx_gfx_pack_tests` | Anything touching `Gfx` or the GBI macros | Proves a 64-bit host pointer survives in `Gfx.w1` |
| `gdx_gfx_convert_tests` | Display-list conversion | The binary N64 → wide converter and its cache |
| `gdx_rsp_boot_tests` | LLE audio / the cxd4 RSP | Booting the real audio microcode. Skips itself if the ucode blobs are absent |
| `resource_smoketest` | Resource loading | The `.o2r` pipeline. Only configured when `libultraship` is a target |

Executables land beside `G-Diffuser` — on the Visual Studio generator, `build/x64/port/Release/`.

**If you touched an asset recipe under `decomp/assets/yaml/`,** also run the asset-binding lint. It
reads the recipes, reports colliding symbols, and writes nothing:

```sh
python tools/gen_asset_bindings.py --lint-only
```

A clean run ends with `LINT-ONLY [us/rev0]: 3553 asset symbols scanned, nothing written`. Four
duplicate-offset warnings in `create_machine_textures` are present today and are **not** caused by
your change.

## Things that will bite you

### 1. A change that builds under `PORT` can still break the retail build

Every host-side behavioural change in `decomp/` is wrapped in `#ifdef PORT`, because the non-`PORT`
build must stay byte-identical to the retail ROM. **The port build never compiles the non-`PORT`
path**, so it cannot warn you when you have broken it.

The sharp edge is a **`PORT`-only helper called from ungated code**. It does not exist in the retail
configuration, so the file stops compiling entirely.

The rule: *if a `PORT`-only helper is reachable from ungated code, it must have a paired non-`PORT`
definition that reproduces retail behaviour exactly.* The codebase does this consistently — two live
examples:

```c
/* decomp/include/global.h — the checkpoint macro compiles away entirely in retail */
#define GDX_CK(x)                 /* line 18: retail */
#define GDX_CK(x) gdx_ck(#x)      /* line 40: PORT   */

/* decomp/src/overlays/course_edit/191080.c — translated-disk labels fall back to the JP string */
#define GDX_EK_LABEL(bound, jp) ((bound)[0] != 0 ? (u8*) (bound) : (u8*) (jp))  /* line 32: PORT   */
#define GDX_EK_LABEL(bound, jp) ((u8*) (jp))                                     /* line 115: retail */
```

Whatever the helper yields in its inert state must equal what retail sees unconditionally.

Verifying that matching still holds is a **separate workflow inside `decomp/`**, with its own
Makefile and the MIPS toolchain, checked against committed ROM checksums — see `decomp/README.md`.
Only that workflow can tell you that you have broken matching, and it runs against `decomp/`'s own
committed submodule pointer, so a decomp edit made for the port can pass everything you run locally
and surface later.

### 2. Never blind-regenerate `port/gen/AssetBindings.c` (or `LinkStubs.c`)

These files are generator output **plus** hand-maintained corrections: real array sizes measured at
runtime, endian-fixup ranges trimmed to true command boundaries, symbols the generator cannot infer.
A blind regenerate deletes those silently, still compiles, and corrupts every asset it touched.

`tools/gen_asset_bindings.py` refuses to write to the tracked path for exactly this reason, and the
refusal compares real paths, so pointing `--out` at the tracked file is caught too. Generate to a
scratch path and diff:

```sh
python tools/gen_asset_bindings.py --profile us/rev0 --out /tmp/AssetBindings.fresh.c
diff /tmp/AssetBindings.fresh.c port/gen/AssetBindings.c
```

To show what is at stake — the fixup table for segment 8 today:

| | Rows |
| --- | --- |
| Tracked `AssetBindings.c` | **76** |
| Freshly generated | **63** |

The generator emits one coarse 32-bit-word-swap row per model. The tracked file **splits** thirteen
of those into a word-swap row for the display-list part plus a *vertex* fixup row (16-byte stride)
for the vertex block — same start offset, same total span, correct treatment for each half. A blind
regenerate would word-swap those vertex blocks as plain 32-bit words and corrupt every vertex in
thirteen models. Across the whole table that is 1776 tracked rows versus 1763 generated.

`--force-overwrite` exists and obliges you to re-apply every hand edit yourself.

### 3. Turn the diagnostics on before you guess

**Almost every diagnostic in this port is off by default, including in Release builds.** A log that
looks complete is usually a log with the interesting lines switched off.

| Switch | Effect |
| --- | --- |
| `GDX_LOG=1` | Opens `gdiffuser-run.log` beside the executable. Without it, no run log is created at all. |
| `GDX_TRACE` | Tri-state, and the one gate whose stock value is not off: **on by default in Debug, off in Release.** Set it explicitly. |
| `GDX_DIAG_VERBOSE=1` | Unlocks whole per-frame families at once (`[gfxdiag] [game] [seg] [sched] …`). |

```sh
GDX_LOG=1 GDX_TRACE=1 GDX_DIAG_VERBOSE=1 ./G-Diffuser
```

There is also an **always-on crash sink**: a crash writes `gdiffuser-crash.txt` beside the
executable with no environment variable set, on both Windows and Linux. Attach it if you have one.

The full gate table is `port/gdx_dev_gates.c`, each row with its own description, and it is
surfaced in-game under **F1 → Dev Tools**. For what each switch reveals — and, importantly, which
ones *change* what is rendered rather than merely observing it — read the diagnostics document
(`DIAGNOSTICS.md`, under `docs/`). Do not file a log captured with a behaviour-altering gate on
without saying so.

### 4. An intermittent bug here is often an order-dependent bug

Several asset segments are reused across mode changes, and the loader skips the reload — **and the
byte-order fixups with it** — when the content it wants is already resident. Anything that scribbles
into one of those buffers survives into a later mode, and the corruption surfaces somewhere with no
obvious connection to the code that caused it.

So: reproduce from a **cold start**, and record which screens you passed through. "Only happens
sometimes" often means "only happens if you visit Course Edit first".

### 5. There are two copies of the libultra headers, kept in sync by hand

`decomp/include/PR/` and `libultraship/include/libultraship/libultra/` both declare the N64 OS
types. They are duplicates, not one shared header, so a layout change has to be made **twice**.

They have already diverged: `OSContPad` is four fields in the decomp copy and `0x24` bytes in the
libultraship copy, which added gyro and right-stick fields. That is currently harmless — the decomp
function that would consume it is `#ifndef PORT` at every call site, and the port feeds input
through `port/input_bridge.c` instead. Treat it as a warning about the mechanism: **if you change a
shared OS struct, change both trees.**

## Code style

There is no repository-wide formatter for port code. `decomp/`, `libultraship/` and `torch/` each
carry their own `.clang-format`; `port/` does not.

- **Match the file you are editing.** Every file in `port/` is internally consistent; follow it.
- **In `decomp/`, follow the decomp's conventions**, not the port's — it is a submodule with its own
  standards, and its `.clang-format` applies.
- **Explain the non-obvious in a comment.** This codebase leans heavily on comments that record
  *why* — measured values, rejected alternatives, hardware quirks. `port/CMakeLists.txt` is the
  model. If you worked something out the hard way, write it down where the next person will trip.

## Where to look

| You want to | Read |
| --- | --- |
| Build it, run it, report a bug | [`README.md`](README.md) |
| Find which file owns a subsystem | [`port/README.md`](port/README.md) |
| Turn on logging, or find the switch that shows your bug | the diagnostics document, under `docs/` |

`docs/` also holds a large body of working notes accumulated during development. Read anything there
as **evidence with a date on it**, not as current truth: most files describe the tree as it was the
day they were written, and the analysis usually outlives the status. `docs/` is not tracked in git
and is not part of a release.

---

## Submitting a change

> **⚠️ PLACEHOLDER — TO BE COMPLETED BY THE PROJECT OWNER (@Zorkats).**
>
> The following are project policy, not technical facts, and have not been decided or recorded
> anywhere in this repository. They are deliberately left blank rather than guessed at, because a
> confidently wrong contribution policy is worse than an obviously blank one.
>
> - **Are outside pull requests accepted at all?** (And if so, from what point — now, or after the
>   first public release?)
> - **Branch naming** — is there a required prefix or format?
> - **Commit message convention** — the existing history uses a
>   `type(scope): summary` style (`fix(port):`, `feat(menu):`, `chore:`), but this has never been
>   written down as a rule. Confirm or replace.
> - **Which branch to target**, and whether to rebase or merge.
> - **Review expectations** — who reviews, what gets asked for, expected turnaround.
> - **Community / discussion venue** — is there a Discord or other channel? *Do not assume one
>   exists; none is referenced anywhere in this repository.*
>
> Until this section is filled in, the reliable route is to **open an issue first** at
> [github.com/Zorkats/G-Diffuser/issues](https://github.com/Zorkats/G-Diffuser/issues) and ask
> before writing code.

## Reporting a bug

Bug reports do not need any of the above. The README has the
[reporting checklist](README.md#reporting-a-bug) — platform and renderer, which build, ROM region,
repro steps, `gdiffuser-run.log`, and `gdiffuser-crash.txt` if the game crashed.

And, once more: **no ROMs, no disk images, no IPL dumps, no generated `fzerox.o2r`.**
