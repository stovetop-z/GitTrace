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
find_package(PkgConfig REQUIRED)
pkg_check_modules(TREE_SITTER REQUIRED IMPORTED_TARGET tree-sitter)
pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)

set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_APP OFF CACHE BOOL "" FORCE)
add_subdirectory(llama.cpp)

add_executable(GitTrace
    main.cc
    GitTrace/laya_client.cpp
    GitTrace/chunkdb/chunkdb.cpp
    GitTrace/gittrace.cpp
    GitTrace/utils/treesitter.cpp
)
target_include_directories(GitTrace PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/llama.cpp/vendor
)
target_link_libraries(GitTrace PRIVATE
    libgit2::libgit2package
    llama
    PkgConfig::TREE_SITTER
    PkgConfig::LIBCURL
    ${CMAKE_DL_LIBS}
)

add_executable(GitTraceRetrievalTest
    test.cpp
    GitTrace/chunkdb/chunkdb.cpp
    GitTrace/gittrace.cpp
    GitTrace/utils/treesitter.cpp
)
target_link_libraries(GitTraceRetrievalTest PRIVATE
    libgit2::libgit2package
    llama
    PkgConfig::TREE_SITTER
    ${CMAKE_DL_LIBS}
)

include(CTest)
enable_testing()
add_test(NAME GitTraceRetrievalTest COMMAND GitTraceRetrievalTest)
set_tests_properties(GitTraceRetrievalTest PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME GitTraceCliTest
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cli.py
                $<TARGET_FILE:GitTrace> ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(GitTraceCliTest PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
