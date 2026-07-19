cmake_minimum_required(VERSION 3.16)
project(power-balance VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

find_package(GTest REQUIRED)

add_executable(power-balance src/power-balance.cpp)
target_compile_options(power-balance PRIVATE -Wall -Wextra)

add_executable(power-status src/power-status.cpp)
target_compile_options(power-status PRIVATE -Wall -Wextra)

# ── Unit tests ──
add_executable(test-power-balance tests/test_power_balance.cpp)
target_include_directories(test-power-balance PRIVATE tests)
target_link_libraries(test-power-balance GTest::gtest GTest::gtest_main)
target_compile_options(test-power-balance PRIVATE -Wall -Wextra)

include(GoogleTest)
gtest_discover_tests(test-power-balance TEST_PREFIX "power-balance." TEST_PROPERTIES TIMEOUT 10)

install(TARGETS power-balance power-status DESTINATION bin)

install(FILES systemd/power-balance.service
  DESTINATION lib/systemd/system
  COMPONENT systemd)
