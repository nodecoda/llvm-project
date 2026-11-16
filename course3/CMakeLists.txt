cmake_minimum_required(VERSION 3.16)

project(rmach LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(rmach
  mach/main.cpp
)

# Chapter 03 onwards will compile the shared decoder and ELF loader straight
# from course 2.  Keeping the include path here from chapter 00 documents the
# dependency before it is used.
target_include_directories(rmach PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../course2/rsim
)

target_compile_options(rmach PRIVATE -Wall -Wextra -Wpedantic)
