// PNR MODULE 04 drawing slice: headless offscreen render + readback.
// No swapchain, no window system, no display. Status: Implemented,
// Tested on software Vulkan (lavapipe); real GPU untested.
#include <cstring>
#include <functional>

#include "graphics/vulkan.hpp"

namespace pnr::graphics {

namespace {
Status vkfail(const std::string& what, int rc) {
  return Status::Fail(100, "vulkan", what + " rc=" + std::to_string(rc));
}
template <typename Sig>
Sig fn(void* p) { return reinterpret_cast<Sig>(p); }
constexpr VkFlags kColorWriteRGBA = 0xF;
}  // namespace

Status VulkanBackend::render_offscreen_rgba(
    uint32_t w, uint32_t h, const std::vector<uint32_t>& vs,
    const std::vector<uint32_t>& fs, uint32_t vertex_count, const float clear[4],
    std::vector<uint8_t>* out) {
  if (!device_ || !out) return Status::Fail(100, "vulkan", "render needs a device and an output buffer");
  if (!(queue_flags_ & VK_QUEUE_GRAPHICS_BIT))
    return Status::Fail(100, "vulkan", "selected queue family has no graphics support; offscreen render Blocked on this device");
  if (w == 0 || h == 0 || w > 4096 || h > 4096)
    return Status::Fail(100, "vulkan", "render size must be 1..4096 per axis");
  if (vertex_count == 0 || vertex_count > 3000 || vertex_count % 3)
    return Status::Fail(100, "vulkan", "vertex_count must be a nonzero multiple of 3 (max 3000)");

  // Cleanup stack: run in reverse order on every exit path.
  std::vector<std::function<void()>> undo;
  struct Guard {
    std::vector<std::function<void()>>& u;
    ~Guard() { for (size_t i = u.size(); i-- > 0;) u[i](); }
  } guard{undo};

  const VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
  VkPhysicalDeviceMemoryProperties mem{};
  fn<void (*)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*)>(
      vkGetPhysicalDeviceMemoryProperties)(phys_, &mem);
  auto pick = [&](uint32_t bits, VkFlags want) -> int {
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & want) == want)
        return (int)i;
    return -1;
  };

  // --- colour image (device local if possible) ---
  VkImageCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = VK_FORMAT_R8G8B8A8_UNORM;
  ici.extent = {w, h, 1};
  ici.mipLevels = 1; ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage image = 0;
  VkResult r = fn<VkResult (*)(VkDevice, const VkImageCreateInfo*, void*, VkImage*)>(
      vkCreateImage)(device_, &ici, nullptr, &image);
  if (r != VK_SUCCESS) return vkfail("vkCreateImage (RGBA8 color attachment)", r);
  undo.push_back([&, image] { fn<void (*)(VkDevice, VkImage, void*)>(vkDestroyImage)(device_, image, nullptr); });
  VkMemoryRequirements ireq{};
  fn<void (*)(VkDevice, VkImage, VkMemoryRequirements*)>(vkGetImageMemoryRequirements)(device_, image, &ireq);
  int it = pick(ireq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (it < 0) it = pick(ireq.memoryTypeBits, 0);
  if (it < 0) return Status::Fail(100, "vulkan", "no memory type for the render image");
  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.allocationSize = ireq.size; mai.memoryTypeIndex = (uint32_t)it;
  VkDeviceMemory imem = 0;
  r = fn<VkResult (*)(VkDevice, const VkMemoryAllocateInfo*, void*, VkDeviceMemory*)>(
      vkAllocateMemory)(device_, &mai, nullptr, &imem);
  if (r != VK_SUCCESS) return vkfail("vkAllocateMemory (image)", r);
  undo.push_back([&, imem] { fn<void (*)(VkDevice, VkDeviceMemory, void*)>(vkFreeMemory)(device_, imem, nullptr); });
  r = fn<VkResult (*)(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize)>(vkBindImageMemory)(device_, image, imem, 0);
  if (r != VK_SUCCESS) return vkfail("vkBindImageMemory", r);

  VkImageViewCreateInfo vci{};
  vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vci.image = image; vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = VK_FORMAT_R8G8B8A8_UNORM;
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageView view = 0;
  r = fn<VkResult (*)(VkDevice, const VkImageViewCreateInfo*, void*, VkImageView*)>(
      vkCreateImageView)(device_, &vci, nullptr, &view);
  if (r != VK_SUCCESS) return vkfail("vkCreateImageView", r);
  undo.push_back([&, view] { fn<void (*)(VkDevice, VkImageView, void*)>(vkDestroyImageView)(device_, view, nullptr); });

  // --- host readback buffer ---
  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = bytes; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer buf = 0;
  r = fn<VkResult (*)(VkDevice, const VkBufferCreateInfo*, void*, VkBuffer*)>(
      vkCreateBuffer)(device_, &bci, nullptr, &buf);
  if (r != VK_SUCCESS) return vkfail("vkCreateBuffer (readback)", r);
  undo.push_back([&, buf] { fn<void (*)(VkDevice, VkBuffer, void*)>(vkDestroyBuffer)(device_, buf, nullptr); });
  VkMemoryRequirements breq{};
  fn<void (*)(VkDevice, VkBuffer, VkMemoryRequirements*)>(vkGetBufferMemoryRequirements)(device_, buf, &breq);
  int bt = pick(breq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (bt < 0) return Status::Fail(100, "vulkan", "no host-visible+coherent memory type for readback");
  mai.allocationSize = breq.size; mai.memoryTypeIndex = (uint32_t)bt;
  VkDeviceMemory bmem = 0;
  r = fn<VkResult (*)(VkDevice, const VkMemoryAllocateInfo*, void*, VkDeviceMemory*)>(
      vkAllocateMemory)(device_, &mai, nullptr, &bmem);
  if (r != VK_SUCCESS) return vkfail("vkAllocateMemory (readback)", r);
  undo.push_back([&, bmem] { fn<void (*)(VkDevice, VkDeviceMemory, void*)>(vkFreeMemory)(device_, bmem, nullptr); });
  r = fn<VkResult (*)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize)>(vkBindBufferMemory)(device_, buf, bmem, 0);
  if (r != VK_SUCCESS) return vkfail("vkBindBufferMemory", r);

  // --- render pass: clear on load, store, end in TRANSFER_SRC ---
  VkAttachmentDescription att{};
  att.format = VK_FORMAT_R8G8B8A8_UNORM; att.samples = VK_SAMPLE_COUNT_1_BIT;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
  VkSubpassDependency dep{};
  dep.srcSubpass = 0; dep.dstSubpass = VK_SUBPASS_EXTERNAL;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
  dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dep.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  VkRenderPassCreateInfo rpci{};
  rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rpci.attachmentCount = 1; rpci.pAttachments = &att;
  rpci.subpassCount = 1; rpci.pSubpasses = &sub;
  rpci.dependencyCount = 1; rpci.pDependencies = &dep;
  VkRenderPass rp = 0;
  r = fn<VkResult (*)(VkDevice, const VkRenderPassCreateInfo*, void*, VkRenderPass*)>(
      vkCreateRenderPass)(device_, &rpci, nullptr, &rp);
  if (r != VK_SUCCESS) return vkfail("vkCreateRenderPass", r);
  undo.push_back([&, rp] { fn<void (*)(VkDevice, VkRenderPass, void*)>(vkDestroyRenderPass)(device_, rp, nullptr); });

  VkFramebufferCreateInfo fbci{};
  fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fbci.renderPass = rp; fbci.attachmentCount = 1; fbci.pAttachments = &view;
  fbci.width = w; fbci.height = h; fbci.layers = 1;
  VkFramebuffer fb = 0;
  r = fn<VkResult (*)(VkDevice, const VkFramebufferCreateInfo*, void*, VkFramebuffer*)>(
      vkCreateFramebuffer)(device_, &fbci, nullptr, &fb);
  if (r != VK_SUCCESS) return vkfail("vkCreateFramebuffer", r);
  undo.push_back([&, fb] { fn<void (*)(VkDevice, VkFramebuffer, void*)>(vkDestroyFramebuffer)(device_, fb, nullptr); });

  // --- shaders + pipeline ---
  auto make_module = [&](const std::vector<uint32_t>& code, const char* what,
                         VkShaderModule* m) -> Status {
    if (code.empty()) return Status::Fail(100, "vulkan", std::string(what) + " shader is empty");
    VkShaderModuleCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sci.codeSize = code.size() * 4; sci.pCode = code.data();
    VkResult rr = fn<VkResult (*)(VkDevice, const VkShaderModuleCreateInfo*, void*, VkShaderModule*)>(
        vkCreateShaderModule)(device_, &sci, nullptr, m);
    if (rr != VK_SUCCESS)
      return vkfail(std::string("vkCreateShaderModule (") + what + ", driver rejected the SPIR-V)", rr);
    VkShaderModule mm = *m;
    undo.push_back([&, mm] { fn<void (*)(VkDevice, VkShaderModule, void*)>(vkDestroyShaderModule)(device_, mm, nullptr); });
    return Status::Ok();
  };
  VkShaderModule vmod = 0, fmod = 0;
  if (Status s = make_module(vs, "vertex", &vmod); !s.ok) return s;
  if (Status s = make_module(fs, "fragment", &fmod); !s.ok) return s;

  VkPipelineLayoutCreateInfo plci{};
  plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  VkPipelineLayout pl = 0;
  r = fn<VkResult (*)(VkDevice, const VkPipelineLayoutCreateInfo*, void*, VkPipelineLayout*)>(
      vkCreatePipelineLayout)(device_, &plci, nullptr, &pl);
  if (r != VK_SUCCESS) return vkfail("vkCreatePipelineLayout", r);
  undo.push_back([&, pl] { fn<void (*)(VkDevice, VkPipelineLayout, void*)>(vkDestroyPipelineLayout)(device_, pl, nullptr); });

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vmod; stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fmod; stages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vin{};
  vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo ia{};
  ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkViewport vp{0, 0, (float)w, (float)h, 0.0f, 1.0f};
  VkRect2D sc{{0, 0}, {w, h}};
  VkPipelineViewportStateCreateInfo vps{};
  vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
  VkPipelineRasterizationStateCreateInfo rs{};
  rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{};
  ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState cba{};
  cba.colorWriteMask = kColorWriteRGBA;
  VkPipelineColorBlendStateCreateInfo cb{};
  cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  cb.attachmentCount = 1; cb.pAttachments = &cba;
  VkGraphicsPipelineCreateInfo gpci{};
  gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  gpci.stageCount = 2; gpci.pStages = stages;
  gpci.pVertexInputState = &vin; gpci.pInputAssemblyState = &ia;
  gpci.pViewportState = &vps; gpci.pRasterizationState = &rs;
  gpci.pMultisampleState = &ms; gpci.pColorBlendState = &cb;
  gpci.layout = pl; gpci.renderPass = rp; gpci.subpass = 0;
  gpci.basePipelineIndex = -1;
  VkPipeline pipe = 0;
  r = fn<VkResult (*)(VkDevice, uint64_t, uint32_t, const VkGraphicsPipelineCreateInfo*, void*, VkPipeline*)>(
      vkCreateGraphicsPipelines)(device_, 0, 1, &gpci, nullptr, &pipe);
  if (r != VK_SUCCESS) return vkfail("vkCreateGraphicsPipelines", r);
  undo.push_back([&, pipe] { fn<void (*)(VkDevice, VkPipeline, void*)>(vkDestroyPipeline)(device_, pipe, nullptr); });

  // --- record + submit ---
  VkCommandPoolCreateInfo kpci{};
  kpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  kpci.queueFamilyIndex = queue_family_;
  VkCommandPool pool = 0;
  r = fn<VkResult (*)(VkDevice, const VkCommandPoolCreateInfo*, void*, VkCommandPool*)>(
      vkCreateCommandPool)(device_, &kpci, nullptr, &pool);
  if (r != VK_SUCCESS) return vkfail("vkCreateCommandPool", r);
  undo.push_back([&, pool] { fn<void (*)(VkDevice, VkCommandPool, void*)>(vkDestroyCommandPool)(device_, pool, nullptr); });
  VkCommandBufferAllocateInfo cbai{};
  cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbai.commandPool = pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VkCommandBuffer cmd = 0;
  r = fn<VkResult (*)(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*)>(
      vkAllocateCommandBuffers)(device_, &cbai, &cmd);
  if (r != VK_SUCCESS) return vkfail("vkAllocateCommandBuffers", r);

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  fn<VkResult (*)(VkCommandBuffer, const VkCommandBufferBeginInfo*)>(vkBeginCommandBuffer)(cmd, &begin);
  VkClearValue cv{};
  for (int i = 0; i < 4; ++i) cv.color.float32[i] = clear[i];
  VkRenderPassBeginInfo rbi{};
  rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rbi.renderPass = rp; rbi.framebuffer = fb; rbi.renderArea = sc;
  rbi.clearValueCount = 1; rbi.pClearValues = &cv;
  fn<void (*)(VkCommandBuffer, const VkRenderPassBeginInfo*, int32_t)>(vkCmdBeginRenderPass)(
      cmd, &rbi, (int32_t)VK_SUBPASS_CONTENTS_INLINE);
  fn<void (*)(VkCommandBuffer, int32_t, VkPipeline)>(vkCmdBindPipeline)(
      cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  fn<void (*)(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t)>(vkCmdDraw)(
      cmd, vertex_count, 1, 0, 0);
  fn<void (*)(VkCommandBuffer)>(vkCmdEndRenderPass)(cmd);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {w, h, 1};
  fn<void (*)(VkCommandBuffer, VkImage, int32_t, VkBuffer, uint32_t, const VkBufferImageCopy*)>(
      vkCmdCopyImageToBuffer)(cmd, image, (int32_t)VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
  fn<VkResult (*)(VkCommandBuffer)>(vkEndCommandBuffer)(cmd);

  VkFenceCreateInfo fci{};
  fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = 0;
  r = fn<VkResult (*)(VkDevice, const VkFenceCreateInfo*, void*, VkFence*)>(
      vkCreateFence)(device_, &fci, nullptr, &fence);
  if (r != VK_SUCCESS) return vkfail("vkCreateFence", r);
  undo.push_back([&, fence] { fn<void (*)(VkDevice, VkFence, void*)>(vkDestroyFence)(device_, fence, nullptr); });
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
  r = fn<VkResult (*)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence)>(vkQueueSubmit)(queue_, 1, &submit, fence);
  if (r != VK_SUCCESS) return vkfail("vkQueueSubmit", r);
  r = fn<VkResult (*)(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t)>(
      vkWaitForFences)(device_, 1, &fence, 1, 30000000000ull);
  if (r != VK_SUCCESS) return vkfail("vkWaitForFences", r);

  void* mapped = nullptr;
  r = fn<VkResult (*)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkFlags, void**)>(
      vkMapMemory)(device_, bmem, 0, bytes, 0, &mapped);
  if (r != VK_SUCCESS) return vkfail("vkMapMemory (readback)", r);
  out->resize((size_t)bytes);
  std::memcpy(out->data(), mapped, (size_t)bytes);
  fn<void (*)(VkDevice, VkDeviceMemory)>(vkUnmapMemory)(device_, bmem);
  return Status::Ok();
}

}  // namespace pnr::graphics
