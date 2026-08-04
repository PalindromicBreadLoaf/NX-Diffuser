# port/ — game glue + decomp game sources compiled for the host.
# Standalone-capable so the decomp-compile (Slice 4a) can iterate without rebuilding libultraship.
cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED PROJECT_NAME)
    project(gdiffuser_port LANGUAGES C CXX)
endif()

set(DECOMP "${CMAKE_CURRENT_SOURCE_DIR}/../decomp")
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Warning flag set for the port's own (non-decomp, non-vendored) targets under GCC/Clang. The
# decomp game library and the vendored cxd4 RSP interpreter stay quiet (-w) since they are not
# ours to fix; the port-authored TUs get -Wall minus the unused-parameter noise (many host shims
# intentionally ignore parameters). No -Werror here: a Linux first-configure should surface
# warnings without failing the build.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(GDX_PORT_WARN_FLAGS -Wall -Wno-unused-parameter)
else()
    set(GDX_PORT_WARN_FLAGS "")
endif()

# Decomp game C compiled for PC. Exclude:
#   libultra/* (libultraship provides most) EXCEPT gu/ (matrix/gfx math) and os/ (the real
#   N64 cooperative scheduler — R6 Starship-style: we run the decomp's own thread scheduler
#   and provide only the low-level context switch __osDispatchThread in the port),
#   rsp/* (RSP microcode .s -> Fast3D), leo/* (64DD -> later slice).
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS
    "${DECOMP}/src/game/*.c"
    "${DECOMP}/src/sys/*.c"
    "${DECOMP}/src/overlays/*.c"
    "${DECOMP}/src/audio/*.c"
    "${DECOMP}/src/framebuffers/*.c"
    "${DECOMP}/src/libultra/gu/*.c"
    "${DECOMP}/src/libultra/os/*.c"
)

# R6: time/timer come from libultraship (os_time.cpp, SDL-based) — exclude the decomp's to
# avoid duplicate osGetTime/osSetTime/osSetTimer. We keep the decomp's threading+messaging+events.
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/gettime\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/settime\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/settimer\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/timerintr\\.c$")
# R6 fiber scheduler: our osCreateThread (port/n64_sched.c) captures entry+arg with full 64-bit
# pointers (the decomp's stores them in truncating u32 context fields) — exclude the decomp's.
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/createthread\\.c$")
# R6: the decomp's osInitialize does real N64 hardware I/O (PIF/PI register reads via IO_READ) —
# replaced by a no-op + the init globals in port/n64_sched.c.
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/initialize\\.c$")
# R6: the decomp's osGetMemSize probes RAM by dereferencing N64 bus addresses (K1BASE+size =
# 0xA0400000...) — replaced by a host version returning osMemSize in port/n64_sched.c.
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/getmemsize\\.c$")

# Expansion Kit: compile the EK/64DD sources into the US-based build.
# EXPANSION_KIT activates the decomp's EK hooks WITHOUT VERSION_JP — EK code
# paths are gated by EXPANSION_KIT in shared files; the VERSION_JP sites are
# JP-cart text/font differences deliberately kept on US behavior. EK modes are
# runtime-gated by disk-image presence (port/n64_leo.c).
option(GDX_EXPANSION_KIT "Compile Expansion Kit (64DD) support into the port" ON)

# ── GDX_DEV_TOOLS — compile-time master switch for the behaviour-altering developer gates ──────
# The port's ~60 GDX_* switches are classified into four buckets (port/gdx_dev_gates.h,
# docs/menu/DEVELOPER_TAB.md). Bucket B is the dangerous set: gates that CHANGE RENDERING OR GAME
# BEHAVIOUR rather than only logging (GDX_NO_REVERB, GDX_HLE_FILTER=0, ...).
# When GDX_DEV_TOOLS is absent, every Bucket B gate is a hard-wired 0 inside gdx_dev_gates.c with
# no getenv and no CVar read, and the Dev Tools page omits their whole section. That is safe by
# construction because the gate table normalizes every gate so 0 == stock behaviour.
#
# ON for every configuration except Release. Written as a generator expression so ONE build tree is
# correct under both single-config (Ninja/Makefiles, CMAKE_BUILD_TYPE) and multi-config (Visual
# Studio) generators. GDX_FORCE_DEV_TOOLS keeps them in a Release build for a soak/QA binary.
option(GDX_FORCE_DEV_TOOLS "Keep the behaviour-altering developer gates in Release builds too" OFF)
if(GDX_FORCE_DEV_TOOLS)
    set(GDX_DEV_TOOLS_DEFS "GDX_DEV_TOOLS=1")
else()
    set(GDX_DEV_TOOLS_DEFS "$<$<NOT:$<CONFIG:Release>>:GDX_DEV_TOOLS=1>")
endif()

# ── GDX_ALLOW_JP_INPUTS — accept Japanese-region GAME DATA in this binary ──────────────────────
# NOT the same switch as GDX_BUILD_JP further down: that one builds a SECOND executable compiled
# with VERSION_JP. This one only decides whether the ORDINARY (US) binary is allowed to ingest
# Japanese-region inputs at all.
#
# WHY IT DEFAULTS OFF. The US binary already accepts the Japanese cartridge dump today, on an
# "experimental" raw-ROM path with no asset extraction. OBSERVED (owner, hands-on testing): that
# path produces "a lot of static and blasting sounds". That is an ear-safety hazard, not a cosmetic
# defect, so a plain (release) configure must refuse Japanese inputs outright rather than half-boot
# them. LIKELY CAUSE, not verified: a raw JP boot still reads cartridge data through this build's
# US-profile generated binding tables (ASSET_VERSION=us; see the GDX_BUILD_JP block below on the
# port being US-only in three compile-time layers), so the audio banks/sequences resolve to offsets
# that hold something else entirely in the Japanese image. Diagnosis is JP-release work; the gate
# does not depend on it. JP support is a LATER release; none of the JP code is deleted, only gated.
#
# WHAT THE GATE COVERS (see the PORT notes at each site):
#   * port/gdx_firstboot.cpp  — the setup/validation layer. Refuses the JP cartridge dump (by SHA-1
#                               AND by the ROM header country code, so an unknown JP dump is also
#                               named as Japanese rather than "unrecognised") and the retail
#                               Japanese Expansion Kit disk, with an explanatory message; also
#                               disables the recorded-JP-hash raw-boot fast path.
#   * port/rom_buffer.cpp     — the runtime cartridge loader (CLI arg / picker / FZEROX_ROM / beside
#                               the exe), so no JP ROM can reach the emulator through a back door.
#   * port/disk_buffer.cpp    — the runtime 64DD disk loader: drops the JP disk FILENAMES from the
#                               search list and refuses the retail JP image by CRC-64 identity.
# The 64DD IPL is deliberately NOT gated: the drive firmware is Japanese by existence (the 64DD was
# Japan-only), the JP retail dump is the canonical one every user has, and it carries no game audio.
#
# ON restores today's behaviour exactly. The G-Diffuser-JP target below always sets it — building
# that target is already an explicit Japanese-profile opt-in.
option(GDX_ALLOW_JP_INPUTS
       "Accept Japanese-region game data (JP cartridge ROM, retail JP Expansion Kit disk). DEV ONLY — the JP raw-ROM path has broken audio." OFF)
if(GDX_ALLOW_JP_INPUTS)
    message(STATUS "G-Diffuser: GDX_ALLOW_JP_INPUTS=ON — Japanese ROM/disk inputs are ACCEPTED (experimental; JP audio is known-bad).")
    set(GDX_JP_INPUT_DEFS "GDX_ALLOW_JP_INPUTS=1")
else()
    # Empty LIST (not an empty string element): an unquoted ${GDX_JP_INPUT_DEFS} then contributes
    # no argument at all to target_compile_definitions.
    set(GDX_JP_INPUT_DEFS)
endif()

