find_program(DOTNET_EXECUTABLE dotnet REQUIRED)
get_filename_component(DOTNET_ROOT "${DOTNET_EXECUTABLE}" DIRECTORY)
file(GLOB DOTNET_HOST_PACKS "${DOTNET_ROOT}/packs/Microsoft.NETCore.App.Host.win-x64/10.0.*")
list(SORT DOTNET_HOST_PACKS COMPARE NATURAL ORDER DESCENDING)
if(NOT DOTNET_HOST_PACKS)
  message(FATAL_ERROR "Install the x64 .NET 10 SDK, including Microsoft.NETCore.App.Host.win-x64")
endif()
list(GET DOTNET_HOST_PACKS 0 DOTNET_HOST_PACK)
set(DOTNET_HOST_NATIVE "${DOTNET_HOST_PACK}/runtimes/win-x64/native")
message(STATUS "Using .NET hosting pack: ${DOTNET_HOST_PACK}")
set(MIRABILIS_MANAGED_DIR "${CMAKE_BINARY_DIR}/managed/$<CONFIG>")
add_custom_target(mirabilis_managed ALL
  COMMAND "${DOTNET_EXECUTABLE}" build "${PROJECT_SOURCE_DIR}/scripting/Mirabilis.SampleScripts/Mirabilis.SampleScripts.csproj"
    -c $<CONFIG> --artifacts-path "${CMAKE_BINARY_DIR}/dotnet" --output "${MIRABILIS_MANAGED_DIR}" --ignore-failed-sources -p:NuGetAudit=false
  VERBATIM)
if(BUILD_TESTING)
  foreach(PROJECT Mirabilis.TestScripts Mirabilis.ScriptTests)
    add_custom_command(TARGET mirabilis_managed POST_BUILD
      COMMAND "${DOTNET_EXECUTABLE}" build "${PROJECT_SOURCE_DIR}/tests/managed/${PROJECT}/${PROJECT}.csproj"
        -c $<CONFIG> --artifacts-path "${CMAKE_BINARY_DIR}/dotnet" --output "${MIRABILIS_MANAGED_DIR}" --ignore-failed-sources -p:NuGetAudit=false
      VERBATIM)
  endforeach()
  foreach(VERSION RANGE 1 6)
    set(RELOAD_OUTPUT "${CMAKE_BINARY_DIR}/reload-fixtures/v${VERSION}/$<CONFIG>")
    add_custom_command(TARGET mirabilis_managed POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${MIRABILIS_MANAGED_DIR}" "${RELOAD_OUTPUT}"
      COMMAND "${DOTNET_EXECUTABLE}" build "${PROJECT_SOURCE_DIR}/tests/managed/Mirabilis.ReloadProbe/Mirabilis.ReloadProbe.csproj"
        -c $<CONFIG> --artifacts-path "${CMAKE_BINARY_DIR}/reload-fixtures/obj-v${VERSION}" --output "${RELOAD_OUTPUT}" --ignore-failed-sources -p:NuGetAudit=false -p:ReloadVersion=${VERSION}
      VERBATIM)
  endforeach()
  add_test(NAME mirabilis_managed_tests COMMAND "${DOTNET_EXECUTABLE}" "${MIRABILIS_MANAGED_DIR}/Mirabilis.ScriptTests.dll")
  set(MIRABILIS_BAD_BRIDGE_DIR "${CMAKE_BINARY_DIR}/badbridge/$<CONFIG>")
  set(MIRABILIS_BAD_RUNTIME_DIR "${CMAKE_BINARY_DIR}/badruntime/$<CONFIG>")
  add_custom_target(mirabilis_bad_bridge ALL
    COMMAND "${DOTNET_EXECUTABLE}" build "${PROJECT_SOURCE_DIR}/tests/managed/Mirabilis.BadBridge/Mirabilis.BadBridge.csproj"
      -c $<CONFIG> --artifacts-path "${CMAKE_BINARY_DIR}/dotnet-badbridge" --output "${MIRABILIS_BAD_BRIDGE_DIR}" --ignore-failed-sources -p:NuGetAudit=false
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DOTNET_HOST_NATIVE}/nethost.dll" "${MIRABILIS_BAD_BRIDGE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MIRABILIS_BAD_RUNTIME_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DOTNET_HOST_NATIVE}/nethost.dll" "${MIRABILIS_BAD_RUNTIME_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MIRABILIS_BAD_BRIDGE_DIR}/Mirabilis.ScriptCore.dll" "${MIRABILIS_BAD_RUNTIME_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/tests/fixtures/script_incompatible.runtimeconfig.json" "${MIRABILIS_BAD_RUNTIME_DIR}/Mirabilis.ScriptCore.runtimeconfig.json"
    VERBATIM)
endif()
function(mirabilis_deploy_scripts TARGET)
  add_dependencies(${TARGET} mirabilis_managed)
  add_custom_command(TARGET mirabilis_managed POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${MIRABILIS_MANAGED_DIR}" "$<TARGET_FILE_DIR:${TARGET}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DOTNET_HOST_NATIVE}/nethost.dll" "$<TARGET_FILE_DIR:${TARGET}>"
    VERBATIM)
endfunction()

