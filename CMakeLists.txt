cmake_minimum_required(VERSION 3.25)
project(OpenUnderworld VERSION 0.45.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(OU_BUILD_TESTS "Build unit tests" ON)
option(OU_SANITIZE "Build with ASan+UBSan" OFF)
option(OU_LTO "Link-time optimisation (IPO) for Release / RelWithDebInfo builds, Jolt and FidelityFX included" ON)

# IPO per configuration, so Debug and sanitizer builds never get it. Set before any target (Jolt, ou_ffx included).
if(OU_LTO AND NOT OU_SANITIZE)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT OU_IPO_OK OUTPUT OU_IPO_ERR LANGUAGES CXX)
  if(OU_IPO_OK)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
  else()
    message(STATUS "OU_LTO: IPO not supported by this toolchain (${OU_IPO_ERR}), building without")
  endif()
endif()

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

# RenderCommon + the D3D9 SM3 -> GLSL translator of the Original Fidelity path (docs/formats/SHADERS.md).
add_library(ou_render_common STATIC src/render/RenderCommon.cpp src/render/ShaderTranslate.cpp src/render/OriginalShading.cpp
  src/render/Water.cpp src/render/Ocean.cpp)
target_include_directories(ou_render_common PUBLIC src)
# libshaderc compiles translated TRU shaders to SPIR-V at load time (Vulkan backend, shader tests).
pkg_check_modules(SHADERC REQUIRED IMPORTED_TARGET shaderc)
target_link_libraries(ou_render_common PUBLIC SDL3::SDL3 glm::glm ou_formats PRIVATE PkgConfig::SHADERC)
# src/render/sun.glsl (shared sun shadow / RT term) as a C++ raw string for the runtime-compiled shaders (GL, translated).
set(OU_SUN_GLSL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/render/sun.glsl)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${OU_SUN_GLSL_SRC})
file(READ ${OU_SUN_GLSL_SRC} OU_SUN_GLSL)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/sun_glsl.inc CONTENT "R\"ouglsl(@OU_SUN_GLSL@)ouglsl\"" @ONLY)
target_include_directories(ou_render_common PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen)

# Enhanced water / splash / underwater-post GLSL shared by GL (runtime compile) and Vulkan (glslc).
foreach(s water.vert water.frag splash.vert splash_splat.frag splash_screen.vert splash_blur.frag splash_composite.frag
        underwater.glsl atmosphere.glsl filter.glsl caustics.glsl waves.glsl tonemap.glsl ocean.comp)
  set(src ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/${s})
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${src})
  file(READ ${src} OU_WATER_SRC)
  file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/${s}.inc CONTENT "R\"ouwater(@OU_WATER_SRC@)ouwater\"" @ONLY)
endforeach()

add_library(ou_render_gl STATIC src/render/gl/GlRenderer.cpp)
target_link_libraries(ou_render_gl PUBLIC ou_render_common PRIVATE PkgConfig::EPOXY)
target_include_directories(ou_render_gl PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen) # tonemap.glsl.inc (post pass)

