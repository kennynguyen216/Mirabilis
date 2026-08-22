#pragma once 

#include <array>

#include <vk_types.h>
#include <vk_descriptors.h>
#include <vk_loader.h>
#include <camera.h>
#include <player_movement.h>
#include <portal.h>
#include <scene.h>
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
    // World-space camera axes.  The panorama is sampled from these directions,
    // so it rotates as the camera rotates but never parallax-shifts when the
    // camera changes position.
    glm::vec4 cameraRight;
    glm::vec4 cameraUp;
    glm::vec4 cameraForward;
};

// The collider overlay has no vertex buffer or descriptors.  Its vertex
// shader contains the 12 unit-cube edges and expands them with this matrix.
struct ColliderDebugPushConstants {
    glm::mat4 viewProjection;
    glm::mat4 model;
};

struct EngineStats {
    float frametime{0.0f};
    int triangle_count{0};
    int drawcall_count{0};
    int world_drawcall_count{0};
    int portal_drawcall_count{0};
    float scene_update_time{0.0f};
    float mesh_draw_time{0.0f};
};

// Kept independent of ImGuizmo so the editor state remains engine-owned.
enum class EditorGizmoOperation : uint8_t {
    Translate,
    Rotate,
    Scale,
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
    MaterialPipeline portalOffscreenPipeline;
    MaterialPipeline portalCompositePipeline;
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
    // 0 remains the teaching gradient; start new sessions on the asset sky.
    int currentBackgroundEffect{1};
    AllocatedImage _depthImage;
    AllocatedImage _whiteImage;
    AllocatedImage _blackImage;
    AllocatedImage _greyImage;
    AllocatedImage _errorCheckerboardImage;
    // A single equirectangular (2:1) panorama used as the world skybox.
    // It is sampled by the main compute background and portal sky pass.
    AllocatedImage _skyboxImage;
    VkSampler _defaultSamplerLinear{};
    VkSampler _defaultSamplerNearest{};
    VkSampler _skyboxSampler{};
    VkDescriptorSet _skyboxDescriptor{};
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
    // This camera exists only while editing.  It lets the user inspect the
    // scene without moving the Source-style player or changing portal state.
    Camera _editorCamera;
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
        void init_portal_camera_targets();
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
            const GPUSceneData& skyCamera,
            uint32_t stencilReference,
            uint32_t stencilCompareMask = 0xff);
        void draw_collider_debug_bounds(VkCommandBuffer cmd);
        void draw_portal_views(VkCommandBuffer cmd);
        void draw_offscreen_portal_views(VkCommandBuffer cmd);
        void draw_geometry_to_portal_camera(
            VkCommandBuffer cmd,
            const DrawContext& drawContext,
            const glm::mat4& viewProjection,
            VkDescriptorSet sceneDescriptor,
            const AllocatedImage& colorTarget);
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
        void update_time_trial(float deltaTime);
        void reset_time_trial();
        bool apply_scene_spawn_point();
        bool try_traverse_portal(
            const Portal& source,
            const Portal& destination,
            const glm::vec3& previousPosition);
        void build_sandbox_scene();
        bool save_editor_scene();
        bool save_editor_scene_as(std::string_view sceneName);
        bool load_editor_scene();
        bool load_editor_scene_named(std::string_view sceneName);
        void restore_last_editor_scene_name();
        void assign_scene_asset(SceneObject& object, SceneAssetKind assetKind);
        void create_runtime_scene_objects();
        void sync_scene_driven_objects();
        void emit_scene_render_objects(RenderLayer layer, DrawContext& drawContext);
        void rebuild_collision_from_scene();
        void draw_hierarchy_panel();
        void draw_inspector_panel();
        void draw_editor_gizmo();
        void select_scene_object_at_screen_position(int screenX, int screenY);
        void draw_editor_menu();
        void setup_default_dock_layout(uint32_t dockspaceID);
        bool delete_selected_scene_object();
        bool duplicate_selected_scene_object();
        SceneObjectID create_editor_actor(
            const char* baseName,
            SceneAssetKind assetKind,
            bool collidable,
            bool portalPlaceable = false,
            TimeTrialRole timeTrialRole = TimeTrialRole::None);
        SceneObjectID import_gltf_actor(std::string_view modelPath);
        void set_mouse_capture(bool captured);
        void set_editor_mode(bool enabled);
        const Camera& render_camera() const;
        void retract_portals();
        void place_portal(Portal& portal, const Portal& otherPortal);

        PlayerInput _playerInput{};
        PlayerMovement _playerMovement{};
        bool _mouseCaptured{true};
        bool _editorMode{false};
        bool _editorCameraLooking{false};
        bool _showDebugPanels{false};
        bool _showColliderBounds{false};
        bool _resetEditorLayoutRequested{false};
        bool _sceneDirty{false};
        std::string _activeSceneFilename{"sandbox.json"};
        std::array<char, 64> _sceneNameInput{};
        std::array<char, 260> _gltfPathInput{};
        EditorGizmoOperation _gizmoOperation{EditorGizmoOperation::Translate};
        bool _gizmoLocalSpace{false};
        bool _gizmoSnapping{true};
        float _translationSnap{0.5f};
        float _rotationSnapDegrees{15.0f};
        float _scaleSnap{0.1f};
        float _portalTraversalCooldown{0.0f};
        float _physicsAccumulator{0.0f};
        float _timeTrialSeconds{0.0f};
        float _timeTrialBestSeconds{-1.0f};
        bool _timeTrialRunning{false};
        bool _timeTrialFinished{false};
        bool _playerInsideStartTrigger{false};
        bool _playerInsideFinishTrigger{false};
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
        GPUMeshBuffers _rampMesh;
        Bounds _rampBounds;
        AllocatedBuffer _playerMaterialBuffer;
        MaterialInstance _playerMaterial;
        Bounds _playerBounds;
        GPUMeshBuffers _portalMesh;
        AllocatedBuffer _bluePortalMaterialBuffer;
        MaterialInstance _bluePortalMaterial;
        AllocatedBuffer _orangePortalMaterialBuffer;
        MaterialInstance _orangePortalMaterial;
        Bounds _portalBounds;
        Portal _bluePortal;
        Portal _orangePortal;
        static constexpr uint32_t PortalCameraTargetCount = 2;
        VkExtent2D _portalCameraExtent{640, 360};
        std::array<AllocatedImage, PortalCameraTargetCount> _portalCameraImages{};
        AllocatedImage _portalCameraDepthImage;
        std::array<AllocatedBuffer, PortalCameraTargetCount> _portalCameraMaterialBuffers{};
        std::array<MaterialInstance, PortalCameraTargetCount> _portalCameraMaterials{};
        bool _useOffscreenPortalCameras{false};
        bool _portalRecursionEnabled{true};
        std::array<GPUSceneData, PortalViewCount> _portalSceneData{};
        MaterialPipeline _portalSkyPipeline;
        MaterialPipeline _colliderDebugPipeline;
        // The sandbox level lives here: the floor, the boundary walls, the
        // portal test panels, and the player body all render and collide from
        // these objects.  Nothing about the level is hard-coded twice.
        Scene _scene;
        SceneObjectID _sandboxRoot{InvalidSceneObject};
        SceneObjectID _floorObject{InvalidSceneObject};
        SceneObjectID _playerObject{InvalidSceneObject};
        SceneObjectID _playerModelObject{InvalidSceneObject};
        SceneObjectID _bluePortalObject{InvalidSceneObject};
        SceneObjectID _orangePortalObject{InvalidSceneObject};
        SceneObjectID _selectedSceneObject{InvalidSceneObject};
        uint32_t _nextCreatedActorNumber{1};
        std::vector<AABB> _activeWallColliders;
        std::vector<GroundPlane> _activeGroundPlanes;
        std::vector<SurfRamp> _activeSurfRamps;
    };
