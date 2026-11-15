// PNR window presentation, GPL-2.0. Slice S3. See window.hpp for scope.
#include "graphics/window.hpp"

#include <cstring>
#include <SDL.h>

// SDL_vulkan.h pulls in the official Vulkan headers, which would collide
// with PNR's shim. Declare the two SDL entry points with ABI-matching
// opaque types instead (VkInstance is a pointer, VkSurfaceKHR a uint64).
extern "C" int SDLCALL SDL_Vulkan_GetInstanceExtensions(SDL_Window* window,
                                                        unsigned int* count,
                                                        const char** names);
extern "C" int SDLCALL SDL_Vulkan_CreateSurface(SDL_Window* window,
                                                void* instance,
                                                uint64_t* surface);

namespace pnr::graphics {

namespace {
Status fail(const char* stage, const std::string& what) {
  return Status::Fail(100, stage, what);
}
template <typename Sig>
Sig fn(void* p) { return reinterpret_cast<Sig>(p); }
}  // namespace

Status WindowPresenter::create(VulkanBackend& vk, const WindowConfig& cfg,
                               std::unique_ptr<WindowPresenter>* out) {
  if (!out) return fail("window", "output pointer required");
  out->reset();
  if (!cfg.width || !cfg.height || cfg.width > 8192 || cfg.height > 8192)
    return fail("window", "window size must be 1..8192 per axis");
  if (SDL_Init(SDL_INIT_VIDEO) != 0)
    return fail("window", std::string("no usable display (SDL: ") +
                          SDL_GetError() + "); presentation is Blocked here");
  struct SdlGuard {
    bool armed = true;
    ~SdlGuard() { if (armed) SDL_QuitSubSystem(SDL_INIT_VIDEO); }
  } guard;

  SDL_Window* win = SDL_CreateWindow(cfg.title, SDL_WINDOWPOS_CENTERED,
                                     SDL_WINDOWPOS_CENTERED, (int)cfg.width,
                                     (int)cfg.height, SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN);
  if (!win)
    return fail("window", std::string("SDL_CreateWindow: ") + SDL_GetError());

  unsigned int ext_count = 0;
  if (!SDL_Vulkan_GetInstanceExtensions(win, &ext_count, nullptr) || !ext_count)
    return fail("window", "SDL reported no Vulkan instance extensions");
  std::vector<const char*> exts(ext_count);
  if (!SDL_Vulkan_GetInstanceExtensions(win, &ext_count, exts.data()))
    return fail("window", "SDL_Vulkan_GetInstanceExtensions failed");

  if (!vk.available())
    return fail("window", "VulkanBackend.open() first");
  auto st = vk.create_instance(cfg.app_name, exts);
  if (!st) return st;

  std::unique_ptr<WindowPresenter> p(new WindowPresenter());
  p->vk_ = &vk;
  p->window_ = win;
  p->width_ = cfg.width;
  p->height_ = cfg.height;
  if (!SDL_Vulkan_CreateSurface(win, vk.instance_handle(), &p->surface_))
    return fail("window", std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError());
  st = vk.create_device_for_present(0, p->surface_);
  if (!st) return st;
  st = p->create_swapchain();
  if (!st) return st;
  *out = std::move(p);
  guard.armed = false;  // the presenter owns the SDL video lifetime now
  return Status::Ok();
}

Status WindowPresenter::create_swapchain() {
  VulkanBackend& vk = *vk_;
  VkSurfaceCapabilitiesKHR caps{};
  if (fn<VkResult (*)(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR*)>(
          vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR)(
          vk.physical_handle(), surface_, &caps) != VK_SUCCESS)
    return fail("window", "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed");
  uint32_t fmt_count = 0;
  fn<VkResult (*)(VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkSurfaceFormatKHR*)>(
      vk.vkGetPhysicalDeviceSurfaceFormatsKHR)(vk.physical_handle(), surface_,
                                               &fmt_count, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(fmt_count);
  if (fmt_count)
    fn<VkResult (*)(VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkSurfaceFormatKHR*)>(
        vk.vkGetPhysicalDeviceSurfaceFormatsKHR)(
        vk.physical_handle(), surface_, &fmt_count, formats.data());
  int format = VK_FORMAT_R8G8B8A8_UNORM;
  bool have_rgba = false, have_bgra = false;
  for (const auto& f : formats) {
    have_rgba |= f.format == VK_FORMAT_R8G8B8A8_UNORM;
    have_bgra |= f.format == VK_FORMAT_B8G8R8A8_UNORM;
  }
  if (have_rgba) { format = VK_FORMAT_R8G8B8A8_UNORM; bgra_ = false; }
  else if (have_bgra) { format = VK_FORMAT_B8G8R8A8_UNORM; bgra_ = true; }
  else return fail("window", "surface supports neither R8G8B8A8 nor B8G8R8A8");
  if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
    return fail("window", "swapchain images cannot be transfer destinations");
  if (caps.currentExtent.width != width_ || caps.currentExtent.height != height_)
    return fail("window", "surface size differs from the requested window; "
                          "swapchain resize is Planned, not faked");
  uint32_t count = caps.minImageCount + 1;
  if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;

  VkSwapchainCreateInfoKHR sci{};
  sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  sci.surface = surface_;
  sci.minImageCount = count;
  sci.imageFormat = format;
  sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  sci.imageExtent = {width_, height_};
  sci.imageArrayLayers = 1;
  sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  sci.preTransform = caps.currentTransform;
  sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // always valid: real vsync pacing
  sci.clipped = 1;
  if (fn<VkResult (*)(VkDevice, const VkSwapchainCreateInfoKHR*, void*,
                      VkSwapchainKHR*)>(vk.vkCreateSwapchainKHR)(
          vk.device_handle(), &sci, nullptr, &swapchain_) != VK_SUCCESS)
    return fail("window", "vkCreateSwapchainKHR failed");
  uint32_t image_count = 0;
  fn<VkResult (*)(VkDevice, VkSwapchainKHR, uint32_t*, VkImage*)>(
      vk.vkGetSwapchainImagesKHR)(vk.device_handle(), swapchain_, &image_count,
                                  nullptr);
  images_.assign(image_count, 0);
  fn<VkResult (*)(VkDevice, VkSwapchainKHR, uint32_t*, VkImage*)>(
      vk.vkGetSwapchainImagesKHR)(vk.device_handle(), swapchain_, &image_count,
                                  images_.data());

  VkSemaphoreCreateInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  if (fn<VkResult (*)(VkDevice, const VkSemaphoreCreateInfo*, void*, VkSemaphore*)>(
          vk.vkCreateSemaphore)(vk.device_handle(), &si, nullptr, &acquire_sem_) != VK_SUCCESS ||
      fn<VkResult (*)(VkDevice, const VkSemaphoreCreateInfo*, void*, VkSemaphore*)>(
          vk.vkCreateSemaphore)(vk.device_handle(), &si, nullptr, &present_sem_) != VK_SUCCESS)
    return fail("window", "vkCreateSemaphore failed");
  VkFenceCreateInfo fi{};
  fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  if (fn<VkResult (*)(VkDevice, const VkFenceCreateInfo*, void*, VkFence*)>(
          vk.vkCreateFence)(vk.device_handle(), &fi, nullptr, &fence_) != VK_SUCCESS)
    return fail("window", "vkCreateFence failed");
  VkCommandPoolCreateInfo cpi{};
  cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpi.flags = 0;  // buffers are allocated fresh per present, never reset
  cpi.queueFamilyIndex = vk.queue_family_index();
  if (fn<VkResult (*)(VkDevice, const VkCommandPoolCreateInfo*, void*,
                      VkCommandPool*)>(vk.vkCreateCommandPool)(
          vk.device_handle(), &cpi, nullptr, &cmd_pool_) != VK_SUCCESS)
    return fail("window", "vkCreateCommandPool failed");
  return Status::Ok();
}

Status WindowPresenter::ensure_staging(uint64_t bytes) {
  VulkanBackend& vk = *vk_;
  if (staging_ && staging_bytes_ >= bytes) return Status::Ok();
  if (staging_) {
    fn<void (*)(VkDevice, VkBuffer, void*)>(vk.vkDestroyBuffer)(vk.device_handle(), staging_, nullptr);
    fn<void (*)(VkDevice, VkDeviceMemory, void*)>(vk.vkFreeMemory)(vk.device_handle(), staging_mem_, nullptr);
    staging_ = 0; staging_mem_ = 0; staging_bytes_ = 0;
  }
  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = bytes;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (fn<VkResult (*)(VkDevice, const VkBufferCreateInfo*, void*, VkBuffer*)>(
          vk.vkCreateBuffer)(vk.device_handle(), &bci, nullptr, &staging_) != VK_SUCCESS)
    return fail("window", "staging vkCreateBuffer failed");
  VkMemoryRequirements req{};
  fn<void (*)(VkDevice, VkBuffer, VkMemoryRequirements*)>(
      vk.vkGetBufferMemoryRequirements)(vk.device_handle(), staging_, &req);
  VkPhysicalDeviceMemoryProperties props{};
  fn<void (*)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*)>(
      vk.vkGetPhysicalDeviceMemoryProperties)(vk.physical_handle(), &props);
  int type = -1;
  for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
    if ((req.memoryTypeBits & (1u << i)) &&
        (props.memoryTypes[i].propertyFlags &
         (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
      type = (int)i; break;
    }
  if (type < 0) return fail("window", "no host-visible coherent memory type");
  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = (uint32_t)type;
  if (fn<VkResult (*)(VkDevice, const VkMemoryAllocateInfo*, void*,
                      VkDeviceMemory*)>(vk.vkAllocateMemory)(
          vk.device_handle(), &mai, nullptr, &staging_mem_) != VK_SUCCESS)
    return fail("window", "staging vkAllocateMemory failed");
  if (fn<VkResult (*)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize)>(
          vk.vkBindBufferMemory)(vk.device_handle(), staging_, staging_mem_,
                                 0) != VK_SUCCESS)
    return fail("window", "staging vkBindBufferMemory failed");
  staging_bytes_ = bytes;
  return Status::Ok();
}

Status WindowPresenter::present(const std::vector<uint8_t>& rgba,
                                uint32_t w, uint32_t h) {
  VulkanBackend& vk = *vk_;
  if (w != width_ || h != height_)
    return fail("window", "frame size differs from the window; swapchain "
                          "resize is Planned, never a stretched fake");
  if (rgba.size() != size_t(w) * h * 4)
    return fail("window", "frame must be w*h*4 RGBA bytes");
  auto st = ensure_staging(rgba.size());
  if (!st) return st;

  void* map = nullptr;
  if (fn<VkResult (*)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize,
                      VkFlags, void**)>(vk.vkMapMemory)(
          vk.device_handle(), staging_mem_, 0, rgba.size(), 0, &map) != VK_SUCCESS || !map)
    return fail("window", "staging vkMapMemory failed");
  if (!bgra_) {
    std::memcpy(map, rgba.data(), rgba.size());
  } else {
    // Real channel swizzle for BGRA swapchains, not a colour flip.
    auto* dst = static_cast<uint8_t*>(map);
    for (size_t i = 0; i < rgba.size(); i += 4) {
      dst[i] = rgba[i + 2]; dst[i + 1] = rgba[i + 1];
      dst[i + 2] = rgba[i]; dst[i + 3] = rgba[i + 3];
    }
  }
  fn<void (*)(VkDevice, VkDeviceMemory)>(vk.vkUnmapMemory)(vk.device_handle(), staging_mem_);

  if (fn<VkResult (*)(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t)>(
          vk.vkWaitForFences)(vk.device_handle(), 1, &fence_, 1, ~0ull) != VK_SUCCESS)
    return fail("window", "present fence wait failed");
  fn<VkResult (*)(VkDevice, uint32_t, const VkFence*)>(vk.vkResetFences)(
      vk.device_handle(), 1, &fence_);
  uint32_t index = 0;
  const VkResult acq = fn<VkResult (*)(VkDevice, VkSwapchainKHR, uint64_t,
                                       VkSemaphore, VkFence, uint32_t*)>(
      vk.vkAcquireNextImageKHR)(vk.device_handle(), swapchain_, ~0ull,
                                acquire_sem_, 0, &index);
  if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR)
    return fail("window", "vkAcquireNextImageKHR rc=" + std::to_string(acq) +
                          " (out-of-date swapchain rebuild is Planned)");

