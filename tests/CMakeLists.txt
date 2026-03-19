cmake_minimum_required(VERSION 3.14)

# Test executables (C++ for GTest support)
add_executable(test_loop test_loop.cpp)
set_target_properties(test_loop PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)
target_link_libraries(test_loop poll_dancer gtest_main)

add_executable(test_events test_events.cpp)
set_target_properties(test_events PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)
target_link_libraries(test_events poll_dancer gtest_main)

# Platform-specific tests
if(POLL_DANCER_PLATFORM STREQUAL "linux")
    add_executable(test_epoll platform/test_epoll.cpp)
    set_target_properties(test_epoll PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)
    target_link_libraries(test_epoll poll_dancer gtest_main)
elseif(POLL_DANCER_PLATFORM STREQUAL "bsd")
    add_executable(test_kqueue platform/test_kqueue.cpp)
    set_target_properties(test_kqueue PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)
    target_link_libraries(test_kqueue poll_dancer gtest_main)
elseif(POLL_DANCER_PLATFORM STREQUAL "windows")
    add_executable(test_iocp platform/test_iocp.cpp)
    set_target_properties(test_iocp PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)
    target_link_libraries(test_iocp poll_dancer gtest_main ws2_32 mswsock)
endif()

# Add tests to CTest
include(GoogleTest)

gtest_discover_tests(test_loop)
gtest_discover_tests(test_events)

if(POLL_DANCER_PLATFORM STREQUAL "linux")
    gtest_discover_tests(test_epoll)
elseif(POLL_DANCER_PLATFORM STREQUAL "bsd")
    gtest_discover_tests(test_kqueue)
elseif(POLL_DANCER_PLATFORM STREQUAL "windows")
    gtest_discover_tests(test_iocp)
endif()