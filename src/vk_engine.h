#pragma once
#include "path_trace_scene.h"

#include <array>
#include <unordered_map>
#include <unordered_set>

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
        // Only the acquire semaphore belongs to the frame.  The one the
        // presentation engine waits on is owned per swapchain image instead,
        // because how long it stays in use depends on the image, not on
        // which of the two frames in flight rendered it.
        VkSemaphore _swapchainSemaphore;
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

// The sunlight depth pass binds no descriptor sets.  Folding the light's
// view-projection into the model matrix on the CPU keeps its vertex shader
// free of scene and material data.
struct ShadowPushConstants {
    glm::mat4 lightMatrix;
    VkDeviceAddress vertexBuffer;
};

// The debug view is a fullscreen triangle over the finished image.
struct RenderDebugPushConstants {
    // x = RenderDebugView, yz = rendered extent in pixels, w = the distance
    // that reads as white in the depth view.
    glm::vec4 settings{0.0f};
};

// The anti-aliasing pass is a fullscreen filter over the composed image.
struct FXAAPushConstants {
    // xy = one texel in UV space, taken from the draw image allocation
    // rather than the rendered region.  z = edge contrast threshold,
    // w = subpixel aliasing removal strength.
    glm::vec4 settings{0.0f};
    // xy = the largest UV the current render scale rendered into, so an edge
    // search cannot walk into the part of the image this frame never touched.
    // z = 1 while the detected edges are shown instead of the image.
    glm::vec4 limits{0.0f};
};

// Shared by the occlusion sampling pass and both blur directions.  The field
// meanings are documented once, in shaders/ssao_common.glsl.
struct SSAOPushConstants {
    glm::ivec4 extents{0};
    glm::vec4 texelSize{0.0f};
    glm::vec4 activeFraction{0.0f};
    glm::vec4 screenTexel{0.0f};
    glm::vec4 settings{0.0f};
};

// The occlusion kernel is a fixed set of points in the +Z hemisphere, sent to
// the GPU once.  vec4 rather than vec3 because std140 pads an array element
// out to sixteen bytes either way.
constexpr uint32_t MaxSSAOKernelSize = 64;
struct SSAOKernelBlock {
    std::array<glm::vec4, MaxSSAOKernelSize> samples{};
};

// How much of the surrounding hemisphere reaches a surface.  These describe
// the lighting of a level, so they travel with the scene; the sample count and
// resolution below them describe what a machine can afford and do not.
struct SSAOSettings {
    // Disabled by default while the screen-space pass still exhibits planar
    // banding. It remains available in the rendering controls for debugging.
    bool enabled{false};
    // The size of the neighbourhood that can occlude a point, in world units.
    // It depends entirely on the scale the level was authored at.
    float radius{0.75f};
    // How far a sample must be behind a surface before it counts as blocked.
    // Too little and surfaces shadow themselves into speckle; too much and
    // occlusion detaches from the corners that produced it.
    float bias{0.025f};
    float intensity{1.0f};
    float power{1.5f};
};

// Sample counts, cheapest first.  Each is a separate pipeline built from the
// same shader with a different specialization constant, so a lower quality
// really does less work rather than skipping loop iterations.
constexpr std::array<int, 3> SSAOKernelSizes{16, 32, 64};

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
    // The shadow and prepass passes draw the world again with their own
    // culling, so their cost is invisible in the counters above.
    int shadow_drawcall_count{0};
    int shadow_triangle_count{0};
    float shadow_record_time{0.0f};
    int prepass_drawcall_count{0};
    int prepass_triangle_count{0};
    float prepass_record_time{0.0f};
    // Ambient occlusion runs entirely on the GPU in three dispatches, so CPU
    // recording time says nothing about what it costs.  These come from
    // timestamp queries when the device supports them.
    int ssao_width{0};
    int ssao_height{0};
    int ssao_kernel_samples{0};
    float ssao_raw_time{0.0f};
    float ssao_blur_horizontal_time{0.0f};
    float ssao_blur_vertical_time{0.0f};
    float ssao_total_time{0.0f};
    bool ssao_time_is_gpu{false};
};

