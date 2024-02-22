cmake_minimum_required(VERSION 3.16)
project(rasta
    LANGUAGES C CXX
    DESCRIPTION "A C implementation of the RaSTA protocol stack")

# Include cmake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

SET(USE_OPENSSL false CACHE BOOL "Use OpenSSL MD4 implementation")

option(BUILD_DOCUMENTATION "Generate documentation" OFF)
option(BUILD_LOCAL_EXAMPLES "Build the RaSTA/SCI examples the run on localhost" ON)
option(BUILD_RASTA_GRPC_BRIDGE "Build the RaSTA/gRPC bridge" OFF)
option(BUILD_SHARED_LIBS "Build using shared libraries" ON)
option(ENABLE_RASTA_TLS "Enable RaSTA over TLS" OFF)
option(ENABLE_RASTA_OPAQUE "Enable Password-Authenticated Session Key Exchange based on OPAQUE" OFF)
option(ENABLE_CODE_COVERAGE "Provide command to generate code coverage report" OFF)
option(ENABLE_STATIC_ANALYSIS "Run cppcheck along with the compiler" OFF)
option(USE_VCPKG "Make changes needed to use dependencies from Vcpkg" OFF)

if(ENABLE_STATIC_ANALYSIS)
    set(CMAKE_C_CPPCHECK "cppcheck" "--enable=performance,information")
endif(ENABLE_STATIC_ANALYSIS)

find_program(MEMORYCHECK_COMMAND valgrind)
set(MEMORYCHECK_COMMAND_OPTIONS "--leak-check=full --show-leak-kinds=all")

include(CTest)
include(GNUInstallDirs)

if(ENABLE_CODE_COVERAGE)
    link_libraries(-lgcov)
    add_compile_options(-fprofile-arcs -ftest-coverage)

    find_program(LCOV lcov)
    find_program(GENHTML genhtml)
    if(EXISTS ${LCOV} AND EXISTS ${GENHTML})
        add_custom_target(coverage_report
            COMMAND "${LCOV}" "--base-directory" "${CMAKE_SOURCE_DIR}"
                              "--no-external"
                              "--directory" "."
                              "--capture"
                              "--output-file" "app.info"
            COMMAND "${GENHTML}" "app.info" "-o" "ccHTML"
            COMMENT "Generate HTML files from preprocessed code coverage data"
            VERBATIM)
    endif()
endif(ENABLE_CODE_COVERAGE)

if(ENABLE_RASTA_OPAQUE)

    find_program(GNU_MAKE_PATH
            NAMES gmake mingw32-make make
            NAMES_PER_DIR
            DOC "GNU Make" REQUIRED)

    configure_file(cmake/IncludeLibOpaque.cmake libopaque-timeout/CMakeLists.txt)
    execute_process(COMMAND ${CMAKE_COMMAND} -DGNU_MAKE_PATH=${GNU_MAKE_PATH} -G "${CMAKE_GENERATOR}" .
            RESULT_VARIABLE result
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/libopaque-timeout)
    if (result)
        message(FATAL_ERROR "CMake step for libopaque failed: ${result}")
    endif ()
    execute_process(COMMAND ${CMAKE_COMMAND} --build .
            RESULT_VARIABLE result
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/libopaque-timeout)
    if (result)
        message(FATAL_ERROR "Build step for libopaque failed: ${result}")
    endif ()
endif()
#
# Compiler settings and options
#

include(cmake/CompileOptions.cmake)

# Set output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})

add_subdirectory(src)
add_subdirectory(examples)

if(BUILD_DOCUMENTATION)
    find_package(Doxygen REQUIRED dot)

    set(DOXYGEN_PROJECT_NAME "Librasta & Libsci")
    set(DOXYGEN_OUTPUT_DIRECTORY "doc")
    set(DOXYGEN_CREATE_SUBDIRS YES)
    set(DOXYGEN_OPTIMIZE_OUTPUT_FOR_C YES)
    set(DOXYGEN_BUILTIN_STL_SUPPORT YES)
    set(DOXYGEN_EXTRACT_ALL YES)
    set(DOXYGEN_EXTRACT_STATIC YES)
    set(DOXYGEN_EXTRACT_ANON_NSPACES YES)
    set(DOXYGEN_CASE_SENSE_NAMES NO)
    set(DOXYGEN_WARN_NO_PARAMDOC YES)
    set(DOXYGEN_RECURSIVE YES)
    set(DOXYGEN_USE_MDFILE_AS_MAINPAGE "README.md")
    set(DOXYGEN_INLINE_SOURCES YES)
    set(DOXYGEN_GENERATE_MAN YES)
    set(DOXYGEN_MACRO_EXPANSION YES)
    set(DOXYGEN_HIDE_UNDOC_RELATIONS NO)
    set(DOXYGEN_PREDEFINED ENABLE_RASTA_TLS ENABLE_RASTA_OPAQUE)
    set(DOXYGEN_FILE_PATTERNS *.h *.md)
    if(DOXYGEN_FOUND)
        doxygen_add_docs(
            documentation
            src/include/rasta
            examples/sci/common
            examples/common/headers/configfile.h
            README.md
            md_doc/
            ALL
            COMMENT "Generate documentation")
    endif(DOXYGEN_FOUND)
endif(BUILD_DOCUMENTATION)

if(BUILD_TESTING)
    add_subdirectory(test)
endif(BUILD_TESTING)
