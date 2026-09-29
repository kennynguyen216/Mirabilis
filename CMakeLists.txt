# CMakeList.txt : CMake project for vulkan_guide, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.8)

project ("vulkan_guide")


find_package(Vulkan REQUIRED)

add_subdirectory(third_party)

set (CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin")
set (CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin")

add_subdirectory(src)

find_program(GLSL_VALIDATOR glslangValidator HINTS /usr/bin /usr/local/bin $ENV{VULKAN_SDK}/Bin/ $ENV{VULKAN_SDK}/Bin32/)

# Shared includes are not shaders themselves, but a change to one must
# rebuild every shader that includes it.
file(GLOB GLSL_INCLUDE_FILES "${PROJECT_SOURCE_DIR}/shaders/*.glsl")



file(GLOB_RECURSE GLSL_SOURCE_FILES
    "${PROJECT_SOURCE_DIR}/shaders/*.frag"
    "${PROJECT_SOURCE_DIR}/shaders/*.vert"
    "${PROJECT_SOURCE_DIR}/shaders/*.comp"
    )

foreach(GLSL ${GLSL_SOURCE_FILES})
  message(STATUS "BUILDING SHADER")
  get_filename_component(FILE_NAME ${GLSL} NAME)
  set(SPIRV "${PROJECT_SOURCE_DIR}/shaders/${FILE_NAME}.spv")
  message(STATUS ${GLSL})
  add_custom_command(
    OUTPUT ${SPIRV}
    COMMAND ${GLSL_VALIDATOR} -V ${GLSL} -o ${SPIRV}
    DEPENDS ${GLSL} ${GLSL_INCLUDE_FILES})
  list(APPEND SPIRV_BINARY_FILES ${SPIRV})
endforeach(GLSL)

# IQ2: ray-query builds of every entry point that calls sunlight_visibility(),
# and IQ8's cache relight, loaded only on devices with VK_KHR_ray_query.  The
# .spv builds above do not change.
foreach(NAME mesh.frag mesh_mask.frag mesh_transparent.frag portal_view.frag
        portal_view_mask.frag ssgi_lumen.comp ssgi_probe.comp ssgi_probe_place.comp
        ssgi_probe_adaptive.comp surface_cache_direct.comp)
  set(GLSL "${PROJECT_SOURCE_DIR}/shaders/${NAME}")
  set(SPIRV "${PROJECT_SOURCE_DIR}/shaders/${NAME}.rt.spv")
  add_custom_command(
    OUTPUT ${SPIRV}
    COMMAND ${GLSL_VALIDATOR} -V --target-env vulkan1.2 --glsl-version 460
            -DMIRABILIS_RT_SHADOWS ${GLSL} -o ${SPIRV}
    DEPENDS ${GLSL} ${GLSL_INCLUDE_FILES})
  list(APPEND SPIRV_BINARY_FILES ${SPIRV})
endforeach(NAME)

add_custom_target(
    Shaders 
    DEPENDS ${SPIRV_BINARY_FILES}
    )

add_dependencies(engine Shaders)
