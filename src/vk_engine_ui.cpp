#include "vk_engine.h"

#include <vk_initializers.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_vulkan.h"
#include <limits>

void VulkanEngine::init_imgui()
{
	// 1: create descriptor pool for IMGUI
	//  the size of the pool is very oversize, but it's copied from imgui demo
	//  itself.
	VkDescriptorPoolSize pool_sizes[] = { { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
		{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 } };

	VkDescriptorPoolCreateInfo pool_info = {};
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	pool_info.maxSets = 1000;
	pool_info.poolSizeCount = (uint32_t)std::size(pool_sizes);
	pool_info.pPoolSizes = pool_sizes;

	VkDescriptorPool imguiPool;
	VK_CHECK(vkCreateDescriptorPool(_device, &pool_info, nullptr, &imguiPool));

	// 2: initialize imgui library

	// this initializes the core structures of imgui
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	// Start the editor with its own layout instead of inheriting the scattered
	// windows saved by the old in-game debug UI.
	io.IniFilename = "mirabilis_editor.ini";

	ImGui::StyleColorsDark();
	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowPadding = ImVec2(10.0f, 8.0f);
	style.FramePadding = ImVec2(7.0f, 4.0f);
	style.ItemSpacing = ImVec2(8.0f, 6.0f);
	style.WindowRounding = 4.0f;
	style.FrameRounding = 3.0f;
	style.GrabRounding = 3.0f;
	style.Colors[ImGuiCol_WindowBg] = ImVec4(0.045f, 0.055f, 0.075f, 0.96f);
	style.Colors[ImGuiCol_TitleBg] = ImVec4(0.055f, 0.075f, 0.105f, 1.0f);
	style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.075f, 0.130f, 0.190f, 1.0f);
	style.Colors[ImGuiCol_Header] = ImVec4(0.090f, 0.180f, 0.290f, 0.85f);
	style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.110f, 0.250f, 0.390f, 0.95f);
	style.Colors[ImGuiCol_Button] = ImVec4(0.080f, 0.190f, 0.310f, 0.90f);
	style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.110f, 0.270f, 0.440f, 1.0f);

	// this initializes imgui for SDL
	ImGui_ImplSDL2_InitForVulkan(_window);

	// this initializes imgui for Vulkan
	ImGui_ImplVulkan_InitInfo init_info = {};
	init_info.ApiVersion = VK_API_VERSION_1_3;
	init_info.Instance = _instance;
	init_info.PhysicalDevice = _chosenGPU;
	init_info.Device = _device;
	init_info.QueueFamily = _graphicsQueueFamily;
	init_info.Queue = _graphicsQueue;
	init_info.DescriptorPool = imguiPool;
	init_info.MinImageCount = 2;
	init_info.ImageCount = static_cast<uint32_t>(_swapchainImages.size());
	init_info.UseDynamicRendering = true;

	//dynamic rendering parameters for imgui to use
	init_info.PipelineInfoMain.PipelineRenderingCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	init_info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
	init_info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &_swapchainImageFormat;
	init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

	ImGui_ImplVulkan_Init(&init_info);
	// The modern Vulkan backend uploads the font texture automatically during
	// its first NewFrame call.

	// add the destroy the imgui created structures
	_mainDeletionQueue.push_function([=]() {
		ImGui_ImplVulkan_Shutdown();
		ImGui_ImplSDL2_Shutdown();
		ImGui::DestroyContext();
		vkDestroyDescriptorPool(_device, imguiPool, nullptr);
	});
}

