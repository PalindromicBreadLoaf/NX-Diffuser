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
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/virtualtophysical\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/libultra/os/physicaltovirtual\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/sys/cartridge_offsets\\.c$")
list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ovl_i2/save\\.c$")        # ROM-DMA ghost loading
list(FILTER GAME_SOURCES EXCLUDE REGEX "/overlays/ending/ending_effects\\.c$")

add_library(gdiffuser_game OBJECT ${GAME_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/decomp_port.c"            # port subsystems (segment/save/pool)
    "${CMAKE_CURRENT_SOURCE_DIR}/n64_sched.c"              # R6: cooperative fiber scheduler
    "${CMAKE_CURRENT_SOURCE_DIR}/n64_vi.c"                # R6: VI bridge (framebuffer + retrace)
    "${DECOMP}/src/libultra/io/devmgr.c"                  # PORT PI manager: host-backed DMA completion
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

# MSVC: suppress warnings from IDO-era C code (int<->pointer truncation, implicit decls).
if(MSVC)
    target_compile_options(gdiffuser_game PRIVATE /W0)
endif()

# Main executable.
add_executable(G-Diffuser
    main.cpp
    input_bridge.c
    n64_gfx_bridge.cpp
    shims.c
    AssetLoader.cpp
    resource/ResourceFactories.cpp
    rom_buffer.cpp
    mio0_wrap.c
    "${CMAKE_CURRENT_SOURCE_DIR}/../torch/lib/libmio0/mio0.c"
    $<TARGET_OBJECTS:gdiffuser_game>)

if(WIN32)
    set_target_properties(G-Diffuser PROPERTIES WIN32_EXECUTABLE TRUE)
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