if(NOT GDX_EXPANSION_KIT)
    # US base only: exclude Expansion Kit / 64DD code.
    # Mirrors the decomp Makefile's EXPANSION_KIT=0 EXCLUSION_FILES list.
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/sys/disk/")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ovl_i2/dd_save\\.c$")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ovl_i2/ovl_i2_data2\\.c$")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ovl_i10/.*187510\\.c$")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/expansion_kit/")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/course_edit/")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/machine_create/")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ead_demo/")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/audio/disk/")
else()
    # Mirror the decomp Makefile's EXPANSION_KIT=1 exclusions: the disk-side
    # subsystems REPLACE their rom-side twins (audio/disk supersedes audio/rom,
    # sys/disk supersedes sys/rom, dd_save.c supersedes save_buffer.c) — they
    # are alternatives, never compiled together.
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/sys/rom/")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/audio/rom/")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ovl_i2/save_buffer\\.c$")
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ovl_i11/")
    # ead_demo (the 64DD attract demo) has broken WIP C bodies upstream
    # (mangled matrix code in ead_demo_engine.c) — exclude until the decomp
    # matures; it is cosmetic-only.
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ead_demo/")
    # libultraship provides the osAi* surface (same reason rom/lib's
    # aisetnextbuf.c is excluded below for the US build).
    list(FILTER GAME_SOURCES EXCLUDE REGEX "/audio/disk/lib/os\\.c$")
    # The leo drive library and MFS filesystem live outside the globbed dirs.
    # Compile only the pure-math leo files (LBA<->byte zone tables); the
    # hardware command/interrupt layer is replaced wholesale by port/n64_leo.c.
    file(GLOB EK_LEO_SOURCES
        "${DECOMP}/src/leo/lib/bytetolba.c"
        "${DECOMP}/src/leo/lib/lbatobyte.c"
        "${DECOMP}/src/leo/lib/leo_tbl.c"
        "${DECOMP}/src/leo/lib/leotranslat.c"
        "${DECOMP}/src/leo/lib/leoutil.c"
        "${DECOMP}/src/leo/mfs/*.c"
    )
    list(APPEND GAME_SOURCES ${EK_LEO_SOURCES})
endif()

# libultraship already provides these libultra functions — exclude to avoid duplicate symbols.
list(FILTER GAME_SOURCES EXCLUDE REGEX "/audio/rom/lib/aisetnextbuf\\.c$")  # osAiSetNextBuffer

# Deferred to port reimplementation (Slice 4c):
#   segment.c / cartridge_offsets.c -> N64 ROM-segment system (linker segment symbols
#     don't exist on host; replaced by libultraship's resource loading).
# ending_effects.c compiles for real as of 2026-07-18: the historical "needs port-aware gbi
# pointer packing" exclusion became stale once the Phase-G1 wide-Gfx ABI landed (static gs*
# macros pack 16-byte commands; the bridge reads EXE statics at the wide stride). Its
# fireworks/podium simulation is the sole producer of gActiveFireworks.
list(FILTER GAME_SOURCES EXCLUDE REGEX "/sys/segment\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/virtualtophysical\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/physicaltovirtual\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/sys/cartridge_offsets\\.c$")

# Save-system slice: ovl_i2/save.c (settings/records/cup-completion/single-slot ghost
# checksums + Sram_Init/Sram_ReadWrite) now compiles for real. It no longer needs
# excluding: Sram_Init/Sram_ReadWrite are PORT-ified inside save.c itself (mirrors the
# Dma_RomCopy #ifdef PORT pattern in sys/dma.c) to read/write the host-backed SRAM
# image in port/sram_buffer.cpp instead of doing N64 PI/DMA hardware access. The one
# ROM-DMA-dependent piece (Save_LoadStaffGhostRecord, which copies staff ghost data
# from the cart ROM via the still-unpopulated EK ROM segment table) stays stubbed
# in-place — separate, not-yet-done slice.

