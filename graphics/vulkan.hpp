// PNR Vulkan backend - MODULE 04 first slice.
// Loaded with dlopen at runtime: builds without the Vulkan SDK, and a host
// with no driver reports BLOCKED honestly instead of failing to link.
// Compute-only for now (queues, SSBO, pipeline, dispatch); swapchains and
// render passes are Planned. Tested on Mesa lavapipe (software Vulkan).
// Status: Implemented (compute), Tested (software Vulkan); GPU untested.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/runtime.hpp"
#include "graphics/vulkan_shim.h"

namespace pnr::graphics {

struct VulkanDeviceInfo {
  std::string name;
  uint32_t vendor_id = 0;
  uint32_t api_version = 0;
  int32_t type = -1;
  bool software = false;  // VK_PHYSICAL_DEVICE_TYPE_CPU
};

class VulkanBackend {
public:
  VulkanBackend() = default;
  VulkanBackend(const VulkanBackend&) = delete;
  VulkanBackend& operator=(const VulkanBackend&) = delete;
  ~VulkanBackend();

  // dlopen libvulkan and resolve the entry points. Fails honestly when
  // the loader or a required function is missing.
  Status open();
  bool available() const { return lib_ != nullptr; }

  Status create_instance(const char* app_name);
  const std::vector<VulkanDeviceInfo>& devices() const { return devices_; }

  // Create a logical device with one compute-capable queue on
  // devices()[physical_index]. Fails honestly when no compute queue exists.
  Status create_device(uint32_t physical_index = 0);
  std::string device_name() const;

  // Run a compute shader (SPIR-V words) with a single SSBO (set 0,
  // binding 0) holding io (words in, words out). Returns an honest error
  // naming the exact stage that failed. Optional uniforms (max 16 uints)
  // are pushed at offset zero for compute; caller must match shader layout.
  Status run_compute_words(const std::vector<uint32_t>& spirv,
                           std::vector<uint32_t>* io,
                           const std::vector<uint32_t>& uniforms = {});

  // MODULE 04 drawing slice: render `vertex_count` vertices (triangle
  // list, no vertex buffers; the vertex shader generates positions from
  // gl_VertexIndex) into an OFFSCREEN RGBA8 image cleared to `clear`,
  // then copy it to host memory (w*h*4 bytes, row-major, top row first).
  // There is NO swapchain and NO display: this is headless rendering.
  // Shaders are SPIR-V words with entry point "main".
  Status render_offscreen_rgba(uint32_t width, uint32_t height,
                               const std::vector<uint32_t>& vert_spirv,
                               const std::vector<uint32_t>& frag_spirv,
                               uint32_t vertex_count, const float clear[4],
                               std::vector<uint8_t>* rgba);

  // Animated slice: interleaved vertex buffer (per vertex: float2 position,
  // float3 colour = 5 floats, locations 0 and 1) and per-frame vertex-stage
  // push constants (every entry the same size, 1..16 words). One RGBA8
  // image is rendered per entry of frame_push; frames[i] holds frame i.
  // Headless: no swapchain, no presentation, no frame pacing.
  Status render_frames_rgba(uint32_t width, uint32_t height,
                            const std::vector<uint32_t>& vert_spirv,
                            const std::vector<uint32_t>& frag_spirv,
                            const std::vector<float>& vertices,
                            const std::vector<std::vector<uint32_t>>& frame_push,
                            const float clear[4],
                            std::vector<std::vector<uint8_t>>* frames);

  void shutdown();

private:
  Status render_core(uint32_t w, uint32_t h, const std::vector<uint32_t>& vs,
                     const std::vector<uint32_t>& fs, uint32_t vertex_count,
                     const std::vector<float>* vertices,
                     const std::vector<std::vector<uint32_t>>* frame_push,
                     const float clear[4], std::vector<std::vector<uint8_t>>* frames);
  void* lib_ = nullptr;
  VkInstance instance_ = 0;
  VkPhysicalDevice phys_ = 0;
  VkDevice device_ = 0;
  VkQueue queue_ = 0;
  uint32_t queue_family_ = 0;
  VkFlags queue_flags_ = 0;
  std::vector<VulkanDeviceInfo> devices_;
  std::vector<VkPhysicalDevice> phys_handles_;

  // Core 1.0 entry points PNR uses, resolved in open() via
  // vkGetInstanceProcAddr. Stored as void* and cast at the call site in
  // vulkan.cpp (keeps the shim header free of PFN typedefs).
#define PNR_VK_LIST(X) \
  X(vkCreateInstance) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) \
  X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
  X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice) X(vkDestroyDevice) \
  X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) \
  X(vkAllocateMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) \
  X(vkFreeMemory) X(vkDestroyBuffer) X(vkCreateShaderModule) \
  X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) \
  X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) \
  X(vkDestroyPipelineLayout) X(vkCreateDescriptorPool) \
  X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) \
  X(vkUpdateDescriptorSets) X(vkCreateComputePipelines) X(vkDestroyPipeline) \
  X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
  X(vkFreeCommandBuffers) X(vkBeginCommandBuffer) X(vkCmdBindPipeline) \
  X(vkCmdBindDescriptorSets) X(vkCmdDispatch) X(vkEndCommandBuffer) \
  X(vkCreateFence) X(vkWaitForFences) X(vkDestroyFence) X(vkQueueSubmit) \
  X(vkQueueWaitIdle) X(vkCmdPushConstants) \
  X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) \
  X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
  X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) \
  X(vkDestroyFramebuffer) X(vkCreateGraphicsPipelines) \
  X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdDraw) \
  X(vkCmdCopyImageToBuffer) X(vkCmdBindVertexBuffers) X(vkResetFences)
#define PNR_VK_MEMBER(name) void* name = nullptr;
  PNR_VK_LIST(PNR_VK_MEMBER)
#undef PNR_VK_MEMBER
};

}  // namespace pnr::graphics
