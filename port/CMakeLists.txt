# port/ — game glue + decomp game sources compiled for the host.
# Standalone-capable so the decomp-compile (Slice 4a) can iterate without rebuilding libultraship.
cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED PROJECT_NAME)
    project(gdiffuser_port LANGUAGES C CXX)
endif()

set(DECOMP "${CMAKE_CURRENT_SOURCE_DIR}/../decomp")

# Decomp game C compiled for PC. Exclude:
#   libultra/* (libultraship provides it) EXCEPT gu/ (matrix/gfx math),
#   rsp/* (RSP microcode .s -> Fast3D), leo/* (64DD -> later slice).
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS
    "${DECOMP}/src/game/*.c"
    "${DECOMP}/src/sys/*.c"
    "${DECOMP}/src/overlays/*.c"
    "${DECOMP}/src/audio/*.c"
    "${DECOMP}/src/framebuffers/*.c"
    "${DECOMP}/src/libultra/gu/*.c"
)

# US base first: exclude Expansion Kit / 64DD disk audio (folded in with the EK slice).
list(FILTER GAME_SOURCES EXCLUDE REGEX "/audio/disk/")

add_library(gdiffuser_game OBJECT ${GAME_SOURCES})

target_include_directories(gdiffuser_game PRIVATE
    "${DECOMP}"            # decomp root: the C uses includes relative to it (e.g. "src/audio/...")
    "${DECOMP}/include"
    "${DECOMP}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"   # Torch-generated asset headers (assets/us/rev0/*.h)
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
