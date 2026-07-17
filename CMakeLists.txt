cmake_minimum_required(VERSION 3.16)
project(power-balance VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(power-balance src/power-balance.cpp)
target_compile_options(power-balance PRIVATE -Wall -Wextra)

add_executable(power-status src/power-status.cpp)
target_compile_options(power-status PRIVATE -Wall -Wextra)

install(TARGETS power-balance power-status DESTINATION bin)

install(FILES systemd/power-balance.service
  DESTINATION lib/systemd/system
  COMPONENT systemd)
