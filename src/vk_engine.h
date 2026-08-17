#pragma once 

#include <array>

#include <vk_types.h>
#include <vk_descriptors.h>
#include <vk_loader.h>
#include <camera.h>
#include <player_movement.h>
#include <portal.h>
#include <world.h>

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
        std::array<AllocatedBuffer, PortalViewCount> portalSceneBuffers;
        std::array<VkDescriptorSet, PortalViewCount> portalSceneDescriptors{};
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

// Fragment constants for the portal background pass.  They mirror the
// compute background's editable data so a portal opening continues the same
// selected gradient/sky rather than using a separate hard-coded colour.
struct PortalSkyPushConstants {
    glm::vec4 data1;
    glm::vec4 data2;
    // x = background effect index, y/z = render width/height.
    glm::vec4 settings;
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
    // These use the same layout as opaquePipeline, but split a portal mask
    // into visible-stencil and stencil-restricted depth-clear passes.
    MaterialPipeline portalStencilPipeline;
    MaterialPipeline portalRecursiveStencilPipeline;
    MaterialPipeline portalMaskPipeline;
    MaterialPipeline portalViewPipeline;
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
    DrawContext worldDrawContext;
    // The main camera is inside the player, so the proxy is added only to
    // portal views.  It lets us see the player body through an opening.
    DrawContext portalViewDrawContext;
    std::unordered_map<std::string, std::shared_ptr<LoadedGLTF>> loadedScenes;
    // Kept separate from loadedScenes because the main camera is first person:
    // this model should appear only in portal views, not around the camera.
    std::shared_ptr<LoadedGLTF> _playerModel;
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
        friend struct GLTFMetallic_Roughness;
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
        void draw_geometry(
            VkCommandBuffer cmd,
            const DrawContext& drawContext,
            const glm::mat4& viewProjection,
            VkDescriptorSet sceneDescriptor,
            bool clearDepthAndStencil,
            MaterialPipeline* overridePipeline = nullptr,
            uint32_t stencilReference = 0,
            bool useFrustumCulling = true,
            uint32_t stencilCompareMask = 0xff);
        void draw_portal_masks(VkCommandBuffer cmd);
        void draw_recursive_portal_mask(
            VkCommandBuffer cmd,
            const Portal& portal,
            MaterialInstance& material,
            VkDescriptorSet sceneDescriptor,
            uint32_t parentStencilReference,
            uint32_t recursiveStencilReference,
            uint32_t recursiveStencilBit);
        void draw_portal_sky(
            VkCommandBuffer cmd,
            uint32_t stencilReference,
            uint32_t stencilCompareMask = 0xff);
        void draw_portal_views(VkCommandBuffer cmd);
        RenderObject make_portal_render_object(
            const Portal& portal,
            MaterialInstance& material) const;
        GPUSceneData build_scene_data(const glm::mat4& view) const;
        GPUSceneData build_portal_scene_data(
            const glm::mat4& view,
            const Portal& destination) const;
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
        bool try_traverse_portal(
            const Portal& source,
            const Portal& destination,
            const glm::vec3& previousPosition);
        void rebuild_active_wall_colliders();
        void set_mouse_capture(bool captured);
        void place_portal(Portal& portal, const Portal& otherPortal);

        PlayerInput _playerInput{};
        PlayerMovement _playerMovement{};
        bool _mouseCaptured{true};
        float _portalTraversalCooldown{0.0f};
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
        AllocatedBuffer _playerMaterialBuffer;
        MaterialInstance _playerMaterial;
        Bounds _playerBounds;
        // The imported model has a large internal glTF scale (about 14 world
        // units tall at 1.0), while the player is roughly 1.8 units tall.
        float _playerModelScale{0.12f};
        float _playerModelYOffset{0.0f};
        float _playerModelYaw{0.0f};
        GPUMeshBuffers _portalMesh;
        AllocatedBuffer _bluePortalMaterialBuffer;
        MaterialInstance _bluePortalMaterial;
        AllocatedBuffer _orangePortalMaterialBuffer;
        MaterialInstance _orangePortalMaterial;
        Bounds _portalBounds;
        Portal _bluePortal;
        Portal _orangePortal;
        std::array<GPUSceneData, PortalViewCount> _portalSceneData{};
        MaterialPipeline _portalSkyPipeline;
        // Arena boundary plus a compact portal test rig around spawn.  All
        // walls share the same render, raycast, and collision representation.
        std::array<Wall, 7> _levelWalls{{
            {{0.0f, 1.5f, -24.75f}, {25.0f, 1.5f, 0.25f}}, // north
            {{0.0f, 1.5f,  24.75f}, {25.0f, 1.5f, 0.25f}}, // south
            {{-24.75f, 1.5f, 0.0f}, {0.25f, 1.5f, 25.0f}}, // west
            {{ 24.75f, 1.5f, 0.0f}, {0.25f, 1.5f, 25.0f}}, // east
            {{0.0f, 1.5f, -5.0f}, {3.0f, 1.5f, 0.25f}}, // test north panel
            {{0.0f, 1.5f,  5.0f}, {3.0f, 1.5f, 0.25f}}, // test south panel
            {{5.0f, 1.5f,  0.0f}, {0.25f, 1.5f, 3.0f}}, // test east panel
        }};
        std::array<AABB, 7> _levelWallColliders{};
        std::vector<AABB> _activeWallColliders;
    };
