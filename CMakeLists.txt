cmake_minimum_required(VERSION 3.0)

project(sample_c C)

option(SOME_FEATURE "Enable something" OFF)

include_directories(${PROJECT_SOURCE_DIR}/include/)

# Correct the path to where the headers were installed
include_directories(/home/fil/Hoang/comp_easy/dependency/prefix/include) # Changed this line

find_library(
    CURL_LIB
    NAMES curl # CMake looks for libcurl.so
    HINTS /home/fil/Hoang/comp_easy/dependency/prefix/lib # Changed this line
)

if (NOT CURL_LIB)
    # Correct the error message path too
    message(FATAL_ERROR "Không tìm thấy libcurl.so trong /home/fil/Hoang/comp_easy/dependency/prefix/lib") # Changed this line
endif()

file(GLOB SHARED_SOURCE
    ${PROJECT_SOURCE_DIR}/src/*.c
)

add_executable(my_program ${SHARED_SOURCE} real_example.c)

target_link_libraries(my_program ${CURL_LIB} m)

# Make sure the executable can find the shared library at runtime
# This adds the library path to the RPATH of the executable
target_link_options(my_program PRIVATE "-Wl,-rpath,/home/fil/Hoang/comp_easy/dependency/prefix/lib")


if(SOME_FEATURE)
    target_compile_definitions(my_program PRIVATE SOME_FEATURE)
endif()
