cmake_minimum_required(VERSION 3.16)
project(CanLinAioSample CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -------------------------------------------------------------------------
# libusb-1.0 discovery
#
# Linux  : pkg-config (libusb-1.0-dev)
# Windows: vcpkg      (vcpkg install libusb)
#          — or set LIBUSB_ROOT to a manual install, e.g.
#            cmake -DLIBUSB_ROOT=C:/libusb ..
# -------------------------------------------------------------------------
if(WIN32)
    # Try vcpkg-installed target first (cmake --toolchain vcpkg.cmake)
    find_package(libusb CONFIG QUIET)
    if(libusb_FOUND)
        set(_LIBUSB_TARGET libusb::libusb)
    else()
        # Fall back to manual LIBUSB_ROOT or standard search paths
        find_path(LIBUSB_INCLUDE_DIR
            NAMES libusb.h
            HINTS
                "$ENV{LIBUSB_ROOT}/include"
                "${LIBUSB_ROOT}/include"
        )
        find_library(LIBUSB_LIBRARY
            NAMES libusb-1.0 usb-1.0
            HINTS
                "$ENV{LIBUSB_ROOT}/lib"
                "$ENV{LIBUSB_ROOT}/MS64/dll"
                "${LIBUSB_ROOT}/lib"
        )
        if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
            message(FATAL_ERROR
                "libusb-1.0 not found.\n"
                "Options:\n"
                "  1. vcpkg: vcpkg install libusb  (pass -DCMAKE_TOOLCHAIN_FILE=...)\n"
                "  2. Manual: set -DLIBUSB_ROOT=<path to libusb install>\n"
            )
        endif()
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
    # pkg_check_modules sets LIBUSB_INCLUDE_DIRS and LIBUSB_LIBRARIES
endif()

# -------------------------------------------------------------------------
# Target
# -------------------------------------------------------------------------
add_executable(CanLinAioSample
    main.cpp
    LindeApi.cpp
    CandeApi.cpp
    AiodeApi.cpp
)

if(WIN32)
    if(libusb_FOUND)
        # vcpkg target already carries include dirs and link libs
        target_link_libraries(CanLinAioSample PRIVATE ${_LIBUSB_TARGET})
    else()
        target_include_directories(CanLinAioSample SYSTEM PRIVATE ${LIBUSB_INCLUDE_DIR})
        target_link_libraries(CanLinAioSample PRIVATE ${LIBUSB_LIBRARY})
    endif()
    # WinUSB backend dependencies (pulled in transitively by vcpkg but
    # needed when linking the static/import lib manually)
    target_link_libraries(CanLinAioSample PRIVATE setupapi winusb)
else()
    # SYSTEM suppresses warnings from libusb headers (e.g. -Wpedantic zero-size array)
    target_include_directories(CanLinAioSample SYSTEM PRIVATE ${LIBUSB_INCLUDE_DIRS})
    target_link_libraries(CanLinAioSample PRIVATE ${LIBUSB_LIBRARIES})
endif()

target_include_directories(CanLinAioSample PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_options(CanLinAioSample PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

# -------------------------------------------------------------------------
# Install (optional)
# -------------------------------------------------------------------------
install(TARGETS CanLinAioSample RUNTIME DESTINATION bin)
