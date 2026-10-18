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

add_library(ou_render_common STATIC src/render/RenderCommon.cpp)
target_include_directories(ou_render_common PUBLIC src)
target_link_libraries(ou_render_common PUBLIC SDL3::SDL3 glm::glm)

add_library(ou_render_gl STATIC src/render/gl/GlRenderer.cpp)
target_link_libraries(ou_render_gl PUBLIC ou_render_common PRIVATE PkgConfig::EPOXY)

# ---- ou_render_vk: Vulkan 1.3 backend; SPIR-V compiled at build time by glslc and embedded ----
find_package(Vulkan REQUIRED COMPONENTS glslc)
set(OU_VK_SPV_DIR ${CMAKE_CURRENT_BINARY_DIR}/vk_shaders)
file(MAKE_DIRECTORY ${OU_VK_SPV_DIR})
set(OU_VK_SPV)
foreach(s scene.vert scene.frag text.vert text.frag line.vert line.frag)
  set(src ${CMAKE_CURRENT_SOURCE_DIR}/src/render/vk/shaders/${s})
  add_custom_command(OUTPUT ${OU_VK_SPV_DIR}/${s}.inc
                     COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 -O -mfmt=num -o ${OU_VK_SPV_DIR}/${s}.inc ${src}
                     DEPENDS ${src} VERBATIM)
  list(APPEND OU_VK_SPV ${OU_VK_SPV_DIR}/${s}.inc)
endforeach()
add_library(ou_render_vk STATIC src/render/vk/VkRenderer.cpp ${OU_VK_SPV})
target_include_directories(ou_render_vk PRIVATE ${OU_VK_SPV_DIR})
target_link_libraries(ou_render_vk PUBLIC ou_render_common PRIVATE Vulkan::Vulkan)
target_compile_options(ou_render_vk PRIVATE -Wno-missing-field-initializers) # Vk{sType} init idiom

add_executable(ou-viewer src/app/viewer/main.cpp)
target_link_libraries(ou-viewer PRIVATE ou_render_gl ou_render_vk ou_placeholder ou_formats)
add_executable(ou-imgdiff tools/ou-imgdiff/main.cpp)

if(OU_BUILD_TESTS)
  add_executable(ou_viewer_tests tests/test_viewer.cpp)
  target_link_libraries(ou_viewer_tests PRIVATE ou_placeholder glm::glm GTest::gtest_main)
  gtest_discover_tests(ou_viewer_tests)
  # Opens a real window; exits 77 (skip) when no display is available.
  add_test(NAME viewer_placeholder_screenshot
           COMMAND ou-viewer --frames 5 --screenshot ${CMAKE_CURRENT_BINARY_DIR}/viewer_placeholder.ppm)
  set_tests_properties(viewer_placeholder_screenshot PROPERTIES SKIP_RETURN_CODE 77)
  # Same, Vulkan backend; exits 77 (skip) without a display or a usable Vulkan 1.3 device.
  add_test(NAME viewer_vulkan_placeholder_screenshot
           COMMAND ou-viewer --renderer vulkan --frames 5 --screenshot ${CMAKE_CURRENT_BINARY_DIR}/viewer_vulkan_placeholder.ppm)
  # GL vs Vulkan placeholder frames without overlay must be near-identical (mean |diff| <= 2/255).
  set(_v $<TARGET_FILE:ou-viewer>)
  set(_d ${CMAKE_CURRENT_BINARY_DIR})
  add_test(NAME viewer_vulkan_matches_gl
           COMMAND sh -ec "\"${_v}\" --no-overlay --frames 3 --screenshot ${_d}/cmp_gl.ppm; \"${_v}\" --renderer vulkan --no-overlay --frames 3 --screenshot ${_d}/cmp_vk.ppm; \"$<TARGET_FILE:ou-imgdiff>\" ${_d}/cmp_gl.ppm ${_d}/cmp_vk.ppm 2")
  add_executable(ou_vk_tests tests/test_vk_renderer.cpp)
  target_link_libraries(ou_vk_tests PRIVATE ou_render_vk ou_placeholder GTest::gtest_main)
  # Mesa RADV leaks 256 bytes per process (see tests/lsan_vulkan.supp).
  set(OU_VK_TEST_ENV "LSAN_OPTIONS=fast_unwind_on_malloc=0:suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan_vulkan.supp")
  gtest_discover_tests(ou_vk_tests PROPERTIES ENVIRONMENT "${OU_VK_TEST_ENV}")
  set_tests_properties(viewer_vulkan_placeholder_screenshot viewer_vulkan_matches_gl PROPERTIES
                       SKIP_RETURN_CODE 77 ENVIRONMENT "${OU_VK_TEST_ENV}")
endif()

# ---- ou_anim: skeletons, animation clips, pose sampling, CPU skinning (docs/formats/ANIMATION.md) ----
add_library(ou_anim STATIC src/anim/Anim.cpp)
target_include_directories(ou_anim PUBLIC src)
target_link_libraries(ou_anim PUBLIC ou_cdc glm::glm)
if(OU_BUILD_TESTS)
  add_executable(ou_anim_tests tests/test_anim.cpp)
  target_link_libraries(ou_anim_tests PRIVATE ou_anim ou_formats GTest::gtest_main)
  gtest_discover_tests(ou_anim_tests)
