cmake_minimum_required(VERSION 3.16)

project(rsim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(rsim
  rsim/main.cpp
  rsim/cpu.cpp
  rsim/decode.cpp
  rsim/elf.cpp
  rsim/memory.cpp
  rsim/selftest.cpp
)

target_compile_options(rsim PRIVATE -Wall -Wextra -Wpedantic)
