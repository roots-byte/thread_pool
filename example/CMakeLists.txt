cmake_minimum_required(VERSION 3.14)
project(thread_pool_example C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# When built standalone (not as part of the root project) we need the include path
if(NOT TARGET thread_pool)
    add_library(thread_pool INTERFACE)
    target_include_directories(thread_pool INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
    if(WIN32)
        # Win32 threads — no extra lib needed
    else()
        target_link_libraries(thread_pool INTERFACE pthread)
    endif()
endif()

add_executable(pi test.c)
target_link_libraries(pi PRIVATE thread_pool m)

if(WIN32)
    set_target_properties(pi PROPERTIES OUTPUT_NAME pi)
endif()
