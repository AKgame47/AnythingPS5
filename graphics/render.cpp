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
  if (!out) return Status::Fail(100, "vulkan", "render needs an output buffer");
  std::vector<std::vector<uint8_t>> fr;
  Status s = render_core(w, h, vs, fs, vertex_count, nullptr, nullptr, clear, &fr);
  if (s && !fr.empty()) *out = std::move(fr[0]);
  return s;
}

Status VulkanBackend::render_frames_rgba(
    uint32_t w, uint32_t h, const std::vector<uint32_t>& vs,
    const std::vector<uint32_t>& fs, const std::vector<float>& vertices,
    const std::vector<std::vector<uint32_t>>& fp, const float clear[4],
    std::vector<std::vector<uint8_t>>* frames) {
  if (vertices.empty() || vertices.size() % 5)
    return Status::Fail(100, "vulkan", "vertices must be float2 position + float3 colour per vertex (multiple of 5 floats)");
  if (fp.empty() || fp.size() > 64)
    return Status::Fail(100, "vulkan", "frame count must be 1..64");
  for (auto& p : fp)
    if (p.empty() || p.size() > 16 || p.size() != fp[0].size())
      return Status::Fail(100, "vulkan", "push constants must be 1..16 words, same size every frame");
  MeshInput m;
  m.vertices = &vertices;
  return render_core(w, h, vs, fs, (uint32_t)(vertices.size() / 5), &m, &fp, clear, frames);
}

Status VulkanBackend::render_clip_mesh_rgba(
    uint32_t w, uint32_t h, const std::vector<uint32_t>& vs,
    const std::vector<uint32_t>& fs, const std::vector<float>& vertices,
    const std::vector<uint32_t>& indices, bool depth, const float clear[4],
    std::vector<uint8_t>* image) {
  if (!image) return Status::Fail(100, "vulkan", "clip mesh render needs an output image");
  if (vertices.empty() || vertices.size() % 7 || vertices.size() / 7 > 65536)
    return Status::Fail(100, "vulkan", "clip mesh vertices must be float4 clip position + float3 colour (multiple of 7 floats, max 65536 vertices)");
  if (indices.empty() || indices.size() % 3 || indices.size() > 30000)
    return Status::Fail(100, "vulkan", "mesh indices must be a nonzero multiple of 3 (max 30000)");
  for (uint32_t i : indices)
    if (i >= vertices.size() / 7)
      return Status::Fail(100, "vulkan", "mesh index out of range of the vertex array");
  MeshInput m;
  m.vertices = &vertices; m.pos_floats = 4; m.indices = &indices; m.depth = depth;
  std::vector<std::vector<uint8_t>> frames;
  auto st = render_core(w, h, vs, fs, (uint32_t)indices.size(), &m, nullptr, clear, &frames);
  if (!st) return st;
  if (frames.size() != 1) return Status::Fail(100, "vulkan", "clip mesh render produced an unexpected frame count");
  *image = std::move(frames[0]);
  return Status::Ok();
}

Status VulkanBackend::render_lit_mesh_frames_rgba(
    uint32_t w, uint32_t h, const std::vector<uint32_t>& vs,
    const std::vector<uint32_t>& fs, const std::vector<float>& vertices,
    const std::vector<uint32_t>& indices, bool depth,
    const std::vector<std::vector<uint32_t>>& fp, const float clear[4],
    std::vector<std::vector<uint8_t>>* frames) {
  if (vertices.empty() || vertices.size() % 9 || vertices.size() / 9 > 65536)
    return Status::Fail(100, "vulkan", "lit mesh vertices must be float3 position + float3 colour + float3 normal (multiple of 9 floats, max 65536 vertices)");
  if (indices.empty() || indices.size() % 3 || indices.size() > 30000)
    return Status::Fail(100, "vulkan", "mesh indices must be a nonzero multiple of 3 (max 30000)");
  for (uint32_t i : indices)
    if (i >= vertices.size() / 9)
      return Status::Fail(100, "vulkan", "mesh index out of range of the vertex array");
  if (fp.empty() || fp.size() > 64)
    return Status::Fail(100, "vulkan", "frame count must be 1..64");
  for (auto& p : fp)
    if (p.empty() || p.size() > 32 || p.size() != fp[0].size())
      return Status::Fail(100, "vulkan", "lit push constants must be 1..32 words, same size every frame");
  MeshInput m;
  m.vertices = &vertices; m.pos_floats = 3; m.extra_floats = 3; m.indices = &indices; m.depth = depth;
  return render_core(w, h, vs, fs, (uint32_t)indices.size(), &m, &fp, clear, frames);
}

