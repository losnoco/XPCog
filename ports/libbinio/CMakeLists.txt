# Replaces upstream's CMakeLists; see portfile.cmake for why it cannot be used.
cmake_minimum_required(VERSION 3.16)
project(libbinio CXX)

include(CheckTypeSize)
include(GNUInstallDirs)

# binio.h.in asks for the widest integer and float the compiler has, and names
# them in the public header -- so these four probes decide the library's ABI
# rather than only its internals.
# LANGUAGE CXX on both, and not only because the project enables no C: these
# two types are named in a C++ public header, and C is entitled to size them
# differently.
check_type_size("long long" SIZEOF_LONG_LONG LANGUAGE CXX)
if(HAVE_SIZEOF_LONG_LONG)
    set(TYPE_INT "long long")
else()
    set(TYPE_INT "long")
endif()

check_type_size("long double" SIZEOF_LONG_DOUBLE LANGUAGE CXX)
if(HAVE_SIZEOF_LONG_DOUBLE)
    set(TYPE_FLOAT "long double")
else()
    set(TYPE_FLOAT "double")
endif()

# Upstream sets all four unconditionally with a "TODO: check library exists"
# beside them, and every one is satisfied by a hosted C++ implementation. Kept
# as they are rather than made conditional: turning any of them off changes
# which overloads binio.h declares, and AdPlug calls into all of them.
set(ENABLE_IOSTREAM 1)
set(ENABLE_STRING 1)
set(ISO_STDLIB 1)
set(WITH_MATH 1)

# Into the build tree, not the source tree. That is the whole fix.
configure_file(src/binio.h.in "${CMAKE_CURRENT_BINARY_DIR}/binio.h")

add_library(binio STATIC
    src/binio.cpp
    src/binfile.cpp
    src/binwrap.cpp
    src/binstr.cpp
)

# The generated header first, so the sources find it; the source directory too,
# for the three headers that ship as they are.
target_include_directories(binio PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

install(TARGETS binio ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/binio.h"
    src/binfile.h
    src/binwrap.h
    src/binstr.h
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
