# port/ — game glue + decomp game sources compiled for the host.
# Standalone-capable so the decomp-compile (Slice 4a) can iterate without rebuilding libultraship.
cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED PROJECT_NAME)
    project(gdiffuser_port LANGUAGES C CXX)
endif()

set(DECOMP "${CMAKE_CURRENT_SOURCE_DIR}/../decomp")
find_package(Python3 COMPONENTS Interpreter REQUIRED)

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
#   ending_effects.c -> static GBI display list; needs port-aware gbi pointer packing.
list(FILTER GAME_SOURCES EXCLUDE REGEX "/sys/segment\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/virtualtophysical\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/physicaltovirtual\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/sys/cartridge_offsets\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ending/ending_effects\\.c$")

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
        "${CMAKE_CURRENT_SOURCE_DIR}/gen/EkLinkStubs.c")   # EK: deferred-subsystem placeholders
    # MFS_VERSION=1 (version B) and BUILD_VERSION=VERSION_J (2.0J libultra API
    # shapes, e.g. 3-arg __osSetHWIntrRoutine) match the decomp Makefile's EK
    # configuration; sethwinterrupt.c and the headers are both gated on it.
    target_compile_definitions(gdiffuser_game PRIVATE EXPANSION_KIT=1 MFS_VERSION=1 BUILD_VERSION=VERSION_J)
endif()

target_include_directories(gdiffuser_game PRIVATE
    "${DECOMP}"            # decomp root: the C uses includes relative to it (e.g. "src/audio/...")
    "${DECOMP}/include"
    "${DECOMP}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"   # Torch-generated asset headers (arrays + #defines)
)

target_compile_definitions(gdiffuser_game PRIVATE
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
if(MSVC)
    target_compile_options(gdiffuser_game PRIVATE /W0 /w14715 /w14716)
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
endif()

# Main executable.
add_executable(G-Diffuser
    main.cpp
    input_bridge.c
    n64_gfx_bridge.cpp
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
    shims.c
    AssetLoader.cpp
    resource/ResourceFactories.cpp
    rom_buffer.cpp
    sram_buffer.cpp
    mio0_wrap.c
    "${CMAKE_CURRENT_SOURCE_DIR}/../torch/lib/libmio0/mio0.c"
    $<TARGET_OBJECTS:gdiffuser_game>)

if(GDX_EXPANSION_KIT)
    # Host-side disk image loader; references gdx_ek_assets_fill from the
    # EK-only EkAssetBindings.c, so it must not build in US-only configs.
    target_sources(G-Diffuser PRIVATE disk_buffer.cpp)
    # shims.c guards its US-only Leo stubs on this define.
    target_compile_definitions(G-Diffuser PRIVATE EXPANSION_KIT=1)
endif()

if(WIN32)
    set_target_properties(G-Diffuser PROPERTIES WIN32_EXECUTABLE TRUE)
    # Graphics wave W4: emit a linker .map so crash RVAs and low32 data
    # addresses in gdiffuser-run.log resolve to symbols without a debugger.
    if(MSVC)
        target_link_options(G-Diffuser PRIVATE "/MAP")
    endif()
endif()

file(GLOB_RECURSE GDIFFUSER_F3D_SHADER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/libultraship/src/fast/shaders/*")
set(GDIFFUSER_F3D_O2R "${CMAKE_CURRENT_BINARY_DIR}/f3d.o2r")
add_custom_command(
    OUTPUT "${GDIFFUSER_F3D_O2R}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_f3d_o2r.py" "${GDIFFUSER_F3D_O2R}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/gen_f3d_o2r.py" ${GDIFFUSER_F3D_SHADER_SOURCES}
    COMMENT "Generating Fast3D shader archive f3d.o2r"
    VERBATIM)
add_custom_target(gdiffuser_f3d_o2r DEPENDS "${GDIFFUSER_F3D_O2R}")
add_dependencies(G-Diffuser gdiffuser_f3d_o2r)
add_custom_command(TARGET G-Diffuser POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${GDIFFUSER_F3D_O2R}" "$<TARGET_FILE_DIR:G-Diffuser>/f3d.o2r"
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
    target_link_libraries(G-Diffuser PRIVATE user32 Comdlg32)
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
add_executable(gdx_dsp_tests
    tests/dsp_tests.c
    n64_audio_hle.c)
if(MSVC)
    target_compile_options(gdx_dsp_tests PRIVATE /W3)
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
endif()
