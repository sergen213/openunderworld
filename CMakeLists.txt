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

# ---- ou-viewer: SDL3 + OpenGL 4.6 core (libepoxy) ------------------------------------
find_package(SDL3 REQUIRED CONFIG)
find_package(glm REQUIRED CONFIG)
find_package(PkgConfig REQUIRED)
pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy)

add_library(ou_placeholder STATIC src/scene/Placeholder.cpp)
target_include_directories(ou_placeholder PUBLIC src)

add_library(ou_render_gl STATIC src/render/gl/GlRenderer.cpp)
target_include_directories(ou_render_gl PUBLIC src)
target_link_libraries(ou_render_gl PUBLIC SDL3::SDL3 glm::glm PRIVATE PkgConfig::EPOXY)

add_executable(ou-viewer src/app/viewer/main.cpp)
target_link_libraries(ou-viewer PRIVATE ou_render_gl ou_placeholder ou_formats)

if(OU_BUILD_TESTS)
  add_executable(ou_viewer_tests tests/test_viewer.cpp)
  target_link_libraries(ou_viewer_tests PRIVATE ou_placeholder glm::glm GTest::gtest_main)
  gtest_discover_tests(ou_viewer_tests)
  # Opens a real window; exits 77 (skip) when no display is available.
  add_test(NAME viewer_placeholder_screenshot
           COMMAND ou-viewer --frames 5 --screenshot ${CMAKE_CURRENT_BINARY_DIR}/viewer_placeholder.ppm)
  set_tests_properties(viewer_placeholder_screenshot PROPERTIES SKIP_RETURN_CODE 77)
endif()