Status VulkanBackend::render_textured_mesh_frames_rgba(
    uint32_t w, uint32_t h, const std::vector<uint32_t>& vs,
    const std::vector<uint32_t>& fs, const std::vector<float>& vertices,
    const std::vector<uint32_t>& indices, bool depth, const TexInput& tex,
    const std::vector<std::vector<uint32_t>>& fp, const float clear[4],
    std::vector<std::vector<uint8_t>>* frames) {
  if (vertices.empty() || vertices.size() % 8 || vertices.size() / 8 > 65536)
    return Status::Fail(100, "vulkan", "textured mesh vertices must be float3 position + float3 colour + float2 uv (multiple of 8 floats, max 65536 vertices)");
  if (indices.empty() || indices.size() % 3 || indices.size() > 30000)
    return Status::Fail(100, "vulkan", "mesh indices must be a nonzero multiple of 3 (max 30000)");
  for (uint32_t i : indices)
    if (i >= vertices.size() / 8)
      return Status::Fail(100, "vulkan", "mesh index out of range of the vertex array");
  if (tex.rgba == nullptr || tex.w == 0 || tex.h == 0 || tex.w > 256 || tex.h > 256)
    return Status::Fail(100, "vulkan", "texture must be 1..256 per side with pixel data");
  if (fp.empty() || fp.size() > 64)
    return Status::Fail(100, "vulkan", "frame count must be 1..64");
  for (auto& p : fp)
    if (p.empty() || p.size() > 32 || p.size() != fp[0].size())
      return Status::Fail(100, "vulkan", "textured push constants must be 1..32 words, same size every frame");
  MeshInput m;
  m.vertices = &vertices; m.pos_floats = 3; m.extra_floats = 2; m.indices = &indices; m.depth = depth;
  m.tex = &tex;
  return render_core(w, h, vs, fs, (uint32_t)indices.size(), &m, &fp, clear, frames);
}

Status VulkanBackend::render_lit_textured_mesh_frames_rgba(
    uint32_t w, uint32_t h, const std::vector<uint32_t>& vs,
    const std::vector<uint32_t>& fs, const std::vector<float>& vertices,
    const std::vector<uint32_t>& indices, bool depth, const TexInput& tex,
    const std::vector<std::vector<uint32_t>>& fp, const float clear[4],
    std::vector<std::vector<uint8_t>>* frames) {
  if (vertices.empty() || vertices.size() % 11 || vertices.size() / 11 > 65536)
    return Status::Fail(100, "vulkan", "lit textured vertices must be float3 position + float3 colour + float2 uv + float3 normal (multiple of 11 floats, max 65536 vertices)");
  if (indices.empty() || indices.size() % 3 || indices.size() > 30000)
    return Status::Fail(100, "vulkan", "mesh indices must be a nonzero multiple of 3 (max 30000)");
  for (uint32_t i : indices)
    if (i >= vertices.size() / 11)
      return Status::Fail(100, "vulkan", "mesh index out of range of the vertex array");
  if (tex.rgba == nullptr || tex.w == 0 || tex.h == 0 || tex.w > 256 || tex.h > 256)
    return Status::Fail(100, "vulkan", "texture must be 1..256 per side with pixel data");
  if (fp.empty() || fp.size() > 64)
    return Status::Fail(100, "vulkan", "frame count must be 1..64");
  for (auto& p : fp)
    if (p.empty() || p.size() > 32 || p.size() != fp[0].size())
      return Status::Fail(100, "vulkan", "lit textured push constants must be 1..32 words, same size every frame");
  MeshInput m;
  m.vertices = &vertices; m.pos_floats = 3; m.extra_floats = 5; m.indices = &indices; m.depth = depth;
  m.tex = &tex;
  return render_core(w, h, vs, fs, (uint32_t)indices.size(), &m, &fp, clear, frames);
}

Status VulkanBackend::render_lit_clip_mesh_rgba(
    uint32_t w, uint32_t h, const std::vector<uint32_t>& vs,
    const std::vector<uint32_t>& fs, const std::vector<float>& vertices,
    const std::vector<uint32_t>& indices, bool depth, const float clear[4],
    std::vector<uint8_t>* image) {
  if (!image) return Status::Fail(100, "vulkan", "lit clip mesh render needs an output image");
  if (vertices.empty() || vertices.size() % 8 || vertices.size() / 8 > 65536)
    return Status::Fail(100, "vulkan", "lit clip vertices must be float4 clip position + float3 colour + float lighting scalar (multiple of 8 floats, max 65536 vertices)");
  if (indices.empty() || indices.size() % 3 || indices.size() > 30000)
    return Status::Fail(100, "vulkan", "mesh indices must be a nonzero multiple of 3 (max 30000)");
  for (uint32_t i : indices)
    if (i >= vertices.size() / 8)
      return Status::Fail(100, "vulkan", "mesh index out of range of the vertex array");
  MeshInput m;
  m.vertices = &vertices; m.pos_floats = 4; m.extra_floats = 1; m.indices = &indices; m.depth = depth;
  std::vector<std::vector<uint8_t>> frames;
  auto st = render_core(w, h, vs, fs, (uint32_t)indices.size(), &m, nullptr, clear, &frames);
  if (!st) return st;
  if (frames.size() != 1) return Status::Fail(100, "vulkan", "lit clip render produced an unexpected frame count");
  *image = std::move(frames[0]);
  return Status::Ok();
}