# ---- ou_render_vk: Vulkan 1.3 backend; SPIR-V compiled at build time by glslc and embedded ----
find_package(Vulkan REQUIRED COMPONENTS glslc)
set(OU_VK_SPV_DIR ${CMAKE_CURRENT_BINARY_DIR}/vk_shaders)
file(MAKE_DIRECTORY ${OU_VK_SPV_DIR})
set(OU_VK_SPV)
foreach(s scene.vert scene.frag text.vert text.frag line.vert line.frag shadow.vert shadow.frag post.vert post.frag
        prepass.vert prepass.frag fsr_mask.vert fsr_mask.frag fsr_camera.frag fsr_fx.frag fsr_splash.frag fsr_weather.frag)
  set(src ${CMAKE_CURRENT_SOURCE_DIR}/src/render/vk/shaders/${s})
  add_custom_command(OUTPUT ${OU_VK_SPV_DIR}/${s}.inc
                     COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 -O -mfmt=num -I ${CMAKE_CURRENT_SOURCE_DIR}/src/render -o ${OU_VK_SPV_DIR}/${s}.inc ${src}
                     DEPENDS ${src} ${OU_SUN_GLSL_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/underwater.glsl
                             ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/atmosphere.glsl
                             ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/filter.glsl
                             ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/tonemap.glsl
                             ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/caustics.glsl
                             ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/waves.glsl VERBATIM)
  list(APPEND OU_VK_SPV ${OU_VK_SPV_DIR}/${s}.inc)
endforeach()
# Original particle effects (src/fx sprites).
foreach(s fx.vert fx.frag)
  set(src ${CMAKE_CURRENT_SOURCE_DIR}/src/render/vk/shaders/${s})
  add_custom_command(OUTPUT ${OU_VK_SPV_DIR}/${s}.inc
                     COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 -O -mfmt=num -o ${OU_VK_SPV_DIR}/${s}.inc ${src}
                     DEPENDS ${src} VERBATIM)
  list(APPEND OU_VK_SPV ${OU_VK_SPV_DIR}/${s}.inc)
endforeach()
# Enhanced water + splash (shared GLSL with GL, -DVULKAN=1 selects the descriptor syntax; water.frag includes sun.glsl
# and shaders/caustics.glsl; water.vert, water.frag and caustics.glsl include shaders/waves.glsl; ocean.comp: the FFT ocean).
foreach(s water.vert water.frag splash.vert splash_splat.frag splash_screen.vert splash_blur.frag splash_composite.frag
        ocean.comp)
  set(src ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/${s})
  add_custom_command(OUTPUT ${OU_VK_SPV_DIR}/${s}.inc
                     COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 -O -mfmt=num -I ${CMAKE_CURRENT_SOURCE_DIR}/src/render -o ${OU_VK_SPV_DIR}/${s}.inc ${src}
                     DEPENDS ${src} ${OU_SUN_GLSL_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/caustics.glsl
                             ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/waves.glsl VERBATIM)
  list(APPEND OU_VK_SPV ${OU_VK_SPV_DIR}/${s}.inc)
endforeach()
# post.frag includes shaders/underwater.glsl (+ caustics.glsl, waves.glsl), shaders/atmosphere.glsl, shaders/filter.glsl and
# shaders/tonemap.glsl.
set_property(SOURCE ${OU_VK_SPV_DIR}/post.frag.inc APPEND PROPERTY OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/underwater.glsl
             ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/atmosphere.glsl ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/filter.glsl
             ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/caustics.glsl ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/waves.glsl
             ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/tonemap.glsl)
# Ray-traced scene variant (-DOU_RT: ray queries against the scene TLAS); only loaded on RT-capable devices.
foreach(stage vert frag)
  set(src ${CMAKE_CURRENT_SOURCE_DIR}/src/render/vk/shaders/scene.${stage})
  add_custom_command(OUTPUT ${OU_VK_SPV_DIR}/scene_rt.${stage}.inc
                     COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 -O -mfmt=num -DOU_RT -fshader-stage=${stage} -I ${CMAKE_CURRENT_SOURCE_DIR}/src/render -o ${OU_VK_SPV_DIR}/scene_rt.${stage}.inc ${src}
                     DEPENDS ${src} ${OU_SUN_GLSL_SRC} VERBATIM)
  list(APPEND OU_VK_SPV ${OU_VK_SPV_DIR}/scene_rt.${stage}.inc)
endforeach()
# Enhanced water with ray-traced reflections (-DOU_RT: water.frag traces the TLAS instead of the SSR march).
add_custom_command(OUTPUT ${OU_VK_SPV_DIR}/water_rt.frag.inc
                   COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 -O -mfmt=num -DOU_RT -I ${CMAKE_CURRENT_SOURCE_DIR}/src/render -o ${OU_VK_SPV_DIR}/water_rt.frag.inc ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/water.frag
                   DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/water.frag ${OU_SUN_GLSL_SRC}
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/caustics.glsl
                           ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/waves.glsl VERBATIM)
list(APPEND OU_VK_SPV ${OU_VK_SPV_DIR}/water_rt.frag.inc)
# RT AO / GI (rt_indirect.comp): trace, trace from MSAA depth, denoise.
foreach(def OU_TRACE OU_MS OU_DENOISE)
  string(REPLACE "OU_TRACE" "rt_indirect" name ${def})
  string(REPLACE "OU_MS" "rt_indirect_ms" name ${name})
  string(REPLACE "OU_DENOISE" "rt_denoise" name ${name})
  set(src ${CMAKE_CURRENT_SOURCE_DIR}/src/render/vk/shaders/rt_indirect.comp)
  add_custom_command(OUTPUT ${OU_VK_SPV_DIR}/${name}.comp.inc
                     COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 -O -mfmt=num -D${def} -I ${CMAKE_CURRENT_SOURCE_DIR}/src/render -o ${OU_VK_SPV_DIR}/${name}.comp.inc ${src}
                     DEPENDS ${src} ${OU_SUN_GLSL_SRC} VERBATIM)
  list(APPEND OU_VK_SPV ${OU_VK_SPV_DIR}/${name}.comp.inc)
endforeach()
# AMD FSR 3.1.6 upscaling + frame generation (FidelityFX SDK 2.4.0 ffx-api, third_party/fidelityfx-sdk; sets OU_HAS_FSR).
add_subdirectory(third_party/fidelityfx-sdk)
add_library(ou_render_vk STATIC src/render/vk/VkRenderer.cpp src/render/vk/VkFsr.cpp ${OU_VK_SPV})
target_compile_definitions(ou_render_vk PRIVATE OU_HAS_FSR=$<BOOL:${OU_HAS_FSR}>)
if(OU_HAS_FSR)
  target_link_libraries(ou_render_vk PRIVATE ou_ffx)
endif()
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
           COMMAND ou-viewer --renderer opengl --frames 5 --screenshot ${CMAKE_CURRENT_BINARY_DIR}/viewer_placeholder.ppm)
  set_tests_properties(viewer_placeholder_screenshot PROPERTIES SKIP_RETURN_CODE 77)
  # Same, Vulkan backend; exits 77 (skip) without a display or a usable Vulkan 1.3 device.
  add_test(NAME viewer_vulkan_placeholder_screenshot
           COMMAND ou-viewer --renderer vulkan --frames 5 --screenshot ${CMAKE_CURRENT_BINARY_DIR}/viewer_vulkan_placeholder.ppm)
  # GL vs Vulkan placeholder frames without overlay must be near-identical (mean |diff| <= 2/255).
  set(_v $<TARGET_FILE:ou-viewer>)
  set(_d ${CMAKE_CURRENT_BINARY_DIR})
  add_test(NAME viewer_vulkan_matches_gl
           COMMAND sh -ec "\"${_v}\" --renderer opengl --preset original --no-overlay --frames 3 --screenshot ${_d}/cmp_gl.ppm; \"${_v}\" --renderer vulkan --preset original --no-overlay --frames 3 --screenshot ${_d}/cmp_vk.ppm; \"$<TARGET_FILE:ou-imgdiff>\" ${_d}/cmp_gl.ppm ${_d}/cmp_vk.ppm 2")
  # Same with the Enhanced preset (4x MSAA + sun shadow map) plus FXAA and HDR tonemap; RT off (GL can't trace).
  add_test(NAME viewer_vulkan_matches_gl_enhanced
           COMMAND sh -ec "\"${_v}\" --renderer opengl --no-overlay --frames 3 --preset enhanced --rt off --fxaa on --hdr on --screenshot ${_d}/cmp_gl_enh.ppm; \"${_v}\" --renderer vulkan --no-overlay --frames 3 --preset enhanced --rt off --fxaa on --hdr on --screenshot ${_d}/cmp_vk_enh.ppm; \"$<TARGET_FILE:ou-imgdiff>\" ${_d}/cmp_gl_enh.ppm ${_d}/cmp_vk_enh.ppm 2")
  # The optional AgX tone mapping (shaders/tonemap.glsl) matches between the backends too.
  add_test(NAME viewer_vulkan_matches_gl_agx
           COMMAND sh -ec "\"${_v}\" --renderer opengl --no-overlay --frames 3 --tonemap agx --screenshot ${_d}/cmp_gl_agx.ppm; \"${_v}\" --renderer vulkan --no-overlay --frames 3 --tonemap agx --screenshot ${_d}/cmp_vk_agx.ppm; \"$<TARGET_FILE:ou-imgdiff>\" ${_d}/cmp_gl_agx.ppm ${_d}/cmp_vk_agx.ppm 2")
  add_executable(ou_vk_tests tests/test_vk_renderer.cpp)
  target_link_libraries(ou_vk_tests PRIVATE ou_render_vk ou_placeholder GTest::gtest_main)
  # Mesa RADV leaks 256 bytes per process (see tests/lsan_vulkan.supp).
  set(OU_VK_TEST_ENV "LSAN_OPTIONS=fast_unwind_on_malloc=0:suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan_vulkan.supp")
  gtest_discover_tests(ou_vk_tests PROPERTIES ENVIRONMENT "${OU_VK_TEST_ENV}")
  set_tests_properties(viewer_vulkan_placeholder_screenshot viewer_vulkan_matches_gl viewer_vulkan_matches_gl_enhanced viewer_vulkan_matches_gl_agx PROPERTIES
                       SKIP_RETURN_CODE 77 ENVIRONMENT "${OU_VK_TEST_ENV}")
  add_executable(ou_shader_tests tests/test_shaders.cpp)
  target_link_libraries(ou_shader_tests PRIVATE ou_render_common GTest::gtest_main)
  gtest_discover_tests(ou_shader_tests DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 1200) # every-shader sweep: ~570 s under ASan
endif()

# ---- ou_anim: skeletons, animation clips, pose sampling, CPU skinning (docs/formats/ANIMATION.md) ----
add_library(ou_anim STATIC src/anim/Anim.cpp src/anim/Blend.cpp src/anim/Hair.cpp src/anim/Ik.cpp)
target_include_directories(ou_anim PUBLIC src)
target_link_libraries(ou_anim PUBLIC ou_cdc glm::glm)
if(OU_BUILD_TESTS)
  add_executable(ou_anim_tests tests/test_anim.cpp)
  target_link_libraries(ou_anim_tests PRIVATE ou_anim ou_formats GTest::gtest_main)
  gtest_discover_tests(ou_anim_tests)
endif()

# ---- ou_fx: original particle effects (GenericFXObject), deterministic sim, sprites (docs/formats/EFFECTS.md) ----
add_library(ou_fx STATIC src/fx/Fx.cpp src/fx/Weather.cpp)
target_link_libraries(ou_fx PUBLIC ou_formats ou_physics glm::glm)
if(OU_BUILD_TESTS)
  add_executable(ou_fx_tests tests/test_fx.cpp)
  target_link_libraries(ou_fx_tests PRIVATE ou_fx ou_formats GTest::gtest_main)
  gtest_discover_tests(ou_fx_tests DISCOVERY_TIMEOUT 30)
endif()

# ---- ou_formats: PCD9 textures, render models, terrain -> ou::scene; ou-asset CLI ----------
add_library(ou_formats STATIC src/formats/Texture.cpp src/formats/Model.cpp src/formats/Scene.cpp src/formats/Level.cpp
            src/formats/Shader.cpp src/formats/Uber.cpp src/formats/AnimGraph.cpp src/formats/Births.cpp)
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

# ---- ou_physics: object collision primitives; rigid bodies, joints, loose bodies on Jolt (docs/formats/PHYSICS.md) ----
# Jolt Physics 5.6.0 (MIT, third_party/jolt/LICENSE), fetched once into the build tree (as DXC: _deps keeps it, so a
# reconfigure or rebuild is offline). Deterministic float math, no AVX (the AppImage runs on any x86-64-v2 CPU), no
# samples/tests/GPU compute. Linked PRIVATE: only Physics.cpp sees Jolt headers and flags.
include(FetchContent)
foreach(v USE_AVX USE_AVX2 USE_AVX512 USE_LZCNT USE_TZCNT USE_F16C USE_FMADD ENABLE_ALL_WARNINGS OVERRIDE_CXX_FLAGS
          GENERATE_DEBUG_SYMBOLS DEBUG_RENDERER_IN_DEBUG_AND_RELEASE PROFILER_IN_DEBUG_AND_RELEASE
          ENABLE_OBJECT_STREAM ENABLE_INSTALL JPH_USE_DX12 JPH_USE_VK JPH_USE_MTL JPH_USE_CPU_COMPUTE
          FLOATING_POINT_EXCEPTIONS_ENABLED)
  set(${v} OFF)
endforeach()
set(CROSS_PLATFORM_DETERMINISTIC ON)
# Jolt's Build/ resets CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE from its own option: follow OU_LTO (Release only).
set(INTERPROCEDURAL_OPTIMIZATION OFF)
if(OU_IPO_OK)
  set(INTERPROCEDURAL_OPTIMIZATION ON)
endif()
set(CPP_RTTI_ENABLED ON) # our code (and UBSan's vptr checks) subclasses / inspects Jolt types
FetchContent_Declare(ou_jolt
  URL https://github.com/jrouwe/JoltPhysics/archive/refs/tags/v5.6.0.tar.gz
  URL_HASH SHA256=6e069ee0172478cc78182047aac87e5310ba14a67a53348ae14cc37801fd3f8e
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR Build)
FetchContent_MakeAvailable(ou_jolt)
add_library(ou_physics STATIC src/physics/Physics.cpp)
target_link_libraries(ou_physics PUBLIC ou_collision ou_formats glm::glm PRIVATE Jolt)
if(OU_BUILD_TESTS)
  add_executable(ou_physics_tests tests/test_physics.cpp)
  target_link_libraries(ou_physics_tests PRIVATE ou_physics GTest::gtest_main)
  gtest_discover_tests(ou_physics_tests DISCOVERY_TIMEOUT 30)
endif()

# ---- ou_audio: IMA ADPCM, FSB4, Wave sections, .mul streams; SDL3 mixer; ou-audio CLI (docs/formats/AUDIO.md) ----
add_library(ou_audio STATIC src/audio/Audio.cpp src/audio/Dsp.cpp src/audio/Mixer.cpp src/audio/Mus.cpp src/audio/MusicPlayer.cpp src/audio/SoundPlex.cpp)
target_link_libraries(ou_audio PUBLIC ou_cdc glm::glm PRIVATE SDL3::SDL3)
add_executable(ou-audio tools/ou-audio/main.cpp)
target_link_libraries(ou-audio PRIVATE ou_audio SDL3::SDL3)
if(OU_BUILD_TESTS)
  add_executable(ou_audio_tests tests/test_audio.cpp)
  target_link_libraries(ou_audio_tests PRIVATE ou_audio SDL3::SDL3 GTest::gtest_main)
  gtest_discover_tests(ou_audio_tests DISCOVERY_TIMEOUT 30)
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
# ---- ou_input: control binding names <-> SDL3 devices, device state from events, recordings (ou-game, launcher) ----
add_library(ou_input STATIC src/platform/Input.cpp)
target_link_libraries(ou_input PUBLIC ou_config SDL3::SDL3)
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
  target_link_libraries(ou-launcher PRIVATE ou_config ou_input ou_update ou_importer ou_locale SDL3::SDL3 Qt6::Widgets Qt6::Concurrent)
  if(OU_BUILD_TESTS)
    add_test(NAME launcher_self_test COMMAND ou-launcher --self-test --config ${CMAKE_CURRENT_BINARY_DIR}/launcher-test-config.json)
    set_tests_properties(launcher_self_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;OU_DISABLE_UPDATES=1")
    # Every launcher page driven through its widgets, offscreen (tests/test_launcher_gui.cpp; real scan with OU_GAME_DIR).
    add_executable(launcher_gui_tests tests/test_launcher_gui.cpp)
    set_target_properties(launcher_gui_tests PROPERTIES AUTOMOC ON)
    target_compile_definitions(launcher_gui_tests PRIVATE OU_VERSION="${PROJECT_VERSION}")
    target_link_libraries(launcher_gui_tests PRIVATE ou_config ou_input ou_update ou_importer ou_locale SDL3::SDL3
                          Qt6::Widgets Qt6::Concurrent PkgConfig::SODIUM GTest::gtest)
    gtest_discover_tests(launcher_gui_tests DISCOVERY_TIMEOUT 30 PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  endif()
else()
  message(STATUS "Qt6 Widgets not found: ou-launcher not built")
endif()

# ---- ou_locale (locals.bin, .sch, cinstream subtitles), ou_video (Bink via libav), ou-movie, ou-text ----
# docs/formats/TEXT.md, docs/formats/VIDEO.md
add_library(ou_locale STATIC src/locale/Locale.cpp)
target_link_libraries(ou_locale PUBLIC ou_cdc)
add_executable(ou-text tools/ou-text/main.cpp)
target_link_libraries(ou-text PRIVATE ou_locale ou_cine)
pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET libavformat libavcodec libswscale libswresample libavutil)
add_library(ou_video STATIC src/video/Movie.cpp src/video/Player.cpp)
target_link_libraries(ou_video PUBLIC ou_locale SDL3::SDL3 PRIVATE ou_ui PkgConfig::LIBAV) # ou_ui: subtitles in the game font
add_executable(ou-movie tools/ou-movie/main.cpp)
target_link_libraries(ou-movie PRIVATE ou_video ou_ui)
install(TARGETS ou-movie ou-text)
if(OU_BUILD_TESTS)
  add_executable(ou_locale_tests tests/test_locale.cpp)
  target_link_libraries(ou_locale_tests PRIVATE ou_locale ou_video GTest::gtest_main)
  gtest_discover_tests(ou_locale_tests DISCOVERY_TIMEOUT 30)
  # Plays 30 frames of intro.bik into a window and writes a PPM; 77 = skipped (no OU_GAME_DIR or display).
  add_test(NAME movie_intro_screenshot
           COMMAND sh -c "[ -n \"$OU_GAME_DIR\" ] || exit 77; exec \"$<TARGET_FILE:ou-movie>\" \"$OU_GAME_DIR/Movies/intro.bik\" --frames 30 --screenshot ${CMAKE_CURRENT_BINARY_DIR}/movie_intro.ppm")
  # SDL_Renderer may pick its Vulkan backend: same RADV leak as the Vulkan tests.
  set_tests_properties(movie_intro_screenshot PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT
                       "LSAN_OPTIONS=fast_unwind_on_malloc=0:suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan_vulkan.supp")
endif()

# ---- ou_script: script types, trigger volumes/planes, switches, doors, blocks (docs/formats/SCRIPTS.md) ----
add_library(ou_script STATIC src/script/Script.cpp src/script/Vm.cpp)
target_link_libraries(ou_script PUBLIC ou_formats ou_anim ou_collision glm::glm nlohmann_json::nlohmann_json)
if(OU_BUILD_TESTS)
  add_executable(ou_script_tests tests/test_script.cpp)
  target_link_libraries(ou_script_tests PRIVATE ou_script GTest::gtest_main)
  gtest_discover_tests(ou_script_tests DISCOVERY_TIMEOUT 30)
endif()

# ---- ou_cine: in-engine cutscene streams (cinstream .mul), camera/actor playback (docs/formats/CINEMATICS.md) ----
add_library(ou_cine STATIC src/cine/Cine.cpp)
target_link_libraries(ou_cine PUBLIC ou_script ou_locale glm::glm)
if(OU_BUILD_TESTS)
  add_executable(ou_cine_tests tests/test_cine.cpp)
  target_link_libraries(ou_cine_tests PRIVATE ou_cine GTest::gtest_main)
  gtest_discover_tests(ou_cine_tests DISCOVERY_TIMEOUT 30)
endif()

# ---- ou_game: world, Lara controller, follow camera, input (docs/GAMEPLAY.md); ou-game ----------
add_library(ou_game STATIC src/game/Player.cpp src/game/World.cpp src/game/Input.cpp src/game/Stream.cpp src/game/Combat.cpp src/game/Ik.cpp src/game/Aim.cpp
  src/game/Water.cpp src/game/Climb.cpp src/game/Grapple.cpp src/game/Traverse.cpp src/game/WallJump.cpp src/game/ai/Enemy.cpp src/game/ai/EnemyData.cpp src/game/ai/Nav.cpp src/game/Progress.cpp
  src/game/Vehicle.cpp src/game/Weather.cpp src/game/Gadgets.cpp src/game/Camera.cpp src/game/Bend.cpp src/game/Feedback.cpp
  src/game/Melee.cpp)
target_link_libraries(ou_game PUBLIC ou_collision ou_anim ou_formats ou_script ou_cine ou_input glm::glm PRIVATE SDL3::SDL3)
# ---- ou_save: save games, atomic slots (docs/SAVES.md) ----
add_library(ou_save STATIC src/save/Save.cpp)
target_include_directories(ou_save PUBLIC src)
target_link_libraries(ou_save PUBLIC ou_game nlohmann_json::nlohmann_json PRIVATE ou_config ZLIB::ZLIB)
# ou_game's object collision + physics doors (src/game/Objects.cpp, src/physics)
target_sources(ou_game PRIVATE src/game/Objects.cpp src/game/Breakables.cpp src/game/Hazards.cpp)
target_link_libraries(ou_game PUBLIC ou_physics)
# ---- ou_ui: menus, PDA page, HUD as renderer-agnostic quads (docs/GAMEPLAY.md "Menus") ----
add_library(ou_ui STATIC src/ui/Ui.cpp src/ui/Hud.cpp src/ui/Art.cpp src/ui/Pda.cpp)
target_link_libraries(ou_ui PUBLIC ou_render_common ou_locale ou_formats)
add_executable(ou-game src/app/game/main.cpp src/app/game/Autopilot.cpp src/app/game/SelfTest.cpp src/app/game/Cinematic.cpp src/app/game/Sound.cpp)
target_link_libraries(ou-game PRIVATE ou_game ou_save ou_ui ou_render_gl ou_render_vk ou_config ou_video ou_audio ou_fx)
if(OU_BUILD_TESTS)
  add_executable(ou_game_tests tests/test_game.cpp tests/test_combat.cpp tests/test_feedback.cpp tests/test_selftest.cpp
                 src/app/game/SelfTest.cpp src/app/game/Autopilot.cpp)
  target_link_libraries(ou_game_tests PRIVATE ou_game GTest::gtest_main)
  target_compile_definitions(ou_game_tests PRIVATE OU_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  gtest_discover_tests(ou_game_tests DISCOVERY_TIMEOUT 30)
  # The ballcourt lever replay at 30 Hz and 240 Hz render rates (360 ticks each, both processes at once) must end on the
  # same simulated state (docs/GAMEPLAY.md "Timing"); --draw-last paces frames and ticks at the render rate but draws only
  # the final frame, so it fits Debug+ASan (tools/release.sh). 77 = skipped (no OU_GAME_DIR or display).
  add_test(NAME game_render_rate_smoke COMMAND sh -c [=[
    [ -n "$OU_GAME_DIR" ] && [ -n "$DISPLAY$WAYLAND_DISPLAY" ] || exit 77
    game=$0 replay=$1 out=$(mktemp -d)
    run() { "$game" --game-dir "$OU_GAME_DIR" --renderer opengl --level 'pc-w\l4_ballcourt.drm' --spawn -20066,14150,3540,0 \
              --input-script "$replay" --no-audio --no-movies --no-overlay --draw-last --width 160 --height 120 \
              --render-hz "$1" --frames "$2" 2>&1 | grep 'final tick' > "$out/$1"; }
    run 30 180 & run 240 1440 & wait
    a=$(cat "$out/30") b=$(cat "$out/240"); rm -rf "$out"; echo "30 Hz:  $a"; echo "240 Hz: $b"
    [ -n "$a" ] && [ "$a" = "$b" ]]=] $<TARGET_FILE:ou-game> ${CMAKE_SOURCE_DIR}/tests/replays/ballcourt_door_lever.txt)
  set_tests_properties(game_render_rate_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
  # Recorded device sessions through ou-game's own input path (--replay-input, docs/GAMEPLAY.md "Recorded input"): the
  # ballcourt lever replay as keyboard + mouse events and as gamepad events with a hotplug, each after a trip through the
  # pause menu, must end where the scripted actions do (keyboard within 5 units, the stick within 60).
  add_test(NAME game_input_replay_smoke COMMAND sh -c [=[
    [ -n "$OU_GAME_DIR" ] && [ -n "$DISPLAY$WAYLAND_DISPLAY" ] || exit 77
    game=$0 dir=$1 out=$(mktemp -d)
    run() { "$game" --game-dir "$OU_GAME_DIR" --renderer opengl --level 'pc-w\l4_ballcourt.drm' --spawn -20066,14150,3540,0 \
              "$@" --no-audio --no-movies --no-overlay --draw-last --width 160 --height 120 --frames 360 2>&1; }
    run --input-script "$dir/ballcourt_door_lever.txt" > "$out/script" &
    run --replay-input "$dir/ballcourt_door_lever_keyboard.events" > "$out/keyboard" &
    run --replay-input "$dir/ballcourt_door_lever_gamepad.events" > "$out/gamepad" & wait
    pos() { grep 'final tick' "$out/$1" | sed 's/.* pos \([^ ]*\) \([^ ]*\) \([^ ]*\) .*/\1 \2 \3/'; }
    near() { echo "$(pos script) $(pos $1) $2" | awk '{ d = 0; for (i = 1; i <= 3; ++i) { e = $i - $(i + 3); d += e * e }
                                                       exit !(NF == 7 && d <= $7 * $7) }'; }
    for f in script keyboard gamepad; do echo "$f: $(grep 'final tick' "$out/$f")"; done
    grep -E 'gamepad|menu action' "$out/gamepad"
    ok=1
    near keyboard 5 || { echo "keyboard replay ended elsewhere"; ok=0; }
    near gamepad 60 || { echo "gamepad replay ended elsewhere"; ok=0; }
    grep -q 'tick 8 gamepad none' "$out/gamepad" && grep -q 'tick 9 gamepad ' "$out/gamepad" || { echo "no hotplug"; ok=0; }
    rm -rf "$out"; [ $ok = 1 ]]=] $<TARGET_FILE:ou-game> ${CMAKE_SOURCE_DIR}/tests/replays)
  set_tests_properties(game_input_replay_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
  # Puzzle replays (docs/GAMEPLAY.md "Receptacle replays"): from a spawn near the puzzle, input scripts recorded by
  # ou-game --autopilot pick a portable up, carry it to its receptacle and set it in (or pull it out, press a plate);
  # each run must end on the patterns given: the "portables held N inserted P>R" exit line, Lara's state, and the
  # downstream reaction tru.exe's scripts define, from --report-objects REPORT (comma-separated uids): "calls TO:VT ..."
  # (script calls to / from them) and "object UID NAME pos .. [graph S] [joint P] [uber S,.. jN X] [in R]" per uid.
  # --no-cinematics: the level-start cinematics still run their events and placement, unshown. 77 = skipped (no
  # OU_GAME_DIR or display).
  function(ou_receptacle_replay name unit spawn frames report)
    add_test(NAME game_replay_${name} COMMAND sh -c [=[
      [ -n "$OU_GAME_DIR" ] && [ -n "$DISPLAY$WAYLAND_DISPLAY" ] || exit 77
      game=$0 script=$1 unit=$2 spawn=$3 frames=$4 report=$5; shift 5
      out=$("$game" --game-dir "$OU_GAME_DIR" --renderer opengl --level "pc-w\\$unit.drm" --spawn "$spawn" --input-script "$script" \
              --no-audio --no-movies --no-cinematics --no-overlay --draw-last --sync-loads --width 160 --height 120 --frames "$frames" \
              --report-objects "$report" 2>&1)
      echo "$out" | grep -E '^ou-game: (final tick|portables held|calls|object)'
      ok=1
      for p; do echo "$out" | grep -Eq -- "$p" || { echo "missing: $p"; ok=0; }; done
      [ $ok = 1 ]]=] $<TARGET_FILE:ou-game> ${CMAKE_SOURCE_DIR}/tests/replays/${name}.txt ${unit} ${spawn} ${frames} ${report} ${ARGN})
    set_tests_properties(game_replay_${name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
  endfunction()
  # Gameplay self-tests (docs/SELFTEST.md): ou-game --self-test tests/selftests/<name>.json enters the scenario's level,
  # runs its steps at fixed 60 Hz ticks and exits 0 only when every step's expectations held against the World's own
  # state; the report goes to <build>/selftest/<name>.report.json. 77 = skipped (no OU_GAME_DIR or display).
  file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)
  function(ou_self_test name)
    add_test(NAME game_selftest_${name} COMMAND sh -c [=[
      [ -n "$OU_GAME_DIR" ] && [ -n "$DISPLAY$WAYLAND_DISPLAY" ] || exit 77
      "$0" --game-dir "$OU_GAME_DIR" --renderer opengl --self-test "$1" --self-test-report "$2" \
        --no-audio --no-movies --no-overlay --draw-last --sync-loads --width 160 --height 120 2>"$2.log"
      code=$?
      [ $code = 0 ] || grep -E 'self-test|error' "$2.log" | tail -n 40
      exit $code]=] $<TARGET_FILE:ou-game> ${CMAKE_SOURCE_DIR}/tests/selftests/${name}.json ${CMAKE_BINARY_DIR}/selftest/${name}.report.json)
    set_tests_properties(game_selftest_${name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
  endfunction()
  file(GLOB OU_SELF_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/selftests/*.json)
  foreach(scenario ${OU_SELF_TESTS})
    get_filename_component(name ${scenario} NAME_WE)
    ou_self_test(${name})
  endforeach()
  # throne 5891: the weight bears its press plate into "in position" (-150..-90) -> seal 5701 vt 54
  ou_receptacle_replay(traprooms_throne_insert l4_traprooms 7400,-34900,4440,-135 900 5883,5891,5701
                       "portables held 0 inserted .*5883>5891" "calls .* 5701:54( |$)" "object 5891 .* joint -(1[0-4][0-9]|150|9[0-9])\\.[0-9] uber 2 ")
  # lever 7189 inserted (graph 1 -> 0), its multibody 79 watch on the slider (a joint); door 7227 waits for the hang
  ou_receptacle_replay(valaskjalf_pole_lever_insert l5_valaskjalf_entryb 1700,-45719,-2499,180 600 7190,7189,7227
                       "portables held 0 inserted .*7190>7189" "object 7189 .* graph 0 joint ")
  # pole 2967 out of hole 2969: its vt 20(0) runs elevator 2960 vt 54 (graph 0 -> 1) and switch 2961 vt 62
  ou_receptacle_replay(puzzleroom_pole_remove l2_puzzleroom -17500,40565,5540,180 400 2967,2969,2960,2961
                       "portables held 2967 inserted" "state idle" "calls .*2960:54 2961:62" "object 2960 .* graph 1")
  # cog wheel 7312 section 1 takes the gem (0 -> 1)
  ou_receptacle_replay(valgrind_cog_gem_insert l5_valgrind 0,3072,180,180 1500 7498,7312
                       "portables held 0 inserted .*7498>7312" "object 7312 .* uber [0-9]+,1,")
  # pole 6503 into lever 6502 (graph 0 -> 1, the slider watch), push block 6504 under the pole, onto the block (its
  # ledge loop), a jump to the pole: the hang slides the lever to -600 and door 6505 rises (twice the drop)
  ou_receptacle_replay(ballcourt_pole_lever_block l4_ballcourt -20300,14700,3484,-90 2700 6503,6502,6504,6505
                       "portables held 0 inserted .*6503>6502" "object 6502 .* graph 1 joint -(3[5-9][0-9]|[45][0-9]{2}|600)\\."
                       "object 6504 .* pos -20[01][0-9]{2}\\.[0-9] 14[34][0-9]{2}\\." "state polehang"
                       "object 6505 .* body -?0\\.0 -?0\\.0 (7[0-9]{2}|[89][0-9]{2}|1[0-2][0-9]{2})\\.")
  # calendar 6506: a push handle gripped and pushed turns the ring (hinge joint 2), section 1 back to waiting (3)
  ou_receptacle_replay(ballcourt_calendar_handle_turn l4_ballcourt -14240,10200,3700,90 700 6506
                       "object 6506 .* uber [0-9]+,3,.* j2 (0\\.[2-9]|[1-9])")
  # The tomb's full solution by ordinary input (the replay's header): both weights thrown through door 223 from plate
  # 219 (219 vt 54 / 55 -> 223 vt 57 / 58), the roll through it, both put down on plate 222, gate 45's ring grappled
  # and pulled from plate 221: both plates at their pressed joint (-50), locks 69 / 70 out (-1250), the gate pulled
  # open on its hinge (>= 85 degrees; 45 vt 54), the weights lying on plate 222.
  ou_receptacle_replay(baldrs_plates_gate l1_baldrs_tomb -5000,-10000,2560,180 5800 219,221,222,45,223,69,70,218,220
                       "portables held 0" "calls .* 219:54 223:57" "calls .* 219:55 223:58" "calls .* 45:54"
                       "object 221 .* joint -(4[5-9]|50)\\.[0-9]" "object 222 .* joint -(4[5-9]|50)\\.[0-9]"
                       "object 69 .* joint -12[0-9]{2}\\.[0-9]" "object 70 .* joint -12[0-9]{2}\\.[0-9]"
                       "object 45 .* joint (8[5-9]|9[0-9])\\.[0-9]"
                       "object 218 .* pos 1[0-5][0-9]{2}\\.[0-9] -(9[89]|10[0-5])[0-9]{2}\\."
                       "object 220 .* pos 1[0-5][0-9]{2}\\.[0-9] -(9[89]|10[0-5])[0-9]{2}\\.")
  # Vulkan + RT sun shadows with the translated shaders on the spawns that lost the device (non-finite ray origins,
  # docs/RENDERING.md "Ray tracing"); 77 = skipped (no OU_GAME_DIR, display or ray-query device).
  add_test(NAME game_vulkan_rt_smoke COMMAND sh -c [=[
    [ -n "$OU_GAME_DIR" ] && [ -n "$DISPLAY$WAYLAND_DISPLAY" ] || exit 77
    game=$0
    for spot in 'l4_ballcourt -20066,14150,3540,0' 'l1_kraken_room -2204,-15106,2212,180'; do
      set -- $spot
      out=$("$game" --game-dir "$OU_GAME_DIR" --renderer vulkan --preset enhanced --shaders original --rt shadows \
              --level "pc-w\\$1.drm" --spawn "$2" --no-audio --no-movies --no-overlay --width 640 --height 360 --frames 40 2>&1)
      code=$?
      echo "$out" | grep -q 'ray queries yes' || exit 77
      echo "$out" | grep -E '^ou-game: (final tick|RT )'
      [ $code -eq 0 ] && echo "$out" | grep -q '^ou-game: RT shadows .*BLAS' && ! echo "$out" | grep -q 'NOT ACTIVE' ||
        { echo "$out" | tail -n 20; echo "$1: exit $code"; exit 1; }
    done]=] $<TARGET_FILE:ou-game>)
  set_tests_properties(game_vulkan_rt_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900 ENVIRONMENT "${OU_VK_TEST_ENV}")
  # Lavapipe (Mesa's CPU Vulkan driver: one memory type, device-local and host-visible): ou_vk_tests, then an ou-game
  # frame of real data with FSR, under validation. 0.30.0 crashed there (a mapped allocation landed in an unmapped
  # device-local block) and FSR found no memory type (ffx ou patch 8). OU_LAVAPIPE_ICD = its ICD json; 77 = no lavapipe.
  find_file(OU_LAVAPIPE_ICD NAMES lvp_icd.json lvp_icd.x86_64.json PATHS /usr/share/vulkan/icd.d /etc/vulkan/icd.d
            NO_DEFAULT_PATH)
  add_test(NAME vulkan_lavapipe COMMAND sh -c [=[
    [ -f "$2" ] || { echo "no lavapipe ICD (OU_LAVAPIPE_ICD)"; exit 77; }
    export VK_DRIVER_FILES="$2" VK_ICD_FILENAMES="$2" SDL_VIDEO_DRIVER=offscreen OU_VK_VALIDATION=1
    out=$("$1" 2>&1); code=$?
    echo "$out" | grep -E '^\[ +(OK|SKIPPED|FAILED) +\]|\[vk error\]'
    [ $code -eq 0 ] && ! echo "$out" | grep -q '\[vk error\]' || exit 1
    [ -n "$OU_GAME_DIR" ] || exit 0
    out=$("$0" --game-dir "$OU_GAME_DIR" --renderer vulkan --level 'pc-w\l4_pool.drm' --upscaler fsr --no-audio \
            --no-movies --width 320 --height 180 --frames 5 2>&1); code=$?
    echo "$out" | grep -E '^ou-game: (Vulkan|final tick|RT )|FSR|\[vk error\]'
    [ $code -eq 0 ] && echo "$out" | grep -q llvmpipe && ! echo "$out" | grep -qE '\[vk error\]|FSR off']=]
    $<TARGET_FILE:ou-game> $<TARGET_FILE:ou_vk_tests> ${OU_LAVAPIPE_ICD})
  set_tests_properties(vulkan_lavapipe PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900 ENVIRONMENT "${OU_VK_TEST_ENV}")
  add_executable(ou_ai_tests tests/test_ai.cpp)
  target_link_libraries(ou_ai_tests PRIVATE ou_game GTest::gtest_main)
  gtest_discover_tests(ou_ai_tests DISCOVERY_TIMEOUT 30)
  add_executable(ou_save_tests tests/test_save.cpp)
  target_link_libraries(ou_save_tests PRIVATE ou_save ou_render_common GTest::gtest_main)
  gtest_discover_tests(ou_save_tests DISCOVERY_TIMEOUT 60)
  add_executable(ou_progress_tests tests/test_progress.cpp)
  target_link_libraries(ou_progress_tests PRIVATE ou_ui ou_game ou_save ZLIB::ZLIB GTest::gtest_main)
  gtest_discover_tests(ou_progress_tests DISCOVERY_TIMEOUT 60)
  add_executable(ou_gates_tests tests/test_gates.cpp src/app/game/Autopilot.cpp)
  target_link_libraries(ou_gates_tests PRIVATE ou_game ou_save ZLIB::ZLIB GTest::gtest_main)
  gtest_discover_tests(ou_gates_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 1200)
  add_executable(ou_hud_tests tests/test_hud.cpp)
  target_link_libraries(ou_hud_tests PRIVATE ou_ui GTest::gtest_main)
  gtest_discover_tests(ou_hud_tests DISCOVERY_TIMEOUT 30)
  add_executable(ou_vehicle_tests tests/test_vehicle.cpp src/app/game/Autopilot.cpp)
  target_link_libraries(ou_vehicle_tests PRIVATE ou_save GTest::gtest_main)
  target_compile_definitions(ou_vehicle_tests PRIVATE OU_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  gtest_discover_tests(ou_vehicle_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 900)
endif()

include(GNUInstallDirs)
install(TARGETS ou-viewer ou-game ou-asset ou-import ou-sign RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(TARGET ou-launcher)
  install(TARGETS ou-launcher RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(TARGETS ou-audio RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/openunderworld.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/openunderworld.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