endif()

# ---- ou_formats: PCD9 textures, render models, terrain -> ou::scene; ou-asset CLI ----------
add_library(ou_formats STATIC src/formats/Texture.cpp src/formats/Model.cpp src/formats/Scene.cpp src/formats/Level.cpp)
target_link_libraries(ou_formats PUBLIC ou_cdc ou_anim)
add_executable(ou-asset tools/ou-asset/main.cpp)
target_link_libraries(ou-asset PRIVATE ou_formats)
if(OU_BUILD_TESTS)
  add_executable(ou_formats_tests tests/test_formats.cpp)
  target_link_libraries(ou_formats_tests PRIVATE ou_formats GTest::gtest_main)
  gtest_discover_tests(ou_formats_tests)
endif()

# ---- ou_collision: unit collision meshes + markup, BVH, queries ---------------------------
add_library(ou_collision STATIC src/collision/Collision.cpp src/collision/LevelCollision.cpp)
target_link_libraries(ou_collision PUBLIC ou_cdc glm::glm)
target_link_libraries(ou-viewer PRIVATE ou_collision)
if(OU_BUILD_TESTS)
  add_executable(ou_collision_tests tests/test_collision.cpp)
  target_link_libraries(ou_collision_tests PRIVATE ou_collision ou_formats GTest::gtest_main)
  gtest_discover_tests(ou_collision_tests DISCOVERY_TIMEOUT 30)
endif()

# ---- ou-import: read-only installation importer ----------------------------------------
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED CONFIG)
add_library(ou_importer STATIC src/importer/Importer.cpp)
target_include_directories(ou_importer PUBLIC src)
target_compile_definitions(ou_importer PRIVATE OU_VERSION="${PROJECT_VERSION}")
target_link_libraries(ou_importer PUBLIC ou_cdc nlohmann_json::nlohmann_json PRIVATE OpenSSL::Crypto)
add_executable(ou-import tools/ou-import/main.cpp)
target_link_libraries(ou-import PRIVATE ou_importer)
if(OU_BUILD_TESTS)
  add_executable(ou_importer_tests tests/test_importer.cpp)
  target_link_libraries(ou_importer_tests PRIVATE ou_importer GTest::gtest_main)
  gtest_discover_tests(ou_importer_tests)
endif()

# ---- shared config, signed updater, ou-sign, Qt launcher (docs/LAUNCHER.md, docs/UPDATES.md) ----
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
find_package(CURL REQUIRED)
add_library(ou_config STATIC src/config/Config.cpp)
target_include_directories(ou_config PUBLIC src)
target_link_libraries(ou_config PUBLIC nlohmann_json::nlohmann_json)
target_link_libraries(ou-viewer PRIVATE ou_config)

# Release public key (hex). Default: packaging/release-pubkey.hex; empty = updates unavailable.
set(OU_UPDATE_PUBLIC_KEY "" CACHE STRING "Ed25519 update public key (64 hex chars)")
set(_ou_pubkey "${OU_UPDATE_PUBLIC_KEY}")
if(NOT _ou_pubkey AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/packaging/release-pubkey.hex)
  file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/packaging/release-pubkey.hex _ou_pubkey LIMIT_COUNT 1)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS packaging/release-pubkey.hex)
endif()
add_library(ou_update STATIC src/update/Update.cpp)
target_include_directories(ou_update PUBLIC src)
target_link_libraries(ou_update PUBLIC nlohmann_json::nlohmann_json PRIVATE PkgConfig::SODIUM CURL::libcurl)
if(_ou_pubkey)
  target_compile_definitions(ou_update PRIVATE OU_UPDATE_PUBLIC_KEY="${_ou_pubkey}")
endif()
add_executable(ou-sign tools/ou-sign/main.cpp)
target_link_libraries(ou-sign PRIVATE ou_update ou_config PkgConfig::SODIUM)
if(OU_BUILD_TESTS)
  add_executable(ou_update_tests tests/test_update.cpp)
  target_link_libraries(ou_update_tests PRIVATE ou_update ou_config PkgConfig::SODIUM GTest::gtest_main)
  gtest_discover_tests(ou_update_tests)
endif()

find_package(Qt6 6.5 COMPONENTS Widgets Concurrent)
if(Qt6_FOUND)
  add_executable(ou-launcher src/launcher/main.cpp)
  target_compile_definitions(ou-launcher PRIVATE OU_VERSION="${PROJECT_VERSION}")
  target_link_libraries(ou-launcher PRIVATE ou_config ou_update ou_importer SDL3::SDL3 Qt6::Widgets Qt6::Concurrent)
  if(OU_BUILD_TESTS)
    add_test(NAME launcher_self_test COMMAND ou-launcher --self-test --config ${CMAKE_CURRENT_BINARY_DIR}/launcher-test-config.json)
    set_tests_properties(launcher_self_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;OU_DISABLE_UPDATES=1")
  endif()
else()
  message(STATUS "Qt6 Widgets not found: ou-launcher not built")
endif()

include(GNUInstallDirs)
install(TARGETS ou-viewer ou-asset ou-import ou-sign RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(TARGET ou-launcher)
  install(TARGETS ou-launcher RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(FILES packaging/openunderworld.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/openunderworld.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
