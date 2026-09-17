
# Add source to this project's executable.
add_executable (engine 
  main.cpp
  vk_types.h
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
  scene.cpp
  portal.h
  player_movement.h
  player_movement.cpp
)

set_property(TARGET engine PROPERTY CXX_STANDARD 20)
target_compile_definitions(engine PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE)
target_include_directories(engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(engine PUBLIC vma glm Vulkan::Vulkan fmt::fmt stb_image SDL2::SDL2 vkbootstrap imgui imguizmo fastgltf::fastgltf fastgltf_simdjson)

target_precompile_headers(engine PUBLIC <optional> <vector> <memory> <string> <vector> <unordered_map> <glm/mat4x4.hpp>  <glm/vec4.hpp> <vulkan/vulkan.h>)

if(WIN32)
  add_custom_command(TARGET engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:engine> $<TARGET_FILE_DIR:engine>
    COMMAND_EXPAND_LISTS
    )
endif()
