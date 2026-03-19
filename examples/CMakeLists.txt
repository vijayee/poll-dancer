cmake_minimum_required(VERSION 3.14)

# Example programs
add_executable(simple_server simple_server.c)
target_link_libraries(simple_server poll_dancer)

add_executable(timer_example timer_example.c)
target_link_libraries(timer_example poll_dancer)