// Which intermediate buffer to show instead of the shaded image.  These are
// the inputs screen-space effects consume, and a mistake in one of them is
// far easier to see here than in a finished ambient-occlusion term.
enum class RenderDebugView : int {
    None = 0,
    Depth = 1,
    ViewNormals = 2,
    ViewPosition = 3,
    WorldPosition = 4,
    // Each occlusion stage on its own.  Seeing the raw pass separately from
    // the filtered one is the only way to tell noise apart from a blur that
    // is smearing occlusion across a silhouette.
    OcclusionRaw = 5,
    OcclusionBlurred = 6,
    OcclusionFinal = 7,
};

struct SceneMaterialRuntime {
    AllocatedBuffer constantsBuffer{};
    MaterialInstance material{};
    std::string texturePath;
    glm::vec4 colorTint{1.0f};
    glm::vec2 uvScale{1.0f};
    float metallic{0.0f};
    float roughness{0.8f};
    bool debugChecker{false};
    bool initialized{false};
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
    enum class RendererMode { Raster, SoftwarePathTrace };
    RendererMode _rendererMode{RendererMode::Raster};
    bool _traceSupported{false};
    std::string _traceStatus{"Software tracer not initialized"};
    VkPipeline _tracePipeline{};
    VkPipelineLayout _traceLayout{};
    VkDescriptorSetLayout _traceSetLayout{};
    DescriptorAllocator _tracePool{};
    VkDescriptorSet _traceSet{};
    AllocatedImage _traceAccum{};
    bool _traceImageInitialized{false};
    void init_path_trace();
    void destroy_path_trace();
    void draw_path_trace(VkCommandBuffer cmd);
    void draw_path_trace_ui();
    void validate_path_trace();
    std::unordered_map<VkDeviceAddress,std::weak_ptr<TraceMeshSource>> _traceMeshSources;
    std::vector<TraceTriangle> _traceTriangles;
    std::vector<TraceMaterial> _traceMaterials;
    AllocatedBuffer _traceTriangleBuffer{}, _traceMaterialBuffer{};
    uint64_t _traceSceneRevision{0}, _traceSceneHash{0};
    void update_trace_scene();
    std::vector<TraceBVHNode> _traceNodes;
    AllocatedBuffer _traceNodeBuffer{};
    int _traceDebugView{8};
    int _traceMaxDepth{2};
    int _traceBaseSeed{1337};
    void capture_path_trace(const char* filename);
    uint32_t _traceSamples{0};
    uint64_t _traceInputHash{0};
    bool _traceWasActive{false};
    float _traceExposure{0};
    VkQueryPool _traceTimestampPool{};
    bool _traceTimingWritten[FRAME_OVERLAP]{};
    float _traceGpuMs{0}, _traceMaxGpuMs{0};
    TraceLighting _traceLighting{};
    AllocatedBuffer _traceLightBuffer{}, _traceEmitterBuffer{};
    AllocatedImage _traceDirect{}, _traceIndirect{};
    std::vector<uint32_t> _traceEmitters;
    std::vector<glm::vec4> read_trace_image(const AllocatedImage& image);
    std::vector<uint32_t> _traceTexels;
    AllocatedBuffer _traceTexelBuffer{};
    int _traceMaterialModel{1};
    uint64_t _traceDrawHash{0};
    std::vector<TracePortal> _tracePortals;
    AllocatedBuffer _tracePortalBuffer{};
    int _tracePortalLimit{2};
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
    // One render-finished semaphore per swapchain image.  With two frames in
    // flight and three images, a per-frame semaphore could be signalled again
    // while an earlier presentation of a different image still waited on it.
    std::vector<VkSemaphore> _swapchainRenderSemaphores;
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
        bool process_event(const SDL_Event& event);
        void draw_frame_ui(float deltaTime);
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
        void init_default_images_and_samplers();
        void init_default_meshes();
        void init_default_materials();
        void init_default_scene();
        void init_portal_camera_targets();
        void init_shadow_resources();
        void init_shadow_pipeline();
        void draw_shadow_map(VkCommandBuffer cmd);
        void init_depth_normal_resources();
        void init_depth_normal_pipeline();
        void init_render_debug_pipeline();
        void init_post_process_resources();
        void init_fxaa_pipeline();
        void init_ssao_resources();
        void init_ssao_pipelines();
        void init_gpu_timestamps();
        void read_gpu_timestamps(uint32_t frameIndex);
        void draw_depth_normal_prepass(VkCommandBuffer cmd);
        void draw_ssao(VkCommandBuffer cmd);
        SSAOPushConstants build_ssao_push_constants() const;
        // Half the rendered region, rounded up, which is what the dispatches
        // cover.  The images themselves stay allocated at half the window.
        VkExtent2D active_ssao_extent() const;
        bool ssao_active() const;
        void draw_render_debug(VkCommandBuffer cmd);
        void draw_fxaa(VkCommandBuffer cmd);
        glm::mat4 compute_sun_view_projection(const glm::vec3& focusPoint) const;
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
        void respawn_player();
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
        MaterialInstance* resolve_scene_material(const SceneObject& object);
        const AllocatedImage& load_scene_texture(std::string_view texturePath);
        void clear_scene_material_resources();
        void rebuild_collision_from_scene();
        void draw_hierarchy_panel();
        void draw_inspector_panel();
        void draw_editor_gizmo();
        void select_scene_object_at_screen_position(int screenX, int screenY);
        void draw_editor_menu();
        void setup_default_dock_layout(uint32_t dockspaceID);
        bool delete_selected_scene_object();
        bool duplicate_selected_scene_object();
        SceneObjectID create_room_with_center_pole_prefab();
        SceneObjectID create_closed_long_room_prefab();
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
        void place_authored_portal_endpoint();
        void clear_authored_portals();
        bool create_three_room_pole_chain();

