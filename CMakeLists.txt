cmake_minimum_required(VERSION 3.0)

project(sample_c C)

option(SOME_FEATURE "Enable something" OFF)

include_directories(${PROJECT_SOURCE_DIR}/include/)

include_directories(/home/fil/dependencies/prefix/curl/include)

find_library(
    CURL_LIB
    NAMES curl
    HINTS /home/fil/dependencies/prefix/curl/lib
)

if (NOT CURL_LIB)
    message(FATAL_ERROR "Không tìm thấy libcurl.so trong /home/fil/dependencies/prefix/curl/lib")
endif()

file(GLOB SHARED_SOURCE
    ${PROJECT_SOURCE_DIR}/src/*.c
)

add_executable(my_program ${SHARED_SOURCE} real_example.c)

target_link_libraries(my_program ${CURL_LIB} m)

if(SOME_FEATURE)
    target_compile_definitions(my_program PRIVATE SOME_FEATURE)
endif()
