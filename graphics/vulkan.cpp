#include "graphics/vulkan.hpp"

#include <cstring>
#include <dlfcn.h>

namespace pnr::graphics {

namespace {
Status fail(const char* stage, const std::string& what) {
  return Status::Fail(100, stage, what);
}
// Cast a resolved void* to the needed signature at each call site.
template <typename Sig>
Sig fn(void* p) { return reinterpret_cast<Sig>(p); }
}  // namespace

Status VulkanBackend::open() {
  if (lib_) return Status::Ok();
  lib_ = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!lib_) lib_ = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  if (!lib_)
    return fail("vulkan", "no Vulkan loader on this host (libvulkan not "
                          "found); graphics is Blocked, not faked");
  auto gp = reinterpret_cast<void* (*)(void*, const char*)>(
      dlsym(lib_, "vkGetInstanceProcAddr"));
  if (!gp) return fail("vulkan", "loader lacks vkGetInstanceProcAddr");
#define PNR_VK_RESOLVE(name) \
  name = gp(nullptr, #name); \
  if (!name && std::strcmp(#name, "vkCreateInstance") != 0) { \
    /* device/instance procs may need an instance; dlsym fallback */ \
    name = dlsym(lib_, #name); \
  } \
  if (!name) return fail("vulkan", std::string("missing entry point: ") + #name);
  PNR_VK_LIST(PNR_VK_RESOLVE)
#undef PNR_VK_RESOLVE
  return Status::Ok();
}

Status VulkanBackend::create_instance(const char* app_name) {
  if (!lib_) return fail("vulkan", "open() first");
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = app_name;
  app.pEngineName = "pnr";
  app.apiVersion = vk_api_version_1_0;
  VkInstanceCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ci.pApplicationInfo = &app;
  VkResult r = fn<VkResult (*)(const VkInstanceCreateInfo*, void*, VkInstance*)>(
      vkCreateInstance)(&ci, nullptr, &instance_);
  if (r != VK_SUCCESS)
    return fail("vulkan", "vkCreateInstance failed rc=" + std::to_string(r) +
                          " (no usable Vulkan driver/ICD)");
  uint32_t count = 0;
  fn<VkResult (*)(VkInstance, uint32_t*, VkPhysicalDevice*)>(
      vkEnumeratePhysicalDevices)(instance_, &count, nullptr);
  if (count == 0)
    return fail("vulkan", "no Vulkan physical devices (driver present, no "
                          "device); graphics is Blocked, recorded honestly");
  std::vector<VkPhysicalDevice> devs(count);
  fn<VkResult (*)(VkInstance, uint32_t*, VkPhysicalDevice*)>(
      vkEnumeratePhysicalDevices)(instance_, &count, devs.data());
  devices_.clear();
  for (auto d : devs) {
    VkPhysicalDeviceProperties p{};
    fn<void (*)(VkPhysicalDevice, VkPhysicalDeviceProperties*)>(
        vkGetPhysicalDeviceProperties)(d, &p);
    VulkanDeviceInfo info;
    info.name = p.deviceName;
    info.vendor_id = p.vendorID;
    info.api_version = p.apiVersion;
    info.type = p.deviceType;
    info.software = (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU);
    devices_.push_back(info);
  }
  // remember the raw handles in the same order for create_device
  phys_handles_.assign(devs.begin(), devs.end());
  return Status::Ok();
}

Status VulkanBackend::create_device(uint32_t physical_index) {
  if (physical_index >= phys_handles_.size())
    return fail("vulkan", "physical device index out of range");
  phys_ = phys_handles_[physical_index];
  uint32_t fam_count = 0;
  fn<void (*)(VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties*)>(
      vkGetPhysicalDeviceQueueFamilyProperties)(phys_, &fam_count, nullptr);
  std::vector<VkQueueFamilyProperties> fams(fam_count);
  fn<void (*)(VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties*)>(
      vkGetPhysicalDeviceQueueFamilyProperties)(phys_, &fam_count, fams.data());
  int family = -1;
  for (uint32_t i = 0; i < fam_count; ++i)
    if (fams[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = (int)i; break; }
  if (family < 0)
    return fail("vulkan", "no compute queue family on " + device_name());
  queue_family_ = (uint32_t)family;
  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{};
  qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qci.queueFamilyIndex = queue_family_;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci{};
  dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  VkResult r = fn<VkResult (*)(VkPhysicalDevice, const VkDeviceCreateInfo*,
                               void*, VkDevice*)>(
      vkCreateDevice)(phys_, &dci, nullptr, &device_);
  if (r != VK_SUCCESS)
    return fail("vulkan", "vkCreateDevice failed rc=" + std::to_string(r));
  fn<void (*)(VkDevice, uint32_t, uint32_t, VkQueue*)>(vkGetDeviceQueue)(
      device_, queue_family_, 0, &queue_);
  return Status::Ok();
}

std::string VulkanBackend::device_name() const {
  if (devices_.empty()) return "(none)";
  return devices_.front().name;
}

Status VulkanBackend::run_compute_words(const std::vector<uint32_t>& spirv,
                                        std::vector<uint32_t>* io) {
  if (!device_ || !io || io->empty())
    return fail("vulkan", "run_compute_words needs a device and a non-empty buffer");
  const VkDeviceSize bytes = io->size() * sizeof(uint32_t);

  VkBuffer buffer = 0;
  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = bytes;
  bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkResult r = fn<VkResult (*)(VkDevice, const VkBufferCreateInfo*, void*,
                               VkBuffer*)>(vkCreateBuffer)(device_, &bci,
                                                           nullptr, &buffer);
  if (r != VK_SUCCESS) return fail("vulkan", "vkCreateBuffer rc=" + std::to_string(r));

  VkMemoryRequirements req{};
  fn<void (*)(VkDevice, VkBuffer, VkMemoryRequirements*)>(
      vkGetBufferMemoryRequirements)(device_, buffer, &req);
  VkPhysicalDeviceMemoryProperties mem{};
  fn<void (*)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*)>(
      vkGetPhysicalDeviceMemoryProperties)(phys_, &mem);
  int type = -1;
  for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
    if ((req.memoryTypeBits & (1u << i)) &&
        (mem.memoryTypes[i].propertyFlags &
         (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
      type = (int)i;
      break;
    }
  if (type < 0) return fail("vulkan", "no host-visible+coherent memory type");

  VkDeviceMemory memory = 0;
  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = (uint32_t)type;
  r = fn<VkResult (*)(VkDevice, const VkMemoryAllocateInfo*, void*,
                      VkDeviceMemory*)>(vkAllocateMemory)(device_, &mai,
                                                          nullptr, &memory);
  if (r != VK_SUCCESS) return fail("vulkan", "vkAllocateMemory rc=" + std::to_string(r));
  fn<VkResult (*)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize)>(
      vkBindBufferMemory)(device_, buffer, memory, 0);

  void* mapped = nullptr;
  r = fn<VkResult (*)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize,
                      VkFlags, void**)>(vkMapMemory)(device_, memory, 0, bytes,
                                                     0, &mapped);
  if (r != VK_SUCCESS) return fail("vulkan", "vkMapMemory rc=" + std::to_string(r));
  std::memcpy(mapped, io->data(), bytes);
  fn<void (*)(VkDevice, VkDeviceMemory)>(vkUnmapMemory)(device_, memory);

  VkShaderModule shader = 0;
  VkShaderModuleCreateInfo sci{};
  sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  sci.codeSize = spirv.size() * sizeof(uint32_t);
  sci.pCode = spirv.data();
  r = fn<VkResult (*)(VkDevice, const VkShaderModuleCreateInfo*, void*,
                      VkShaderModule*)>(vkCreateShaderModule)(device_, &sci,
                                                              nullptr, &shader);
  if (r != VK_SUCCESS)
    return fail("vulkan", "vkCreateShaderModule rc=" + std::to_string(r) +
                          " (driver rejected the SPIR-V)");

  VkDescriptorSetLayoutBinding bind{};
  bind.binding = 0;
  bind.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bind.descriptorCount = 1;
  bind.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo lci{};
  lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  lci.bindingCount = 1;
  lci.pBindings = &bind;
  VkDescriptorSetLayout dlayout = 0;
  r = fn<VkResult (*)(VkDevice, const VkDescriptorSetLayoutCreateInfo*, void*,
                      VkDescriptorSetLayout*)>(vkCreateDescriptorSetLayout)(
      device_, &lci, nullptr, &dlayout);
  if (r != VK_SUCCESS) return fail("vulkan", "vkCreateDescriptorSetLayout");

  VkPipelineLayoutCreateInfo plci{};
  plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &dlayout;
  VkPipelineLayout playout = 0;
  r = fn<VkResult (*)(VkDevice, const VkPipelineLayoutCreateInfo*, void*,
                      VkPipelineLayout*)>(vkCreatePipelineLayout)(
      device_, &plci, nullptr, &playout);
  if (r != VK_SUCCESS) return fail("vulkan", "vkCreatePipelineLayout");

  VkDescriptorPoolSize psize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
  VkDescriptorPoolCreateInfo pci{};
  pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pci.maxSets = 1;
  pci.poolSizeCount = 1;
  pci.pPoolSizes = &psize;
  VkDescriptorPool pool = 0;
  r = fn<VkResult (*)(VkDevice, const VkDescriptorPoolCreateInfo*, void*,
                      VkDescriptorPool*)>(vkCreateDescriptorPool)(
      device_, &pci, nullptr, &pool);
  if (r != VK_SUCCESS) return fail("vulkan", "vkCreateDescriptorPool");

  VkDescriptorSetAllocateInfo dai{};
  dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  dai.descriptorPool = pool;
  dai.descriptorSetCount = 1;
  dai.pSetLayouts = &dlayout;
  VkDescriptorSet dset = 0;
  r = fn<VkResult (*)(VkDevice, const VkDescriptorSetAllocateInfo*,
                      VkDescriptorSet*)>(vkAllocateDescriptorSets)(device_,
                                                                   &dai, &dset);
  if (r != VK_SUCCESS) return fail("vulkan", "vkAllocateDescriptorSets");

  VkDescriptorBufferInfo dbi{buffer, 0, bytes};
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = dset;
  write.dstBinding = 0;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write.pBufferInfo = &dbi;
  fn<void (*)(VkDevice, uint32_t, const VkWriteDescriptorSet*, uint32_t,
              void*)>(vkUpdateDescriptorSets)(device_, 1, &write, 0, nullptr);

  VkPipelineShaderStageCreateInfo stage{};
  stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stage.module = shader;
  stage.pName = "main";
  VkComputePipelineCreateInfo cpci{};
  cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  cpci.stage = stage;
  cpci.layout = playout;
  VkPipeline pipeline = 0;
  r = fn<VkResult (*)(VkDevice, uint64_t, uint32_t,
                      const VkComputePipelineCreateInfo*, void*, VkPipeline*)>(
      vkCreateComputePipelines)(device_, 0, 1, &cpci, nullptr, &pipeline);
  if (r != VK_SUCCESS)
    return fail("vulkan", "vkCreateComputePipelines rc=" + std::to_string(r));

  VkCommandPoolCreateInfo kpci{};
  kpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  kpci.queueFamilyIndex = queue_family_;
  VkCommandPool cmdpool = 0;
  r = fn<VkResult (*)(VkDevice, const VkCommandPoolCreateInfo*, void*,
                      VkCommandPool*)>(vkCreateCommandPool)(device_, &kpci,
                                                            nullptr, &cmdpool);
  if (r != VK_SUCCESS) return fail("vulkan", "vkCreateCommandPool");

  VkCommandBufferAllocateInfo cbai{};
  cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbai.commandPool = cmdpool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VkCommandBuffer cmd = 0;
  r = fn<VkResult (*)(VkDevice, const VkCommandBufferAllocateInfo*,
                      VkCommandBuffer*)>(vkAllocateCommandBuffers)(
      device_, &cbai, &cmd);
  if (r != VK_SUCCESS) return fail("vulkan", "vkAllocateCommandBuffers");

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  fn<VkResult (*)(VkCommandBuffer, const VkCommandBufferBeginInfo*)>(
      vkBeginCommandBuffer)(cmd, &begin);
  fn<void (*)(VkCommandBuffer, int32_t, VkPipeline)>(vkCmdBindPipeline)(
      cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  fn<void (*)(VkCommandBuffer, int32_t, VkPipelineLayout, uint32_t, uint32_t,
              const VkDescriptorSet*, uint32_t, const uint32_t*)>(
      vkCmdBindDescriptorSets)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, playout,
                               0, 1, &dset, 0, nullptr);
  fn<void (*)(VkCommandBuffer, uint32_t, uint32_t, uint32_t)>(vkCmdDispatch)(
      cmd, 1, 1, 1);
  fn<VkResult (*)(VkCommandBuffer)>(vkEndCommandBuffer)(cmd);

  VkFenceCreateInfo fci{};
  fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = 0;
  r = fn<VkResult (*)(VkDevice, const VkFenceCreateInfo*, void*, VkFence*)>(
      vkCreateFence)(device_, &fci, nullptr, &fence);
  if (r != VK_SUCCESS) return fail("vulkan", "vkCreateFence");

  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  r = fn<VkResult (*)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence)>(
      vkQueueSubmit)(queue_, 1, &submit, fence);
  if (r != VK_SUCCESS) return fail("vulkan", "vkQueueSubmit rc=" + std::to_string(r));
  r = fn<VkResult (*)(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t)>(
      vkWaitForFences)(device_, 1, &fence, 1, 30000000000ull);
  if (r != VK_SUCCESS) return fail("vulkan", "vkWaitForFences rc=" + std::to_string(r));

  r = fn<VkResult (*)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize,
                      VkFlags, void**)>(vkMapMemory)(device_, memory, 0, bytes,
                                                     0, &mapped);
  if (r != VK_SUCCESS) return fail("vulkan", "readback vkMapMemory failed");
  std::memcpy(io->data(), mapped, bytes);
  fn<void (*)(VkDevice, VkDeviceMemory)>(vkUnmapMemory)(device_, memory);

  fn<void (*)(VkDevice, VkFence, void*)>(vkDestroyFence)(device_, fence, nullptr);
  fn<void (*)(VkDevice, VkCommandPool, void*)>(vkDestroyCommandPool)(device_, cmdpool, nullptr);
  fn<void (*)(VkDevice, VkPipeline, void*)>(vkDestroyPipeline)(device_, pipeline, nullptr);
  fn<void (*)(VkDevice, VkDescriptorPool, void*)>(vkDestroyDescriptorPool)(device_, pool, nullptr);
  fn<void (*)(VkDevice, VkPipelineLayout, void*)>(vkDestroyPipelineLayout)(device_, playout, nullptr);
  fn<void (*)(VkDevice, VkDescriptorSetLayout, void*)>(vkDestroyDescriptorSetLayout)(device_, dlayout, nullptr);
  fn<void (*)(VkDevice, VkShaderModule, void*)>(vkDestroyShaderModule)(device_, shader, nullptr);
  fn<void (*)(VkDevice, VkBuffer, void*)>(vkDestroyBuffer)(device_, buffer, nullptr);
  fn<void (*)(VkDevice, VkDeviceMemory, void*)>(vkFreeMemory)(device_, memory, nullptr);
  return Status::Ok();
}

void VulkanBackend::shutdown() {
  if (device_) {
    fn<void (*)(VkDevice, void*)>(vkDestroyDevice)(device_, nullptr);
    device_ = 0;
  }
  if (instance_) {
    fn<void (*)(VkInstance, void*)>(vkDestroyInstance)(instance_, nullptr);
    instance_ = 0;
  }
  devices_.clear();
  phys_handles_.clear();
}

VulkanBackend::~VulkanBackend() {
  shutdown();
  if (lib_) dlclose(lib_);
}

}  // namespace pnr::graphics
