#include "vk_engine.h"
#include "vk_engine_render_helpers.h"
#include "vk_images.h"
#include "vk_pipelines.h"
#include "imgui.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fstream>

namespace {
struct TracePush {
    glm::vec4 origin, right, up, forward;
    glm::uvec4 control;
    glm::vec4 settings;
    glm::uvec4 sampling;
};
static_assert(sizeof(TracePush)==112);
TracePush camera_push(const Camera& camera, VkExtent2D extent) {
    const auto r=camera.getRotationMatrix();
    return {glm::vec4(camera.position,1),r*glm::vec4(1,0,0,0),r*glm::vec4(0,1,0,0),r*glm::vec4(0,0,-1,0),
        glm::uvec4(extent.width,extent.height,0,0),glm::vec4(std::tan(glm::radians(35.f)),0,0,0)};
}
}
void VulkanEngine::init_path_trace() {
    if(std::getenv("MIRABILIS_TEST_FRAMES")&&std::getenv("MIRABILIS_TEST_DISABLE_TRACE")) {
        _traceStatus="Software tracing disabled by validation override; using Raster.";
        fmt::print("GI fallback: {}\n",_traceStatus); return;
    }
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(_chosenGPU,&props);
    VkFormatProperties format{};
    vkGetPhysicalDeviceFormatProperties(_chosenGPU,VK_FORMAT_R32G32B32A32_SFLOAT,&format);
    uint32_t count=0; vkGetPhysicalDeviceQueueFamilyProperties(_chosenGPU,&count,nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(_chosenGPU,&count,queues.data());
    fmt::print("GI device: {} driver={} API={}.{}.{} RGBA32F storage={} compute={}\n",props.deviceName,props.driverVersion,
        VK_API_VERSION_MAJOR(props.apiVersion),VK_API_VERSION_MINOR(props.apiVersion),VK_API_VERSION_PATCH(props.apiVersion),
        bool(format.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT),bool(queues[_graphicsQueueFamily].queueFlags&VK_QUEUE_COMPUTE_BIT));
    if (!(format.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) || !(queues[_graphicsQueueFamily].queueFlags&VK_QUEUE_COMPUTE_BIT)) {
        _traceStatus="Software tracing unavailable: RGBA32F storage or compute queue unsupported; using Raster."; return;
    }
    ScopedShaderModule shader(_device);
    if (!shader.load("../../shaders/path_trace.comp.spv")) {
        _traceStatus="Software shader missing; build Shaders. Using Raster."; return;
    }
    DescriptorLayoutBuilder builder;
    builder.add_binding(0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    builder.add_binding(1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    builder.add_binding(2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    builder.add_binding(3,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    builder.add_binding(4,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    builder.add_binding(5,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    builder.add_binding(6,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    builder.add_binding(7,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    builder.add_binding(8,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    builder.add_binding(9,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    builder.add_binding(10,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    builder.add_binding(11,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    _traceSetLayout=builder.build(_device,VK_SHADER_STAGE_COMPUTE_BIT);
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(TracePush)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount=1; layout.pSetLayouts=&_traceSetLayout; layout.pushConstantRangeCount=1; layout.pPushConstantRanges=&range;
    VK_CHECK(vkCreatePipelineLayout(_device,&layout,nullptr,&_traceLayout));
    VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.layout=_traceLayout;
    pipeline.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,shader.get(),"main",nullptr};
    const VkResult result=vkCreateComputePipelines(_device,VK_NULL_HANDLE,1,&pipeline,nullptr,&_tracePipeline);
    if (result!=VK_SUCCESS) { _traceStatus="Software pipeline unavailable; using Raster."; return; }
    _traceAccum=create_image(_drawImage.imageExtent,VK_FORMAT_R32G32B32A32_SFLOAT,VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    _traceDirect=create_image(_drawImage.imageExtent,VK_FORMAT_R32G32B32A32_SFLOAT,VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    _traceIndirect=create_image(_drawImage.imageExtent,VK_FORMAT_R32G32B32A32_SFLOAT,VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    DescriptorAllocator::PoolSizeRatio ratios[]={{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,4},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,6},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1}};
    _tracePool.init_pool(_device,1,ratios); _traceSet=_tracePool.allocate(_device,_traceSetLayout);
    DescriptorWriter writer;
    writer.write_image(0,_drawImage.imageView,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_GENERAL,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    writer.write_image(1,_traceAccum.imageView,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_GENERAL,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    writer.write_image(8,_traceDirect.imageView,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_GENERAL,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    writer.write_image(9,_traceIndirect.imageView,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_GENERAL,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    writer.write_image(5,_whiteImage.imageView,_prepass.sampler,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    writer.update_set(_device,_traceSet);
    _traceSupported=true;
    update_trace_scene();
    _traceStatus="Software compute reference (no hardware RT)";
    if(queues[_graphicsQueueFamily].timestampValidBits) {
        VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        query.queryType=VK_QUERY_TYPE_TIMESTAMP; query.queryCount=2*FRAME_OVERLAP;
        VK_CHECK(vkCreateQueryPool(_device,&query,nullptr,&_traceTimestampPool));
    }
}
void VulkanEngine::destroy_path_trace() {
    if(_traceTimestampPool) vkDestroyQueryPool(_device,_traceTimestampPool,nullptr);
    destroy_buffer(_traceLightBuffer); destroy_buffer(_traceEmitterBuffer);
    destroy_buffer(_traceTexelBuffer);
    destroy_buffer(_tracePortalBuffer);
    if(_traceDirect.image) destroy_image(_traceDirect);
    if(_traceIndirect.image) destroy_image(_traceIndirect);
    destroy_buffer(_traceNodeBuffer);
    destroy_buffer(_traceTriangleBuffer);
    destroy_buffer(_traceMaterialBuffer);
    if (_traceAccum.image) destroy_image(_traceAccum);
    if (_tracePool.pool) _tracePool.destroy_pool(_device);
    if (_tracePipeline) vkDestroyPipeline(_device,_tracePipeline,nullptr);
    if (_traceLayout) vkDestroyPipelineLayout(_device,_traceLayout,nullptr);
    if (_traceSetLayout) vkDestroyDescriptorSetLayout(_device,_traceSetLayout,nullptr);
}
void VulkanEngine::draw_path_trace(VkCommandBuffer cmd) {
    update_trace_scene();
    auto pc=camera_push(render_camera(),_drawExtent);
    pc.settings.y=float(_traceTriangles.size()); pc.settings.z=float(_traceSettings.debugView);
    pc.sampling=glm::uvec4(_traceSettings.maxDepth,_traceSettings.baseSeed,_traceSettings.materialModel,_traceSettings.portalLimit);
    uint64_t hash=1469598103934665603ull;
    auto append=[&](const void* data,size_t bytes) {auto p=static_cast<const uint8_t*>(data); for(size_t i=0;i<bytes;++i) {hash^=p[i];hash*=1099511628211ull;}};
    append(&pc.origin,64); append(&_drawExtent,sizeof(_drawExtent)); append(&renderScale,sizeof(renderScale));
    append(&pc.sampling,sizeof(pc.sampling)); append(&_traceSceneRevision,sizeof(_traceSceneRevision));
    if(!_traceWasActive||hash!=_traceInputHash||_traceSamples>=16777216u) {
        _traceSamples=0; _traceInputHash=hash;
        if(std::getenv("MIRABILIS_TEST_FRAMES")) fmt::print("GI accumulation reset: input hash={} revision={} extent={}x{}\n",hash,_traceSceneRevision,_drawExtent.width,_drawExtent.height);
    }
    _traceWasActive=true;
    pc.control.z=_traceSamples++;
    pc.origin.w=std::exp2(_traceSettings.exposure);
    const uint32_t slot=uint32_t(_frameNumber%FRAME_OVERLAP);
    if(_traceTimestampPool) {
        if(_traceTimingWritten[slot]) {
            uint64_t ticks[2]{};
            if(vkGetQueryPoolResults(_device,_traceTimestampPool,slot*2,2,sizeof(ticks),ticks,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS) {
                VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(_chosenGPU,&properties);
                _traceGpuMs=float(ticks[1]-ticks[0])*properties.limits.timestampPeriod/1e6f;
                _traceMaxGpuMs=std::max(_traceMaxGpuMs,_traceGpuMs);
            }
        }
        vkCmdResetQueryPool(cmd,_traceTimestampPool,slot*2,2);
        vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,_traceTimestampPool,slot*2);
    }
    vkutil::transition_image(cmd,_drawImage.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);
    vkutil::transition_image(cmd,_traceAccum.image,_traceImageInitialized?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);
    vkutil::transition_image(cmd,_traceDirect.image,_traceImageInitialized?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);
    vkutil::transition_image(cmd,_traceIndirect.image,_traceImageInitialized?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);
    _traceImageInitialized=true;
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,_tracePipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,_traceLayout,0,1,&_traceSet,0,nullptr);
    vkCmdPushConstants(cmd,_traceLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(pc),&pc);
    vkCmdDispatch(cmd,(_drawExtent.width+7)/8,(_drawExtent.height+7)/8,1);
    if(_traceTimestampPool) {
        vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,_traceTimestampPool,slot*2+1);
        _traceTimingWritten[slot]=true;
    }
    vkutil::transition_image(cmd,_drawImage.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
}
void VulkanEngine::draw_path_trace_ui() {
    int mode=static_cast<int>(_rendererMode);
    if (ImGui::Combo("Renderer",&mode,"Raster\0Path Traced Reference (Software)\0"))
        _rendererMode=(_traceSupported&&mode==1)?RendererMode::SoftwarePathTrace:RendererMode::Raster;
    ImGui::TextWrapped("%s",_traceStatus.c_str());
    if(_rendererMode==RendererMode::SoftwarePathTrace) {
        ImGui::Combo("Trace view",&_traceSettings.debugView,"Geometric normal\0Shading normal\0Distance\0Albedo\0Material ID\0Node visits\0Triangle tests\0One noisy sample\0Accumulated radiance\0Nonfinite / traversal errors\0Direct radiance\0Indirect radiance\0Portal traversals\0Portal limit\0");
        ImGui::SliderInt("Surface scattering events",&_traceSettings.maxDepth,1,4);
        ImGui::InputInt("Base seed",&_traceSettings.baseSeed);
        ImGui::Combo("Material model",&_traceSettings.materialModel,"Lambertian reference\0GGX + dielectric\0");
        ImGui::SliderFloat("Exposure (EV)",&_traceSettings.exposure,-6,6);
        bool edited=ImGui::ColorEdit3("Reference sun radiance",&_traceSettings.lighting.sunRadiance.x,ImGuiColorEditFlags_Float|ImGuiColorEditFlags_HDR);
        edited|=ImGui::SliderFloat("Environment intensity",&_traceSettings.lighting.environment.x,0,4);
        bool black=_traceSettings.lighting.environment.y>0.5f;
        if(ImGui::Checkbox("Black environment",&black)) {edited=true;_traceSettings.lighting.environment.y=black?1.f:0.f;}
        if(edited) _sceneDocument.dirty=true;
        if(ImGui::Button("Reset accumulation")) _traceWasActive=false;
        ImGui::Text("Samples %u | GPU %.2f ms (max %.2f)",_traceSamples,_traceGpuMs,_traceMaxGpuMs);
        ImGui::Text("Triangles %zu | BVH nodes %zu | revision %llu",_traceTriangles.size(),_traceNodes.size(),_traceSceneRevision);
        ImGui::SliderInt("Portal traversals per path",&_traceSettings.portalLimit,0,4);
        ImGui::Text("Portal apertures %zu (same scene)",_tracePortals.size());
        ImGui::TextWrapped("Portal limit terminates the path in black. Unlinked apertures are marked as invalid.");
    }
}
void VulkanEngine::validate_path_trace() {
    if (!_traceSupported) { fmt::print("GI TEST FAIL: unavailable\n"); std::abort(); }
    _drawExtent={_windowExtent.width,_windowExtent.height};
    update_scene(0);
    update_trace_scene();
    const VkExtent2D extent{17,13};
    Camera camera; camera.yaw=0.37f; camera.pitch=-0.21f;
    if(const char* depth=std::getenv("MIRABILIS_TEST_DEPTH")) _traceSettings.maxDepth=std::clamp(std::atoi(depth),1,4);
    if(const char* limit=std::getenv("MIRABILIS_TEST_PORTAL_LIMIT")) _traceSettings.portalLimit=std::clamp(std::atoi(limit),0,4);
    for (uint32_t diagnostic=1;diagnostic<=12;++diagnostic) {
    auto pc=camera_push(camera,extent); pc.control.w=diagnostic; pc.settings.y=float(_traceTriangles.size());
    pc.sampling=glm::uvec4(2,1337,1,_traceSettings.portalLimit);
    if(diagnostic==4) {
        _drawExtent=extent; update_scene(0);
        pc=camera_push(render_camera(),extent); pc.control.w=4; pc.settings.y=float(_traceTriangles.size());
        DescriptorWriter writer;
        writer.write_image(5,_prepass.depthImage.imageView,_prepass.sampler,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        writer.update_set(_device,_traceSet);
    }
    auto buffer=create_buffer(extent.width*extent.height*sizeof(glm::vec4),VK_BUFFER_USAGE_TRANSFER_DST_BIT,VMA_MEMORY_USAGE_GPU_TO_CPU);
    immediate_submit([&](VkCommandBuffer cmd) {
        if(diagnostic==4) draw_depth_normal_prepass(cmd);
        vkutil::transition_image(cmd,_drawImage.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);
        vkutil::transition_image(cmd,_traceAccum.image,_traceImageInitialized?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);
    vkutil::transition_image(cmd,_traceDirect.image,_traceImageInitialized?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);
    vkutil::transition_image(cmd,_traceIndirect.image,_traceImageInitialized?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,_tracePipeline);
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,_traceLayout,0,1,&_traceSet,0,nullptr);
        for(uint32_t sample=0;sample<(diagnostic==5?3u:1u);++sample) {
            pc.control.z=sample;
            vkCmdPushConstants(cmd,_traceLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(pc),&pc);
            vkCmdDispatch(cmd,3,2,1);
            vkutil::transition_image(cmd,_traceAccum.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
        }
        vkutil::transition_image(cmd,_traceAccum.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={17,13,1};
        vkCmdCopyImageToBuffer(cmd,_traceAccum.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer.buffer,1,&copy);
        vkutil::transition_image(cmd,_traceAccum.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
    });
    _traceImageInitialized=true;
    void* mapped=nullptr;
    VK_CHECK(vmaMapMemory(_allocator,buffer.allocation,&mapped));
    vmaInvalidateAllocation(_allocator,buffer.allocation,0,VK_WHOLE_SIZE);
    const auto* rays=static_cast<const glm::vec4*>(mapped);
    float error=0;
    if(diagnostic==5) for(uint32_t i=0;i<17*13;++i) error=std::max(error,glm::length(glm::vec3(rays[i])-glm::vec3(4,8,16)));
    if(diagnostic==6||diagnostic==8) {
        glm::vec3 expected=diagnostic==6?glm::vec3(0.04f,1.f,0.2140411405f):glm::vec3(1.f/3.f,1,1);
        for(uint32_t i=0;i<17*13;++i) error=std::max(error,glm::length(glm::vec3(rays[i])-expected));
    }
    if(diagnostic==7) {
        float mean=0;
        for(uint32_t i=0;i<17*13;++i) {
            const auto v=rays[i];
            if(!std::isfinite(v.x)||v.x<0||v.x>1.00001f||v.w<0) std::abort();
            mean+=v.x/(17.f*13.f);
        }
        fmt::print("GI GGX white furnace normal-incidence mean={} (single scattering, expected 0.7..1)\n",mean);
        if(mean<0.7f||mean>1.00001f) std::abort();
    }
    if(diagnostic==9) {
        for(uint32_t i=0;i<17*13;++i) {
            uint32_t rgba=_traceTexels.empty()?0:_traceTexels[i%_traceTexels.size()]; glm::vec3 expected;
            for(int c=0;c<3;++c) {float value=float((rgba>>(c*8))&255)/255.f;expected[c]=value<=0.04045f?value/12.92f:std::pow((value+0.055f)/1.055f,2.4f);}
            error=std::max(error,glm::length(glm::vec3(rays[i])-expected));
        }
    }
    if(diagnostic==10||diagnostic==11) {
        for(uint32_t y=0;y<13;++y) for(uint32_t x=0;x<17;++x) {
            glm::vec3 expected(0);
            if(!_tracePortals.empty()) {
                const auto& portal=_tracePortals[(y*17+x)%_tracePortals.size()];
                glm::vec2 local=(glm::vec2(x,y)+0.5f)/glm::vec2(17,13)*1.6f-0.8f;
                auto point=glm::vec3(portal.center)+glm::vec3(portal.rightWidth)*local.x*portal.rightWidth.w+glm::vec3(portal.upHeight)*local.y*portal.upHeight.w;
                auto direction=glm::normalize(glm::vec3(portal.transfer*glm::vec4(-glm::vec3(portal.normal),0)));
                expected=diagnostic==10?glm::vec3(portal.transfer*glm::vec4(point,1))+direction*0.0004f:direction;
            }
            error=std::max(error,glm::length(expected-glm::vec3(rays[y*17+x]))/std::max(1.f,glm::length(expected)));
        }
    }
    if(diagnostic==12) {
        uint32_t count=0;bool limited=false;
        if(!_tracePortals.empty()) {
            auto origin=glm::vec3(_tracePortals[0].center+_tracePortals[0].normal),direction=-glm::vec3(_tracePortals[0].normal);
            for(int step=0;step<=4;++step) {
                auto hit=trace_cpu_intersect(_traceTriangles,_traceNodes,origin,direction,true);
                float distance=hit.t;int selected=-1;
                for(size_t i=0;i<_tracePortals.size();++i) {
                    const auto& portal=_tracePortals[i];float denominator=glm::dot(direction,glm::vec3(portal.normal));
                    if(std::abs(denominator)<1e-8f) continue;
                    float t=glm::dot(glm::vec3(portal.center)-origin,glm::vec3(portal.normal))/denominator;
                    auto local=origin+direction*t-glm::vec3(portal.center);
                    if(t>0.0001f&&t<=distance+0.0001f&&std::abs(glm::dot(local,glm::vec3(portal.rightWidth)))<=portal.rightWidth.w&&std::abs(glm::dot(local,glm::vec3(portal.upHeight)))<=portal.upHeight.w) {selected=int(i);distance=t;}
                }
                if(selected<0) break;
                if(count>=uint32_t(_traceSettings.portalLimit)) {limited=true;break;}
                const auto& portal=_tracePortals[selected];++count;
                origin=glm::vec3(portal.transfer*glm::vec4(origin+direction*distance,1));
                direction=glm::normalize(glm::vec3(portal.transfer*glm::vec4(direction,0)));origin+=direction*0.0004f;
            }
        }
        for(uint32_t i=0;i<17*13;++i) error=std::max(error,glm::length(glm::vec3(rays[i])-glm::vec3(count,limited?1:0,0)));
        fmt::print("GI portal CPU/GPU chain: count={} limited={}\n",count,limited);
    }
    for (uint32_t y: {0u,6u,12u}) for(uint32_t x:{0u,8u,16u}) {
        glm::vec2 uv=(glm::vec2(x,y)+0.5f)/glm::vec2(17,13)*2.f-1.f;
        auto expected=glm::normalize(glm::vec3(pc.forward)+glm::vec3(pc.right)*uv.x*(17.f/13.f)*pc.settings.x-glm::vec3(pc.up)*uv.y*pc.settings.x);
        if(diagnostic==1) error=std::max(error,glm::length(expected-glm::vec3(rays[y*17+x])));
    }
    if (diagnostic==2) {
        for (uint32_t i=0;i<17*13;++i) {
            const auto t=_traceTriangles.empty()?TraceTriangle{}:_traceTriangles[i%_traceTriangles.size()];
            const float red=_traceMaterials.empty()?0:_traceMaterials[t.meta.x].baseColor.x;
            const auto expected=glm::vec4(glm::vec3(t.p0),red);
            if (!std::isfinite(glm::length(expected-rays[i]))) std::abort();
            error=std::max(error,glm::length(expected-rays[i]));
        }
    }
    if(diagnostic==3) {
        for(uint32_t y=0;y<13;++y) for(uint32_t x=0;x<17;++x) {
            glm::vec2 uv=(glm::vec2(x,y)+0.5f)/glm::vec2(17,13)*2.f-1.f;
            auto d=glm::normalize(glm::vec3(pc.forward)+glm::vec3(pc.right)*uv.x*(17.f/13.f)*pc.settings.x-glm::vec3(pc.up)*uv.y*pc.settings.x);
            auto h=trace_cpu_intersect(_traceTriangles,_traceNodes,glm::vec3(pc.origin),d,true);
            const auto gpu=rays[y*17+x];
            if((h.triangle<0)!=(gpu.x<0)||gpu.w!=0) std::abort();
            if(h.triangle>=0) error=std::max(error,std::abs(h.t-gpu.x)/std::max(1.f,h.t));
        }
    }
    if(diagnostic==4) {
        for(uint32_t y=0;y<13;++y) for(uint32_t x=0;x<17;++x) {
            auto gpu=rays[y*17+x];
            if((gpu.x==0)!=(gpu.y<0)||gpu.w!=0) {
                // Raster vertices are snapped to a fixed subpixel grid. A
                // coverage disagreement is allowed only next to a projected
                // triangle edge, within the device's advertised precision.
                float edgeDistance=1e30f;
                glm::vec2 pixel(x+0.5f,y+0.5f);
                for(const auto& triangle:_traceTriangles) {
                    glm::vec4 clip[3]={sceneData.viewproj*triangle.p0,sceneData.viewproj*triangle.p1,sceneData.viewproj*triangle.p2};

                    for(int k=0;k<3;++k) {
                        auto ca=clip[k],cb=clip[(k+1)%3];
                        if(ca.w<0.1f&&cb.w<0.1f) continue;
                        if(ca.w<0.1f) ca=glm::mix(ca,cb,(0.1f-ca.w)/(cb.w-ca.w));
                        if(cb.w<0.1f) cb=glm::mix(cb,ca,(0.1f-cb.w)/(ca.w-cb.w));
                        auto a=(glm::vec2(ca)/ca.w*0.5f+0.5f)*glm::vec2(17,13);
                        auto b=(glm::vec2(cb)/cb.w*0.5f+0.5f)*glm::vec2(17,13),ab=b-a;
                        if(glm::dot(ab,ab)<1e-12f) continue;
                        float t=glm::clamp(glm::dot(pixel-a,ab)/glm::dot(ab,ab),0.f,1.f);
                        edgeDistance=std::min(edgeDistance,glm::length(pixel-(a+ab*t)));
                    }
                }
                VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(_chosenGPU,&properties);
                float tolerance=std::ldexp(1.f,-int(properties.limits.subPixelPrecisionBits))+1e-5f;
                fmt::print("GI raster coverage boundary at {},{}: edge distance={} pixels, hardware precision={} pixels\n",x,y,edgeDistance,tolerance);
                if(edgeDistance>tolerance||gpu.w!=0) std::abort();
                continue;
            }            if(gpu.x>0) {
                glm::vec2 uv=(glm::vec2(x,y)+0.5f)/glm::vec2(17,13)*2.f-1.f;
                auto world=sceneData.inverseViewProjection*glm::vec4(uv,gpu.x,1);
                float distance=glm::length(glm::vec3(world)/world.w-glm::vec3(pc.origin));
                error=std::max(error,std::abs(distance-gpu.y)/std::max(1.f,distance));
                auto d=glm::normalize(glm::vec3(world)/world.w-glm::vec3(pc.origin));
                auto hit=trace_cpu_intersect(_traceTriangles,_traceNodes,glm::vec3(pc.origin),d,true);
                if(hit.triangle<0) std::abort();
                const auto& t=_traceTriangles[hit.triangle];
                glm::vec4 clip[3]={sceneData.viewproj*t.p0,sceneData.viewproj*t.p1,sceneData.viewproj*t.p2};
                glm::vec3 projected[3];
                for(int k=0;k<3;++k) projected[k]=glm::vec3(clip[k])/clip[k].w;
                auto a=projected[1]-projected[0],b=projected[2]-projected[0];
                float determinant=a.x*b.y-a.y*b.x;
                float dx=(a.z*b.y-a.y*b.z)/determinant*2.f/17.f;
                float dy=(a.x*b.z-a.z*b.x)/determinant*2.f/13.f;
                auto pointClip=sceneData.viewproj*glm::vec4(glm::vec3(pc.origin)+d*hit.t,1);
                VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(_chosenGPU,&properties);
                float depthTolerance=2.f*std::ldexp(1.f,-int(properties.limits.subPixelPrecisionBits))*(std::abs(dx)+std::abs(dy))+1e-7f;
                if(std::abs(gpu.x-pointClip.z/pointClip.w)>depthTolerance) {
                    fmt::print("GI raster depth outside subpixel plane bound at {},{}\n",x,y); std::abort();
                }
            }
        }
        DescriptorWriter writer;
        writer.write_image(5,_whiteImage.imageView,_prepass.sampler,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        writer.update_set(_device,_traceSet);
    }
    vmaUnmapMemory(_allocator,buffer.allocation);
    destroy_buffer(buffer);
    fmt::print("GI GPU readback diagnostic={}: max error={} ({}); rays=9 or scene records=221\n",diagnostic,error,
        diagnostic==4?"per-triangle raster precision bound":"tolerance 2e-6");
    // Raster depth interpolation uses snapped vertices; its per-triangle
    // depth-gradient bound is checked above instead of the ray tolerance.
    if (!std::isfinite(error)||(diagnostic!=4&&error>2e-6f)) std::abort();
    }
}

void VulkanEngine::update_trace_scene() {
    std::vector<TracePortal> portals;
    auto addPortal=[&](const Portal& source,const Portal* destination) {
        if(!source.placed) return;
        TracePortal record;
        record.center=glm::vec4(source.position,destination?1.f:0.f);
        record.rightWidth=glm::vec4(glm::normalize(glm::cross(source.up,source.normal)),source.halfWidth);
        record.upHeight=glm::vec4(source.up,source.halfHeight);
        record.normal=glm::vec4(source.normal,0);
        if(destination) {record.transfer=get_portal_transfer_transform(source,*destination);record.inverseTransfer=glm::inverse(record.transfer);}
        portals.push_back(record);
    };
    addPortal(_bluePortal,_orangePortal.placed?&_orangePortal:nullptr);
    addPortal(_orangePortal,_bluePortal.placed?&_bluePortal:nullptr);
    for(const auto& pair:_authoredPortals.pairs) {addPortal(pair.first,&pair.second);addPortal(pair.second,&pair.first);}
    uint64_t drawHash=1469598103934665603ull;
    auto append=[&](const void* data,size_t bytes) {auto p=static_cast<const uint8_t*>(data);for(size_t i=0;i<bytes;++i) {drawHash^=p[i];drawHash*=1099511628211ull;}};
    append(&_shadow.sunlightDirection,sizeof(_shadow.sunlightDirection));
    append(&_traceSettings.lighting.sunRadiance,2*sizeof(glm::vec4));
    append(portals.data(),portals.size()*sizeof(TracePortal));
    for(const auto& draw:worldDrawContext.OpaqueSurfaces) {
        append(&draw.transform,sizeof(draw.transform)); append(&draw.vertexBufferAddress,sizeof(draw.vertexBufferAddress));
        append(&draw.firstIndex,sizeof(draw.firstIndex)); append(&draw.indexCount,sizeof(draw.indexCount));
        if(draw.material) {
            append(&draw.material->traceBaseColor,3*sizeof(glm::vec4));
            append(&draw.material->traceUVScale,sizeof(glm::vec2));
            const auto* texture=draw.material->traceTexture.get(); append(&texture,sizeof(texture));
        }
    }
    if(_traceTriangleBuffer.buffer&&drawHash==_traceDrawHash) return;
    _traceDrawHash=drawHash;
    std::vector<TraceTriangle> triangles;
    std::vector<TraceMaterial> materials;
    std::vector<uint32_t> texels;
    std::unordered_map<const TraceTextureSource*,glm::uvec4> textures;
    std::unordered_map<const MaterialInstance*,uint32_t> materialIds;
    for (const auto& draw : worldDrawContext.OpaqueSurfaces) {
        auto found=_traceMeshSources.find(draw.vertexBufferAddress);
        auto source=found==_traceMeshSources.end()?nullptr:found->second.lock();
        if (!source || !draw.material || draw.material->passType!=MaterialPass::MainColor) continue;
        auto [entry,inserted]=materialIds.emplace(draw.material,uint32_t(materials.size()));
        if (inserted) {
            materials.push_back({draw.material->traceBaseColor,draw.material->traceEmission,draw.material->traceParameters});
            materials.back().uvScale=glm::vec4(draw.material->traceUVScale,0,0);
            if(auto texture=draw.material->traceTexture) {
                auto [record,isNew]=textures.emplace(texture.get(),glm::uvec4(uint32_t(texels.size()),texture->width,texture->height,0));
                if(isNew) texels.insert(texels.end(),texture->rgba.begin(),texture->rgba.end());
                materials.back().texture=record->second;
            }
        }
        const glm::mat3 linear(draw.transform);
        if (std::abs(glm::determinant(linear))<1e-12f) continue;
        const glm::mat3 normalMatrix=glm::transpose(glm::inverse(linear));
        const size_t end=std::min(source->indices.size(),size_t(draw.firstIndex)+draw.indexCount);
        for (size_t i=draw.firstIndex;i+2<end;i+=3) {
            uint32_t a=source->indices[i],b=source->indices[i+1],c=source->indices[i+2];
            if (std::max({a,b,c})>=source->vertices.size()) continue;
            const auto& v0=source->vertices[a]; const auto& v1=source->vertices[b]; const auto& v2=source->vertices[c];
            TraceTriangle t{};
            t.p0=draw.transform*glm::vec4(v0.position,1); t.p1=draw.transform*glm::vec4(v1.position,1); t.p2=draw.transform*glm::vec4(v2.position,1);
            auto geometric=glm::cross(glm::vec3(t.p1-t.p0),glm::vec3(t.p2-t.p0));
            if (!std::isfinite(glm::dot(geometric,geometric))||glm::dot(geometric,geometric)<1e-16f) continue;
            const auto normal=[&](glm::vec3 n) { n=normalMatrix*n; return glm::dot(n,n)>1e-16f?glm::normalize(n):glm::normalize(geometric); };
            t.n0=glm::vec4(normal(v0.normal),v0.uv_x); t.n1=glm::vec4(normal(v1.normal),v0.uv_y); t.n2=glm::vec4(normal(v2.normal),0);
            t.uv12=glm::vec4(v1.uv_x,v1.uv_y,v2.uv_x,v2.uv_y); t.meta.x=entry->second;
            t.c0=v0.color; t.c1=v1.color; t.c2=v2.color;
            triangles.push_back(t);
        }
    }
    uint64_t hash=1469598103934665603ull;
    const auto hashBytes=[&](const void* data,size_t size) { auto p=static_cast<const unsigned char*>(data); for(size_t i=0;i<size;++i) { hash^=p[i]; hash*=1099511628211ull; } };
    _traceSettings.lighting.sunDirection=glm::vec4(glm::normalize(glm::dot(_shadow.sunlightDirection,_shadow.sunlightDirection)>1e-10f?_shadow.sunlightDirection:glm::vec3(0,1,0)),0);
    hashBytes(&_traceSettings.lighting.sunDirection,3*sizeof(glm::vec4));
    hashBytes(triangles.data(),triangles.size()*sizeof(TraceTriangle)); hashBytes(materials.data(),materials.size()*sizeof(TraceMaterial));
    hashBytes(texels.data(),texels.size()*sizeof(uint32_t));
    hashBytes(portals.data(),portals.size()*sizeof(TracePortal));
    if (_traceTriangleBuffer.buffer && hash==_traceSceneHash) return;
    VK_CHECK(vkDeviceWaitIdle(_device)); // Rare scene edits retire all readers before replacing descriptors/buffers.
    _traceSceneHash=hash; ++_traceSceneRevision;
    _traceTriangles=std::move(triangles); _traceMaterials=std::move(materials);
    _traceTexels=std::move(texels);
    _tracePortals=std::move(portals);
    _traceSettings.lighting.counts.y=uint32_t(_tracePortals.size());
    _traceNodes=build_trace_bvh(_traceTriangles);
    _traceEmitters.clear();
    for(uint32_t i=0;i<_traceTriangles.size();++i) {
        const auto& m=_traceMaterials[_traceTriangles[i].meta.x];
        if(glm::length(glm::vec3(m.emission))>0) _traceEmitters.push_back(i);
    }
    _traceSettings.lighting.counts.x=uint32_t(_traceEmitters.size());
    if(std::getenv("MIRABILIS_TEST_FRAMES")) validate_trace_bvh(_traceTriangles,_traceNodes);
    auto upload=[&](AllocatedBuffer& buffer,const void* data,size_t bytes,size_t minimum) {
        destroy_buffer(buffer); buffer=create_buffer(std::max(bytes,minimum),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VMA_MEMORY_USAGE_CPU_TO_GPU);
        std::memset(buffer.info.pMappedData,0,std::max(bytes,minimum));
        if(bytes) std::memcpy(buffer.info.pMappedData,data,bytes);
        vmaFlushAllocation(_allocator,buffer.allocation,0,VK_WHOLE_SIZE);
    };
    upload(_traceTriangleBuffer,_traceTriangles.data(),_traceTriangles.size()*sizeof(TraceTriangle),sizeof(TraceTriangle));
    upload(_traceMaterialBuffer,_traceMaterials.data(),_traceMaterials.size()*sizeof(TraceMaterial),sizeof(TraceMaterial));
    upload(_traceNodeBuffer,_traceNodes.data(),_traceNodes.size()*sizeof(TraceBVHNode),sizeof(TraceBVHNode));
    upload(_traceEmitterBuffer,_traceEmitters.data(),_traceEmitters.size()*sizeof(uint32_t),sizeof(uint32_t));
    upload(_traceTexelBuffer,_traceTexels.data(),_traceTexels.size()*sizeof(uint32_t),sizeof(uint32_t));
    upload(_tracePortalBuffer,_tracePortals.data(),_tracePortals.size()*sizeof(TracePortal),sizeof(TracePortal));
    destroy_buffer(_traceLightBuffer);
    _traceLightBuffer=create_buffer(sizeof(TraceLighting),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,VMA_MEMORY_USAGE_CPU_TO_GPU);
    std::memcpy(_traceLightBuffer.info.pMappedData,&_traceSettings.lighting,sizeof(_traceSettings.lighting));
    vmaFlushAllocation(_allocator,_traceLightBuffer.allocation,0,VK_WHOLE_SIZE);
    DescriptorWriter writer;
    writer.write_buffer(6,_traceLightBuffer.buffer,sizeof(TraceLighting),0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    writer.write_buffer(7,_traceEmitterBuffer.buffer,VK_WHOLE_SIZE,0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.write_buffer(10,_traceTexelBuffer.buffer,VK_WHOLE_SIZE,0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.write_buffer(11,_tracePortalBuffer.buffer,VK_WHOLE_SIZE,0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.write_buffer(4,_traceNodeBuffer.buffer,VK_WHOLE_SIZE,0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.write_buffer(2,_traceTriangleBuffer.buffer,VK_WHOLE_SIZE,0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.write_buffer(3,_traceMaterialBuffer.buffer,VK_WHOLE_SIZE,0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.update_set(_device,_traceSet);
    fmt::print("GI scene revision={} triangles={} materials={} hash={}\n",_traceSceneRevision,_traceTriangles.size(),_traceMaterials.size(),_traceSceneHash);
}

std::vector<glm::vec4> VulkanEngine::read_trace_image(const AllocatedImage& image) {
    VK_CHECK(vkDeviceWaitIdle(_device));
    const size_t pixels=size_t(_drawExtent.width)*_drawExtent.height;
    auto buffer=create_buffer(pixels*sizeof(glm::vec4),VK_BUFFER_USAGE_TRANSFER_DST_BIT,VMA_MEMORY_USAGE_GPU_TO_CPU);
    immediate_submit([&](VkCommandBuffer cmd) {
        vkutil::transition_image(cmd,image.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={_drawExtent.width,_drawExtent.height,1};
        vkCmdCopyImageToBuffer(cmd,image.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer.buffer,1,&copy);
        vkutil::transition_image(cmd,image.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
    });
    void* mapped=nullptr; VK_CHECK(vmaMapMemory(_allocator,buffer.allocation,&mapped));
    vmaInvalidateAllocation(_allocator,buffer.allocation,0,VK_WHOLE_SIZE);
    std::vector<glm::vec4> values(pixels);
    std::memcpy(values.data(),mapped,pixels*sizeof(glm::vec4));
    vmaUnmapMemory(_allocator,buffer.allocation); destroy_buffer(buffer);
    return values;
}
void VulkanEngine::capture_path_trace(const char* filename) {
    if(!_traceSupported||_rendererMode!=RendererMode::SoftwarePathTrace) return;
    const size_t pixels=size_t(_drawExtent.width)*_drawExtent.height;
    const auto values=read_trace_image(_traceAccum);
    const auto direct=read_trace_image(_traceDirect),indirect=read_trace_image(_traceIndirect);
    float sumError=0; double directSum=0,indirectSum=0;
    for(size_t i=0;i<pixels;++i) {
        for(int c=0;c<3;++c) if(!std::isfinite(direct[i][c])||!std::isfinite(indirect[i][c])) std::abort();
        if(values[i].a!=0) {fmt::print("GI TEST FAIL: persistent transport fault at pixel {}\n",i);std::abort();}
        sumError=std::max(sumError,glm::length(glm::vec3(values[i]-(direct[i]+indirect[i]))));
        directSum+=(direct[i].x+direct[i].y+direct[i].z)/3;
        indirectSum+=(indirect[i].x+indirect[i].y+indirect[i].z)/3;
    }
    for(int component=0;component<2;++component) {
        const auto& data=component==0?direct:indirect;
        std::ofstream file(std::string(filename)+(component==0?".direct.pfm":".indirect.pfm"),std::ios::binary);
        file<<"PF\n"<<_drawExtent.width<<" "<<_drawExtent.height<<"\n-1.0\n";
        for(int y=int(_drawExtent.height)-1;y>=0;--y) for(uint32_t x=0;x<_drawExtent.width;++x)
            file.write(reinterpret_cast<const char*>(&data[size_t(y)*_drawExtent.width+x]),3*sizeof(float));
        if(!file) std::abort();
    }
    fmt::print("GI components: direct mean={} indirect mean={} sum residual={}\n",directSum/pixels,indirectSum/pixels,sumError);
    if(sumError>1e-5f) std::abort();    std::ofstream linear(std::string(filename)+".pfm",std::ios::binary);
    linear<<"PF\n"<<_drawExtent.width<<" "<<_drawExtent.height<<"\n-1.0\n";
    for(int y=int(_drawExtent.height)-1;y>=0;--y) for(uint32_t x=0;x<_drawExtent.width;++x)
        linear.write(reinterpret_cast<const char*>(&values[size_t(y)*_drawExtent.width+x]),3*sizeof(float));
    const uint32_t stride=(_drawExtent.width*3+3)&~3u,size=54+stride*_drawExtent.height;
    std::ofstream bitmap(std::string(filename)+".bmp",std::ios::binary);
    auto word=[&](uint32_t v,int bytes){for(int i=0;i<bytes;++i) bitmap.put(char((v>>(i*8))&255));};
    bitmap.write("BM",2); word(size,4); word(0,4); word(54,4); word(40,4); word(_drawExtent.width,4); word(_drawExtent.height,4);
    word(1,2); word(24,2); word(0,4); word(stride*_drawExtent.height,4); word(2835,4); word(2835,4); word(0,4); word(0,4);
    double sum=0; uint32_t invalid=0;
    for(int y=int(_drawExtent.height)-1;y>=0;--y) {
        for(uint32_t x=0;x<_drawExtent.width;++x) {
            glm::vec3 value(values[size_t(y)*_drawExtent.width+x]);
            if(!std::isfinite(value.x)||!std::isfinite(value.y)||!std::isfinite(value.z)) {++invalid;value=glm::vec3(0);}
            sum+=(value.x+value.y+value.z)/3;
            value=glm::max(value,glm::vec3(0))*std::exp2(_traceSettings.exposure);
            value=value/(1.f+value);
            for(int c=0;c<3;++c) value[c]=value[c]<=0.0031308f?12.92f*value[c]:1.055f*std::pow(value[c],1.f/2.4f)-0.055f;
            for(int c=2;c>=0;--c) bitmap.put(char(glm::clamp(value[c]*255.f+0.5f,0.f,255.f)));
        }
        for(uint32_t pad=_drawExtent.width*3;pad<stride;++pad) bitmap.put(0);
    }

    fmt::print("GI capture {}: {}x{}, depth={}, seed={}, linear mean={}, nonfinite={}\n",filename,_drawExtent.width,_drawExtent.height,_traceSettings.maxDepth,_traceSettings.baseSeed,sum/pixels,invalid);
    VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(_chosenGPU,&properties);
    std::ofstream metadata(std::string(filename)+".txt");
    metadata<<"GPU: "<<properties.deviceName<<"\nDriver integer: "<<properties.driverVersion<<"\nVulkan API integer: "<<properties.apiVersion
        <<"\nBuild: "
#ifdef NDEBUG
        <<"Release"
#else
        <<"Debug"
#endif
        <<"\nScene: "<<_sceneDocument.activeFilename<<"\nTrace revision: "<<_traceSceneRevision<<"\nScene hash: "<<_traceSceneHash
        <<"\nDimensions: "<<_drawExtent.width<<" x "<<_drawExtent.height<<"\nRender scale: "<<renderScale<<"\nSamples: "<<_traceSamples
        <<"\nSeed: "<<_traceSettings.baseSeed<<"\nDepth: "<<_traceSettings.maxDepth<<"\nExposure EV: "<<_traceSettings.exposure<<"\nMax GPU dispatch ms: "<<_traceMaxGpuMs
        <<"\nMaterial model: "<<_traceSettings.materialModel<<"\nPortal limit: "<<_traceSettings.portalLimit<<"\nPortal apertures: "<<_tracePortals.size()
        <<"\nSource revision/state: "<<(std::getenv("MIRABILIS_SOURCE_STATE")?std::getenv("MIRABILIS_SOURCE_STATE"):"unrecorded; use scripts/validate_software_trace.ps1 for source manifest")
        <<"\nCamera: "<<render_camera().position.x<<","<<render_camera().position.y<<","<<render_camera().position.z
        <<" pitch="<<render_camera().pitch<<" yaw="<<render_camera().yaw<<"\n";
    fmt::print("GI capture samples={} max GPU dispatch={} ms\n",_traceSamples,_traceMaxGpuMs);
    metadata<<"Sun radiance: "<<_traceSettings.lighting.sunRadiance.x<<","<<_traceSettings.lighting.sunRadiance.y<<","<<_traceSettings.lighting.sunRadiance.z
        <<"\nEnvironment intensity: "<<_traceSettings.lighting.environment.x<<" black="<<(_traceSettings.lighting.environment.y>0.5f)
        <<"\nEmitter triangles: "<<_traceEmitters.size()<<"\nDirect mean: "<<directSum/pixels<<"\nIndirect mean: "<<indirectSum/pixels<<"\n";
    if(std::getenv("MIRABILIS_EXPECT_BLACK")&&sum!=0) std::abort();
    if(std::getenv("MIRABILIS_EXPECT_LIT")&&(directSum<=0||indirectSum<=0)) std::abort();
    if(!linear||!bitmap||!metadata||invalid) std::abort();
}

std::vector<glm::vec4> VulkanEngine::read_ssgi_image(
    const AllocatedImage& image, VkExtent2D extent)
{
    VK_CHECK(vkDeviceWaitIdle(_device));
    const size_t pixels = size_t(extent.width) * extent.height;
    auto buffer = create_buffer(
        pixels * 4 * sizeof(uint16_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_GPU_TO_CPU);
    immediate_submit([&](VkCommandBuffer cmd) {
        vkutil::transition_image(cmd, image.image, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(cmd, image.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.buffer, 1, &copy);
        vkutil::transition_image(cmd, image.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    });
    void* mapped = nullptr;
    VK_CHECK(vmaMapMemory(_allocator, buffer.allocation, &mapped));
    vmaInvalidateAllocation(_allocator, buffer.allocation, 0, VK_WHOLE_SIZE);
    const auto* packed = static_cast<const uint16_t*>(mapped);
    std::vector<glm::vec4> values(pixels);
    for (size_t pixel = 0; pixel < pixels; ++pixel) {
        values[pixel] = glm::vec4(
            glm::unpackHalf1x16(packed[pixel * 4 + 0]),
            glm::unpackHalf1x16(packed[pixel * 4 + 1]),
            glm::unpackHalf1x16(packed[pixel * 4 + 2]),
            glm::unpackHalf1x16(packed[pixel * 4 + 3]));
    }
    vmaUnmapMemory(_allocator, buffer.allocation);
    destroy_buffer(buffer);
    return values;
}

void VulkanEngine::capture_ssgi(const char* filename)
{
    if (_rendererMode == RendererMode::SoftwarePathTrace || !_ssgi.enabled) {
        fmt::print("SSGI capture skipped: raster SSGI is not active\n");
        return;
    }
    const VkExtent2D extent = active_ssgi_extent();
    const size_t pixels = size_t(extent.width) * extent.height;
    const auto values = read_ssgi_image(_ssgi.filteredImage, extent);
    double sum = 0.0;
    uint32_t invalid = 0;
    for (const glm::vec4& value : values) {
        if (!std::isfinite(value.x) || !std::isfinite(value.y) ||
            !std::isfinite(value.z)) {
            ++invalid;
        } else {
            sum += (value.x + value.y + value.z) / 3.0;
        }
    }

    const auto writePfm = [&](const std::string& path,
                              const std::vector<glm::vec4>& data) {
        std::ofstream file(path, std::ios::binary);
        file << "PF\n" << extent.width << " " << extent.height << "\n-1.0\n";
        for (int y = int(extent.height) - 1; y >= 0; --y) {
            for (uint32_t x = 0; x < extent.width; ++x) {
                const glm::vec4& value = data[size_t(y) * extent.width + x];
                file.write(reinterpret_cast<const char*>(&value),
                    3 * sizeof(float));
            }
        }
        if (!file) std::abort();
    };
    writePfm(std::string(filename) + ".indirect.pfm", values);

    std::ofstream metadata(std::string(filename) + ".txt");
    metadata << "Scene: " << _sceneDocument.activeFilename
        << "\nDimensions: " << extent.width << " x " << extent.height
        << "\nFull draw dimensions: " << _drawExtent.width << " x "
        << _drawExtent.height << "\nPreset: " << _ssgi.qualityPreset
        << "\nRays per pixel: " << _ssgi.raysPerPixel
        << "\nRay steps: " << _ssgi.stepCount
        << "\nRay length: " << _ssgi.rayLength
        << "\nThickness: " << _ssgi.thickness
        << "\nHistory weight: " << _ssgi.historyWeight
        << "\nFilter enabled: " << _ssgi.spatialFilterEnabled
        << "\nFilter radius: " << _ssgi.filterRadius
        << "\nIntensity: " << _ssgi.intensity
        << "\nAmbient retention: " << _ssgi.ambientRetention
        << "\nMiss fill: "
        << (_ssgi.traceEnvironmentMap ? "environment map" : "analytic gradient")
        << "\nEnvironment mip: " << _skyboxEnvironmentLod
        << "\nIndirect sky/sun split: " << _skyboxIndirectClamp
        // Captures taken before the albedo multiply moved to the
        // composite hold reflected colour, so a comparison across that
        // change needs to know which quantity it is looking at.
        << "\nQuantity: incident radiance"
        << " (receiver albedo applied at composite)"
        << "\nLinear indirect mean: " << sum / pixels
        << "\nNonfinite pixels: " << invalid << "\nCamera: "
        << render_camera().position.x << "," << render_camera().position.y
        << "," << render_camera().position.z << " pitch="
        << render_camera().pitch << " yaw=" << render_camera().yaw << "\n";

    if (const char* referencePath = std::getenv("MIRABILIS_SSGI_REFERENCE")) {
        std::ifstream referenceFile(referencePath, std::ios::binary);
        std::string magic;
        uint32_t width = 0, height = 0;
        float scale = 0.0f;
        referenceFile >> magic >> width >> height >> scale;
        referenceFile.get();
        if (!referenceFile || magic != "PF" || width != extent.width ||
            height != extent.height || scale >= 0.0f) {
            fmt::print("SSGI reference is not a matching little-endian RGB PFM: {}\n",
                referencePath);
            std::abort();
        }
        std::vector<glm::vec4> reference(pixels, glm::vec4(0.0f));
        for (int y = int(height) - 1; y >= 0; --y) {
            for (uint32_t x = 0; x < width; ++x) {
                glm::vec3 rgb{};
                referenceFile.read(reinterpret_cast<char*>(&rgb),
                    3 * sizeof(float));
                reference[size_t(y) * width + x] = glm::vec4(rgb, 1.0f);
            }
        }
        if (!referenceFile) std::abort();

        std::vector<glm::vec4> difference(pixels);
        double absoluteError = 0.0;
        double squaredError = 0.0;
        double referenceMagnitude = 0.0;
        float maximumError = 0.0f;
        for (size_t pixel = 0; pixel < pixels; ++pixel) {
            const glm::vec3 delta = glm::vec3(values[pixel] - reference[pixel]);
            const glm::vec3 absolute = glm::abs(delta);
            difference[pixel] = glm::vec4(absolute, 1.0f);
            for (int channel = 0; channel < 3; ++channel) {
                absoluteError += absolute[channel];
                squaredError += delta[channel] * delta[channel];
                referenceMagnitude += std::abs(reference[pixel][channel]);
                maximumError = std::max(maximumError, absolute[channel]);
            }
        }
        const double samples = static_cast<double>(pixels) * 3.0;
        const double mae = absoluteError / samples;
        const double rmse = std::sqrt(squaredError / samples);
        const double relativeMae = absoluteError /
            std::max(referenceMagnitude, 1e-12);
        writePfm(std::string(filename) + ".difference.pfm", difference);
        metadata << "Reference: " << referencePath
            << "\nMAE: " << mae << "\nRMSE: " << rmse
            << "\nRelative MAE: " << relativeMae
            << "\nMaximum absolute channel error: " << maximumError << "\n";
        fmt::print(
            "SSGI comparison: MAE={} RMSE={} relative-MAE={} max={}",
            mae, rmse, relativeMae, maximumError);
        fmt::print("\n");
    }
    if (!metadata || invalid != 0) std::abort();
    fmt::print("SSGI capture {}: {}x{} linear indirect mean={} nonfinite={}\n",
        filename, extent.width, extent.height, sum / pixels, invalid);
}
