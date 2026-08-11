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
cmake --build build --config Debug --parallel
```

## Run

```powershell
.\bin\Debug\engine.exe
```

Press `Esc` or close the window to exit.