Status VulkanBackend::render_mesh_frames_rgba(
    uint32_t w, uint32_t h, const std::vector<uint32_t>& vs,
    const std::vector<uint32_t>& fs, const std::vector<float>& vertices,
    const std::vector<uint32_t>& indices, bool depth,
    const std::vector<std::vector<uint32_t>>& fp, const float clear[4],
    std::vector<std::vector<uint8_t>>* frames) {
  if (vertices.empty() || vertices.size() % 6 || vertices.size() / 6 > 65536)
    return Status::Fail(100, "vulkan", "mesh vertices must be float3 position + float3 colour (multiple of 6 floats, max 65536 vertices)");
  if (indices.empty() || indices.size() % 3 || indices.size() > 30000)
    return Status::Fail(100, "vulkan", "mesh indices must be a nonzero multiple of 3 (max 30000)");
  for (uint32_t i : indices)
    if (i >= vertices.size() / 6)
      return Status::Fail(100, "vulkan", "mesh index out of range of the vertex array");
  if (fp.empty() || fp.size() > 64)
    return Status::Fail(100, "vulkan", "frame count must be 1..64");
  for (auto& p : fp)
    if (p.empty() || p.size() > 16 || p.size() != fp[0].size())
      return Status::Fail(100, "vulkan", "push constants must be 1..16 words, same size every frame");
  MeshInput m;
  m.vertices = &vertices; m.pos_floats = 3; m.indices = &indices; m.depth = depth;
  return render_core(w, h, vs, fs, (uint32_t)indices.size(), &m, &fp, clear, frames);
}

