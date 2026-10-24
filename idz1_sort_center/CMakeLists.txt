cmake_minimum_required(VERSION 3.10)
project(sort_center C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -pedantic)

include_directories(src)

file(GLOB SOURCES "src/*.c")

add_executable(sort_center ${SOURCES})
