# Setup

## Install first

On Windows, install:

- Git
- CMake 3.8 or newer
- Visual Studio with the **Desktop development with C++** workload
- The [Vulkan SDK](https://vulkan.lunarg.com/sdk/home/)

The Vulkan SDK installer should configure the `VULKAN_SDK` environment variable. Restart your terminal after installing it.

SDL2, fmt, vk-bootstrap, VMA, GLM, ImGui, and fastgltf are already included in this repository under `third_party/`.

## Download the project

```powershell
git clone https://github.com/kennynguyen216/Mirabilis.git
cd Mirabilis
```

## Configure and build

```powershell
cmake -S . -B build
cmake --build build --config Release --parallel
```

## Run

```powershell
Push-Location .\bin\Release
.\chapter_5.exe
Pop-Location
```

This project is pinned to the current tutorial's official `all-chapters-2`
Chapter 5 implementation. The executable loads shaders and assets relative to
`bin\Release`, so its working directory must be `bin\Release`. The included VS Code
task and debug configuration set this automatically.

Press `Esc` or close the window to exit.