Status VulkanBackend::render_core(
    uint32_t w, uint32_t h, const std::vector<uint32_t>& vs,
    const std::vector<uint32_t>& fs, uint32_t vertex_count,
    const MeshInput* mesh, const std::vector<std::vector<uint32_t>>* fp,
    const float clear[4], std::vector<std::vector<uint8_t>>* out) {
  const std::vector<float>* verts = mesh ? mesh->vertices : nullptr;
  const std::vector<uint32_t>* idx = mesh ? mesh->indices : nullptr;
  const bool depth = mesh && mesh->depth;
  const uint32_t pos_floats = mesh ? mesh->pos_floats : 2;
  const uint32_t extra_floats = mesh ? mesh->extra_floats : 0;
  const uint32_t vstride = (pos_floats + 3 + extra_floats) * 4;
  const uint32_t nframes = fp ? (uint32_t)fp->size() : 1;
  const uint32_t push_words = fp ? (uint32_t)(*fp)[0].size() : 0;
  if (!device_ || !out) return Status::Fail(100, "vulkan", "render needs a device and an output buffer");
  if (!(queue_flags_ & VK_QUEUE_GRAPHICS_BIT))
    return Status::Fail(100, "vulkan", "selected queue family has no graphics support; offscreen render Blocked on this device");
  if (w == 0 || h == 0 || w > 4096 || h > 4096)
    return Status::Fail(100, "vulkan", "render size must be 1..4096 per axis");
  if (vertex_count == 0 || vertex_count > 30000 || vertex_count % 3)
    return Status::Fail(100, "vulkan", "vertex_count must be a nonzero multiple of 3 (max 30000)");

  // Cleanup stack: run in reverse order on every exit path.
  std::vector<std::function<void()>> undo;
  struct Guard {
    std::vector<std::function<void()>>& u;
    ~Guard() { for (size_t i = u.size(); i-- > 0;) u[i](); }
  } guard{undo};

  const VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
  const VkDeviceSize total = bytes * nframes;
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

  // --- optional depth image (D32_SFLOAT) ---
  VkImageView dview = 0;
  VkImage dimage = 0;
  if (depth) {
    VkFormatProperties fprops{};
    fn<void (*)(VkPhysicalDevice, int32_t, VkFormatProperties*)>(vkGetPhysicalDeviceFormatProperties)(
        phys_, (int32_t)VK_FORMAT_D32_SFLOAT, &fprops);
    if (!(fprops.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
      return Status::Fail(100, "vulkan", "D32_SFLOAT depth attachment unsupported on this device; depth rendering Blocked");
    VkImageCreateInfo dci = ici;
    dci.format = VK_FORMAT_D32_SFLOAT;
    dci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    r = fn<VkResult (*)(VkDevice, const VkImageCreateInfo*, void*, VkImage*)>(
        vkCreateImage)(device_, &dci, nullptr, &dimage);
    if (r != VK_SUCCESS) return vkfail("vkCreateImage (depth)", r);
    undo.push_back([&, dimage] { fn<void (*)(VkDevice, VkImage, void*)>(vkDestroyImage)(device_, dimage, nullptr); });
    VkMemoryRequirements dreq{};
    fn<void (*)(VkDevice, VkImage, VkMemoryRequirements*)>(vkGetImageMemoryRequirements)(device_, dimage, &dreq);
    int dt = pick(dreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (dt < 0) dt = pick(dreq.memoryTypeBits, 0);
    if (dt < 0) return Status::Fail(100, "vulkan", "no memory type for the depth image");
    VkMemoryAllocateInfo dmai{};
    dmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    dmai.allocationSize = dreq.size; dmai.memoryTypeIndex = (uint32_t)dt;
    VkDeviceMemory dmem = 0;
    r = fn<VkResult (*)(VkDevice, const VkMemoryAllocateInfo*, void*, VkDeviceMemory*)>(
        vkAllocateMemory)(device_, &dmai, nullptr, &dmem);
    if (r != VK_SUCCESS) return vkfail("vkAllocateMemory (depth)", r);
    undo.push_back([&, dmem] { fn<void (*)(VkDevice, VkDeviceMemory, void*)>(vkFreeMemory)(device_, dmem, nullptr); });
    r = fn<VkResult (*)(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize)>(vkBindImageMemory)(device_, dimage, dmem, 0);
    if (r != VK_SUCCESS) return vkfail("vkBindImageMemory (depth)", r);
    VkImageViewCreateInfo dvci = vci;
    dvci.image = dimage; dvci.format = VK_FORMAT_D32_SFLOAT;
    dvci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    r = fn<VkResult (*)(VkDevice, const VkImageViewCreateInfo*, void*, VkImageView*)>(
        vkCreateImageView)(device_, &dvci, nullptr, &dview);
    if (r != VK_SUCCESS) return vkfail("vkCreateImageView (depth)", r);
    undo.push_back([&, dview] { fn<void (*)(VkDevice, VkImageView, void*)>(vkDestroyImageView)(device_, dview, nullptr); });
  }

  // --- host readback buffer ---
  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = total; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
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

  // --- optional vertex / index buffers (host visible, written once) ---
  auto host_buffer = [&](const void* data, size_t nbytes, VkFlags usage, const char* what,
                         VkBuffer* out_buf) -> Status {
    VkBufferCreateInfo b{};
    b.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    b.size = nbytes; b.usage = usage; b.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult rr = fn<VkResult (*)(VkDevice, const VkBufferCreateInfo*, void*, VkBuffer*)>(
        vkCreateBuffer)(device_, &b, nullptr, out_buf);
    if (rr != VK_SUCCESS) return vkfail(std::string("vkCreateBuffer (") + what + ")", rr);
    VkBuffer bb = *out_buf;
    undo.push_back([&, bb] { fn<void (*)(VkDevice, VkBuffer, void*)>(vkDestroyBuffer)(device_, bb, nullptr); });
    VkMemoryRequirements q{};
    fn<void (*)(VkDevice, VkBuffer, VkMemoryRequirements*)>(vkGetBufferMemoryRequirements)(device_, bb, &q);
    int t = pick(q.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t < 0) return Status::Fail(100, "vulkan", std::string("no host-visible+coherent memory type for the ") + what + " buffer");
    VkMemoryAllocateInfo m{};
    m.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    m.allocationSize = q.size; m.memoryTypeIndex = (uint32_t)t;
    VkDeviceMemory mm = 0;
    rr = fn<VkResult (*)(VkDevice, const VkMemoryAllocateInfo*, void*, VkDeviceMemory*)>(
        vkAllocateMemory)(device_, &m, nullptr, &mm);
    if (rr != VK_SUCCESS) return vkfail(std::string("vkAllocateMemory (") + what + ")", rr);
    undo.push_back([&, mm] { fn<void (*)(VkDevice, VkDeviceMemory, void*)>(vkFreeMemory)(device_, mm, nullptr); });
    rr = fn<VkResult (*)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize)>(vkBindBufferMemory)(device_, bb, mm, 0);
    if (rr != VK_SUCCESS) return vkfail(std::string("vkBindBufferMemory (") + what + ")", rr);
    void* p = nullptr;
    rr = fn<VkResult (*)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkFlags, void**)>(
        vkMapMemory)(device_, mm, 0, nbytes, 0, &p);
    if (rr != VK_SUCCESS) return vkfail(std::string("vkMapMemory (") + what + ")", rr);
    std::memcpy(p, data, nbytes);
    fn<void (*)(VkDevice, VkDeviceMemory)>(vkUnmapMemory)(device_, mm);
    return Status::Ok();
  };
  VkBuffer vbuf = 0, ibuf = 0;
  if (verts)
    if (Status s = host_buffer(verts->data(), verts->size() * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "vertex", &vbuf); !s.ok) return s;
  if (idx)
    if (Status s = host_buffer(idx->data(), idx->size() * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "index", &ibuf); !s.ok) return s;

  // --- optional sampled texture: image + view + sampler + staging + descriptor ---
  const TexInput* tex = mesh ? mesh->tex : nullptr;
  VkDescriptorSetLayout dlayout = 0;
  VkDescriptorSet tset = 0;
  VkImage timg = 0;
  VkBuffer tstage = 0;
  if (tex) {
    if (!tex->rgba || tex->w == 0 || tex->h == 0 || tex->w > 256 || tex->h > 256)
      return Status::Fail(100, "vulkan", "texture must be 1..256 per side with pixel data");
    VkFormatProperties fprops{};
    fn<void (*)(VkPhysicalDevice, int32_t, VkFormatProperties*)>(vkGetPhysicalDeviceFormatProperties)(
        phys_, (int32_t)VK_FORMAT_R8G8B8A8_UNORM, &fprops);
    if (!(fprops.optimalTilingFeatures & 0x1))
      return Status::Fail(100, "vulkan", "RGBA8 sampled images unsupported on this device; texture Blocked");
    if (tex->linear && !(fprops.optimalTilingFeatures & 0x1000))
      return Status::Fail(100, "vulkan", "linear filtering of RGBA8 unsupported on this device; use nearest");
    if (Status s = host_buffer(tex->rgba, (size_t)tex->w * tex->h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "texture staging", &tstage); !s.ok) return s;
    VkImageCreateInfo tci{};
    tci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    tci.imageType = VK_IMAGE_TYPE_2D; tci.format = VK_FORMAT_R8G8B8A8_UNORM;
    tci.extent = {tex->w, tex->h, 1}; tci.mipLevels = 1; tci.arrayLayers = 1;
    tci.samples = VK_SAMPLE_COUNT_1_BIT; tci.tiling = VK_IMAGE_TILING_OPTIMAL;
    tci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    tci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; tci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    r = fn<VkResult (*)(VkDevice, const VkImageCreateInfo*, void*, VkImage*)>(vkCreateImage)(device_, &tci, nullptr, &timg);
    if (r != VK_SUCCESS) return vkfail("vkCreateImage (texture)", r);
    undo.push_back([&] { fn<void (*)(VkDevice, VkImage, void*)>(vkDestroyImage)(device_, timg, nullptr); });
    VkMemoryRequirements treq{};
    fn<void (*)(VkDevice, VkImage, VkMemoryRequirements*)>(vkGetImageMemoryRequirements)(device_, timg, &treq);
    int tt = pick(treq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (tt < 0) tt = pick(treq.memoryTypeBits, 0);
    if (tt < 0) return Status::Fail(100, "vulkan", "no memory type for the texture image");
    VkMemoryAllocateInfo tmai{};
    tmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    tmai.allocationSize = treq.size; tmai.memoryTypeIndex = (uint32_t)tt;
    VkDeviceMemory tmem = 0;
    r = fn<VkResult (*)(VkDevice, const VkMemoryAllocateInfo*, void*, VkDeviceMemory*)>(vkAllocateMemory)(device_, &tmai, nullptr, &tmem);
    if (r != VK_SUCCESS) return vkfail("vkAllocateMemory (texture)", r);
    undo.push_back([&, tmem] { fn<void (*)(VkDevice, VkDeviceMemory, void*)>(vkFreeMemory)(device_, tmem, nullptr); });
    r = fn<VkResult (*)(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize)>(vkBindImageMemory)(device_, timg, tmem, 0);
    if (r != VK_SUCCESS) return vkfail("vkBindImageMemory (texture)", r);
    VkImageViewCreateInfo tvci{};
    tvci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    tvci.image = timg; tvci.viewType = VK_IMAGE_VIEW_TYPE_2D; tvci.format = VK_FORMAT_R8G8B8A8_UNORM;
    tvci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView tview = 0;
    r = fn<VkResult (*)(VkDevice, const VkImageViewCreateInfo*, void*, VkImageView*)>(vkCreateImageView)(device_, &tvci, nullptr, &tview);
    if (r != VK_SUCCESS) return vkfail("vkCreateImageView (texture)", r);
    undo.push_back([&, tview] { fn<void (*)(VkDevice, VkImageView, void*)>(vkDestroyImageView)(device_, tview, nullptr); });
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = sci.minFilter = tex->linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW =
        tex->repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.minLod = 0.0f; sci.maxLod = 0.0f;
    VkSampler samp = 0;
    r = fn<VkResult (*)(VkDevice, const VkSamplerCreateInfo*, void*, VkSampler*)>(vkCreateSampler)(device_, &sci, nullptr, &samp);
    if (r != VK_SUCCESS) return vkfail("vkCreateSampler", r);
    undo.push_back([&, samp] { fn<void (*)(VkDevice, VkSampler, void*)>(vkDestroySampler)(device_, samp, nullptr); });
    VkDescriptorSetLayoutBinding lb{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dlci{};
    dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = 1; dlci.pBindings = &lb;
    r = fn<VkResult (*)(VkDevice, const VkDescriptorSetLayoutCreateInfo*, void*, VkDescriptorSetLayout*)>(
        vkCreateDescriptorSetLayout)(device_, &dlci, nullptr, &dlayout);
    if (r != VK_SUCCESS) return vkfail("vkCreateDescriptorSetLayout (texture)", r);
    undo.push_back([&] { fn<void (*)(VkDevice, VkDescriptorSetLayout, void*)>(vkDestroyDescriptorSetLayout)(device_, dlayout, nullptr); });
    VkDescriptorPoolSize dps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &dps;
    VkDescriptorPool dpool = 0;
    r = fn<VkResult (*)(VkDevice, const VkDescriptorPoolCreateInfo*, void*, VkDescriptorPool*)>(
        vkCreateDescriptorPool)(device_, &dpci, nullptr, &dpool);
    if (r != VK_SUCCESS) return vkfail("vkCreateDescriptorPool (texture)", r);
    undo.push_back([&, dpool] { fn<void (*)(VkDevice, VkDescriptorPool, void*)>(vkDestroyDescriptorPool)(device_, dpool, nullptr); });
    VkDescriptorSetAllocateInfo tdai{};
    tdai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    tdai.descriptorPool = dpool; tdai.descriptorSetCount = 1; tdai.pSetLayouts = &dlayout;
    r = fn<VkResult (*)(VkDevice, const VkDescriptorSetAllocateInfo*, VkDescriptorSet*)>(
        vkAllocateDescriptorSets)(device_, &tdai, &tset);
    if (r != VK_SUCCESS) return vkfail("vkAllocateDescriptorSets (texture)", r);
    VkDescriptorImageInfo dii{samp, tview, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet tw{};
    tw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    tw.dstSet = tset; tw.dstBinding = 0; tw.descriptorCount = 1;
    tw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; tw.pImageInfo = &dii;
    fn<void (*)(VkDevice, uint32_t, const VkWriteDescriptorSet*, uint32_t, void*)>(
        vkUpdateDescriptorSets)(device_, 1, &tw, 0, nullptr);
  }

  // --- render pass: clear on load, store, end in TRANSFER_SRC ---
  VkAttachmentDescription att[2]{};
  att[0].format = VK_FORMAT_R8G8B8A8_UNORM; att[0].samples = VK_SAMPLE_COUNT_1_BIT;
  att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  att[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  if (depth) {
    att[1].format = VK_FORMAT_D32_SFLOAT; att[1].samples = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    sub.pDepthStencilAttachment = &dref;
  }
  sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
  VkSubpassDependency dep{};
  dep.srcSubpass = 0; dep.dstSubpass = VK_SUBPASS_EXTERNAL;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
  dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dep.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  VkRenderPassCreateInfo rpci{};
  rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rpci.attachmentCount = depth ? 2 : 1; rpci.pAttachments = att;
  rpci.subpassCount = 1; rpci.pSubpasses = &sub;
  rpci.dependencyCount = 1; rpci.pDependencies = &dep;
  VkRenderPass rp = 0;
  r = fn<VkResult (*)(VkDevice, const VkRenderPassCreateInfo*, void*, VkRenderPass*)>(
      vkCreateRenderPass)(device_, &rpci, nullptr, &rp);
  if (r != VK_SUCCESS) return vkfail("vkCreateRenderPass", r);
  undo.push_back([&, rp] { fn<void (*)(VkDevice, VkRenderPass, void*)>(vkDestroyRenderPass)(device_, rp, nullptr); });

  VkImageView fbviews[2] = {view, dview};
  VkFramebufferCreateInfo fbci{};
  fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fbci.renderPass = rp; fbci.attachmentCount = depth ? 2 : 1; fbci.pAttachments = fbviews;
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
  VkPushConstantRange prange{VK_SHADER_STAGE_VERTEX_BIT, 0, push_words * 4};
  if (push_words) { plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &prange; }
  if (tex) { plci.setLayoutCount = 1; plci.pSetLayouts = &dlayout; }
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
  VkVertexInputBindingDescription vbind{0, vstride, VK_VERTEX_INPUT_RATE_VERTEX};
  using Fmt = decltype(VkVertexInputAttributeDescription::format);
  VkVertexInputAttributeDescription vattr[4] = {
      {0, 0, static_cast<Fmt>(pos_floats == 4 ? (int32_t)VK_FORMAT_R32G32B32A32_SFLOAT : pos_floats == 3 ? (int32_t)VK_FORMAT_R32G32B32_SFLOAT : (int32_t)VK_FORMAT_R32G32_SFLOAT), 0},
      {1, 0, static_cast<Fmt>((int32_t)VK_FORMAT_R32G32B32_SFLOAT), pos_floats * 4},
      {2, 0, static_cast<Fmt>(extra_floats == 3 ? (int32_t)VK_FORMAT_R32G32B32_SFLOAT : (extra_floats == 2 || extra_floats == 5) ? (int32_t)VK_FORMAT_R32G32_SFLOAT : (int32_t)VK_FORMAT_R32_SFLOAT), (pos_floats + 3) * 4},
      {3, 0, static_cast<Fmt>((int32_t)VK_FORMAT_R32G32B32_SFLOAT), (pos_floats + 5) * 4}};
  if (verts) {
    vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vbind;
    vin.vertexAttributeDescriptionCount = extra_floats == 5 ? 4 : extra_floats ? 3 : 2; vin.pVertexAttributeDescriptions = vattr;
  }
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
  VkPipelineDepthStencilStateCreateInfo dss{};
  dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  dss.depthTestEnable = 1; dss.depthWriteEnable = 1;
  dss.depthCompareOp = VK_COMPARE_OP_LESS; dss.maxDepthBounds = 1.0f;
  VkGraphicsPipelineCreateInfo gpci{};
  gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  gpci.stageCount = 2; gpci.pStages = stages;
  gpci.pVertexInputState = &vin; gpci.pInputAssemblyState = &ia;
  gpci.pViewportState = &vps; gpci.pRasterizationState = &rs;
  gpci.pMultisampleState = &ms; gpci.pColorBlendState = &cb;
  if (depth) gpci.pDepthStencilState = &dss;
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
  std::vector<VkCommandBuffer> cmds(nframes);
  VkCommandBufferAllocateInfo cbai{};
  cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbai.commandPool = pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = nframes;
  r = fn<VkResult (*)(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*)>(
      vkAllocateCommandBuffers)(device_, &cbai, cmds.data());
  if (r != VK_SUCCESS) return vkfail("vkAllocateCommandBuffers", r);

  VkClearValue cv[2]{};
  for (int i = 0; i < 4; ++i) cv[0].color.float32[i] = clear[i];
  cv[1].depthStencil.depth = 1.0f;
  VkRenderPassBeginInfo rbi{};
  rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rbi.renderPass = rp; rbi.framebuffer = fb; rbi.renderArea = sc;
  rbi.clearValueCount = depth ? 2 : 1; rbi.pClearValues = cv;
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  for (uint32_t f = 0; f < nframes; ++f) {
    VkCommandBuffer cmd = cmds[f];
    fn<VkResult (*)(VkCommandBuffer, const VkCommandBufferBeginInfo*)>(vkBeginCommandBuffer)(cmd, &begin);
    fn<void (*)(VkCommandBuffer, const VkRenderPassBeginInfo*, int32_t)>(vkCmdBeginRenderPass)(
        cmd, &rbi, (int32_t)VK_SUBPASS_CONTENTS_INLINE);
    fn<void (*)(VkCommandBuffer, int32_t, VkPipeline)>(vkCmdBindPipeline)(
        cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    if (tset)
      fn<void (*)(VkCommandBuffer, int32_t, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*)>(
          vkCmdBindDescriptorSets)(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &tset, 0, nullptr);
    if (vbuf) {
      VkDeviceSize off = 0;
      fn<void (*)(VkCommandBuffer, uint32_t, uint32_t, const VkBuffer*, const VkDeviceSize*)>(
          vkCmdBindVertexBuffers)(cmd, 0, 1, &vbuf, &off);
    }
    if (push_words)
      fn<void (*)(VkCommandBuffer, VkPipelineLayout, VkFlags, uint32_t, uint32_t, const void*)>(
          vkCmdPushConstants)(cmd, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, push_words * 4, (*fp)[f].data());
    if (ibuf) {
      fn<void (*)(VkCommandBuffer, VkBuffer, VkDeviceSize, int32_t)>(vkCmdBindIndexBuffer)(
          cmd, ibuf, 0, (int32_t)VK_INDEX_TYPE_UINT32);
      fn<void (*)(VkCommandBuffer, uint32_t, uint32_t, uint32_t, int32_t, uint32_t)>(vkCmdDrawIndexed)(
          cmd, vertex_count, 1, 0, 0, 0);
    } else {
      fn<void (*)(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t)>(vkCmdDraw)(
          cmd, vertex_count, 1, 0, 0);
    }
    fn<void (*)(VkCommandBuffer)>(vkCmdEndRenderPass)(cmd);
    VkBufferImageCopy region{};
    region.bufferOffset = bytes * f;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    fn<void (*)(VkCommandBuffer, VkImage, int32_t, VkBuffer, uint32_t, const VkBufferImageCopy*)>(
        vkCmdCopyImageToBuffer)(cmd, image, (int32_t)VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
    fn<VkResult (*)(VkCommandBuffer)>(vkEndCommandBuffer)(cmd);
  }

  // One submit + fence wait per frame: frames are strictly sequential, the
  // single image is reused, and no frame pacing or presentation is implied.
  VkFenceCreateInfo fci{};
  fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = 0;
  r = fn<VkResult (*)(VkDevice, const VkFenceCreateInfo*, void*, VkFence*)>(
      vkCreateFence)(device_, &fci, nullptr, &fence);
  if (r != VK_SUCCESS) return vkfail("vkCreateFence", r);
  undo.push_back([&, fence] { fn<void (*)(VkDevice, VkFence, void*)>(vkDestroyFence)(device_, fence, nullptr); });
  if (tex) {
    // One-time upload: UNDEFINED -> TRANSFER_DST, copy staging, -> SHADER_READ_ONLY.
    VkCommandBuffer ucmd = nullptr;
    VkCommandBufferAllocateInfo uai{};
    uai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    uai.commandPool = pool; uai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; uai.commandBufferCount = 1;
    r = fn<VkResult (*)(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*)>(
        vkAllocateCommandBuffers)(device_, &uai, &ucmd);
    if (r != VK_SUCCESS) return vkfail("vkAllocateCommandBuffers (texture upload)", r);
    fn<VkResult (*)(VkCommandBuffer, const VkCommandBufferBeginInfo*)>(vkBeginCommandBuffer)(ucmd, &begin);
    VkImageMemoryBarrier b1{};
    b1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b1.srcAccessMask = 0; b1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b1.srcQueueFamilyIndex = b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.image = timg; b1.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    using BarrierFn = void (*)(VkCommandBuffer, VkFlags, VkFlags, VkFlags, uint32_t, const void*, uint32_t, const void*, uint32_t, const VkImageMemoryBarrier*);
    fn<BarrierFn>(vkCmdPipelineBarrier)(ucmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b1);
    VkBufferImageCopy tr{};
    tr.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    tr.imageExtent = {tex->w, tex->h, 1};
    fn<void (*)(VkCommandBuffer, VkBuffer, VkImage, int32_t, uint32_t, const VkBufferImageCopy*)>(
        vkCmdCopyBufferToImage)(ucmd, tstage, timg, (int32_t)VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &tr);
    VkImageMemoryBarrier b2 = b1;
    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    fn<BarrierFn>(vkCmdPipelineBarrier)(ucmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b2);
    fn<VkResult (*)(VkCommandBuffer)>(vkEndCommandBuffer)(ucmd);
    VkSubmitInfo us{};
    us.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    us.commandBufferCount = 1; us.pCommandBuffers = &ucmd;
    r = fn<VkResult (*)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence)>(vkQueueSubmit)(queue_, 1, &us, fence);
    if (r != VK_SUCCESS) return vkfail("vkQueueSubmit (texture upload)", r);
    r = fn<VkResult (*)(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t)>(
        vkWaitForFences)(device_, 1, &fence, 1, 30000000000ull);
    if (r != VK_SUCCESS) return vkfail("vkWaitForFences (texture upload)", r);
    r = fn<VkResult (*)(VkDevice, uint32_t, const VkFence*)>(vkResetFences)(device_, 1, &fence);
    if (r != VK_SUCCESS) return vkfail("vkResetFences", r);
  }
  for (uint32_t f = 0; f < nframes; ++f) {
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1; submit.pCommandBuffers = &cmds[f];
    r = fn<VkResult (*)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence)>(vkQueueSubmit)(queue_, 1, &submit, fence);
    if (r != VK_SUCCESS) return vkfail("vkQueueSubmit", r);
    r = fn<VkResult (*)(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t)>(
        vkWaitForFences)(device_, 1, &fence, 1, 30000000000ull);
    if (r != VK_SUCCESS) return vkfail("vkWaitForFences", r);
    r = fn<VkResult (*)(VkDevice, uint32_t, const VkFence*)>(vkResetFences)(device_, 1, &fence);
    if (r != VK_SUCCESS) return vkfail("vkResetFences", r);
  }

  void* mapped = nullptr;
  r = fn<VkResult (*)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkFlags, void**)>(
      vkMapMemory)(device_, bmem, 0, total, 0, &mapped);
  if (r != VK_SUCCESS) return vkfail("vkMapMemory (readback)", r);
  out->assign(nframes, std::vector<uint8_t>());
  for (uint32_t f = 0; f < nframes; ++f) {
    const uint8_t* src = static_cast<const uint8_t*>(mapped) + bytes * f;
    (*out)[f].assign(src, src + bytes);
  }
  fn<void (*)(VkDevice, VkDeviceMemory)>(vkUnmapMemory)(device_, bmem);
  return Status::Ok();
}

}  // namespace pnr::graphics
