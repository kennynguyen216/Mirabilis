#pragma once
#include "path_trace_scene.h"

#include <algorithm>
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
struct TonemapPushConstants {
    // x = exposure multiplier applied before the curve, y = operator
    // (0 ACES, 1 Reinhard), z = 1 while the curve is bypassed and only the
    // sRGB encode runs.
    glm::vec4 settings{1.0f, 0.0f, 0.0f, 0.0f};
};

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

// One unfiltered SSGI sample per pixel. extents.xy is the live render size;
// settings are ray length, thickness, start offset, and step count.
struct SSGIPushConstants {
    glm::uvec4 control{0};
    glm::vec4 settings{0.0f};
    // x = temporal history weight, y = depth rejection threshold,
    // z = normal dot threshold, w = velocity rejection threshold.
    glm::vec4 temporal{0.0f};
    // x = rays per pixel; remaining components reserved.
    glm::uvec4 quality{1u, 0u, 0u, 0u};
};

struct SSGIFilterPushConstants {
    glm::ivec4 control{0};
    glm::vec4 settings{0.0f};
};

struct SSGICompositePushConstants {
    // xy = full render extent, zw = active SSGI extent.
    glm::vec4 extents{0.0f};
    // x = indirect intensity, y = half-resolution flag.
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
    // Offset the sample hemisphere along the surface normal, in world units.
    // Too little and surfaces shadow themselves into speckle; too much and
    // occlusion detaches from the corners that produced it.
    float bias{0.075f};
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
    float ssgi_raw_time{0.0f};
    float ssgi_temporal_time{0.0f};
    float ssgi_filter_time{0.0f};
    float ssgi_composite_time{0.0f};
    float ssgi_total_time{0.0f};
    bool ssgi_time_is_gpu{false};
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
    Albedo = 8,
    MotionVectors = 9,
    PortalMask = 10,
    DirectLighting = 11,
    SSGIRaw = 12,
    SSGIHitMiss = 13,
    SSGISteps = 14,
    SSGITemporal = 15,
    SSGIHistoryRejection = 16,
    SSGIReprojection = 17,
    SSGIFiltered = 18,
    SSGIFallback = 19,
    SSGIReferenceDifference = 20,
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
    // The alpha-tested counterparts of the three passes that shade geometry.
    // They exist as separate pipelines rather than as a branch inside the
    // opaque ones so that discard - which costs every opaque surface its
    // early depth rejection - is confined to the materials that need it.
    MaterialPipeline maskPipeline;
    MaterialPipeline portalViewMaskPipeline;
    MaterialPipeline portalOffscreenMaskPipeline;
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
    // Render internally below the window size by default.  The swapchain
    // remains 1280x720, while the expensive prepass/SSGI work starts at 75%
    // resolution and is upscaled for presentation.
    float renderScale {0.75f};
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
    void capture_ssgi(const char* filename);
    std::vector<glm::vec4> read_ssgi_image(
        const AllocatedImage& image, VkExtent2D extent);
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
    // Only the selected equirectangular panorama is resident.  Keeping the two
    // 4K HDR choices out of memory until selected saves roughly 64 MiB each.
    inline static constexpr std::array<const char*, 3> SkyboxDisplayNames{
        "Legacy PNG", "Qwantani Noon (HDR)", "Kloofendal Clear (HDR)"};
    inline static constexpr std::array<const char*, 3> SkyboxIds{
        "legacy", "qwantani_noon", "kloofendal_clear"};
    inline static constexpr std::array<const char*, 3> SkyboxPaths{
        "../../assets/textures/skybox.png",
        "../../assets/textures/skyboxes/qwantani_noon_puresky_4k.hdr",
        "../../assets/textures/skyboxes/kloofendal_43d_clear_puresky_4k.hdr"};
    // Legacy scenes with no saved choice retain their previous appearance.
    int _skyboxSelection{0};
    AllocatedImage _skyboxImage{};
    VkSampler _defaultSamplerLinear{};
    VkSampler _defaultSamplerNearest{};
    VkSampler _skyboxSampler{};
    // A second view of the same panorama for the SSGI trace.  The background
    // pass wants it sharp at level 0; an indirect ray wants a coarse mip, so
    // this one is the only sampler allowed past the first level.
    VkSampler _skyboxEnvironmentSampler{};
    // The mip a traced miss ray samples, derived from the loaded panorama's
    // width so a 1x1 fallback and a 4K HDR both land on a sane footprint.
    float _skyboxEnvironmentLod{0.0f};
    // The radiance ceiling a traced miss ray is allowed to return.  A clear-
    // sky HDR keeps most of its energy in the sun disk -- four orders of
    // magnitude above the sky, and a fraction of a percent of its pixels --
    // and that disk is already delivered by the shadow-mapped direct term.
    // Derived per panorama rather than fixed, because how bright the sky is
    // relative to its sun is a property of the capture.
    float _skyboxIndirectClamp{1.0e4f};
    // Anisotropic filtering is an optional device feature, so nothing may
    // request it before init_vulkan has both confirmed support and read the
    // device's ceiling.  _textureAnisotropy is the preset's requested level;
    // material_anisotropy() is what a sampler is actually allowed to ask for.
    bool _samplerAnisotropySupported{false};
    float _maxSamplerAnisotropy{1.0f};
    float _textureAnisotropy{16.0f};
    // Clamped to the device limit and to 1 when the feature is missing, so a
    // caller can assign the result unconditionally: 1.0 means "off", which is
    // exactly what anisotropyEnable = VK_FALSE would have produced.
    float material_anisotropy() const
    {
        if (!_samplerAnisotropySupported) {
            return 1.0f;
        }
        return std::clamp(_textureAnisotropy, 1.0f, _maxSamplerAnisotropy);
    }
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
        void draw_sun_shadow_settings();
        void draw_ambient_occlusion_settings();
        void draw_tonemap_settings();
        void draw_antialiasing_settings();
        void draw_screen_buffer_settings();
        void draw_background_effect_settings();
        void draw_movement_tuning_panel();
        void draw_statistics_panel(float horizontalSpeed);
        void draw_play_overlay(float horizontalSpeed);
        void init_swapchain();
        void init_commands();
        void init_sync_structures();
        void init_descriptors();
        void init_descriptor_pools();
        void init_background_descriptors();
        void init_post_process_descriptors();
        void init_scene_descriptors();
        void init_ssao_descriptors();
        void init_prepass_descriptors();
        void init_ssgi_descriptors();
        void init_descriptor_cleanup();
        VkImageView ssao_occlusion_view() const;
        VkSampler ssao_occlusion_sampler() const;
        void create_swapchain(uint32_t width, uint32_t height);
        void destroy_swapchain();
        void resize_swapchain();
        void draw_background(VkCommandBuffer cmd);
        void init_pipelines();
        void init_background_pipelines();
        void init_default_data();
        bool set_skybox(int selection);
        void update_skybox_descriptors();
        void write_ssgi_trace_descriptors();
        void init_default_images_and_samplers();
        void init_default_meshes();
        void init_default_materials();
        void init_default_scene();
        void init_portal_camera_targets();
        void init_shadow_resources();
        void init_shadow_pipeline();
        void init_shadow_mask_pipeline();
        void draw_shadow_map(VkCommandBuffer cmd);
        void init_depth_normal_resources();
        void init_ssgi_resources();
        void init_depth_normal_pipeline();
        void init_depth_normal_mask_pipeline();
        void init_render_debug_pipeline();
        void init_post_process_resources();
        void init_tonemap_pipeline();
        void init_fxaa_pipeline();
        void init_ssao_resources();
        void init_ssao_pipelines();
        void init_ssgi_pipelines();
        void init_gpu_timestamps();
        void read_gpu_timestamps(uint32_t frameIndex);
        void draw_depth_normal_prepass(VkCommandBuffer cmd);
        void draw_ssao(VkCommandBuffer cmd);
        void draw_ssgi_portal_mask(VkCommandBuffer cmd);
        void draw_ssgi(VkCommandBuffer cmd);
        void draw_ssgi_composite(VkCommandBuffer cmd);
        VkExtent2D active_ssgi_extent() const;
        void apply_ssgi_quality_preset(int preset);
        void apply_max_fidelity_settings();
        SSAOPushConstants build_ssao_push_constants() const;
        // Half the rendered region, rounded up, which is what the dispatches
        // cover.  The images themselves stay allocated at half the window.
        VkExtent2D active_ssao_extent() const;
        bool ssao_active() const;
        void load_ao_preferences();
        void save_ao_preferences() const;
        void draw_render_debug(VkCommandBuffer cmd);
        void draw_tonemap(VkCommandBuffer cmd);
        void draw_fxaa(VkCommandBuffer cmd);
        glm::mat4 compute_sun_view_projection(const glm::vec3& focusPoint) const;
        void draw_geometry(
            VkCommandBuffer cmd,
            const DrawContext& drawContext,
            const glm::mat4& viewProjection,
            VkDescriptorSet sceneDescriptor,
            bool clearDepthAndStencil,
            MaterialPipeline* overridePipeline = nullptr,
            // Used in place of overridePipeline for alpha-masked materials.
            // Null means masked surfaces fall back to overridePipeline, which
            // is right for the passes that write a stencil or a mask rather
            // than shading anything.
            MaterialPipeline* overrideMaskPipeline = nullptr,
            uint32_t stencilReference = 0,
            bool useFrustumCulling = true,
            uint32_t stencilCompareMask = 0xff,
            bool writeGBuffer = false);
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
        AllocatedImage create_image(
            void* data,
            size_t dataSize,
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
        struct NoClipState {
            bool enabled{false};
            bool up{false};
            bool down{false};
            float speed{12.0f};
        };
        NoClipState _noClip;
        bool _editorCameraLooking{false};
        struct EditorUiState {
            bool showDebugPanels{false};
            bool resetLayoutRequested{false};
        };
        EditorUiState _editorUi;
        struct SceneDocumentState {
            bool dirty{false};
            std::string activeFilename{"sandbox.json"};
        };
        SceneDocumentState _sceneDocument;
        struct EditorInputState {
            std::array<char, 64> sceneName{};
            std::array<char, 260> gltfPath{};
        };
        EditorInputState _editorInputs;
        struct MaterialEditorState {
            std::array<char, 260> texturePathInput{};
            SceneObjectID object{InvalidSceneObject};
        };
        MaterialEditorState _materialEditor;
        struct EditorGizmoState {
            EditorGizmoOperation operation{EditorGizmoOperation::Translate};
            bool localSpace{false};
            bool snapping{true};
            float translationSnap{0.5f};
            float rotationSnapDegrees{15.0f};
            float scaleSnap{0.1f};
        };
        EditorGizmoState _editorGizmo;
        struct PhysicsStepState {
            float portalTraversalCooldown{0.0f};
            float accumulator{0.0f};
            // Camera yaw is sampled once per rendered frame, but physics runs at a
            // fixed step that can tick up to MaxPhysicsSteps times for that one
            // frame.  Holding both ends of the frame's turn lets each tick receive
            // its own share of it, so air control and strafing stay the same at 30
            // and 300 fps.  Portal traversal snaps both: that rotation is a
            // discontinuity, not something the player turned through.
            float previousPlayerYaw{0.0f};
            float targetPlayerYaw{0.0f};
        };
        PhysicsStepState _physicsStep;
        struct TimeTrialState {
            float seconds{0.0f};
            float bestSeconds{-1.0f};
            bool running{false};
            bool finished{false};
            bool playerInsideStartTrigger{false};
            bool playerInsideFinishTrigger{false};
        };
        TimeTrialState _timeTrial;
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
        struct AuthoredPortalState {
            std::vector<AuthoredPortalPair> pairs;
            std::optional<Portal> draft;
            int selectedPair{-1};
            bool selectedSecond{false};
        };
        AuthoredPortalState _authoredPortals;
        static constexpr uint32_t PortalCameraTargetCount = 2;
        struct PortalCameraState {
            VkExtent2D extent{640, 360};
            std::array<AllocatedImage, PortalCameraTargetCount> images{};
            AllocatedImage depthImage;
            std::array<AllocatedBuffer, PortalCameraTargetCount> materialBuffers{};
            std::array<MaterialInstance, PortalCameraTargetCount> materials{};
            bool useOffscreen{false};
        };
        PortalCameraState _portalCameras;
        struct PortalRenderState {
            bool recursionEnabled{true};
            std::array<GPUSceneData, PortalViewCount> sceneData{};
            MaterialPipeline skyPipeline;
        };
        PortalRenderState _portalRender;
        struct DebugViewState {
            MaterialPipeline colliderPipeline;
            MaterialPipeline renderPipeline;
            bool showColliderBounds{false};
            RenderDebugView view{RenderDebugView::None};
            // How far from the camera reads as white in the depth debug view.
            float depthRange{60.0f};
        };
        DebugViewState _debugViews;
        // One directional shadow map covers a box centred on the active
        // camera.  Every camera in the frame - main and portal - samples it,
        // because the lookup is done from world-space positions.
        static constexpr uint32_t ShadowMapResolution = 4096;
        struct ShadowState {
            AllocatedImage mapImage;
            // Alpha-tested shadow casting.  The opaque shadow pipeline binds
            // no descriptors at all, so this one cannot share its layout: it
            // needs the per-material set to sample the base colour's alpha.
            MaterialPipeline maskPipeline;
            VkSampler sampler{};
            MaterialPipeline pipeline;
            glm::mat4 sunViewProjection{1.0f};
            // Points from a surface towards the sun, matching how the
            // fragment shaders use it.  The light itself travels along its
            // negation.
            glm::vec3 sunlightDirection{0.0f, 1.0f, 0.5f};
            bool enabled{true};
            bool showBounds{false};
            // Half-width of the shadowed box, in world units.
            float radius{60.0f};
            // Extra depth in front of and behind that box, so a caster
            // standing outside the lit region still reaches the map.
            float depthMargin{120.0f};
            float depthBias{0.0006f};
            float normalBias{0.08f};
            float filterRadius{3.0f};
            // Reported in the statistics panel; chosen from what the GPU
            // actually supports rather than assumed.
            const char* formatName{"none"};
        };
        ShadowState _shadow;
        glm::mat4 _previousMainViewProjection{1.0f};
        bool _previousMainViewProjectionValid{false};
        struct PrepassState {
            // Depth and view-space normals for the main camera, written before
            // the colour pass.  Ambient occlusion is the first consumer, but
            // reflections, outlines, and depth-aware fog need the same two
            // images.  They are deliberately separate from _depthImage, whose
            // depth and stencil contents the portal passes overwrite.
            AllocatedImage depthImage;
            AllocatedImage normalImage;
            VkSampler sampler{};
            VkDescriptorSetLayout imageDescriptorLayout{};
            // The prepass targets are allocated once at window size, so one
            // persistent set describes them for the whole run.
            VkDescriptorSet imageDescriptor{};
            MaterialPipeline pipeline;
            // Alpha-tested prepass.  Without it, occlusion and screen-space GI
            // would be computed against the quad a leaf texture is drawn on
            // rather than against the leaves.
            MaterialPipeline maskPipeline;
            bool enabled{true};
        };
        PrepassState _prepass;
        struct SceneRenderTargets {
            AllocatedImage gbufferAlbedo;
            AllocatedImage gbufferVelocity;
            AllocatedImage directLighting;
            AllocatedImage portalMask;
            std::array<AllocatedImage, 2> directLightingHistory{};
        };
        SceneRenderTargets _sceneTargets;
        struct SSGIState {
            AllocatedImage rawImage;
            AllocatedImage debugImage;
            AllocatedImage fallbackImage;
            std::array<AllocatedImage, 2> temporalHistory{};
            std::array<AllocatedImage, 2> metadataHistory{};
            AllocatedImage temporalDiagnosticImage;
            AllocatedImage filterScratchImage;
            AllocatedImage filteredImage;
            AllocatedImage referenceImage;
            bool referenceLoaded{false};
            VkDescriptorSetLayout descriptorLayout{};
            std::array<VkDescriptorSet, 2> descriptors{};
            VkDescriptorSetLayout debugDescriptorLayout{};
            std::array<VkDescriptorSet, 2> debugDescriptors{};
            VkDescriptorSetLayout filterDescriptorLayout{};
            std::array<VkDescriptorSet, 3> filterDescriptors{};
            MaterialPipeline portalMaskPipeline;
            VkPipelineLayout pipelineLayout{};
            VkPipeline pipeline{};
            VkPipeline temporalPipeline{};
            VkPipelineLayout filterPipelineLayout{};
            VkPipeline filterPipeline{};
            VkDescriptorSetLayout compositeDescriptorLayout{};
            std::array<VkDescriptorSet, 2> compositeDescriptors{};
            MaterialPipeline compositePipeline;
            bool enabled{true};
            bool historyValid{false};
            uint32_t historyWriteIndex{0};
            VkExtent2D historyExtent{0, 0};
            int stepCount{32};
            int raysPerPixel{4};
            float rayLength{12.0f};
            float thickness{0.35f};
            float startOffset{0.08f};
            float historyWeight{0.92f};
            float depthRejection{0.003f};
            float normalRejection{0.85f};
            float velocityRejection{0.10f};
            bool spatialFilterEnabled{true};
            int filterRadius{3};
            float filterDepthFalloff{800.0f};
            float filterNormalPower{32.0f};
            float intensity{1.0f};
            // How much of the flat ambient term survives while SSGI is on.
            // Zero is the coherent setting in the sense that nothing is
            // counted twice, but it is not the honest one yet: the bounce this
            // renderer gathers comes from a direct-lighting buffer that
            // excludes ambient, so stone out of the sun contributes nothing
            // at all to it, and an arcade lit only by what the sun reaches
            // reads far darker than the same geometry in life.  Half is a
            // starting point to tune by eye, not a derived value, and it is a
            // slider because the right answer depends on how enclosed the
            // scene is.
            float ambientRetention{0.5f};
            // Fill ray misses from the selected skybox rather than the
            // analytic gradient.  Turning this off restores parity with the
            // software path tracer, which still lights its misses from the
            // gradient.
            bool traceEnvironmentMap{true};
            bool halfResolution{false};
            int qualityPreset{0};
            VkQueryPool timestampPool{VK_NULL_HANDLE};
            std::array<bool, FRAME_OVERLAP> timingWritten{};
        };
        SSGIState _ssgi;
        struct SSAOState {
            // Ambient occlusion, at half resolution.  Three images rather than
            // one because a compute pass cannot read and write the same
            // storage image, and because each stage is then separately
            // inspectable.
            AllocatedImage rawImage;
            AllocatedImage blurImage;
            AllocatedImage finalImage;
            // 16x16 tiled rotation vectors.  Turning the kernel differently at
            // neighbouring pixels converts a visible banding pattern into
            // noise, which the bilateral blur can then remove.
            AllocatedImage noiseImage;
            AllocatedBuffer kernelBuffer;
            // Linear and clamped: material shading samples the half-resolution
            // result at full resolution, so the taps land between texels.
            VkSampler sampler{};
            VkSampler noiseSampler{};
            VkDescriptorSetLayout descriptorLayout{};
            VkDescriptorSetLayout blurDescriptorLayout{};
            VkDescriptorSetLayout debugDescriptorLayout{};
            VkDescriptorSet descriptor{};
            VkDescriptorSet blurHorizontalDescriptor{};
            VkDescriptorSet blurVerticalDescriptor{};
            VkDescriptorSet debugDescriptor{};
            VkPipelineLayout pipelineLayout{};
            VkPipelineLayout blurPipelineLayout{};
            // One per entry in SSAOKernelSizes.
            std::array<VkPipeline, SSAOKernelSizes.size()> pipelines{};
            VkPipeline blurPipeline{};
            VkExtent2D extent{0, 0};
            VkFormat format{VK_FORMAT_UNDEFINED};
            const char* formatName{"none"};
            SSAOSettings settings{};
            bool sceneOverride{false};
            bool globalEnabled{true};
            // A machine setting, not a scene one: it buys quality with GPU
            // time and says nothing about how the level is lit.
            int quality{1};
            // Drops the sunlight term so occlusion can be judged on its own.
            // With ambient light at zero as well, a correct implementation
            // produces no visible difference at all.
            bool ambientOnly{false};
            // How sharply the blur rejects a neighbour on a different surface.
            float depthFalloff{12.0f};
            float normalFalloff{16.0f};
        };
        SSAOState _ssao;
        // Four marks per frame - before the sampling pass and after each of
        // the three dispatches - read back once the frame's fence has passed.
        static constexpr uint32_t TimestampsPerFrame = 4;
        struct GpuTimingState {
            VkQueryPool timestampPool{VK_NULL_HANDLE};
            // Nanoseconds per timestamp tick, from the device.
            float timestampPeriod{0.0f};
            bool supported{false};
            std::array<bool, FRAME_OVERLAP> timestampsPending{};
        };
        GpuTimingState _gpuTiming;
        static constexpr uint32_t SSGITimestampsPerFrame = 5;
        // Anti-aliasing runs last, on the composed colour, so one filter
        // covers world geometry, portal contents, and the debug overlays
        // without any pass needing to know about it.  It cannot read and
        // write _drawImage at once, so it resolves into this second image and
        // that is what reaches the swapchain while it is enabled.
        struct PostProcessState {
            AllocatedImage image;
            VkSampler sampler{};
            // Tonemap output. The tonemap reads _drawImage and resolves into
            // this image in the swapchain's own 8-bit format; anti-aliasing
            // then runs on that, because FXAA's edge detection assumes
            // display-range values and behaves poorly on the unbounded ones it
            // used to be handed.
            AllocatedImage tonemapImage;
            // _drawImage and tonemapImage bound as textures.  They are
            // allocated once, so these sets are written once and never
            // revisited.
            VkDescriptorSet tonemapInputDescriptor{};
            VkDescriptorSet fxaaInputDescriptor{};
            MaterialPipeline tonemapPipeline;
            MaterialPipeline fxaaPipeline;
            // Off leaves the frame linear and clipped, which is how it looked
            // before this pass existed.  Kept as a comparison, not a default.
            bool tonemapEnabled{true};
            // Stops in photographic terms would be friendlier, but every other
            // exposure-like control in this engine is a plain multiplier.
            float tonemapExposure{1.0f};
            // 0 = ACES filmic, 1 = Reinhard.
            int tonemapOperator{0};
            // Runs the sRGB encode without the curve, so the curve's
            // contribution can be told apart from the transfer function's.
            bool tonemapBypassCurve{false};
            bool fxaaEnabled{true};
            // The fraction of the local maximum luma a pixel must differ by
            // before it counts as an edge.  Lower catches more, at the cost of
            // filtering detail that was never aliased.
            // Preserve more fine surface detail while still smoothing strong
            // silhouette edges. The previous settings softened the whole image.
            float fxaaEdgeThreshold{0.10f};
            // How strongly features too small for the edge search to trace - a
            // thin pole, a specular sparkle - are blended towards their
            // neighbourhood.
            float fxaaSubpixelStrength{0.30f};
            bool fxaaShowEdges{false};
        };
        PostProcessState _postProcess;
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
        std::unordered_map<SceneObjectID, glm::mat4> _previousSceneObjectTransforms;
        std::unordered_map<std::string, AllocatedImage> _sceneTextureCache;
        std::unordered_set<std::string> _failedSceneTextures;
    };
