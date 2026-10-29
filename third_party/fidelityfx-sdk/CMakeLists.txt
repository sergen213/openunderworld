# AMD FidelityFX / FSR SDK 2.4.0 subset (README.md): ffx-api with the FSR 3.1.6 upscaler and frame-generation providers
# and the Vulkan backend, linked statically into ou_ffx. Shaders: HLSL -> SPIR-V with DXC (tools/ffx_shaders.py).
# Sets OU_HAS_FSR in the parent scope; without DXC (system `dxc` or the pinned release download) FSR is left out.
option(OU_FSR "Build AMD FSR (FidelityFX SDK 2.4.0, Vulkan)" ON)
set(OU_HAS_FSR OFF PARENT_SCOPE)
if(NOT OU_FSR)
  return()
endif()

find_program(OU_DXC NAMES dxc)
if(NOT OU_DXC AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  # DirectXShaderCompiler v1.9.2609 (LLVM/NCSA licence), the official Linux release build.
  include(FetchContent)
  FetchContent_Declare(ou_dxc
    URL https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/linux_dxc_2026_09_28.x86_x64.tar.gz
    URL_HASH SHA256=96faadc7f5c282d2ffda49804beb4c3ee38127bc252b723234e3c5cdf7aa39a1
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(ou_dxc)
  set(OU_DXC ${ou_dxc_SOURCE_DIR}/bin/dxc)
endif()
if(NOT OU_DXC OR NOT EXISTS ${OU_DXC})
  message(WARNING "FSR disabled: no DXC (install directx-shader-compiler or allow the release download)")
  return()
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(sdk ${CMAKE_CURRENT_SOURCE_DIR})
set(gen ${CMAKE_CURRENT_BINARY_DIR}/permutations)
file(GLOB_RECURSE gpu_sources CONFIGURE_DEPENDS ${sdk}/*.hlsl ${sdk}/*/gpu/*.h)
add_custom_command(OUTPUT ${gen}/stamp
  COMMAND ${Python3_EXECUTABLE} ${sdk}/../../tools/ffx_shaders.py --dxc ${OU_DXC} --sdk ${sdk} --output ${gen}
  COMMAND ${CMAKE_COMMAND} -E touch ${gen}/stamp
  DEPENDS ${sdk}/../../tools/ffx_shaders.py ${gpu_sources}
  COMMENT "FidelityFX: HLSL -> SPIR-V permutations (DXC)" VERBATIM)
add_custom_target(ou_ffx_shaders DEPENDS ${gen}/stamp)

set(upstream
  api/internal/ffx_api.cpp api/internal/ffx_assert.cpp api/internal/ffx_message.cpp api/internal/ffx_object_management.cpp
  api/internal/ffx_query_fallback.cpp
  backend/vk/ffx_vk.cpp backend/vk/ffx_backends_vk.cpp
  upscalers/fsr3/internal/ffx_fsr3upscaler.cpp upscalers/fsr3/internal/ffx_fsr3upscaler_shaderblobs.cpp
  upscalers/fsr3/internal/ffx_provider_fsr3upscale.cpp
  framegeneration/fsr3/internal/ffx_frameinterpolation.cpp framegeneration/fsr3/internal/ffx_frameinterpolation_shaderblobs.cpp
  framegeneration/fsr3/internal/ffx_opticalflow.cpp framegeneration/fsr3/internal/ffx_opticalflow_shaderblobs.cpp
  framegeneration/fsr3/internal/ffx_provider_fsr3framegeneration.cpp)
add_library(ou_ffx STATIC ${upstream} ou/ffx_providers.cpp)
add_dependencies(ou_ffx ou_ffx_shaders)
find_package(Vulkan REQUIRED)
target_link_libraries(ou_ffx PUBLIC Vulkan::Vulkan)
target_include_directories(ou_ffx SYSTEM PUBLIC ${sdk}/api/include ${sdk}/upscalers/include ${sdk}/framegeneration/include)
target_include_directories(ou_ffx PRIVATE ${sdk}/ou ${gen} ${sdk}/api/internal)
target_compile_definitions(ou_ffx PRIVATE FFX_BACKEND_VK FFX_UPSCALER)
# ffx_api.h marks its entry points __declspec(dllexport) unconditionally.
target_compile_options(ou_ffx INTERFACE "-D__declspec(x)=")
target_compile_options(ou_ffx PRIVATE -w -include ${sdk}/ou/ffx_compat.h)
install(FILES LICENSE.txt README.md DESTINATION share/licenses/openunderworld/fidelityfx-sdk)
set(OU_HAS_FSR ON PARENT_SCOPE)
