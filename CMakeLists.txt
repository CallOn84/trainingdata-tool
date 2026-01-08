# CMakeList.txt : CMake project for trainingdata-tool, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.8)
project(trainingdata-tool)

set(CMAKE_REQUIRED_FLAGS -std=c++20)

file(GLOB_RECURSE sources src/*.cpp src/*.h)

set (
    lc0
    "lc0/src/chess/board.cc"
    "lc0/src/chess/position.cc"
    "lc0/src/neural/encoder.cc"
    "lc0/src/trainingdata/writer.cc"
    "lc0/src/utils/commandline.cc"
    "lc0/src/utils/logging.cc"
    "lc0/src/utils/random.cc"
    "lc0/src/utils/string.cc"
)

if (WIN32)
    set (lc0_filesystem "lc0/src/utils/filesystem.win32.cc")
else (WIN32)
    set (lc0_filesystem "lc0/src/utils/filesystem.posix.cc")
endif (WIN32)

AUX_SOURCE_DIRECTORY(polyglot/src polyglot)

# Use system zlib on Unix to avoid old bundled zlib issues with modern compilers
if (UNIX)
    find_package(ZLIB REQUIRED)
    set(ZLIB_LIBS ZLIB::ZLIB)
    set(ZLIB_INCLUDE ${ZLIB_INCLUDE_DIRS})
else()
    AUX_SOURCE_DIRECTORY(zlib zlib_sources)
    set(ZLIB_LIBS "")
    set(ZLIB_INCLUDE "zlib")
endif()

# Add source to this project's executable.
if (UNIX)
    add_executable(trainingdata-tool ${sources} ${lc0} ${lc0_filesystem} ${polyglot})
else()
    add_executable(trainingdata-tool ${sources} ${lc0} ${lc0_filesystem} ${polyglot} ${zlib_sources})
endif()

set_target_properties(trainingdata-tool PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
)

if (UNIX)
    target_link_libraries(trainingdata-tool -lpthread -lstdc++fs ${ZLIB_LIBS})
endif(UNIX)

set(CMAKE_BUILD_TYPE Release)

include_directories(
    "lc0/src"
    "src"
    "polyglot/src"
    ${ZLIB_INCLUDE}
    "."
)

add_compile_definitions(NO_PEXT)

# MSVC-specific settings for compatibility with legacy C code in polyglot
if (MSVC)
    # Disable deprecation warnings for unsafe CRT functions
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    # Allow string literal to char* conversion (needed by polyglot's getopt.h)
    add_compile_options(/Zc:strictStrings-)
endif()

# TODO: Add tests and install targets if needed.
