cmake_minimum_required(VERSION 3.10.0)
project(GitTrace VERSION 0.1.0 LANGUAGES C CXX)

find_package(Git QUIET)
set(GITTRACE_BUILD_ID "${PROJECT_VERSION}+unknown")
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE GITTRACE_GIT_REV
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=normal
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE GITTRACE_GIT_STATUS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(GITTRACE_GIT_REV)
        set(GITTRACE_BUILD_ID "${PROJECT_VERSION}+${GITTRACE_GIT_REV}")
        if(GITTRACE_GIT_STATUS)
            string(APPEND GITTRACE_BUILD_ID "-dirty")
        endif()
    endif()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# libgit2 must be installed with its CMake package config. Set this prefix
# only when it is outside the system's normal package search paths.
set(LIBGIT2_ROOT "" CACHE PATH
    "libgit2 installation prefix")
if(LIBGIT2_ROOT)
    list(PREPEND CMAKE_PREFIX_PATH "${LIBGIT2_ROOT}")
endif()

# Enable compile_commands.json generation for VS Code
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Load the libgit2 configuration and imported targets
find_package(libgit2 CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(TREE_SITTER REQUIRED IMPORTED_TARGET tree-sitter)
pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)

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
    GitTrace/chunkdb/retrieval.cpp
    GitTrace/chunkdb/index_store.cpp
    GitTrace/gittrace.cpp
    GitTrace/utils/treesitter.cpp
)
target_compile_definitions(GitTrace PRIVATE GITTRACE_BUILD_ID="${GITTRACE_BUILD_ID}")
target_include_directories(GitTrace PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/llama.cpp/vendor
)
target_link_libraries(GitTrace PRIVATE
    libgit2::libgit2package
    llama
    PkgConfig::TREE_SITTER
    PkgConfig::LIBCURL
    PkgConfig::SQLITE3
    ${CMAKE_DL_LIBS}
)

add_executable(GitTraceRetrievalTest
    test.cpp
    GitTrace/chunkdb/chunkdb.cpp
    GitTrace/chunkdb/retrieval.cpp
    GitTrace/chunkdb/index_store.cpp
    GitTrace/gittrace.cpp
    GitTrace/utils/treesitter.cpp
)
target_link_libraries(GitTraceRetrievalTest PRIVATE
    libgit2::libgit2package
    llama
    PkgConfig::TREE_SITTER
    PkgConfig::SQLITE3
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
