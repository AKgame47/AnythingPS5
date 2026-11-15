// PNR window presentation, GPL-2.0. Slice S3.
// SDL2 window + real Vulkan surface/swapchain on top of VulkanBackend.
// Frames are uploaded from host RGBA8 (staging buffer -> swapchain image):
// the offscreen renderers already produce host images, so this first window
// slice does a host roundtrip per frame. Rendering directly into the
// swapchain image is Planned, not hidden. Fixed-size window: an SDL resize
// does not rebuild the swapchain yet (vkQueuePresentKHR out-of-date then
// fails honestly). Built only with -DPNR_BUILD_WINDOW=ON (needs SDL2 dev
// files); headless CI hosts simply do not build it, same as pnr-ui.
// Status: Implemented, Tested under Xvfb (see pnr_window_smoke).
#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include "core/runtime.hpp"
#include "graphics/vulkan.hpp"

namespace pnr::graphics {

struct WindowConfig {
  uint32_t width = 640, height = 480;
  const char* title = "pnr";
  const char* app_name = "pnr-window";
};

class WindowPresenter {
public:
  WindowPresenter(const WindowPresenter&) = delete;
  WindowPresenter& operator=(const WindowPresenter&) = delete;
  // Waits for the queue, then destroys swapchain, surface, window.
  // Destroy BEFORE the VulkanBackend it was made from.
  ~WindowPresenter();

  // SDL window + Vulkan instance (with SDL's extensions) + present-capable
  // device + swapchain. Honest failure at the first missing stage.
  static Status create(VulkanBackend& vk, const WindowConfig& cfg,
                       std::unique_ptr<WindowPresenter>* out);

  // Upload one host RGBA8 frame (w*h*4 bytes, row-major, top row first) and
  // present it. Serial: acquires, copies, presents, and waits for the fence
  // each call (no frames in flight yet). BGRA swapchains get a real byte
  // swizzle during the upload. w/h must match the window size: a swapchain
  // resize is honest failure, never a silently stretched frame.
  Status present(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h);

  // Pump SDL events; false once the window was asked to close.
  bool pump();

  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  bool swapchain_bgra() const { return bgra_; }
  uint32_t presented_frames() const { return presented_; }

private:
  WindowPresenter() = default;
  Status create_swapchain();
  void destroy_swapchain();
  Status ensure_staging(uint64_t bytes);

  VulkanBackend* vk_ = nullptr;
  void* window_ = nullptr;  // SDL_Window*, kept opaque to this header
  VkSurfaceKHR surface_ = 0;
  VkSwapchainKHR swapchain_ = 0;
  std::vector<VkImage> images_;
  VkSemaphore acquire_sem_ = 0, present_sem_ = 0;
  VkFence fence_ = 0;
  VkCommandPool cmd_pool_ = 0;
  VkCommandBuffer cmd_ = 0;
  VkBuffer staging_ = 0;
  VkDeviceMemory staging_mem_ = 0;
  uint64_t staging_bytes_ = 0;
  uint32_t width_ = 0, height_ = 0;
  bool bgra_ = false;
  uint32_t presented_ = 0;
};

}  // namespace pnr::graphics
