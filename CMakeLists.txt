# G-Diffuser — F-Zero X PC source port
# Top-level build (scaffold — fleshed out in milestone M0).
cmake_minimum_required(VERSION 3.20)

project(GDiffuser
    LANGUAGES C CXX
    DESCRIPTION "F-Zero X PC source port"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# --- Submodule presence check -------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libultraship/CMakeLists.txt")
    message(FATAL_ERROR
        "Submodules not initialized. Run:\n"
        "  git submodule update --init --recursive")
endif()

# --- Components ---------------------------------------------------------------
add_subdirectory(libultraship)     # runtime + Fast3D renderer
# add_subdirectory(port)           # game glue + decomp sources (Slice 4)
#
# Asset extraction (Torch) is driven as a build step against the user-supplied ROM.

message(STATUS "G-Diffuser: libultraship wired via add_subdirectory.")