add_library(mirabilis_core STATIC scene.cpp scene_math.cpp scene_queries.cpp scene_serialization.cpp player_movement.cpp physics_world.cpp script_host.cpp)
target_compile_features(mirabilis_core PUBLIC cxx_std_20)
target_compile_definitions(mirabilis_core PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE)
target_include_directories(mirabilis_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(mirabilis_core PRIVATE "${DOTNET_HOST_NATIVE}")
target_compile_definitions(mirabilis_core PUBLIC
  MIRABILIS_SCRIPT_PROJECT="${PROJECT_SOURCE_DIR}/scripting"
  MIRABILIS_SCRIPT_RELOAD_BUILD="${CMAKE_BINARY_DIR}/script-reload/$<CONFIG>"
  MIRABILIS_SCRIPT_DOTNET="${DOTNET_EXECUTABLE}"
  MIRABILIS_SCRIPT_CONFIGURATION="$<CONFIG>")
target_link_libraries(mirabilis_core PUBLIC glm EnTT::EnTT fastgltf_simdjson Jolt)
if(BUILD_TESTING)
  add_executable(mirabilis_core_tests ../tests/core_smoke.cpp ../tests/scene_ecs_tests.cpp ../tests/scene_serialization_tests.cpp ../tests/physics_tests.cpp ../tests/script_tests.cpp)
  target_link_libraries(mirabilis_core_tests PRIVATE mirabilis_core doctest::doctest)
  target_compile_definitions(mirabilis_core_tests PRIVATE MIRABILIS_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  target_compile_definitions(mirabilis_core_tests PRIVATE MIRABILIS_TEST_MANAGED_DIR="$<TARGET_FILE_DIR:mirabilis_core_tests>")
  target_compile_definitions(mirabilis_core_tests PRIVATE MIRABILIS_TEST_RELOAD_DIR="${CMAKE_BINARY_DIR}/reload-fixtures")
  target_compile_definitions(mirabilis_core_tests PRIVATE MIRABILIS_TEST_CONFIGURATION="$<CONFIG>")
  mirabilis_deploy_scripts(mirabilis_core_tests)
  add_test(NAME mirabilis_core_tests COMMAND mirabilis_core_tests)
  add_executable(mirabilis_script_abi_probe ../tests/script_abi_probe.cpp)
  target_link_libraries(mirabilis_script_abi_probe PRIVATE mirabilis_core)
  add_dependencies(mirabilis_script_abi_probe mirabilis_bad_bridge)
  foreach(MISMATCH version size null reload-null)
    add_test(NAME mirabilis_script_bad_${MISMATCH} COMMAND mirabilis_script_abi_probe "${MIRABILIS_BAD_BRIDGE_DIR}" ${MISMATCH})
  endforeach()
  add_test(NAME mirabilis_script_missing_runtime COMMAND mirabilis_script_abi_probe "${MIRABILIS_BAD_RUNTIME_DIR}" runtime)
endif()
if(MIRABILIS_BUILD_ENGINE)

# Add source to this project's executable.
add_executable (engine 
  main.cpp
  vk_types.h
  env_flags.h
  r4_contributions.h
  vk_initializers.cpp
  vk_initializers.h
  vk_images.h
  vk_images.cpp 
  vk_descriptors.h
  vk_descriptors.cpp
  vk_pipelines.h
  vk_pipelines.cpp
  vk_engine.h
  vk_engine.cpp
  vk_engine_render_helpers.h
  vk_engine_path_trace.cpp
  path_trace_scene.cpp
  vk_engine_editor.cpp
  vk_engine_ibl.cpp
  vk_engine_gameplay.cpp
  vk_engine_input.cpp
  vk_engine_materials.cpp
  vk_engine_portals.cpp
  vk_engine_renderer.cpp
  vk_engine_debug_views.cpp
  vk_engine_post_process.cpp
  vk_engine_prepass.cpp
  vk_engine_shadows.cpp
  vk_engine_ssgi.cpp
  vk_engine_ssao.cpp
  vk_engine_sdf.cpp
  vk_engine_surface_cache.cpp
  vk_engine_resources.cpp
  vk_engine_scene.cpp
  vk_engine_scene_materials.cpp
  vk_engine_scene_io.cpp
  vk_engine_ui.cpp
  vk_loader.h
  vk_loader.cpp
  tangents.h
  tangents.cpp
  camera.h
  camera.cpp
  world.h
  scene.h
  portal.h
  player_movement.h
)

set_property(TARGET engine PROPERTY CXX_STANDARD 20)
target_compile_definitions(engine PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE)
target_include_directories(engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(engine PUBLIC mirabilis_core vma glm Vulkan::Vulkan fmt::fmt stb_image SDL2::SDL2 vkbootstrap imgui imguizmo fastgltf::fastgltf fastgltf_simdjson)
mirabilis_deploy_scripts(engine)

target_precompile_headers(engine PUBLIC <optional> <vector> <memory> <string> <vector> <unordered_map> <glm/mat4x4.hpp>  <glm/vec4.hpp> <vulkan/vulkan.h>)

if(WIN32)
  add_custom_command(TARGET engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:engine> $<TARGET_FILE_DIR:engine>
    COMMAND_EXPAND_LISTS
    )
endif()

endif()
