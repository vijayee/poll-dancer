cmake_minimum_required(VERSION 3.14)
project(poll_dancer VERSION 0.1.0 LANGUAGES C CXX)

# C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# C++ standard for tests
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# C++ standard (for tests)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Options
option(POLL_DANCER_BUILD_TESTS "Build tests" ON)
option(POLL_DANCER_BUILD_EXAMPLES "Build examples" OFF)
option(POLL_DANCER_BUILD_SHARED "Build shared library" OFF)
option(POLL_DANCER_THREAD_SAFE "Enable thread safety" ON)

# Platform detection
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(POLL_DANCER_PLATFORM "linux")
    set(POLL_DANCER_BACKEND "epoll")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR
       CMAKE_SYSTEM_NAME STREQUAL "FreeBSD" OR
       CMAKE_SYSTEM_NAME STREQUAL "OpenBSD" OR
       CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
    set(POLL_DANCER_PLATFORM "bsd")
    set(POLL_DANCER_BACKEND "kqueue")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(POLL_DANCER_PLATFORM "windows")
    set(POLL_DANCER_BACKEND "iocp")
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

message(STATUS "Platform: ${POLL_DANCER_PLATFORM}")
message(STATUS "Backend: ${POLL_DANCER_BACKEND}")

# Thread safety configuration
if(POLL_DANCER_THREAD_SAFE)
    find_package(Threads REQUIRED)
    add_compile_definitions(PD_THREAD_SAFE)
endif()

# Source files
set(POLL_DANCER_SOURCES
    src/loop.c
    src/watcher.c
    src/timer.c
    src/error.c
    src/init.c
    src/platform.c
)

# Platform-specific sources
if(POLL_DANCER_BACKEND STREQUAL "epoll")
    list(APPEND POLL_DANCER_SOURCES src/platform/epoll.c)
elseif(POLL_DANCER_BACKEND STREQUAL "kqueue")
    list(APPEND POLL_DANCER_SOURCES src/platform/kqueue.c)
elseif(POLL_DANCER_BACKEND STREQUAL "iocp")
    list(APPEND POLL_DANCER_SOURCES src/platform/iocp.c)
endif()

# Library target
if(POLL_DANCER_BUILD_SHARED)
    add_library(poll_dancer SHARED ${POLL_DANCER_SOURCES})
else()
    add_library(poll_dancer STATIC ${POLL_DANCER_SOURCES})
endif()

# Include directories
target_include_directories(poll_dancer
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
)

# Platform-specific libraries
if(POLL_DANCER_THREAD_SAFE)
    target_link_libraries(poll_dancer PUBLIC Threads::Threads)
endif()

if(POLL_DANCER_PLATFORM STREQUAL "windows")
    target_link_libraries(poll_dancer PRIVATE ws2_32 mswsock)
endif()

# Version information
set_target_properties(poll_dancer PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME poll_dancer
)

# Installation
include(GNUInstallDirs)

install(TARGETS poll_dancer
    EXPORT poll_dancerTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/poll-dancer
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

# Generate and install package config
include(CMakePackageConfigHelpers)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/poll-dancerConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/poll-dancerConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/poll-dancer
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/poll-dancerConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/poll-dancerConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/poll-dancerConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/poll-dancer
)

install(EXPORT poll_dancerTargets
    FILE poll-dancerTargets.cmake
    NAMESPACE poll_dancer::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/poll-dancer
)

# Tests
if(POLL_DANCER_BUILD_TESTS)
    # Add Google Test as a subdirectory from git submodule
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory(deps/googletest)

    # Prevent GoogleTest from overriding our compiler/linker settings on Windows
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

    enable_testing()
    add_subdirectory(tests)
endif()

# Examples
if(POLL_DANCER_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Export targets for build tree
export(EXPORT poll_dancerTargets
    FILE ${CMAKE_CURRENT_BINARY_DIR}/poll-dancerTargets.cmake
    NAMESPACE poll_dancer::
)