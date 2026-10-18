cmake_minimum_required(VERSION 3.25)
project(OpenUnderworld VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(OU_BUILD_TESTS "Build unit tests" ON)
option(OU_SANITIZE "Build with ASan+UBSan" OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
  if(OU_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

find_package(ZLIB REQUIRED)

add_library(ou_cdc STATIC src/cdc/Cdc.cpp)
target_include_directories(ou_cdc PUBLIC src)
target_link_libraries(ou_cdc PUBLIC ZLIB::ZLIB)

if(OU_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  add_executable(ou_tests tests/test_cdc.cpp)
  target_link_libraries(ou_tests PRIVATE ou_cdc GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(ou_tests)
endif()
