# port/ — game glue + decomp game sources compiled for the host.
# Standalone-capable so the decomp-compile (Slice 4a) can iterate without rebuilding libultraship.
cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED PROJECT_NAME)
    project(gdiffuser_port LANGUAGES C CXX)
endif()

set(DECOMP "${CMAKE_CURRENT_SOURCE_DIR}/../decomp")

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

# US base first: exclude Expansion Kit / 64DD code (folded in with the EK slice).
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

# libultraship already provides these libultra functions — exclude to avoid duplicate symbols.
list(FILTER GAME_SOURCES EXCLUDE REGEX "/audio/rom/lib/aisetnextbuf\\.c$")  # osAiSetNextBuffer

# Deferred to port reimplementation (Slice 4c):
#   segment.c / cartridge_offsets.c -> N64 ROM-segment system (linker segment symbols
#     don't exist on host; replaced by libultraship's resource loading).
#   ending_effects.c -> static GBI display list; needs port-aware gbi pointer packing.
list(FILTER GAME_SOURCES EXCLUDE REGEX "/sys/segment\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/sys/cartridge_offsets\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ovl_i2/save\\.c$")        # ROM-DMA ghost loading
list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ending/ending_effects\\.c$")

add_library(gdiffuser_game OBJECT ${GAME_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/decomp_port.c"            # port subsystems (segment/save/pool)
    "${CMAKE_CURRENT_SOURCE_DIR}/n64_sched.c"             # R6: cooperative fiber scheduler
    "${CMAKE_CURRENT_SOURCE_DIR}/n64_vi.c"               # R6: VI bridge (framebuffer + retrace)
    "${CMAKE_CURRENT_SOURCE_DIR}/gen/AssetBindings.c"      # R2: asset symbol defs
    "${CMAKE_CURRENT_SOURCE_DIR}/gen/LinkStubs.c")         # R4/R5 bring-up: placeholder defs to link

target_include_directories(gdiffuser_game PRIVATE
    "${DECOMP}"            # decomp root: the C uses includes relative to it (e.g. "src/audio/...")
    "${DECOMP}/include"
    "${DECOMP}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"   # Torch-generated asset headers (arrays + #defines)
)

target_compile_definitions(gdiffuser_game PRIVATE
    PORT=1
    NON_MATCHING=1
    NON_EQUIVALENT=1
    AVOID_UB=1
    F3DEX_GBI_2=1
    _LANGUAGE_C=1
    VERSION_US=1
    ASSET_VERSION=us       # used by ASSET_HEADER() to build include paths
    ASSET_REVISION=rev0
)

# clang-cl: Slice 4a is about *compiling*. Downgrade the decomp-porting diagnostics that
# clang errors on by default (int<->pointer, implicit decls) so the legacy IDO-era C builds.
# NOTE: -Wint-conversion masks the real 64-bit pointer-truncation risk (decomp stores N64
# pointers in u32 / 32-bit uintptr_t) — must be revisited for M1 runtime correctness.
if(MSVC)
    target_compile_options(gdiffuser_game PRIVATE
        /W0
        -Wno-int-conversion
        -Wno-implicit-function-declaration
        -Wno-incompatible-pointer-types
        -Wno-incompatible-function-pointer-types
    )
endif()

# Slice 4b stage 1: link the game objects + stub main, with NO libultraship, to surface
# the unresolved-symbol set (the shim surface). This will NOT link cleanly yet — that is
# the diagnostic. Real libultraship linking + entry point come next.
add_executable(G-Diffuser
    main.cpp
    shims.c
    AssetLoader.cpp
    resource/ResourceFactories.cpp
    $<TARGET_OBJECTS:gdiffuser_game>)
target_include_directories(G-Diffuser PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"   # for "resource/ResourceFactories.h"
    "${DECOMP}/include"
    "${DECOMP}/src")

# Link libultraship when built from the top-level project (it provides the libultra HLE:
# os* threading/messaging, VI, controller, Fast3D). Remaining unresolved symbols after this
# (ROM-segment symbols, audio microcode) are the port-reimplemented set for Slice 4c.
if(MSVC)
    target_link_options(G-Diffuser PRIVATE /errorlimit:0)  # show ALL unresolved (4c scoping)
endif()
if(TARGET libultraship)
    target_link_libraries(G-Diffuser PRIVATE libultraship)
    target_include_directories(G-Diffuser PRIVATE
        "${CMAKE_SOURCE_DIR}/libultraship/src"
        "${CMAKE_SOURCE_DIR}/libultraship/include"
    )
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
