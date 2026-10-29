cmake_minimum_required(VERSION 3.25)
project(OpenUnderworld VERSION 0.9.0 LANGUAGES CXX)

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

# RenderCommon + the D3D9 SM3 -> GLSL translator of the Original Fidelity path (docs/formats/SHADERS.md).
add_library(ou_render_common STATIC src/render/RenderCommon.cpp src/render/ShaderTranslate.cpp src/render/OriginalShading.cpp
  src/render/Water.cpp)
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
foreach(s water.vert water.frag splash.vert splash.frag underwater.glsl atmosphere.glsl)
  set(src ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/${s})
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${src})
  file(READ ${src} OU_WATER_SRC)
  file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/${s}.inc CONTENT "R\"ouwater(@OU_WATER_SRC@)ouwater\"" @ONLY)
endforeach()

add_library(ou_render_gl STATIC src/render/gl/GlRenderer.cpp)
target_link_libraries(ou_render_gl PUBLIC ou_render_common PRIVATE PkgConfig::EPOXY)

# ---- ou_render_vk: Vulkan 1.3 backend; SPIR-V compiled at build time by glslc and embedded ----
find_package(Vulkan REQUIRED COMPONENTS glslc)
set(OU_VK_SPV_DIR ${CMAKE_CURRENT_BINARY_DIR}/vk_shaders)
file(MAKE_DIRECTORY ${OU_VK_SPV_DIR})
set(OU_VK_SPV)
foreach(s scene.vert scene.frag text.vert text.frag line.vert line.frag shadow.vert shadow.frag post.vert post.frag
        prepass.vert prepass.frag fsr_mask.vert fsr_mask.frag fsr_camera.frag)
  set(src ${CMAKE_CURRENT_SOURCE_DIR}/src/render/vk/shaders/${s})
  add_custom_command(OUTPUT ${OU_VK_SPV_DIR}/${s}.inc
                     COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 -O -mfmt=num -I ${CMAKE_CURRENT_SOURCE_DIR}/src/render -o ${OU_VK_SPV_DIR}/${s}.inc ${src}
                     DEPENDS ${src} ${OU_SUN_GLSL_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/underwater.glsl
                             ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/atmosphere.glsl VERBATIM)
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
# Enhanced water + splash (shared GLSL with GL, -DVULKAN=1 selects the descriptor syntax).
foreach(s water.vert water.frag splash.vert splash.frag)
  set(src ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/${s})
  add_custom_command(OUTPUT ${OU_VK_SPV_DIR}/${s}.inc
                     COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 -O -mfmt=num -I ${CMAKE_CURRENT_SOURCE_DIR}/src/render -o ${OU_VK_SPV_DIR}/${s}.inc ${src}
                     DEPENDS ${src} VERBATIM)
  list(APPEND OU_VK_SPV ${OU_VK_SPV_DIR}/${s}.inc)
endforeach()
# post.frag includes shaders/underwater.glsl and shaders/atmosphere.glsl.
set_property(SOURCE ${OU_VK_SPV_DIR}/post.frag.inc APPEND PROPERTY OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/underwater.glsl
             ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/atmosphere.glsl)
# Ray-traced scene variant (-DOU_RT: ray queries against the scene TLAS); only loaded on RT-capable devices.
foreach(stage vert frag)
  set(src ${CMAKE_CURRENT_SOURCE_DIR}/src/render/vk/shaders/scene.${stage})
  add_custom_command(OUTPUT ${OU_VK_SPV_DIR}/scene_rt.${stage}.inc
                     COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 -O -mfmt=num -DOU_RT -fshader-stage=${stage} -I ${CMAKE_CURRENT_SOURCE_DIR}/src/render -o ${OU_VK_SPV_DIR}/scene_rt.${stage}.inc ${src}
                     DEPENDS ${src} ${OU_SUN_GLSL_SRC} VERBATIM)
  list(APPEND OU_VK_SPV ${OU_VK_SPV_DIR}/scene_rt.${stage}.inc)
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
  add_executable(ou_vk_tests tests/test_vk_renderer.cpp)
  target_link_libraries(ou_vk_tests PRIVATE ou_render_vk ou_placeholder GTest::gtest_main)
  # Mesa RADV leaks 256 bytes per process (see tests/lsan_vulkan.supp).
  set(OU_VK_TEST_ENV "LSAN_OPTIONS=fast_unwind_on_malloc=0:suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan_vulkan.supp")
  gtest_discover_tests(ou_vk_tests PROPERTIES ENVIRONMENT "${OU_VK_TEST_ENV}")
  set_tests_properties(viewer_vulkan_placeholder_screenshot viewer_vulkan_matches_gl viewer_vulkan_matches_gl_enhanced PROPERTIES
                       SKIP_RETURN_CODE 77 ENVIRONMENT "${OU_VK_TEST_ENV}")
  add_executable(ou_shader_tests tests/test_shaders.cpp)
  target_link_libraries(ou_shader_tests PRIVATE ou_render_common GTest::gtest_main)
  gtest_discover_tests(ou_shader_tests DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 1200) # every-shader sweep: ~570 s under ASan
endif()

# ---- ou_anim: skeletons, animation clips, pose sampling, CPU skinning (docs/formats/ANIMATION.md) ----
add_library(ou_anim STATIC src/anim/Anim.cpp src/anim/Blend.cpp src/anim/Hair.cpp)
target_include_directories(ou_anim PUBLIC src)
target_link_libraries(ou_anim PUBLIC ou_cdc glm::glm)
if(OU_BUILD_TESTS)
  add_executable(ou_anim_tests tests/test_anim.cpp)
  target_link_libraries(ou_anim_tests PRIVATE ou_anim ou_formats GTest::gtest_main)
  gtest_discover_tests(ou_anim_tests)
endif()

# ---- ou_fx: original particle effects (GenericFXObject), deterministic sim, sprites (docs/formats/EFFECTS.md) ----
add_library(ou_fx STATIC src/fx/Fx.cpp)
target_link_libraries(ou_fx PUBLIC ou_formats ou_physics glm::glm)
if(OU_BUILD_TESTS)
  add_executable(ou_fx_tests tests/test_fx.cpp)
  target_link_libraries(ou_fx_tests PRIVATE ou_fx ou_formats GTest::gtest_main)
  gtest_discover_tests(ou_fx_tests DISCOVERY_TIMEOUT 30)
endif()

# ---- ou_formats: PCD9 textures, render models, terrain -> ou::scene; ou-asset CLI ----------
add_library(ou_formats STATIC src/formats/Texture.cpp src/formats/Model.cpp src/formats/Scene.cpp src/formats/Level.cpp
            src/formats/Shader.cpp src/formats/Uber.cpp)
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

# ---- ou_physics: object collision primitives, rigid bodies, hinges, movers (docs/formats/PHYSICS.md) ----
add_library(ou_physics STATIC src/physics/Physics.cpp)
target_link_libraries(ou_physics PUBLIC ou_collision ou_formats glm::glm)
if(OU_BUILD_TESTS)
  add_executable(ou_physics_tests tests/test_physics.cpp)
  target_link_libraries(ou_physics_tests PRIVATE ou_physics GTest::gtest_main)
  gtest_discover_tests(ou_physics_tests DISCOVERY_TIMEOUT 30)
endif()

# ---- ou_audio: IMA ADPCM, FSB4, Wave sections, .mul streams; SDL3 mixer; ou-audio CLI (docs/formats/AUDIO.md) ----
add_library(ou_audio STATIC src/audio/Audio.cpp src/audio/Mixer.cpp src/audio/Mus.cpp src/audio/SoundPlex.cpp)
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
# ---- ou_input: control binding names <-> SDL3 devices (ou-game, launcher) ----
add_library(ou_input STATIC src/platform/Input.cpp)
target_link_libraries(ou_input PUBLIC ou_config PRIVATE SDL3::SDL3)
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
  target_link_libraries(ou-launcher PRIVATE ou_config ou_input ou_update ou_importer SDL3::SDL3 Qt6::Widgets Qt6::Concurrent)
  if(OU_BUILD_TESTS)
    add_test(NAME launcher_self_test COMMAND ou-launcher --self-test --config ${CMAKE_CURRENT_BINARY_DIR}/launcher-test-config.json)
    set_tests_properties(launcher_self_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;OU_DISABLE_UPDATES=1")
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
find_package(SDL3_ttf CONFIG)
add_library(ou_video STATIC src/video/Movie.cpp src/video/Player.cpp)
target_link_libraries(ou_video PUBLIC ou_locale SDL3::SDL3 PRIVATE PkgConfig::LIBAV)
if(TARGET SDL3_ttf::SDL3_ttf)
  target_link_libraries(ou_video PRIVATE SDL3_ttf::SDL3_ttf)
  target_compile_definitions(ou_video PRIVATE OU_HAVE_TTF)
else()
  message(STATUS "SDL3_ttf not found: movie subtitles use SDL's ASCII debug font")
endif()
add_executable(ou-movie tools/ou-movie/main.cpp)
target_link_libraries(ou-movie PRIVATE ou_video)
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
add_library(ou_game STATIC src/game/Player.cpp src/game/World.cpp src/game/Input.cpp src/game/Stream.cpp src/game/Combat.cpp
  src/game/Water.cpp src/game/Climb.cpp src/game/Grapple.cpp src/game/Traverse.cpp src/game/ai/Enemy.cpp src/game/ai/EnemyData.cpp src/game/Progress.cpp
  src/game/Vehicle.cpp src/game/Weather.cpp src/game/Gadgets.cpp)
target_link_libraries(ou_game PUBLIC ou_collision ou_anim ou_formats ou_script ou_cine ou_input glm::glm PRIVATE SDL3::SDL3)
# ---- ou_save: save games, atomic slots (docs/SAVES.md) ----
add_library(ou_save STATIC src/save/Save.cpp)
target_include_directories(ou_save PUBLIC src)
target_link_libraries(ou_save PUBLIC ou_game nlohmann_json::nlohmann_json PRIVATE ou_config ZLIB::ZLIB)
# ou_game's object collision + physics doors (src/game/Objects.cpp, src/physics)
target_sources(ou_game PRIVATE src/game/Objects.cpp)
target_link_libraries(ou_game PUBLIC ou_physics)
# ---- ou_ui: menus, PDA page, HUD as renderer-agnostic quads (docs/GAMEPLAY.md "Menus") ----
add_library(ou_ui STATIC src/ui/Ui.cpp src/ui/Hud.cpp src/ui/Art.cpp)
target_link_libraries(ou_ui PUBLIC ou_render_common ou_locale ou_formats)
add_executable(ou-game src/app/game/main.cpp src/app/game/Cinematic.cpp src/app/game/Sound.cpp)
target_link_libraries(ou-game PRIVATE ou_game ou_save ou_ui ou_render_gl ou_render_vk ou_config ou_video ou_audio ou_fx)
if(OU_BUILD_TESTS)
  add_executable(ou_game_tests tests/test_game.cpp tests/test_combat.cpp)
  target_link_libraries(ou_game_tests PRIVATE ou_game GTest::gtest_main)
  gtest_discover_tests(ou_game_tests DISCOVERY_TIMEOUT 30)
  add_executable(ou_ai_tests tests/test_ai.cpp)
  target_link_libraries(ou_ai_tests PRIVATE ou_game GTest::gtest_main)
  gtest_discover_tests(ou_ai_tests DISCOVERY_TIMEOUT 30)
  add_executable(ou_save_tests tests/test_save.cpp)
  target_link_libraries(ou_save_tests PRIVATE ou_save GTest::gtest_main)
  gtest_discover_tests(ou_save_tests DISCOVERY_TIMEOUT 60)
  add_executable(ou_progress_tests tests/test_progress.cpp)
  target_link_libraries(ou_progress_tests PRIVATE ou_ui ou_game ou_save ZLIB::ZLIB GTest::gtest_main)
  gtest_discover_tests(ou_progress_tests DISCOVERY_TIMEOUT 60)
  add_executable(ou_hud_tests tests/test_hud.cpp)
  target_link_libraries(ou_hud_tests PRIVATE ou_ui GTest::gtest_main)
  gtest_discover_tests(ou_hud_tests DISCOVERY_TIMEOUT 30)
  add_executable(ou_vehicle_tests tests/test_vehicle.cpp)
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