  VkCommandBufferAllocateInfo cai{};
  cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cai.commandPool = cmd_pool_;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  if (fn<VkResult (*)(VkDevice, const VkCommandBufferAllocateInfo*,
                      VkCommandBuffer*)>(vk.vkAllocateCommandBuffers)(
          vk.device_handle(), &cai, &cmd_) != VK_SUCCESS)
    return fail("window", "vkAllocateCommandBuffers failed");
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  fn<VkResult (*)(VkCommandBuffer, const VkCommandBufferBeginInfo*)>(
      vk.vkBeginCommandBuffer)(cmd_, &bi);
  VkImageMemoryBarrier bar{};
  bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  bar.image = images_[index];
  bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  fn<void (*)(VkCommandBuffer, VkFlags, VkFlags, uint32_t, uint32_t,
              const void*, uint32_t, const void*, uint32_t,
              const VkImageMemoryBarrier*)>(vk.vkCmdPipelineBarrier)(
      cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
      0, 0, nullptr, 0, nullptr, 1, &bar);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {w, h, 1};
  fn<void (*)(VkCommandBuffer, VkBuffer, VkImage, int32_t, uint32_t,
              const VkBufferImageCopy*)>(vk.vkCmdCopyBufferToImage)(
      cmd_, staging_, images_[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
      &region);
  bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  bar.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  bar.dstAccessMask = 0;
  fn<void (*)(VkCommandBuffer, VkFlags, VkFlags, uint32_t, uint32_t,
              const void*, uint32_t, const void*, uint32_t,
              const VkImageMemoryBarrier*)>(vk.vkCmdPipelineBarrier)(
      cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
      0, nullptr, 0, nullptr, 1, &bar);
  if (fn<VkResult (*)(VkCommandBuffer)>(vk.vkEndCommandBuffer)(cmd_) != VK_SUCCESS)
    return fail("window", "vkEndCommandBuffer failed");
  const VkFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo sub{};
  sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  sub.waitSemaphoreCount = 1;
  sub.pWaitSemaphores = &acquire_sem_;
  sub.pWaitDstStageMask = &wait_stage;
  sub.commandBufferCount = 1;
  sub.pCommandBuffers = &cmd_;
  sub.signalSemaphoreCount = 1;
  sub.pSignalSemaphores = &present_sem_;
  if (fn<VkResult (*)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence)>(
          vk.vkQueueSubmit)(vk.queue_handle(), 1, &sub, fence_) != VK_SUCCESS)
    return fail("window", "present vkQueueSubmit failed");
  VkPresentInfoKHR pi{};
  pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  pi.waitSemaphoreCount = 1;
  pi.pWaitSemaphores = &present_sem_;
  pi.swapchainCount = 1;
  pi.pSwapchains = &swapchain_;
  pi.pImageIndices = &index;
  const VkResult pr = fn<VkResult (*)(VkQueue, const VkPresentInfoKHR*)>(
      vk.vkQueuePresentKHR)(vk.queue_handle(), &pi);
  fn<VkResult (*)(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t)>(
      vk.vkWaitForFences)(vk.device_handle(), 1, &fence_, 1, ~0ull);
  fn<void (*)(VkDevice, VkCommandPool, uint32_t, const VkCommandBuffer*)>(
      vk.vkFreeCommandBuffers)(vk.device_handle(), cmd_pool_, 1, &cmd_);
  cmd_ = 0;
  if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR)
    return fail("window", "vkQueuePresentKHR rc=" + std::to_string(pr) +
                          " (out-of-date swapchain rebuild is Planned)");
  ++presented_;
  return Status::Ok();
}

