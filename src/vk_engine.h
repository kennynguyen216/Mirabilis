#pragma once 

#include <vk_types.h>
#include <vk_descriptors.h>
#include <vk_loader.h>
#include <camera.h>
#include <player_movement.h>

struct DeletionQueue
{
    std::deque<std::function<void()>> deletors;
    void push_function(std::function<void()>&& function) {
        deletors.push_back(function);
    }
    void flush(){
        // reverse iterate the deletion queue to execute all the functions
        for(auto it = deletors.rbegin(); it != deletors.rend(); it++){
            (*it)(); // call functors
        }
        deletors.clear();
    }
};
struct FrameData {
        VkSemaphore _swapchainSemaphore, _renderSemaphore;
        VkFence _renderFence;
        VkCommandPool _commandPool;
        VkCommandBuffer _mainCommandBuffer;
        DeletionQueue _deletionQueue;
        DescriptorAllocatorGrowable _frameDescriptors;
        AllocatedBuffer sceneBuffer;
        VkDescriptorSet sceneDescriptor{};
    };
    constexpr unsigned int FRAME_OVERLAP = 2;

struct ComputePushConstants {
	glm::vec4 data1;
	glm::vec4 data2;
	glm::vec4 data3;
	glm::vec4 data4;
};

struct ComputeEffect {
    const char* name;
    VkPipeline pipeline{};
    VkPipelineLayout layout{};
    ComputePushConstants data{};
};

struct EngineStats {
    float frametime{0.0f};
    int triangle_count{0};
    int drawcall_count{0};
    float scene_update_time{0.0f};
    float mesh_draw_time{0.0f};
};

struct GLTFMetallic_Roughness {
    MaterialPipeline opaquePipeline;
    MaterialPipeline transparentPipeline;
    VkDescriptorSetLayout materialLayout{};

    struct MaterialConstants {
        glm::vec4 colorFactors;
        glm::vec4 metal_rough_factors;
        glm::vec4 extra[14];
    };

    struct MaterialResources {
        AllocatedImage colorImage;
        VkSampler colorSampler{};
        AllocatedImage metalRoughImage;
        VkSampler metalRoughSampler{};
        VkBuffer dataBuffer{};
        uint32_t dataBufferOffset{};
    };

    DescriptorWriter writer;
    void build_pipelines(class VulkanEngine* engine);
    void clear_resources(VkDevice device);
    MaterialInstance write_material(
        VkDevice device,
        MaterialPass pass,
        const MaterialResources& resources,
        DescriptorAllocatorGrowable& descriptorAllocator);
};

struct MeshNode : public Node {
    std::shared_ptr<MeshAsset> mesh;
    void Draw(const glm::mat4& topMatrix, DrawContext& ctx) override;
};

class VulkanEngine{
    public:
    bool _isInitialized{ false };
    int _frameNumber {0};
    bool stop_rendering {false};
    bool resize_requested {false};
    float renderScale {1.0f};
    VkExtent2D _windowExtent{1280, 720};

    FrameData _frames[FRAME_OVERLAP];
    FrameData& get_current_frame() { return _frames[_frameNumber % FRAME_OVERLAP];};
    VkQueue _graphicsQueue;
    uint32_t _graphicsQueueFamily;
    DeletionQueue _mainDeletionQueue;
    struct SDL_Window* _window {nullptr};
    AllocatedImage _drawImage;
    DescriptorAllocatorGrowable globalDescriptorAllocator;

    VkDescriptorSet _drawImageDescriptors;
    VkDescriptorSetLayout _drawImageDescriptorLayout;
    VkDescriptorSetLayout _singleImageDescriptorLayout{};
    VkDescriptorSetLayout _gpuSceneDataDescriptorLayout{};
    VkExtent2D _drawExtent;
    
