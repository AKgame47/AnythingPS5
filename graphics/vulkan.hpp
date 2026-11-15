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

// Mesh description for the 3D slice. Vertices are interleaved: pos_floats
// position floats (2 or 3) then 3 colour floats. indices may be null (plain
// vertex list). depth enables a D32_SFLOAT depth buffer (LESS, write on).
// One RGBA8 texture. rgba is w*h*4 bytes for one level, row 0 at the top;
// the sampler is nearest or linear, repeat or clamp-to-edge. Slice S19: a
// mip chain is levels > 1 packed levels (level l tightly pitched at
// max(1,w>>l) x max(1,h>>l), one after another after level 0).
struct TexInput {
  const uint8_t* rgba = nullptr;
  uint32_t w = 0, h = 0;
  bool linear = false;
  bool repeat = false;
  // Slice S23: stage as VK_FORMAT_R8G8B8A8_SRGB so the host sampler does the
  // sRGB -> linear conversion (the guest bytes are sRGB-encoded).
  bool srgb = false;
  // Slice S17 overrides (VK_FILTER_* / VK_SAMPLER_ADDRESS_MODE_* values);
  // -1 keeps the linear/repeat-derived default so older callers are unchanged.
  int32_t mag_filter = -1, min_filter = -1;
  int32_t addr_u = -1, addr_v = -1;
  // Slice S19 mip state: levels = packed mip count (1 = no chain);
  // mip_mode is a VK_SAMPLER_MIPMAP_MODE_* value (-1 = nearest); max_lod
  // < 0 means the top level index.
  uint32_t levels = 1;
  int32_t mip_mode = -1;
  float min_lod = 0.0f, max_lod = -1.0f;
};

// Colour blend state for one target (slice S14), decoded from GNM
// CB_BLEND0_CONTROL. Fields hold VK_BLEND_FACTOR_*/VK_BLEND_OP_* values.
struct BlendState {
  bool enable = false;
  int32_t src_color = VK_BLEND_FACTOR_ONE, dst_color = VK_BLEND_FACTOR_ZERO;
  int32_t color_op = VK_BLEND_OP_ADD;
  int32_t src_alpha = VK_BLEND_FACTOR_ONE, dst_alpha = VK_BLEND_FACTOR_ZERO;
  int32_t alpha_op = VK_BLEND_OP_ADD;
};

// Frame-persistent render targets (slice S13): a colour + depth attachment
// pair kept across draws so consecutive draws accumulate into one frame and
// test against earlier depth, the way GNM games bind CB_COLOR0_* and DB_Z_*
// once per frame. Created and cleared on first use (or after destroy_frame),
// reloaded (LOAD_OP_LOAD) afterwards. Owned by the caller; destroy with
// destroy_frame(). Single-frame renders only.
struct FrameSurface {
  VkImage color_image = 0;
  VkDeviceMemory color_memory = 0;
  VkImageView color_view = 0;
  VkImage depth_image = 0;
  VkDeviceMemory depth_memory = 0;
  VkImageView depth_view = 0;
  uint32_t width = 0, height = 0;
};

struct MeshInput {
  const std::vector<float>* vertices = nullptr;
  uint32_t pos_floats = 2;
  uint32_t extra_floats = 0;  // optional attributes after colour: 1 (float), 2 (vec2 uv), 3 (vec3), 5 (vec2 uv at 2 + vec3 normal at 3)
  const std::vector<uint32_t>* indices = nullptr;
  bool depth = false;
  const TexInput* tex = nullptr;  // sampled at set 0 binding 0 (fragment stage)
  FrameSurface* shared_frame = nullptr;  // slice S13: persistent targets; null = per-draw
  bool depth_write = true;               // DB_DEPTH_CONTROL depth_write_enable
  const BlendState* blend = nullptr;     // slice S14: target 0 blend state; null = no blending
  // Slice S18 (PA_SU_SC_MODE_CNTL): VK_CULL_MODE_* / VK_FRONT_FACE_* values;
  // the NONE/CCW defaults keep the pre-S18 behaviour (no culling).
  int32_t cull_mode = VK_CULL_MODE_NONE;
  int32_t front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
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
  // Variant enabling extra instance extensions (e.g. VK_KHR_surface plus a
  // platform surface extension for window presentation). Missing extensions
  // surface as the honest vkCreateInstance error code.
  Status create_instance(const char* app_name,
                         const std::vector<const char*>& extra_extensions);
  const std::vector<VulkanDeviceInfo>& devices() const { return devices_; }