        PlayerInput _playerInput{};
        PlayerMovement _playerMovement{};
        bool _mouseCaptured{true};
        bool _editorMode{false};
        bool _noClipMode{false};
        bool _noClipUp{false};
        bool _noClipDown{false};
        float _noClipSpeed{12.0f};
        bool _editorCameraLooking{false};
        bool _showDebugPanels{false};
        bool _showColliderBounds{false};
        bool _resetEditorLayoutRequested{false};
        bool _sceneDirty{false};
        std::string _activeSceneFilename{"sandbox.json"};
        std::array<char, 64> _sceneNameInput{};
        std::array<char, 260> _gltfPathInput{};
        std::array<char, 260> _texturePathInput{};
        SceneObjectID _materialEditorObject{InvalidSceneObject};
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
        static constexpr size_t MaxAuthoredPortalPairs = (MaxPortalSurfaces - 2) / 2;
        std::vector<AuthoredPortalPair> _authoredPortalPairs;
        std::optional<Portal> _authoredPortalDraft;
        int _selectedAuthoredPortalPair{-1};
        bool _selectedAuthoredPortalSecond{false};
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
        // One directional shadow map covers a box centred on the active
        // camera.  Every camera in the frame - main and portal - samples it,
        // because the lookup is done from world-space positions.
        static constexpr uint32_t ShadowMapResolution = 2048;
        AllocatedImage _shadowMapImage;
        VkSampler _shadowSampler{};
        MaterialPipeline _shadowPipeline;
        glm::mat4 _sunViewProjection{1.0f};
        // Points from a surface towards the sun, matching how the fragment
        // shaders use it.  The light itself travels along its negation.
        glm::vec3 _sunlightDirection{0.0f, 1.0f, 0.5f};
        bool _shadowsEnabled{true};
        bool _showShadowBounds{false};
        // Half-width of the shadowed box, in world units.
        float _shadowRadius{60.0f};
        // Extra depth in front of and behind that box, so a caster standing
        // outside the lit region still reaches the map.
        float _shadowDepthMargin{120.0f};
        float _shadowDepthBias{0.0006f};
        float _shadowNormalBias{0.08f};
        // Reported in the statistics panel; chosen from what the GPU
        // actually supports rather than assumed.
        const char* _shadowFormatName{"none"};
        // Depth and view-space normals for the main camera, written before
        // the colour pass.  Ambient occlusion is the first consumer, but
        // reflections, outlines, and depth-aware fog need the same two
        // images.  They are deliberately separate from _depthImage, whose
        // depth and stencil contents the portal passes overwrite.
        AllocatedImage _prepassDepthImage;
        AllocatedImage _prepassNormalImage;
        VkSampler _prepassSampler{};
        VkDescriptorSetLayout _prepassImageDescriptorLayout{};
        // The prepass targets are allocated once at window size, so one
        // persistent set describes them for the whole run.
        VkDescriptorSet _prepassImageDescriptor{};
        MaterialPipeline _depthNormalPipeline;
        MaterialPipeline _renderDebugPipeline;
        bool _depthNormalPrepassEnabled{true};
        // Ambient occlusion, at half resolution.  Three images rather than
        // one because a compute pass cannot read and write the same storage
        // image, and because each stage is then separately inspectable.
        AllocatedImage _ssaoRawImage;
        AllocatedImage _ssaoBlurImage;
        AllocatedImage _ssaoFinalImage;
        // 16x16 tiled rotation vectors.  Turning the kernel differently at
        // neighbouring pixels converts a visible banding pattern into noise,
        // which the bilateral blur can then remove.
        AllocatedImage _ssaoNoiseImage;
        AllocatedBuffer _ssaoKernelBuffer;
        // Linear and clamped: material shading samples the half-resolution
        // result at full resolution, so the taps land between texels.
        VkSampler _ssaoSampler{};
        VkSampler _ssaoNoiseSampler{};
        VkDescriptorSetLayout _ssaoDescriptorLayout{};
        VkDescriptorSetLayout _ssaoBlurDescriptorLayout{};
        VkDescriptorSetLayout _ssaoDebugDescriptorLayout{};
        VkDescriptorSet _ssaoDescriptor{};
        VkDescriptorSet _ssaoBlurHorizontalDescriptor{};
        VkDescriptorSet _ssaoBlurVerticalDescriptor{};
        VkDescriptorSet _ssaoDebugDescriptor{};
        VkPipelineLayout _ssaoPipelineLayout{};
        VkPipelineLayout _ssaoBlurPipelineLayout{};
        // One per entry in SSAOKernelSizes.
        std::array<VkPipeline, SSAOKernelSizes.size()> _ssaoPipelines{};
        VkPipeline _ssaoBlurPipeline{};
        VkExtent2D _ssaoExtent{0, 0};
        VkFormat _ssaoFormat{VK_FORMAT_UNDEFINED};
        const char* _ssaoFormatName{"none"};
        SSAOSettings _ssaoSettings{};
        // A machine setting, not a scene one: it buys quality with GPU time
        // and says nothing about how the level is lit.
        int _ssaoQuality{1};
        // Drops the sunlight term so occlusion can be judged on its own.  With
        // ambient light at zero as well, a correct implementation produces no
        // visible difference at all.
        bool _ssaoAmbientOnly{false};
        // How sharply the blur rejects a neighbour on a different surface.
        float _ssaoDepthFalloff{12.0f};
        float _ssaoNormalFalloff{16.0f};
        // Four marks per frame - before the sampling pass and after each of
        // the three dispatches - read back once the frame's fence has passed.
        static constexpr uint32_t TimestampsPerFrame = 4;
        VkQueryPool _timestampPool{VK_NULL_HANDLE};
        // Nanoseconds per timestamp tick, from the device.
        float _timestampPeriod{0.0f};
        bool _gpuTimingSupported{false};
        std::array<bool, FRAME_OVERLAP> _timestampsPending{};
        // Anti-aliasing runs last, on the composed colour, so one filter
        // covers world geometry, portal contents, and the debug overlays
        // without any pass needing to know about it.  It cannot read and
        // write _drawImage at once, so it resolves into this second image and
        // that is what reaches the swapchain while it is enabled.
        AllocatedImage _postProcessImage;
        VkSampler _postProcessSampler{};
        // _drawImage bound as a texture.  It is allocated once, so this set
        // is written once and never revisited.
        VkDescriptorSet _fxaaInputDescriptor{};
        MaterialPipeline _fxaaPipeline;
        bool _fxaaEnabled{true};
        // The fraction of the local maximum luma a pixel must differ by
        // before it counts as an edge.  Lower catches more, at the cost of
        // filtering detail that was never aliased.
        // Preserve more fine surface detail while still smoothing strong
        // silhouette edges. The previous settings softened the whole image.
        float _fxaaEdgeThreshold{0.10f};
        // How strongly features too small for the edge search to trace - a
        // thin pole, a specular sparkle - are blended towards their
        // neighbourhood.
        float _fxaaSubpixelStrength{0.30f};
        bool _fxaaShowEdges{false};
        RenderDebugView _renderDebugView{RenderDebugView::None};
        // How far from the camera reads as white in the depth debug view.
        float _renderDebugDepthRange{60.0f};
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
        std::unordered_map<SceneObjectID, SceneMaterialRuntime> _sceneMaterialRuntimes;
        std::unordered_map<std::string, AllocatedImage> _sceneTextureCache;
        std::unordered_set<std::string> _failedSceneTextures;
    };