bool WindowPresenter::pump() {
  SDL_Event e;
  while (SDL_PollEvent(&e)) if (e.type == SDL_QUIT) return false;
  return true;
}

void WindowPresenter::destroy_swapchain() {
  VulkanBackend& vk = *vk_;
  VkDevice dev = vk.device_handle();
  if (!dev) return;
  fn<VkResult (*)(VkQueue)>(vk.vkQueueWaitIdle)(vk.queue_handle());
  if (cmd_pool_) fn<void (*)(VkDevice, VkCommandPool, void*)>(vk.vkDestroyCommandPool)(dev, cmd_pool_, nullptr);
  if (fence_) fn<void (*)(VkDevice, VkFence, void*)>(vk.vkDestroyFence)(dev, fence_, nullptr);
  if (acquire_sem_) fn<void (*)(VkDevice, VkSemaphore, void*)>(vk.vkDestroySemaphore)(dev, acquire_sem_, nullptr);
  if (present_sem_) fn<void (*)(VkDevice, VkSemaphore, void*)>(vk.vkDestroySemaphore)(dev, present_sem_, nullptr);
  if (staging_) {
    fn<void (*)(VkDevice, VkBuffer, void*)>(vk.vkDestroyBuffer)(dev, staging_, nullptr);
    fn<void (*)(VkDevice, VkDeviceMemory, void*)>(vk.vkFreeMemory)(dev, staging_mem_, nullptr);
  }
  if (swapchain_) fn<void (*)(VkDevice, VkSwapchainKHR, void*)>(vk.vkDestroySwapchainKHR)(dev, swapchain_, nullptr);
  cmd_pool_ = 0; fence_ = 0; acquire_sem_ = 0; present_sem_ = 0;
  staging_ = 0; staging_mem_ = 0; staging_bytes_ = 0; swapchain_ = 0;
}

WindowPresenter::~WindowPresenter() {
  destroy_swapchain();
  if (vk_ && surface_ && vk_->vkDestroySurfaceKHR && vk_->instance_handle())
    fn<void (*)(VkInstance, VkSurfaceKHR, void*)>(vk_->vkDestroySurfaceKHR)(
        vk_->instance_handle(), surface_, nullptr);
  if (window_) SDL_DestroyWindow(static_cast<SDL_Window*>(window_));
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

}  // namespace pnr::graphics