  // Window-presentation device: requires a queue family that is graphics +
  // compute AND can present to `surface`, and enables VK_KHR_swapchain.
  // Fails honestly when no such family exists. create_instance must have
  // been called with the surface's extensions first.
  Status create_device_for_present(uint32_t physical_index, VkSurfaceKHR surface);
  VkInstance instance_handle() const { return instance_; }
  VkDevice device_handle() const { return device_; }
  VkPhysicalDevice physical_handle() const { return phys_; }
  VkQueue queue_handle() const { return queue_; }
  uint32_t queue_family_index() const { return queue_family_; }

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

  // 3D slice: indexed mesh (float3 position + float3 colour, uint32
  // indices), depth buffer optional, per-frame vertex push constants
  // (typically a column-major 4x4 MVP = 16 words). Headless, sequential.
  Status render_mesh_frames_rgba(uint32_t width, uint32_t height,
                                 const std::vector<uint32_t>& vert_spirv,
                                 const std::vector<uint32_t>& frag_spirv,
                                 const std::vector<float>& vertices,
                                 const std::vector<uint32_t>& indices, bool depth,
                                 const std::vector<std::vector<uint32_t>>& frame_push,
                                 const float clear[4],
                                 std::vector<std::vector<uint8_t>>* frames);

  // Single frame, no push constants: vertices are float4 clip-space position
  // (x,y,z,w) + float3 colour (7 floats each), indexed, depth optional. The
  // vertex shader is expected to pass the position through unchanged.
  Status render_clip_mesh_rgba(uint32_t width, uint32_t height,
                               const std::vector<uint32_t>& vert_spirv,
                               const std::vector<uint32_t>& frag_spirv,
                               const std::vector<float>& vertices,
                               const std::vector<uint32_t>& indices, bool depth,
                               const float clear[4], std::vector<uint8_t>* image,
                               FrameSurface* shared_frame = nullptr,
                               bool depth_write = true,
                               const BlendState* blend = nullptr,
                               int32_t cull_mode = VK_CULL_MODE_NONE,
                               int32_t front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE);

  // Destroys a FrameSurface's Vulkan objects and zeroes it (slice S13).
  void destroy_frame(FrameSurface& s);

  // Lit variants: one extra attribute at location 2 after the colour.
  // Hand-authored path: float3 position + float3 colour + float3 normal (9
  // floats), push constants 1..32 words per frame. Clip path: float4 clip
  // position + float3 colour + float lighting scalar (8 floats), one frame.
  Status render_lit_mesh_frames_rgba(uint32_t width, uint32_t height,
                                     const std::vector<uint32_t>& vert_spirv,
                                     const std::vector<uint32_t>& frag_spirv,
                                     const std::vector<float>& vertices,
                                     const std::vector<uint32_t>& indices, bool depth,
                                     const std::vector<std::vector<uint32_t>>& frame_push,
                                     const float clear[4],
                                     std::vector<std::vector<uint8_t>>* frames);
  Status render_lit_clip_mesh_rgba(uint32_t width, uint32_t height,
                                   const std::vector<uint32_t>& vert_spirv,
                                   const std::vector<uint32_t>& frag_spirv,
                                   const std::vector<float>& vertices,
                                   const std::vector<uint32_t>& indices, bool depth,
                                   const float clear[4], std::vector<uint8_t>* image);