void VulkanEngine::draw_imgui(VkCommandBuffer cmd, VkImageView targetImageView)
{
	VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(
		targetImageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingInfo renderInfo = vkinit::rendering_info(
		_swapchainExtent, &colorAttachment, nullptr);

	vkCmdBeginRendering(cmd, &renderInfo);
	ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
	vkCmdEndRendering(cmd);
}

void VulkanEngine::draw_profiling_panel()
{
    if (!ImGui::Begin("Profiler")) { ImGui::End(); return; }
    bool enabled = _profiler.enabled();
    if (ImGui::Checkbox("Enable CPU profiling", &enabled)) _profiler.set_enabled(enabled);
    if (ImGui::Button("Start trace")) { _profiler.start_capture(); _profileExportStatus.clear(); }
    ImGui::SameLine();
    if (ImGui::Button("Stop trace")) _profiler.stop_capture();
    ImGui::SameLine();
    if (ImGui::Button("Save trace")) {
        _profiler.stop_capture();
        const auto path = _profileTraceOutput.empty() ? std::filesystem::path("mirabilis-profile.json") : _profileTraceOutput;
        _profileExportStatus = (_profiler.save_trace(path) ? "Saved: " : "Could not write: ") + path.string();
    }
    const auto trace = _profiler.trace_status();
    ImGui::Text("Trace: %s, %zu / %zu events, %llu dropped",
        trace.capturing ? "recording" : "stopped", trace.events, mirabilis::Profiler::MaxTraceEvents,
        static_cast<unsigned long long>(trace.dropped));
    if (!_profileExportStatus.empty()) ImGui::TextWrapped("%s", _profileExportStatus.c_str());
    if (enabled) {
        const auto& frame = _profiler.latest();
        ImGui::Text("Completed host frame %llu; start-to-start %.3f ms",
            static_cast<unsigned long long>(frame.frame), frame.wallMilliseconds);
        ImGui::PlotLines("Host frame ms", _profiler.frame_history(), _profiler.history_size(),
            _profiler.history_offset(), nullptr, 0.0f, std::numeric_limits<float>::max(), ImVec2(0, 65));
        ImGui::TextWrapped("CPU timings are inclusive. Frame and render include waits; GPU samples below arrive after completion.");
        if (ImGui::BeginTable("CPU scopes", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Operation"); ImGui::TableSetupColumn("ms"); ImGui::TableSetupColumn("Calls");
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < frame.zones.size(); ++i) {
                const auto& sample = frame.zones[i];
                if (!sample.calls) continue;
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::TextUnformatted(mirabilis::zone_name(static_cast<mirabilis::CpuZone>(i)));
                ImGui::TableNextColumn(); ImGui::Text("%.3f", sample.milliseconds);
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(sample.calls));
            }
            ImGui::EndTable();
        }
    }
    ImGui::Separator();
    if (stats.ssao_time_is_gpu) ImGui::Text("Completed GPU AO: %.3f ms", stats.ssao_total_time);
    if (stats.ssgi_time_is_gpu) ImGui::Text("Completed GPU GI: %.3f ms", stats.ssgi_total_time);
    if (_autoExposure.enabled) ImGui::Text("Completed GPU exposure: %.3f ms", _autoExposure.gpuMilliseconds);
    if (_rendererMode == RendererMode::SoftwarePathTrace) ImGui::Text("Completed GPU trace: %.3f ms", _traceRuntime.gpuMs);
    ImGui::Text("Draw-index scratch: %zu bytes, %llu growths",
        _opaqueDrawScratch.capacity() * sizeof(uint32_t), static_cast<unsigned long long>(stats.draw_index_buffer_grows));
    if (_tasks) {
        const auto tasks = _tasks->statistics();
        ImGui::Text("Tasks: %zu workers, %zu fibers (%s)", tasks.workers, tasks.fibers,
            tasks.backend == mirabilis::FiberBackend::Assembly ? "x64 assembly" : "Windows fibers");
        ImGui::Text("Queued %zu, running %zu, parked %zu, completed %zu", tasks.queued, tasks.running, tasks.parked, tasks.completed);
        ImGui::Text("Pool-exhaustion fallbacks %zu, cancelled %zu", tasks.inlineFallbacks, tasks.cancelled);
        ImGui::Text("Task scratch: %.2f MiB committed, %.2f MiB peak; small pools %.2f MiB",
            double(tasks.scratchCommitted) / 1048576, double(tasks.scratchPeak) / 1048576, double(tasks.poolCommitted) / 1048576);
    }
    if (ImGui::Button("Sample GPU allocations")) {
        VmaTotalStatistics memory{};
        vmaCalculateStatistics(_allocator, &memory);
        stats.gpu_allocation_bytes = memory.total.statistics.allocationBytes;
        stats.gpu_block_bytes = memory.total.statistics.blockBytes;
        stats.gpu_allocation_count = memory.total.statistics.allocationCount;
        stats.gpu_memory_sampled = true;
    }
    if (stats.gpu_memory_sampled) ImGui::Text("Last sample: %.2f MiB allocated, %.2f MiB in blocks, %u allocations",
        double(stats.gpu_allocation_bytes) / (1024 * 1024), double(stats.gpu_block_bytes) / (1024 * 1024), stats.gpu_allocation_count);
    ImGui::End();
}