    VkPipelineLayout _gradientPipelineLayout;
    std::vector<ComputeEffect> backgroundEffects;
    int currentBackgroundEffect{0};
    AllocatedImage _depthImage;
    AllocatedImage _whiteImage;
    AllocatedImage _blackImage;
    AllocatedImage _greyImage;
    AllocatedImage _errorCheckerboardImage;
    VkSampler _defaultSamplerLinear{};
    VkSampler _defaultSamplerNearest{};
    DrawContext mainDrawContext;
    std::unordered_map<std::string, std::shared_ptr<LoadedGLTF>> loadedScenes;
    GLTFMetallic_Roughness metalRoughMaterial;
    Camera mainCamera;
    GPUSceneData sceneData{};
    EngineStats stats{};



    VkInstance _instance; //vulkan librbary handle
    VkDebugUtilsMessengerEXT _debug_messenger; // Vulkan debug output
    VkPhysicalDevice _chosenGPU; // Gpu chosen as the deafult dwvice
    VkDevice _device; // vulkan device for commands
    VkSurfaceKHR _surface; // Vulkan window surface this is bridge from the api to ur screen

    VkSwapchainKHR _swapchain;
    VkFormat _swapchainImageFormat;

    std::vector<VkImage> _swapchainImages;
    std::vector<VkImageView> _swapchainImageViews;
    VkExtent2D _swapchainExtent;
    VmaAllocator _allocator;

    VkFence _immFence;
    VkCommandBuffer _immCommandBuffer;
    VkCommandPool _immCommandPool;


    static VulkanEngine&  Get();

    // initializes everything in the engine

    void init();
    //shuts down engine

    void cleanup();

    // draw loop
    void draw(float deltaTime);

    //run main loop
    void run();

    void immediate_submit(std::function<void(VkCommandBuffer cmd)>&& function);

    private:
        friend std::optional<std::vector<std::shared_ptr<MeshAsset>>> loadGltfMeshes(
            VulkanEngine*, std::filesystem::path);
        friend std::optional<std::shared_ptr<LoadedGLTF>> loadGltf(
            VulkanEngine*, std::filesystem::path);
        friend struct LoadedGLTF;
        void init_vulkan();
        void init_swapchain();
        void init_commands();
        void init_sync_structures();
        void init_descriptors();
        void create_swapchain(uint32_t width, uint32_t height);
        void destroy_swapchain();
        void resize_swapchain();
        void draw_background(VkCommandBuffer cmd);
        void init_pipelines();
        void init_background_pipelines();
        void init_default_data();
        void draw_geometry(VkCommandBuffer cmd);
        void update_scene(float deltaTime);
        AllocatedBuffer create_buffer(size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage);
        void destroy_buffer(const AllocatedBuffer& buffer);
        GPUMeshBuffers uploadMesh(std::span<uint32_t> indices, std::span<Vertex> vertices);
        AllocatedImage create_image(
            VkExtent3D size,
            VkFormat format,
            VkImageUsageFlags usage,
            bool mipmapped = false);
        AllocatedImage create_image(
            void* data,
            VkExtent3D size,
            VkFormat format,
            VkImageUsageFlags usage,
            bool mipmapped = false);
        void destroy_image(const AllocatedImage& image);
        void init_imgui();
        void draw_imgui(VkCommandBuffer cmd, VkImageView targetImageView);
        void update_physics(float deltaTime);
        void set_mouse_capture(bool captured);

        PlayerInput _playerInput{};
        PlayerMovement _playerMovement{};
        bool _mouseCaptured{true};
        float _physicsAccumulator{0.0f};
        static constexpr float PhysicsDt = 1.0f / 120.0f;
        static constexpr int MaxPhysicsSteps = 8;

        GPUMeshBuffers _floorMesh;
        AllocatedBuffer _floorMaterialBuffer;
        MaterialInstance _floorMaterial;
        Bounds _floorBounds;
        GPUMeshBuffers _wallMesh;
        AllocatedBuffer _wallMaterialBuffer;
        MaterialInstance _wallMaterial;
        Bounds _wallBounds;
    };
