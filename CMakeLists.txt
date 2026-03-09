cmake_minimum_required(VERSION 3.14)
project(thread_pool C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Interface library — consumers just include the headers and compile thread_pool.c themselves
add_library(thread_pool INTERFACE)
target_include_directories(thread_pool INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    # Win32 threads are used automatically via thread_pool_wrapper.h
else()
    target_link_libraries(thread_pool INTERFACE pthread)
endif()

add_subdirectory(example)
