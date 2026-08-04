#pragma once 

#include <vk_types.h>

class VulkanEngine{
    public:
    bool _isInitialized{ false };
    int _frameNumber {0};
    bool stop_rendering {false};
    VkExtent2D _windowExtent{1700 , 900};

    struct SDL_Window* _window {nullptr};

    VkInstance _instance; //vulkan librbary handle
    VkDebugUtilsMessengerEXT _debug_messenger; // Vulkan debug output
    VkPhysicalDevice _chosenGPU; // Gpu chosen as the deafult dwvice
    VkDevice _device; // vulkan device for commands
    VkSurfaceKHR _surface; // Vulkan window surface this is bridge from the api to ur screen

    static VulkanEngine&  Get();

    // initializes everything in the engine

    void init();
    //shuts down engine

    void cleanup();

    // draw loop
    void draw();

    //run main loop
    void run();

    private:
        void init_vulkan();
        void init_swapchain();
        void init_commands();
        void init_sync_structures();
};