add_library(gdiffuser_game OBJECT ${GAME_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/decomp_port.c"            # port subsystems (segment/save/pool)
    "${CMAKE_CURRENT_SOURCE_DIR}/n64_sched.c"              # R6: cooperative fiber scheduler
    "${CMAKE_CURRENT_SOURCE_DIR}/n64_vi.c"                # R6: VI bridge (framebuffer + retrace)
    "${DECOMP}/src/libultra/io/devmgr.c"                  # PORT PI manager: host-backed DMA completion
    "${CMAKE_CURRENT_SOURCE_DIR}/gen/AssetBindings.c"      # R2: asset symbol defs
    "${CMAKE_CURRENT_SOURCE_DIR}/gen/LinkStubs.c")         # R4/R5 bring-up: placeholder defs to link

if(GDX_EXPANSION_KIT)
    target_sources(gdiffuser_game PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/n64_leo.c"            # EK: 64DD drive backed by disk image
        "${CMAKE_CURRENT_SOURCE_DIR}/gen/EkAssetBindings.c" # EK: disk asset symbol defs + fill
        "${CMAKE_CURRENT_SOURCE_DIR}/gen/EkLinkStubs.c"    # EK: deferred-subsystem placeholders
        "${CMAKE_CURRENT_SOURCE_DIR}/gdx_ek_disk_overrides.c") # EK: post-fill translated-disk label fixups
    # MFS_VERSION=1 (version B) and BUILD_VERSION=VERSION_J (2.0J libultra API
    # shapes, e.g. 3-arg __osSetHWIntrRoutine) match the decomp Makefile's EK
    # configuration; sethwinterrupt.c and the headers are both gated on it.
    target_compile_definitions(gdiffuser_game PRIVATE EXPANSION_KIT=1 MFS_VERSION=1 BUILD_VERSION=VERSION_J)
endif()

# Cooperative-context (fiber) backend for the R6 scheduler (n64_sched.c calls the
# gdx_fiber.h abstraction). Win32 fibers on Windows, POSIX ucontext elsewhere.
# Exactly one backend is compiled per platform.
if(WIN32)
    target_sources(gdiffuser_game PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/gdx_fiber_win32.c")
else()
    target_sources(gdiffuser_game PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/gdx_fiber_ucontext.c")
endif()

target_include_directories(gdiffuser_game PRIVATE
    "${DECOMP}"            # decomp root: the C uses includes relative to it (e.g. "src/audio/...")
    "${DECOMP}/include"
    "${DECOMP}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"   # Torch-generated asset headers (arrays + #defines)
)

target_compile_definitions(gdiffuser_game PRIVATE
    ${GDX_DEV_TOOLS_DEFS}
    PORT=1
    GDIFFUSER_PORT=1
    NON_MATCHING=1
    NON_EQUIVALENT=1
    AVOID_UB=1
    F3DEX_GBI_2=1
    _LANGUAGE_C=1
    VERSION_US=1
    ASSET_VERSION=us       # used by ASSET_HEADER() to build include paths
    ASSET_REVISION=rev0
)

# MSVC: suppress warnings from IDO-era C code (int<->pointer truncation, implicit decls),
# EXCEPT the ones that are undefined behavior on x64 and have caused real crashes:
#   C4715/C4716 — non-void function falls off the end (the MIPS compiler left the
#   return value in $v0 by accident; MSVC returns garbage — AudioHeap_AllocPermanent
#   handed 0x18 to the audio loader as a buffer pointer).
#   C4013 — implicit declaration, assumed to return int. Same family, same cause:
#   a 64-bit pointer comes back in RAX, the implicit-int rule truncates it to EAX,
#   and the compiler emits `cltq` to sign-extend it back. On MIPS that was the
#   native ABI and cost nothing; on x64 the caller writes through a bogus pointer.
#   Course Edit's "Test Course" button crashed exactly this way for want of a
#   Camera_DrawCourseEditTestRun prototype (fault addr 0xFFFFFFFF87D06E90 — a host
#   pointer's low 32 bits, sign-extended).
#
# These MUST be spelled /we (error), not /wN (set-level). /W0 means "emit no
# warnings" and outranks any level assigned by /w1NNNN, so the /w14715 /w14716
# that stood here were dead flags -- the class they claim to keep fatal was never
# actually checked. Verified directly against cl.exe 14.44: with /W0, /w14715 is
# silent on a falls-off-the-end function while /we4715 errors on it. /we is
# independent of the warning level, so it survives /W0.
if(MSVC)
    target_compile_options(gdiffuser_game PRIVATE /W0 /we4715 /we4716 /we4013)
else()
    # GCC/Clang (Linux): the decomp C is IDO-era and noisy, so silence warnings like /W0 --
    # but keep the "non-void function falls off the end" class FATAL. On MSVC that is the
    # /w14715 /w14716 promotion (a real x64 crash source: garbage left in the return register);
    # -Werror=return-type is the GCC/Clang equivalent. Everything else stays quiet.
    # GCC 14+ additionally promotes int-conversion / incompatible-pointer-types /
    # implicit-function-declaration to HARD ERRORS by default, and -w does not downgrade
    # default-errors. The decomp's IDO-era code freely mixes pointers and 32-bit integers
    # (uintptr_t round-trips through u32 fields, N64-address arithmetic) -- authentic decomp
    # behavior that MSVC's /W0 accepted. Downgrade those classes back to (silenced) warnings.
    # -fcommon: the generated asset bindings and the decomp asset TUs both carry tentative
    # definitions of the same symbols; MSVC and pre-GCC-10 merged them (the decomp relies on
    # it). Modern GCC defaults -fno-common and hard-errors the duplicate definitions.
    # -fno-strict-aliasing / -fwrapv are LOAD-BEARING at -O2/-O3, not stylistic. This is authentic
    # IDO-era decomp C: it type-puns constantly (the AudioCmd union written through void** casts in
    # AudioThread_QueueCmd, uintptr_t<->u32 round-trips, N64-address arithmetic) and assumes MIPS
    # two's-complement wraparound. GCC's default -fstrict-aliasing (TBAA) and signed-overflow-is-UB
    # assumptions treat all of that as undefined and MISCOMPILE it at -O3 while leaving -O0 correct.
    # Measured symptom: the Linux Release build synthesized near-silent audio (ai-tap rms 482 vs the
    # Debug build's 2498) because the audio command path was optimized away, while Debug was healthy.
    # -O0 Debug never exposed it. Every serious N64 decomp port disables both. Keep these on ALL
    # build types so a Release build can never diverge from Debug on this class of UB again.
    target_compile_options(gdiffuser_game PRIVATE -w -Werror=return-type -fcommon
        -fno-strict-aliasing -fwrapv
        -Wno-error=int-conversion
        -Wno-error=incompatible-pointer-types
        -Wno-error=implicit-function-declaration)
endif()

# cxd4 RSP interpreter (LLE audio), vendored under rsp/cxd4 (CC0). Built as a static lib
# so its C89 sources compile with isolated (quiet) flags and its module-level globals
# (DRAM/DMEM/CR/conf/run_task/message/...) stay out of the game TUs. gdx_audio_lle.c
# drives it through gdx_rsp_driver.h. NOTE: gdx_rsp_boot_tests compiles its own copy of
# these sources with -DSP_EXECUTE_LOG (instruction hook), so this lib is game-only.
add_library(cxd4_rsp STATIC
    rsp/cxd4/gdx_rsp_driver.c
    rsp/cxd4/su.c
    rsp/cxd4/vu/add.c
    rsp/cxd4/vu/divide.c
    rsp/cxd4/vu/logical.c
    rsp/cxd4/vu/multiply.c
    rsp/cxd4/vu/select.c
    rsp/cxd4/vu/vu.c)
target_include_directories(cxd4_rsp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/rsp/cxd4")
# SP_EXECUTE_LOG enables cxd4's per-instruction hook so the driver's watchdog
# (gdx_rsp_driver.c step_SP_commands) can longjmp out of a runaway/mis-marshalled task
# instead of spinning the audio thread forever.
target_compile_definitions(cxd4_rsp PRIVATE SP_EXECUTE_LOG=1)
if(MSVC)
    target_compile_options(cxd4_rsp PRIVATE /W0)
    target_compile_definitions(cxd4_rsp PRIVATE _CRT_SECURE_NO_WARNINGS=1)
else()
    # Vendored C89 CC0 interpreter: keep quiet like /W0. -fno-strict-aliasing / -fwrapv for the same
    # reason as gdiffuser_game above: this bit-exact RSP core type-puns DMEM/registers and relies on
    # defined integer wraparound in its vector ops; -O3 TBAA/overflow assumptions would miscompile it.
    target_compile_options(cxd4_rsp PRIVATE -w -fno-strict-aliasing -fwrapv)
    target_compile_definitions(cxd4_rsp PRIVATE _CRT_SECURE_NO_WARNINGS=1)
endif()

# Main executable.
add_executable(G-Diffuser
    main.cpp
    gdx_gui.cpp                # Fast3D GUI font setup for the modern full-screen menu.
    gdx_menu.cpp               # In-game ImGui enhancement menu (F1). Pure port-side wiring against
                                # LUS's ImGui + CVar API; uses the libultraship include dirs the
                                # G-Diffuser target already has (see target_include_directories +
                                # target_link_libraries(libultraship) below). This half is the SHELL
                                # (window, sidebar, search, MenuDrawItem) plus the custom blocks.
    gdx_menu_registry.cpp      # The menu's CONTENTS, declared as data against port/ui/MenuTypes.h
                                # (the declarative registry ported from HarbourMasters/Lighthouse,
                                # CC0 1.0). One AddWidget() per control; the shell walks the same
                                # tree to draw AND to search, which is what makes the search box
                                # able to find an individual control instead of only a page.
                                # Separate TU purely for size: it is a long flat declaration with no
                                # logic worth reading alongside the shell.
    gdx_console_log.cpp        # Feeds the port log (and spdlog) into LUS's Console window, which
                                # ships with no log feed of its own.
    ui/UIWidgets.cpp           # CVar-bound ImGui widgets (ported from HarbourMasters/Lighthouse,
                                # CC0 1.0). Folds the read + draw + write + save + tooltip sequence
                                # gdx_menu.cpp used to hand-write for every option into one call.
                                # Consumers include it as "ui/UIWidgets.hpp" -- port/ is already an
                                # include dir, so no target_include_directories change is needed.
                                # gdx_menu.cpp is migrated onto it: 113 call sites, and the
                                # ImGui::SetTooltip count in that file went from 61 to 2.
    enhancements/events/GameEvents.cpp      # Enhancement event bus. Owns the EventIDs (it is the one
                                # TU that defines INIT_EVENT_IDS) and the C fire helpers the
                                # PORT-gated decomp shims call. REQUIRED: decomp/src/game/racer.c
                                # references GameEvents_FireOnBoostStart, so omitting this is an
                                # unresolved-external link failure, not a missing feature.
    enhancements/tuning/BoostDuration.cpp   # Demo enhancement proving the event layer end to end:
                                # rewrites the boost timer from gEnhancements.Tuning.BoostDuration
                                # (0 = stock). Self-registers via a static initializer.
    input_bridge.c
    gdx_input_script.c        # GDX_INPUT_SCRIPT: dev-only deterministic tick-level input playback for
                                # unattended automated testing. No-op (single cached getenv) unless the
                                # env var is set. See its own file header for the script format.
    n64_gfx_bridge.cpp
    gdx_interp.cpp             # R6-P1: matrix frame-interpolation math (guMtxL2F/F2L float lerp),
                                # dual-pool prev/cur resolution, and per-tick snap state. Drives the
                                # bridge's G_MTX scratch-slot indirection. Default-OFF (GDX_INTERP_P1).
    n64_gfx_convert.cpp        # G2: binary N64 (8-byte) -> wide 16-byte boundary
                                # converter + cross-frame cache. Standalone C++
                                # (no bridge/LUS/decomp deps); unit-tested by the
                                # gdx_gfx_convert_tests target.
    gdx_vi_convert.c            # RGBA5551->RGBA8888 conversion for the VI-scanout
                                # fallback (gdx_vi_present_fallback). Standalone C,
                                # unit-tested by the gdx_vi_fallback_tests target.
    n64_audio_hle.c            # software (HLE) audio RSP interpreter -- executes the Acmd
                                # command lists AudioSynth_Update builds (see n64_sched.c's
                                # M_AUDTASK hook); host-side file, standard C, not part of
                                # gdiffuser_game (no decomp headers needed -- see its own
                                # header comment for why).
    gdx_audio_lle.c            # LLE audio dispatch (branch lle-audio): file-toggled seam that
                                # routes M_AUDTASK to the real aspMain ucode on the cxd4 RSP
                                # (cxd4_rsp lib) via a per-tick memory-model bridge, else HLE.
    gdx_audio_thread.cpp       # Phase 3: dedicated SoH-style std::thread that owns per-tick
                                # audio production (replaces the cooperative-fiber-driven path
                                # for silence-hole elimination). See its header comment for the
                                # full cross-thread touchpoint enumeration and design notes.
    gdx_audio_capture.c        # R2 bit-identical PCM gate (C-R2.3): streaming s16-stereo capture off
                                # the audio tap (thread.c:84), armed by GDX_PCM_CAPTURE, writing
                                # <prefix>.pcm + a SHA-256 sidecar. Standalone C, unit-tested by the
                                # gdx_pcm_capture_tests target.
    gdx_frame_pacer.c          # Optional wall-clock host-loop pacer (default OFF): holds the
                                # loop to the N64 NTSC field rate when
                                # gEnhancements.Graphics.FramePacing is enabled. libultraship's
                                # Fast3D backend already paces EndFrame() to 60fps, so this is
                                # opt-in. Standalone C (windows.h + a local CVarGetInteger extern,
                                # same boundary idiom as input_bridge.c).
    shims.c
    AssetLoader.cpp
    resource/ResourceFactories.cpp
    gdx_firstboot.cpp          # First-boot setup routing + portable data-directory resolution. Runs
                                # before libultraship init and defers acquisition to the in-window UI;
                                # all user data remains beside the executable.
    gdx_extract_launch.cpp     # Runtime O2R extraction: validates the user's ROM, runs the gdx-extract
                                # companion, and atomically installs fzerox.o2r into the data dir.
                                # Contract: docs/investigation/2026-07-18/o2r-migration/P0_CONTRACTS.md.
    gdx_dump_launch.cpp        # R8 Step 4b: per-class offline "Dump All" launcher for the Workshop menu.
                                # Spawns tools/gen_dump_all.py once per selected class on a worker thread
                                # (per-class failure isolation); UI reads a shared snapshot. Dev-facing.
    gdx_firstboot_gui.cpp      # In-window ImGui first-time setup flow (replaces the old pre-window
                                # Win32 wizard). Runs after the window/Gui/FileDropMgr exist and before
                                # the game boots: acquires/validates/copies the ROM+EK disk+IPL (Browse
                                # picker + drag-and-drop), drives the async O2R extraction with live
                                # progress, and hot-mounts fzerox.o2r. See gdx_firstboot_gui.{h,cpp}.
    rom_buffer.cpp
    sram_buffer.cpp
    disk_savefile.cpp          # Durable 64DD disk-save sidecar: copy-on-write dirty-LBA journal
                                # next to the exe (saves/<disk>.gdd). Records disk writes and
                                # replays them over a freshly loaded pristine .ndd at boot, so the
                                # user's .ndd is never mutated. Also hosts the D6 format guard.
    gdx_ghost_io.c             # GDG1 import/export plus the per-exact-course host player-ghost
                                # library. Uses the game's checksum/conversion routines and keeps
                                # the vanilla SRAM slot as a compatibility mirror.
    gdx_ghost_window.cpp       # Ghost Browser GuiWindow: table/export UI for every validated player
                                 # ghost in the host library. Registered via AddGuiWindow in main.cpp.
    gdx_input_viewer.cpp       # Draggable N64 input overlay using the state seen by the game.
    gdx_fps_overlay.cpp        # Optional top-right FPS/frame-time overlay using ImGui's live metrics.
    gdx_perf.cpp               # GDX_PERF=1 frame-time telemetry: per-phase spike attribution +
                                # periodic p50/p95/p99 summaries through the port log.
    gdx_dev_gates.c            # Developer-gate table + live cache behind every GDX_* diagnostic /
                                # behaviour switch surfaced in F1 > Dev Tools. Dependency-free C:
                                # the CVar entry points arrive as function pointers from main.cpp,
                                # so the standalone test targets below compile it unchanged.
    gdx_imgui_nav.cpp          # Feeds ImGui menu navigation from the SDL controller directly, so a
                                # raw DualSense (invisible to ImGui's Win32/XInput backend) can open
                                # and drive the menu on every platform. Gated on gControlNav.
    gdx_workshop.cpp           # Workshop W0: texture-pack override shim, decoded-texture dump
                                # (vendored stb_image_write), and hot reload. All CVar-gated OFF.
    mio0_wrap.c
    gdx_segment_source.c       # R1 (C-R1.3): single archive-first byte-source shim behind every
                                # ROM read (segment_blob table -> raw gdx_rom_buffer fallback) with
                                # per-family fallback telemetry. Linked into the exe because it needs
                                # GDiffuser_LoadArchiveFileBytes (AssetLoader.cpp), gdx_rom_buffer
                                # (rom_buffer.cpp), and gdx_lookup_segment_blob (gen/AssetBindings.c).
    "${CMAKE_CURRENT_SOURCE_DIR}/../torch/lib/libmio0/mio0.c"
    $<TARGET_OBJECTS:gdiffuser_game>)

if(GDX_EXPANSION_KIT)
    # Host-side disk image loader; references gdx_ek_assets_fill from the
    # EK-only EkAssetBindings.c, so it must not build in US-only configs.
    # gen/EkTranslatedOverrides.c (EK reconciliation Phase 3/4a): sparse translated-disk geometry
    # override table consumed by disk_buffer.cpp's gdx_disk_finalize; EK-only (references the
    # EK-only machine_create label symbols), so it is gated here alongside disk_buffer.cpp.
    # gdx_ek_strings.c + gen/EkTranslatedStrings.c (EK reconciliation Phase 4b): the same idea
    # applied to TEXT rather than textures -- the translated disk's own English, bound to the
    # overlay string symbols at disk-load time. EK-only for the same reason as the line above:
    # the table names EK overlay .data symbols that do not exist in a US-only build.
    target_sources(G-Diffuser PRIVATE disk_buffer.cpp gen/EkTranslatedOverrides.c
                                      gdx_ek_strings.c gen/EkTranslatedStrings.c)
    # shims.c guards its US-only Leo stubs on this define.
    target_compile_definitions(G-Diffuser PRIVATE EXPANSION_KIT=1)
endif()

target_compile_definitions(G-Diffuser PRIVATE ${GDX_DEV_TOOLS_DEFS} ${GDX_JP_INPUT_DEFS})

if(WIN32)
    set_target_properties(G-Diffuser PROPERTIES WIN32_EXECUTABLE TRUE)
    # Application icon (desktop/Explorer/taskbar + SDL window class). Windows-only: the .rc format
    # is a Win32 resource script; Linux desktop entries carry their icon separately.
    target_sources(G-Diffuser PRIVATE gdiffuser.rc)
    # Graphics wave W4: emit a linker .map so crash RVAs and low32 data
    # addresses in gdiffuser-run.log resolve to symbols without a debugger.
    if(MSVC)
        target_link_options(G-Diffuser PRIVATE "/MAP")
    endif()
endif()

# Remove the former game-derived icon from incremental build directories. Release builds use the
# platform default until original project artwork is supplied.
add_custom_command(TARGET G-Diffuser POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E rm -f "$<TARGET_FILE_DIR:G-Diffuser>/gdiffuser.png"
    VERBATIM)

file(GLOB_RECURSE GDIFFUSER_F3D_SHADER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/libultraship/src/fast/shaders/*")
# The engine archive is named after the port (gdiffuser.o2r): it contains only the MIT-licensed
# Fast3D shaders. The input viewer is drawn with ImGui primitives and contributes no image assets.
set(GDIFFUSER_ENGINE_O2R "${CMAKE_CURRENT_BINARY_DIR}/gdiffuser.o2r")
add_custom_command(
    OUTPUT "${GDIFFUSER_ENGINE_O2R}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_f3d_o2r.py" "${GDIFFUSER_ENGINE_O2R}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/gen_f3d_o2r.py"
            "${CMAKE_SOURCE_DIR}/assets/branding/gdiffuser-logo-menu.png"
            ${GDIFFUSER_F3D_SHADER_SOURCES}
    COMMENT "Generating engine archive gdiffuser.o2r (Fast3D shaders)"
    VERBATIM)
add_custom_target(gdiffuser_engine_o2r DEPENDS "${GDIFFUSER_ENGINE_O2R}")
add_dependencies(G-Diffuser gdiffuser_engine_o2r)
add_custom_command(TARGET G-Diffuser POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${GDIFFUSER_ENGINE_O2R}" "$<TARGET_FILE_DIR:G-Diffuser>/gdiffuser.o2r"
    VERBATIM)

# The modern menu uses the same readable UI/monospace pairing as SoH. Keep the assets loose beside
# the executable so GdxFast3dGui can add them to ImGui's atlas before the renderer uploads it.
set(GDIFFUSER_GUI_FONT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts")
add_custom_command(TARGET G-Diffuser POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:G-Diffuser>/fonts"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${GDIFFUSER_GUI_FONT_DIR}/Montserrat-Regular.ttf"
            "${GDIFFUSER_GUI_FONT_DIR}/Inconsolata-Regular.ttf"
            "${GDIFFUSER_GUI_FONT_DIR}/Montserrat-OFL.txt"
            "${GDIFFUSER_GUI_FONT_DIR}/Inconsolata-OFL.txt"
            "$<TARGET_FILE_DIR:G-Diffuser>/fonts"
    VERBATIM)

# Keep the project license and every bundled/static dependency notice with binary releases.
set(GDIFFUSER_LICENSE_DIR "${CMAKE_SOURCE_DIR}/LICENSES")
add_custom_command(TARGET G-Diffuser POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/LICENSE"
            "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
            "$<TARGET_FILE_DIR:G-Diffuser>"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${GDIFFUSER_LICENSE_DIR}"
            "$<TARGET_FILE_DIR:G-Diffuser>/LICENSES"
    VERBATIM)

# --- gdx-extract: standalone deterministic O2R extractor ----------------------
# First-boot spawns this as a child process to build generic.o2r from the user's ROM
# (contract C2). It is built as an isolated ExternalProject, NOT add_subdirectory'd
# into the game link: Torch pulls its own spdlog/fmt/yaml-cpp/tinyxml2/libgfxd via
# FetchContent, and keeping it out-of-tree prevents any ODR/version clash with
# libultraship's own spdlog/fmt. See docs/investigation/.../W0_TORCH_DEPENDENCIES.md.
#
# Determinism (GDX_DETERMINISTIC=ON): MINIZ_NO_TIME + sorted entry walk + pinned
# deflate level => byte-identical archives for an identical ROM + recipes.
# Trim: BUILD_STORMLIB=OFF (O2R/miniz path only, no StormLib, no libbz2).
# CMP0091=NEW lets Torch's CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded (/MT) take effect
# so the exe is self-contained (matches G-Diffuser, no VC++ redist).
include(ExternalProject)

set(GDX_EXTRACT_SRC_DIR "${CMAKE_SOURCE_DIR}/torch")
set(GDX_EXTRACT_PREFIX  "${CMAKE_BINARY_DIR}/gdx-extract")
set(GDX_EXTRACT_INSTALL "${GDX_EXTRACT_PREFIX}/install")
set(GDX_EXTRACT_EXE     "${GDX_EXTRACT_INSTALL}/bin/gdx-extract${CMAKE_EXECUTABLE_SUFFIX}")

ExternalProject_Add(gdx_extract_ext
    SOURCE_DIR  "${GDX_EXTRACT_SRC_DIR}"
    PREFIX      "${GDX_EXTRACT_PREFIX}"
    INSTALL_DIR "${GDX_EXTRACT_INSTALL}"
    CMAKE_CACHE_ARGS
        -DUSE_STANDALONE:BOOL=ON
        -DBUILD_STORMLIB:BOOL=OFF
        -DGDX_DETERMINISTIC:BOOL=ON
        -DGDX_OUTPUT_NAME:STRING=gdx-extract
        -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
        -DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}
        -DCMAKE_POLICY_DEFAULT_CMP0091:STRING=NEW
    BUILD_COMMAND   "${CMAKE_COMMAND}" --build   <BINARY_DIR> --config $<CONFIG>
    INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR> --config $<CONFIG>
    # BUILD_ALWAYS ON: ExternalProject stamps do not track torch/ source changes, so with OFF a
    # torch edit kept serving a STALE extractor from the install prefix (bit us twice — the game's
    # POST_BUILD deploy silently overwrote a freshly built exe with the day-one binary). The
    # incremental no-change rebuild is a few seconds; correctness wins.
    BUILD_ALWAYS ON
    BUILD_BYPRODUCTS "${GDX_EXTRACT_EXE}")

# Convenience target so `cmake --build ... --target gdx-extract` works directly.
add_custom_target(gdx-extract DEPENDS gdx_extract_ext)

# Ensure the extractor is built (and deployed) whenever the game is built.
add_dependencies(G-Diffuser gdx_extract_ext)

# Deploy the extractor next to the game executable.
add_custom_command(TARGET G-Diffuser POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${GDX_EXTRACT_EXE}"
            "$<TARGET_FILE_DIR:G-Diffuser>/gdx-extract${CMAKE_EXECUTABLE_SUFFIX}"
    VERBATIM)

# Deploy the recipe tree (US rev0 launch combo) as decomp-recipes/ next to the exe.
# Torch resolves `-s <srcdir>/config.yml`, then gAssetPath = <srcdir>/<config path:>
# where the US rev0 key declares `path: assets/yaml/us/rev0`. So the packaged layout
# mirrors the decomp source tree verbatim and config.yml needs NO edits.
# Only us/rev0 is shipped (28 YAML files, ~455 KB); the other revisions' config keys
# are never dereferenced because recipe selection is by the matched ROM SHA1.
#   decomp-recipes/config.yml
#   decomp-recipes/assets/yaml/us/rev0/**
set(GDX_RECIPES_SRC "${CMAKE_SOURCE_DIR}/decomp")
set(GDX_RECIPES_DST "$<TARGET_FILE_DIR:G-Diffuser>/decomp-recipes")
# R8 Step 2: the EK slice manifest (tools/gen_ek_assets.py output) ships beside the recipes so the
# runtime disk step can pass it to `gdx-extract disk -m` and produce the ek/<symbol> per-asset entries.
set(GDX_EK_SLICE_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/gen/ek_slice_manifest.txt")
add_custom_command(TARGET G-Diffuser POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GDX_RECIPES_DST}/assets/yaml/us"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${GDX_RECIPES_SRC}/config.yml"
            "${GDX_RECIPES_DST}/config.yml"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${GDX_RECIPES_SRC}/LICENSE"
            "${GDX_RECIPES_DST}/LICENSE"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${GDX_EK_SLICE_MANIFEST}"
            "${GDX_RECIPES_DST}/ek_slice_manifest.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${GDX_RECIPES_SRC}/assets/yaml/us/rev0"
            "${GDX_RECIPES_DST}/assets/yaml/us/rev0"
    # Native Dump All data files: emit (a) the parsed gSoundFontTable/gSequenceTable + evaluated audio
    # tables FROM the decomp source (audio_tables.c) via the oracle's own parser/evaluator, so the
    # `tables`, `audio` and `midi` classes reconstruct the exact same output on machines without decomp
    # source; and (b) the EK CI4/CI8 TLUT-resolution map (ek_tlut_map.json) FROM the EK recipe tree via
    # the oracle's own ek_recipe_index(), so the `textures` class resolves EK palettes with no
    # --ek-yaml-dir. Same "generate FROM the single source of truth at build time" pattern as the EK
    # slice manifest above.
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_dump_tables_data.py"
            --audio-tables "${GDX_RECIPES_SRC}/src/audio/disk/audio_tables.c"
            --out-dir "${GDX_RECIPES_DST}"
            --ek-yaml-dir "${CMAKE_SOURCE_DIR}/fzerox-expansion-kit/assets/yaml/jp"
    VERBATIM)

target_include_directories(G-Diffuser PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"   # for "resource/ResourceFactories.h"
    "${DECOMP}/include"
    "${DECOMP}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../torch/lib/libmio0"  # S5: mio0.h + utils.h for mio0_wrap.c and mio0.c
)

# cxd4 RSP interpreter for LLE audio (gdx_audio_lle.c -> gdx_rsp_driver.h).
target_link_libraries(G-Diffuser PRIVATE cxd4_rsp)

# Link libultraship when built from the top-level project (it provides the libultra HLE:
# os* threading/messaging, VI, controller, Fast3D).
if(WIN32)
    # Win32-only host deps: user32 (MessageBox) + Comdlg32 (GetOpenFileName ROM picker)
    # + Comctl32 (extraction progress dialog marquee, gdx_extract_launch.cpp).
    target_link_libraries(G-Diffuser PRIVATE user32 Comdlg32 Comctl32)
else()
    # POSIX host deps: pthread (ucontext fiber backend uses pthread_self; audio thread is
    # std::thread) and dl (dladdr in n64_gfx_bridge.cpp's GetMainModuleRange; -ldl is needed on
    # older glibc, harmless on newer where it folds into libc). ucontext itself lives in libc.
    find_package(Threads REQUIRED)
    target_link_libraries(G-Diffuser PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()

# Port-authored warning flags under GCC/Clang (no-op on MSVC; the port keeps its MSVC /W tuning
# in the per-target blocks below).
if(GDX_PORT_WARN_FLAGS)
    target_compile_options(G-Diffuser PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()
# Same UB-hardening as gdiffuser_game/cxd4_rsp: the port glue reinterprets N64 RDRAM through host
# pointers (gdx_rdram + offset), widens 4-byte N64 pointers to host width, and resolves registered
# host ranges by bit-pattern -- all strict-aliasing-sensitive. Disable TBAA and signed-overflow
# assumptions here too so the -O3 Release port glue can't miscompile that the way the audio path did.
if(NOT MSVC)
    target_compile_options(G-Diffuser PRIVATE -fno-strict-aliasing -fwrapv)
    # CRITICAL Linux audio fix (engram audio/linux-silent-binary-poison): the decomp's N64 libultra
    # sinf/cosf were being EXPORTED from the executable's .dynsym (WEAK/DEFAULT) and interposing the
    # sinf/cosf that libSDL3's audio resampler imports -> silent/garbled game audio that also distorted
    # the shared PipeWire sink. This version script localizes sinf/cosf so libSDL3 uses glibc's; the
    # decomp keeps its own via static resolution. See port/gdx_hide_libm.map for the full writeup.
    target_link_options(G-Diffuser PRIVATE
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/gdx_hide_libm.map")
endif()
if(TARGET libultraship)
    target_link_libraries(G-Diffuser PRIVATE libultraship)
    target_include_directories(G-Diffuser PRIVATE
        "${CMAKE_SOURCE_DIR}/libultraship/src"
        "${CMAKE_SOURCE_DIR}/libultraship/include"
    )
endif()

# DSP unit-test harness: standalone console exe compiling n64_audio_hle.c UNMODIFIED alongside
# port/tests/dsp_tests.c, which drives it entirely through its real public entry point
# (gdx_audio_hle_run) with known Acmd command-vector tests (VADPCM decode/loop-restore/nibble-
# unpack, resample unity/pitch-stepping/continuity, 8-tap FIR filter, envelope mixer). No game
# objects, no libultraship, no decomp headers, no file/asset dependencies -- see dsp_tests.c's
# header comment for the harness design (why a black-box approach through the public API was
# chosen over extracting the DSP kernels or #include-ing the .c with a testing guard). Stand-in
# definitions of the two gdx_resolve_*_host_address externs (normally in n64_gfx_bridge.cpp) live
# in dsp_tests.c itself, so n64_gfx_bridge.cpp is not part of this target.
# gdx_dev_gates.c: n64_audio_hle.c reads the developer-gate cache (and port_log.h's log/trace
# gates do too), so the harness links the same dependency-free table the game uses. It is never
# bound to a console here, so every gate keeps its env-seeded value — which is exactly what the
# existing _putenv-based test setup expects.
add_executable(gdx_dsp_tests
    tests/dsp_tests.c
    n64_audio_hle.c
    gdx_dev_gates.c)
if(MSVC)
    target_compile_options(gdx_dsp_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_dsp_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
    target_link_libraries(gdx_dsp_tests PRIVATE m) # sin/floor: libm is separate on UNIX
endif()

# PCM capture unit test (C-R2.3): standalone console exe compiling gdx_audio_capture.c UNMODIFIED
# alongside port/tests/pcm_capture_tests.c, which drives the module's public API (init/arm/feed/
# active/finished/shutdown) plus its test-only reset seam. Verifies streamed .pcm byte layout, the
# GDX_PCM_CAPTURE_FRAMES frame-cap stop, the sidecar SHA-256 (cross-checked against a second,
# independent SHA-256 in the test), the unconfigured no-op path, and run-to-run determinism. No game
# objects, no libultraship, no decomp headers -- runnable directly after a normal build.
add_executable(gdx_pcm_capture_tests
    tests/pcm_capture_tests.c
    gdx_audio_capture.c
    gdx_dev_gates.c)   # port_log.h's log/trace gates read the developer-gate cache
if(MSVC)
    target_compile_options(gdx_pcm_capture_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_pcm_capture_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

# VI-scanout fallback unit test: standalone console exe compiling gdx_vi_convert.c
# UNMODIFIED alongside port/tests/test_vi_fallback.c, which drives the public
# gdx_convert_rgba5551_to_rgba8888 entry point with known RGBA5551 pixel patterns
# (pure/mid channels, alpha bit, multi-pixel buffer). No game objects, no
# libultraship, no decomp headers -- runnable directly after a normal build.
add_executable(gdx_vi_fallback_tests
    tests/test_vi_fallback.c
    gdx_vi_convert.c)
if(MSVC)
    target_compile_options(gdx_vi_fallback_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_vi_fallback_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

# Phase G1 wide-Gfx packing test: standalone console exe compiling ONLY the real
# decomp gbi.h (header-only) with the same defines the game build uses, so the
# gSP*/gDP* macros under test are exactly the ones the game emits. Verifies the
# pointer-carrying w1 word holds a full 64-bit host pointer (no truncation) and
# that a bridge-style reader (w0 @0, w1 @8, 16-byte stride) recovers it. No game
# objects, no libultraship. See tests/gfx_pack_tests.c.
add_executable(gdx_gfx_pack_tests tests/gfx_pack_tests.c)
target_include_directories(gdx_gfx_pack_tests PRIVATE "${DECOMP}/include")
target_compile_definitions(gdx_gfx_pack_tests PRIVATE
    PORT=1
    GDIFFUSER_PORT=1
    F3DEX_GBI_2=1
    _LANGUAGE_C=1
    VERSION_US=1)
if(MSVC)
    target_compile_options(gdx_gfx_pack_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_gfx_pack_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

# Phase G2 binary-DL converter test: standalone console exe compiling the real
# n64_gfx_convert.cpp UNMODIFIED alongside port/tests/gfx_convert_tests.cpp,
# which drives its public API (ConvertList / ClassifyW1 / GfxWideCache) with
# synthetic 8-byte N64 display lists: byteswapped big-endian streams, segmented
# vs physical pointer ops, value ops, nested G_DL, and ENDDL. It verifies the
# wide output word-by-word, the deterministic pointer policy, and the cache's
# same-input-same-buffer + invalidation-drops-it behavior. No game objects, no
# libultraship, no decomp headers -- runnable directly after a normal build.
add_executable(gdx_gfx_convert_tests
    tests/gfx_convert_tests.cpp
    n64_gfx_convert.cpp)
target_include_directories(gdx_gfx_convert_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(gdx_gfx_convert_tests PRIVATE /W3)
elseif(GDX_PORT_WARN_FLAGS)
    target_compile_options(gdx_gfx_convert_tests PRIVATE ${GDX_PORT_WARN_FLAGS})
endif()

# R1 resource smoke test: links libultraship ONLY (no game objects), so it builds and runs
# now to verify the .o2r resource pipeline, independent of the not-yet-linking full game.
if(TARGET libultraship)
    add_executable(resource_smoketest resource_smoketest.cpp resource/ResourceFactories.cpp)
    target_include_directories(resource_smoketest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/libultraship/src"
        "${CMAKE_SOURCE_DIR}/libultraship/include"
    )
    target_link_libraries(resource_smoketest PRIVATE libultraship)
endif()

# LLE audio core boot test (branch lle-audio): standalone console exe compiling the
# vendored cxd4 RSP interpreter (rsp/cxd4/su.c + vu/*.c) with the thin driver shim
# (rsp/cxd4/gdx_rsp_driver.c) and rsp/tests/rsp_boot_test.c. Proves the interpreter
# boots and executes the REAL aspMain audio microcode (rsp/aspmain_text.bin) with the
# correct IMEM BE->LE byte-swap, DMEM big-endian storage, and OSTask boot contract.
# SP_EXECUTE_LOG enables cxd4's per-instruction hook so the test can bound the run and
# inspect the executed stream. No game objects, no libultraship. Skips gracefully if the
# ROM-extracted ucode blobs are absent. See docs/LLE_IMPL_STATE.md.
add_executable(gdx_rsp_boot_tests
    rsp/tests/rsp_boot_test.c
    rsp/cxd4/gdx_rsp_driver.c
    rsp/cxd4/su.c
    rsp/cxd4/vu/add.c
    rsp/cxd4/vu/divide.c
    rsp/cxd4/vu/logical.c
    rsp/cxd4/vu/multiply.c
    rsp/cxd4/vu/select.c
    rsp/cxd4/vu/vu.c)
target_include_directories(gdx_rsp_boot_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/rsp/cxd4")
target_compile_definitions(gdx_rsp_boot_tests PRIVATE
    SP_EXECUTE_LOG=1
    GDX_RSP_TEST_STEP_HOOK=1   # test provides its own step_SP_commands; exclude the driver's watchdog one
    _CRT_SECURE_NO_WARNINGS=1
    "RSP_BLOB_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/rsp\"")
if(MSVC)
    target_compile_options(gdx_rsp_boot_tests PRIVATE /W3)
else()
    # Vendored C89 cxd4 sources dominate this target: keep quiet.
    target_compile_options(gdx_rsp_boot_tests PRIVATE -w)
endif()

# ============================================================================================
# R5 (C-R5.1/C-R5.3): EXPERIMENTAL second binary — G-Diffuser-JP (VERSION_JP profile).
#
# The port is US-only in three independent compile-time layers (generated binding tables, golden
# constants, and 69 VERSION_JP decomp behavior branches), so US+JP cannot share one executable
# without weeks of decomp churn. The frozen decision (QUALITY_PARITY_ROADMAP §R5) is TWO binaries
# from one tree, sharing one data dir and one launcher/wizard, with per-profile archives
# (fzerox.o2r / fzerox-jp.o2r).
#
# This target is guarded behind GDX_BUILD_JP (default OFF) so the DEFAULT build is byte-unchanged —
# the owner flips it ON when the JP ROM lands. It mirrors the US gdiffuser_game / G-Diffuser targets
# but bakes VERSION_JP=1 ASSET_VERSION=jp ASSET_REVISION=rev0 and consumes the JP-profile generated
# sources (gen/AssetBindings.jp.c, gen/LinkStubs.jp.c from tools/gen_*.py --profile jp/rev0).
#
# KNOWN PREREQUISITES (owner-run; these are EXPECTED build findings, not target bugs):
#   * Torch-generated JP asset headers do NOT exist yet: ASSET_HEADER expands to
#     assets/jp/rev0/<x> (macros.h), but only include/assets/us exists. 54 decomp TUs include
#     these — the JP decomp compile fails until Torch codegen for jp/rev0 (+ jp/ek) is run against
#     the owner's JP ROM. This is the #1 JP compile blocker.
#   * The 69 VERSION_JP sites in decomp/src have never been compiled in this port; expect JP-only
#     compile errors (missing JP symbols, ifdef mismatches). Decomp-side VERSION_JP fixes are a
#     follow-up wave item, tracked in the R5-AB report.
#   * The EK gen files (gen/EkAssetBindings.c) are US-EK; a real JP-EK (jp/ek) binding set is a
#     follow-up. The JP target reuses the US-EK files to mirror the EXPANSION_KIT matrix.
#   * gen/LinkStubs.jp.c is a placeholder seed (JP yaml table symbols only) — its function stubs
#     must be regenerated from a JP build's own link log (see the file's banner).
# ============================================================================================
option(GDX_BUILD_JP "Build the EXPERIMENTAL second binary G-Diffuser-JP (VERSION_JP profile)" OFF)

if(GDX_BUILD_JP)
    message(STATUS "G-Diffuser: GDX_BUILD_JP=ON — configuring EXPERIMENTAL VERSION_JP target G-Diffuser-JP.")

    # --- JP decomp game library (mirror of gdiffuser_game with the JP profile) -----------------
    # GAME_SOURCES is the region-agnostic decomp .c list already resolved above (same EK exclusions);
    # only the defines and the generated binding/stub files differ between profiles.
    add_library(gdiffuser_game_jp OBJECT ${GAME_SOURCES}
        "${CMAKE_CURRENT_SOURCE_DIR}/decomp_port.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/n64_sched.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/n64_vi.c"
        "${DECOMP}/src/libultra/io/devmgr.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/gen/AssetBindings.jp.c"   # R5: JP-profile asset symbol defs
        "${CMAKE_CURRENT_SOURCE_DIR}/gen/LinkStubs.jp.c")      # R5: JP-profile placeholder defs

    if(GDX_EXPANSION_KIT)
        target_sources(gdiffuser_game_jp PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/n64_leo.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/gen/EkAssetBindings.c"   # NOTE: US-EK bindings reused (jp/ek TODO)
            "${CMAKE_CURRENT_SOURCE_DIR}/gen/EkLinkStubs.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/gdx_ek_disk_overrides.c")
        target_compile_definitions(gdiffuser_game_jp PRIVATE EXPANSION_KIT=1 MFS_VERSION=1 BUILD_VERSION=VERSION_J)
    endif()

    if(WIN32)
        target_sources(gdiffuser_game_jp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/gdx_fiber_win32.c")
    else()
        target_sources(gdiffuser_game_jp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/gdx_fiber_ucontext.c")
    endif()

    target_include_directories(gdiffuser_game_jp PRIVATE
        "${DECOMP}"
        "${DECOMP}/include"
        "${DECOMP}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/../include")   # Torch asset headers: needs include/assets/jp/rev0 (owner-run)

    # Same UB-hardening + define set as gdiffuser_game, but the JP profile triplet.
    target_compile_definitions(gdiffuser_game_jp PRIVATE
        ${GDX_DEV_TOOLS_DEFS}
        PORT=1
        GDIFFUSER_PORT=1
        NON_MATCHING=1
        NON_EQUIVALENT=1
        AVOID_UB=1
        F3DEX_GBI_2=1
        _LANGUAGE_C=1
        VERSION_JP=1
        ASSET_VERSION=jp
        ASSET_REVISION=rev0
    )
    if(MSVC)
        target_compile_options(gdiffuser_game_jp PRIVATE /W0 /w14715 /w14716)
    else()
        target_compile_options(gdiffuser_game_jp PRIVATE -w -Werror=return-type -fcommon
            -fno-strict-aliasing -fwrapv
            -Wno-error=int-conversion
            -Wno-error=incompatible-pointer-types
            -Wno-error=implicit-function-declaration)
    endif()

    # --- JP executable (mirror of G-Diffuser, linking the JP game objects) ---------------------
    add_executable(G-Diffuser-JP
        main.cpp
        gdx_gui.cpp
        gdx_menu.cpp
        gdx_menu_registry.cpp      # Declarative menu registry contents (see US target note). REQUIRED
                                   # here as well: this target compiles the same gdx_menu.cpp, which
                                   # declares GdxMenu::RegisterMenu / RegisterDisableReasons and
                                   # calls them from InitElement -- omitting it is an unresolved
                                   # external, not a missing feature.
        gdx_console_log.cpp        # Console window log feed (see US target note)
        ui/UIWidgets.cpp           # CVar-bound ImGui widgets (see US target note). Listed here too
                                   # because this target compiles the same gdx_menu.cpp, so the
                                   # migration must not break JP.
        enhancements/events/GameEvents.cpp      # Enhancement event bus (see US target note). REQUIRED
                                   # here as well: this target compiles the same PORT-gated racer.c
                                   # and therefore references the same GameEvents_FireOnBoostStart.
        enhancements/tuning/BoostDuration.cpp   # Demo enhancement (see US target note)
        input_bridge.c
        gdx_input_script.c        # GDX_INPUT_SCRIPT (see US target note)
        n64_gfx_bridge.cpp
        gdx_interp.cpp             # R6-P1: matrix frame-interpolation (see US target note)
        n64_gfx_convert.cpp
        gdx_vi_convert.c
        n64_audio_hle.c
        gdx_audio_lle.c
        gdx_audio_thread.cpp
        gdx_audio_capture.c
        gdx_frame_pacer.c
        shims.c
        AssetLoader.cpp
        resource/ResourceFactories.cpp
        gdx_firstboot.cpp
        gdx_extract_launch.cpp
        gdx_dump_launch.cpp
        gdx_firstboot_gui.cpp
        rom_buffer.cpp
        sram_buffer.cpp
        disk_savefile.cpp
        gdx_ghost_io.c
        gdx_ghost_window.cpp
        gdx_input_viewer.cpp
        gdx_fps_overlay.cpp
        gdx_perf.cpp
        gdx_dev_gates.c            # Developer-gate table + live cache (see the US target note)
        gdx_imgui_nav.cpp
        gdx_workshop.cpp
        mio0_wrap.c
        gdx_segment_source.c
        "${CMAKE_CURRENT_SOURCE_DIR}/../torch/lib/libmio0/mio0.c"
        $<TARGET_OBJECTS:gdiffuser_game_jp>)

    # The port glue (main.cpp, extraction launcher, gfx bridge, etc.) is region-agnostic C/C++ that
    # keys profile behavior off the VERSION_JP define, so the exe TUs must see it too (they include
    # decomp headers whose region branches depend on it).
    # GDX_ALLOW_JP_INPUTS is unconditional here regardless of the option's value: this target IS the
    # Japanese profile, so refusing Japanese game data would leave it with nothing to load. The
    # option only governs the ordinary US binary above.
    target_compile_definitions(G-Diffuser-JP PRIVATE
        ${GDX_DEV_TOOLS_DEFS}
        PORT=1 GDIFFUSER_PORT=1 F3DEX_GBI_2=1 _LANGUAGE_C=1 VERSION_JP=1
        GDX_ALLOW_JP_INPUTS=1
        ASSET_VERSION=jp ASSET_REVISION=rev0)

    if(GDX_EXPANSION_KIT)
        # gen/EkTranslatedOverrides.c, gdx_ek_strings.c: see the matching US-target comment above.
        target_sources(G-Diffuser-JP PRIVATE disk_buffer.cpp gen/EkTranslatedOverrides.c
                                             gdx_ek_strings.c gen/EkTranslatedStrings.c)
        target_compile_definitions(G-Diffuser-JP PRIVATE EXPANSION_KIT=1)
    endif()

    if(WIN32)
        set_target_properties(G-Diffuser-JP PROPERTIES WIN32_EXECUTABLE TRUE)
        target_sources(G-Diffuser-JP PRIVATE gdiffuser.rc)
        if(MSVC)
            target_link_options(G-Diffuser-JP PRIVATE "/MAP")
        endif()
    endif()

    target_include_directories(G-Diffuser-JP PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${DECOMP}/include"
        "${DECOMP}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/../include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../torch/lib/libmio0")

    target_link_libraries(G-Diffuser-JP PRIVATE cxd4_rsp)

    if(WIN32)
        target_link_libraries(G-Diffuser-JP PRIVATE user32 Comdlg32 Comctl32)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(G-Diffuser-JP PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    endif()

    if(GDX_PORT_WARN_FLAGS)
        target_compile_options(G-Diffuser-JP PRIVATE ${GDX_PORT_WARN_FLAGS})
    endif()
    if(NOT MSVC)
        target_compile_options(G-Diffuser-JP PRIVATE -fno-strict-aliasing -fwrapv)
        target_link_options(G-Diffuser-JP PRIVATE
            "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/gdx_hide_libm.map")
    endif()
    if(TARGET libultraship)
        target_link_libraries(G-Diffuser-JP PRIVATE libultraship)
        target_include_directories(G-Diffuser-JP PRIVATE
            "${CMAKE_SOURCE_DIR}/libultraship/src"
            "${CMAKE_SOURCE_DIR}/libultraship/include")
    endif()

    # Deploy the shared engine archive + the extractor + recipe tree (US rev0 AND JP rev0 so the
    # shared extraction/launcher can build either profile's archive from one binary).
    add_dependencies(G-Diffuser-JP gdiffuser_engine_o2r gdx_extract_ext)
    add_custom_command(TARGET G-Diffuser-JP POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${GDIFFUSER_ENGINE_O2R}" "$<TARGET_FILE_DIR:G-Diffuser-JP>/gdiffuser.o2r"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${GDX_EXTRACT_EXE}" "$<TARGET_FILE_DIR:G-Diffuser-JP>/gdx-extract${CMAKE_EXECUTABLE_SUFFIX}"
        VERBATIM)
    add_custom_command(TARGET G-Diffuser-JP POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:G-Diffuser-JP>/decomp-recipes/assets/yaml/us"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:G-Diffuser-JP>/decomp-recipes/assets/yaml/jp"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${GDX_RECIPES_SRC}/config.yml" "$<TARGET_FILE_DIR:G-Diffuser-JP>/decomp-recipes/config.yml"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${GDX_EK_SLICE_MANIFEST}" "$<TARGET_FILE_DIR:G-Diffuser-JP>/decomp-recipes/ek_slice_manifest.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${GDX_RECIPES_SRC}/assets/yaml/us/rev0" "$<TARGET_FILE_DIR:G-Diffuser-JP>/decomp-recipes/assets/yaml/us/rev0"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${GDX_RECIPES_SRC}/assets/yaml/jp/rev0" "$<TARGET_FILE_DIR:G-Diffuser-JP>/decomp-recipes/assets/yaml/jp/rev0"
        VERBATIM)
endif()
