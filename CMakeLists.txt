cmake_minimum_required(VERSION 3.10.0)
project(GitTrace VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# libgit2's build/cmake directory contains a package config, but not the
# complete installed export set required by that config. Point CMake at the
# installation prefix instead. Override this when libgit2 is installed
# elsewhere, for example: -DLIBGIT2_ROOT=/path/to/libgit2-install
set(LIBGIT2_ROOT "/Users/stovetop/lib/libgit2-install" CACHE PATH
    "libgit2 installation prefix")
list(PREPEND CMAKE_PREFIX_PATH "${LIBGIT2_ROOT}")

# Enable compile_commands.json generation for VS Code
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Load the libgit2 configuration and imported targets
find_package(libgit2 CONFIG REQUIRED)

set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_APP OFF CACHE BOOL "" FORCE)
add_subdirectory(llama.cpp)

add_executable(GitTrace
    main.cc
    GitTrace/chunkdb/chunkdb.cpp
)
target_link_libraries(GitTrace PRIVATE libgit2::libgit2package llama)

include(CTest)
enable_testing()

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
