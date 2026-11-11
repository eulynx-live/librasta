# Template for the libopaque sub-project, configured by the top-level
# CMakeLists.txt with configure_file(@ONLY): @VAR@ is replaced with values from
# the librasta project, ${VAR} is evaluated in this sub-project.
cmake_minimum_required(VERSION 3.10)

project(libopaque NONE)

option(GNU_MAKE_PATH "Path to GNU make" "make")

message("Using make=${GNU_MAKE_PATH}")

# resolves to something like build/lib/libopaque.a
set(opaque_LIBRARY @CMAKE_INSTALL_PREFIX@/lib/@CMAKE_STATIC_LIBRARY_PREFIX@opaque@CMAKE_STATIC_LIBRARY_SUFFIX@)

include(ExternalProject)

# Replaces libopaque's default CFLAGS, which contain -march=native: that
# produces non-portable binaries and AVX-512 instructions Valgrind cannot execute.
if(BUILD_TESTING)
    set(OPAQUE_CFLAGS "-g -O0 -fpic -Wall")
else()
    set(OPAQUE_CFLAGS "-g -O2 -fpic -Wall -fstack-protector-strong -D_FORTIFY_SOURCE=2")
endif()

# directories for output library and headers
file(MAKE_DIRECTORY @CMAKE_BINARY_DIR@/lib)
file(MAKE_DIRECTORY @CMAKE_BINARY_DIR@/bin)
file(MAKE_DIRECTORY @CMAKE_BINARY_DIR@/include)

ExternalProject_Add(libopaque
GIT_REPOSITORY https://github.com/stef/libopaque.git
GIT_TAG bc83e13732330af044fd27476b9ee0d68217eff6
GIT_SUBMODULES "" # update all submodules
SOURCE_DIR        "@CMAKE_BINARY_DIR@/opaque-src"
BUILD_IN_SOURCE TRUE
CONFIGURE_COMMAND ""
# Only build what the install target needs
BUILD_COMMAND ${CMAKE_COMMAND} -E env "CFLAGS=${OPAQUE_CFLAGS}" ${GNU_MAKE_PATH} -C @CMAKE_BINARY_DIR@/opaque-src/src PREFIX=@CMAKE_BINARY_DIR@ libopaque.so libopaque.a utils/opaque
INSTALL_COMMAND ${GNU_MAKE_PATH} -C @CMAKE_BINARY_DIR@/opaque-src/src install PREFIX=@CMAKE_BINARY_DIR@
BUILD_BYPRODUCTS ${opaque_LIBRARY}
)

message("Installed libopaque headers to @CMAKE_BINARY_DIR@/include, libraries to @CMAKE_BINARY_DIR@/lib, binaries to @CMAKE_BINARY_DIR@/bin")
