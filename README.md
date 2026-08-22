# Mirabilis

Mirabilis is a C++ Vulkan-based real-time engine prototype focused on editor-authored scenes, Source/Quake-style movement, and momentum-preserving linked portals.

[Watch the current milestone demo](https://kennynguyen216.github.io/portfolio/media/mirabilis-milestone2-demo.mp4) | [View my portfolio](https://kennynguyen216.github.io/portfolio/)

## Highlights

### Vulkan rendering

- Renders GPU mesh and material pipelines with glTF/GLB scene loading.
- Manages Vulkan command buffers, synchronization, descriptor sets, dynamic rendering, and swapchain presentation.
- Includes a compute-rendered background and ongoing portal-rendering experiments.

### Linked portals

- Uses stencil-mask passes to restrict linked-world rendering to each visible portal surface.
- Includes an experimental recursive stencil path for a linked portal visible through another portal.
- Places portals on eligible walls, carves portal-shaped openings from collision, and rejects overlapping or invalid placements.
- Transforms player position, velocity, and view orientation through linked portal frames while preserving momentum.

### Scene editor and tooling

- Provides an ImGui editor with a scene hierarchy, object inspector, selection, duplication, and deletion.
- Supports translate, rotate, and scale gizmos with optional snapping.
- Exposes editable box colliders and gameplay roles for scene-authored objects.
- Saves and loads scene hierarchies, transforms, collider data, imported assets, and actor roles as JSON.

### Movement and time trials

- Runs Source/Quake-inspired movement on a fixed timestep with acceleration, friction, air control, jump buffering, and bunny hopping.
- Supports surf ramps and scene-authored spawn points, start triggers, finish triggers, and a timer HUD.

## Technology

- **Language:** C++
- **Graphics:** Vulkan, GLSL
- **Platform and input:** SDL2
- **Editor UI:** ImGui and ImGuizmo
- **Assets and math:** fastgltf, simdjson, GLM, stb_image
- **Build:** CMake

SDL2, fmt, vk-bootstrap, VMA, GLM, ImGui, ImGuizmo, fastgltf, simdjson, and other supporting libraries are included under `third_party/`. Mirabilis uses these libraries as dependencies; the engine, renderer integration, editor systems, movement, collision, and portal features live in this project.

## Build and run

### Requirements

On Windows, install:

- Git
- CMake 3.8 or newer
- Visual Studio with the **Desktop development with C++** workload
- The [Vulkan SDK](https://vulkan.lunarg.com/sdk/home/)

The Vulkan SDK installer should configure the `VULKAN_SDK` environment variable. Restart your terminal after installing it.

### Clone

```powershell
git clone https://github.com/kennynguyen216/Mirabilis.git
cd Mirabilis
```

### Configure and build

```powershell
cmake -S . -B build
cmake --build build --config Debug --parallel
```

### Run

```powershell
.\bin\Debug\engine.exe
```

Press `Esc` or close the window to exit.

## Status

Mirabilis is an active engine and rendering prototype. Features, controls, and scene formats may change as portal rendering, movement, and editor tooling evolve.

