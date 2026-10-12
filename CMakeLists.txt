cmake_minimum_required(VERSION 3.16)
project(ai-tools VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Data directory compiled into the binary (used by the installer; the env
# variable AI_TOOLS_DATA_DIR still overrides it at runtime).
set(AI_TOOLS_DATA_DIR "" CACHE STRING "Default data directory compiled into the binary")
if(AI_TOOLS_DATA_DIR)
    add_compile_definitions(AI_TOOLS_DEFAULT_DATA_DIR="${AI_TOOLS_DATA_DIR}")
endif()

# Compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -O2")
set(CMAKE_CXX_FLAGS_DEBUG "-g -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# Find dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED sqlite3 openssl)

# Tree-sitter runtime + grammars (C, C++, Python) as existing libraries.
# The runtime comes from the system package extracted under third_party/tree-sitter;
# the grammar parser.c files are pre-generated under third_party/grammars.
# No downloads and no tree-sitter CLI generation are required.
set(TS_ROOT ${CMAKE_SOURCE_DIR}/third_party/tree-sitter)
set(TS_GRAMMARS ${CMAKE_SOURCE_DIR}/third_party/grammars)

add_library(tree-sitter STATIC IMPORTED GLOBAL)
set_target_properties(tree-sitter PROPERTIES
    IMPORTED_LOCATION ${TS_ROOT}/usr/lib/x86_64-linux-gnu/libtree-sitter.a
    INTERFACE_INCLUDE_DIRECTORIES ${TS_ROOT}/usr/include)

add_library(tree-sitter-c STATIC
    ${TS_GRAMMARS}/tree-sitter-c/src/parser.c)
target_include_directories(tree-sitter-c PRIVATE ${TS_GRAMMARS}/tree-sitter-c/src)
set_target_properties(tree-sitter-c PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON LINKER_LANGUAGE C)

add_library(tree-sitter-cpp STATIC
    ${TS_GRAMMARS}/tree-sitter-cpp/src/parser.c
    ${TS_GRAMMARS}/tree-sitter-cpp/src/scanner.c)
target_include_directories(tree-sitter-cpp PRIVATE ${TS_GRAMMARS}/tree-sitter-cpp/src)
set_target_properties(tree-sitter-cpp PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON LINKER_LANGUAGE C)

add_library(tree-sitter-python STATIC
    ${TS_GRAMMARS}/tree-sitter-python/src/parser.c
    ${TS_GRAMMARS}/tree-sitter-python/src/scanner.c)
target_include_directories(tree-sitter-python PRIVATE ${TS_GRAMMARS}/tree-sitter-python/src)
set_target_properties(tree-sitter-python PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON LINKER_LANGUAGE C)

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${CMAKE_SOURCE_DIR}/third_party/usr/include)
include_directories(${CMAKE_SOURCE_DIR}/third_party/ankerl-unordered_dense)
include_directories(${DEPS_INCLUDE_DIRS})

# Link directories
link_directories(${DEPS_LIBRARY_DIRS})

# Auto-discover source files (third_party is built via the imported targets above)
file(GLOB SOURCES
    src/main.cpp
    src/server/*.cpp
    src/engine/*.cpp
    src/engine/parser/*.cpp
    src/utils/*.cpp
)

# Main executable
add_executable(ai-tools ${SOURCES})

target_link_libraries(ai-tools
    ${DEPS_LIBRARIES}
    tree-sitter
    tree-sitter-c
    tree-sitter-cpp
    tree-sitter-python
)

# Test executable
enable_testing()
file(GLOB TEST_SOURCES
    tests/*.cpp
    src/server/*.cpp
    src/engine/*.cpp
    src/engine/parser/*.cpp
    src/utils/*.cpp
)
add_executable(run_tests ${TEST_SOURCES})

target_link_libraries(run_tests
    ${DEPS_LIBRARIES}
    tree-sitter
    tree-sitter-c
    tree-sitter-cpp
    tree-sitter-python
)

add_test(NAME unit_tests COMMAND run_tests)

# Install
install(TARGETS ai-tools DESTINATION bin)