  // Clip-space textured variant (slice S17): float4 clip position + float3
  // colour (tint) + float2 uv (9 floats), single frame, optional shared frame
  // surface, depth write and blend like render_clip_mesh_rgba. The fragment
  // shader samples a combined image sampler at set 0 binding 0.
  Status render_textured_clip_mesh_rgba(uint32_t width, uint32_t height,
                                        const std::vector<uint32_t>& vert_spirv,
                                        const std::vector<uint32_t>& frag_spirv,
                                        const std::vector<float>& vertices,
                                        const std::vector<uint32_t>& indices, bool depth,
                                        const TexInput& tex,
                                        const float clear[4], std::vector<uint8_t>* image,
                                        FrameSurface* shared_frame = nullptr,
                                        bool depth_write = true,
                                        const BlendState* blend = nullptr,
                                        int32_t cull_mode = VK_CULL_MODE_NONE,
                                        int32_t front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE);

  // Textured variant: float3 position + float3 colour (tint) + float2 uv (8
  // floats), push constants 1..32 words per frame (vertex stage). The fragment
  // shader samples a combined image sampler at set 0 binding 0.
  Status render_textured_mesh_frames_rgba(uint32_t width, uint32_t height,
                                          const std::vector<uint32_t>& vert_spirv,
                                          const std::vector<uint32_t>& frag_spirv,
                                          const std::vector<float>& vertices,
                                          const std::vector<uint32_t>& indices, bool depth,
                                          const TexInput& tex,
                                          const std::vector<std::vector<uint32_t>>& frame_push,
                                          const float clear[4],
                                          std::vector<std::vector<uint8_t>>* frames);

  // Lit textured variant: float3 position + float3 colour (tint) + float2 uv
  // + float3 normal (11 floats), push constants 1..32 words per frame.
  Status render_lit_textured_mesh_frames_rgba(uint32_t width, uint32_t height,
                                              const std::vector<uint32_t>& vert_spirv,
                                              const std::vector<uint32_t>& frag_spirv,
                                              const std::vector<float>& vertices,
                                              const std::vector<uint32_t>& indices, bool depth,
                                              const TexInput& tex,
                                              const std::vector<std::vector<uint32_t>>& frame_push,
                                              const float clear[4],
                                              std::vector<std::vector<uint8_t>>* frames);

  void shutdown();

private:
  friend class WindowPresenter;  // uses resolved entry points for swapchain I/O
  Status create_frame_targets(uint32_t w, uint32_t h, bool with_depth, FrameSurface& s);
  Status render_core(uint32_t w, uint32_t h, const std::vector<uint32_t>& vs,
                     const std::vector<uint32_t>& fs, uint32_t vertex_count,
                     const MeshInput* mesh,
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
  X(vkCmdCopyImageToBuffer) X(vkCmdBindVertexBuffers) X(vkResetFences) \
  X(vkGetPhysicalDeviceFormatProperties) X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) \
  X(vkCreateSampler) X(vkDestroySampler) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage)
#define PNR_VK_MEMBER(name) void* name = nullptr;
  PNR_VK_LIST(PNR_VK_MEMBER)
#undef PNR_VK_MEMBER
  // Optional presentation entry points (KHR_surface/KHR_swapchain). Resolved
  // in open() but NOT required there: a headless host without them stays a
  // working compute/offscreen backend; presentation fails honestly later.
#define PNR_VK_LIST_PRESENT(X) \
  X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
  X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
  X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkCreateSwapchainKHR) \
  X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
  X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkCreateSemaphore) \
  X(vkDestroySemaphore)
#define PNR_VK_MEMBER(name) void* name = nullptr;
  PNR_VK_LIST_PRESENT(PNR_VK_MEMBER)
#undef PNR_VK_MEMBER
};

}  // namespace pnr::graphics
