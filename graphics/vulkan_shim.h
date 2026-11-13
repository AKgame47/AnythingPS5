// PNR minimal Vulkan declarations - same pattern as library/sqlite3_shim.h.
// The repo builds without the Vulkan SDK: this header declares only what
// PNR's graphics module uses, with layouts and constants taken verbatim
// from the official Vulkan-Headers (Apache-2.0 OR MIT, KhronosGroup).
// On a machine with the SDK installed, define PNR_USE_SYSTEM_VULKAN and
// include <vulkan/vulkan.h> instead - tests are run BOTH ways to prove
// the shim is ABI-compatible (see tests/test_main.cpp "vk:" checks).
#ifndef PNR_GRAPHICS_VULKAN_SHIM_H
#define PNR_GRAPHICS_VULKAN_SHIM_H

#ifdef PNR_USE_SYSTEM_VULKAN
#include <vulkan/vulkan.h>
// Alias so PNR code uses one name under both headers.
inline constexpr uint32_t vk_api_version_1_0 = VK_API_VERSION_1_0;
#else
#include <cstddef>
#include <cstdint>

extern "C" {

using VkFlags = uint32_t;
using VkBool32 = uint32_t;
using VkDeviceSize = uint64_t;
using VkResult = int32_t;
using VkStructureType = int32_t;

// Dispatchable handles are opaque pointers; non-dispatchable are uint64
// on 64-bit hosts (x86-64 only project, so no 32-bit path exists).
typedef struct VkInstance_T* VkInstance;
typedef struct VkPhysicalDevice_T* VkPhysicalDevice;
typedef struct VkDevice_T* VkDevice;
typedef struct VkQueue_T* VkQueue;
typedef struct VkCommandBuffer_T* VkCommandBuffer;
typedef uint64_t VkDeviceMemory;
typedef uint64_t VkBuffer;
typedef uint64_t VkShaderModule;
typedef uint64_t VkDescriptorSetLayout;
typedef uint64_t VkDescriptorPool;
typedef uint64_t VkDescriptorSet;
typedef uint64_t VkPipeline;
typedef uint64_t VkPipelineLayout;
typedef uint64_t VkCommandPool;
typedef uint64_t VkFence;
typedef uint64_t VkSemaphore;

enum : VkResult { VK_SUCCESS = 0 };
enum : VkStructureType {
  VK_STRUCTURE_TYPE_APPLICATION_INFO = 0,
  VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1,
  VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2,
  VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3,
  VK_STRUCTURE_TYPE_SUBMIT_INFO = 4,
  VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO = 5,
  VK_STRUCTURE_TYPE_FENCE_CREATE_INFO = 8,
  VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO = 12,
  VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO = 16,
  VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO = 18,
  VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO = 29,
  VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO = 30,
  VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO = 32,
  VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO = 33,
  VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO = 34,
  VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET = 35,
  VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO = 39,
  VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO = 40,
  VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO = 42,
};
enum : VkFlags {
  VK_QUEUE_COMPUTE_BIT = 0x2,
  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT = 0x20,
  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 0x2,
  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 0x4,
  VK_SHADER_STAGE_COMPUTE_BIT = 0x20,
  VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT = 0x1,
  VK_FENCE_CREATE_SIGNALED_BIT = 0x1,
};
enum : int32_t {
  VK_PHYSICAL_DEVICE_TYPE_CPU = 4,
  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER = 7,
  VK_PIPELINE_BIND_POINT_COMPUTE = 1,
  VK_COMMAND_BUFFER_LEVEL_PRIMARY = 0,
  VK_SHARING_MODE_EXCLUSIVE = 0,
};
inline constexpr uint32_t vk_api_version_1_0 = (1u << 22);

typedef struct VkApplicationInfo {
  VkStructureType sType; const void* pNext; const char* pApplicationName;
  uint32_t applicationVersion; const char* pEngineName;
  uint32_t engineVersion; uint32_t apiVersion;
} VkApplicationInfo;
typedef struct VkInstanceCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags;
  const VkApplicationInfo* pApplicationInfo; uint32_t enabledLayerCount;
  const char* const* ppEnabledLayerNames; uint32_t enabledExtensionCount;
  const char* const* ppEnabledExtensionNames;
} VkInstanceCreateInfo;
typedef struct VkExtent3D { uint32_t width, height, depth; } VkExtent3D;
typedef struct VkQueueFamilyProperties {
  VkFlags queueFlags; uint32_t queueCount; uint32_t timestampValidBits;
  VkExtent3D minImageTransferGranularity;
} VkQueueFamilyProperties;
// Only the head of VkPhysicalDeviceProperties is read; the tail (limits,
// sparseProperties) is reserved space so the driver can write the full
// official struct without overrunning this one.
typedef struct VkPhysicalDeviceProperties {
  uint32_t apiVersion; uint32_t driverVersion; uint32_t vendorID;
  uint32_t deviceID; int32_t deviceType; char deviceName[256];
  uint8_t pipelineCacheUUID[16]; uint8_t reserved[2048];
} VkPhysicalDeviceProperties;
typedef struct VkDeviceQueueCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags;
  uint32_t queueFamilyIndex; uint32_t queueCount;
  const float* pQueuePriorities;
} VkDeviceQueueCreateInfo;
typedef struct VkDeviceCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags;
  uint32_t queueCreateInfoCount; const VkDeviceQueueCreateInfo* pQueueCreateInfos;
  uint32_t enabledLayerCount; const char* const* ppEnabledLayerNames;
  uint32_t enabledExtensionCount; const char* const* ppEnabledExtensionNames;
  const void* pEnabledFeatures;
} VkDeviceCreateInfo;
typedef struct VkMemoryType { VkFlags propertyFlags; uint32_t heapIndex; } VkMemoryType;
typedef struct VkMemoryHeap { VkDeviceSize size; VkFlags flags; } VkMemoryHeap;
typedef struct VkPhysicalDeviceMemoryProperties {
  uint32_t memoryTypeCount; VkMemoryType memoryTypes[32];
  uint32_t memoryHeapCount; VkMemoryHeap memoryHeaps[16];
} VkPhysicalDeviceMemoryProperties;
typedef struct VkBufferCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; VkDeviceSize size;
  VkFlags usage; int32_t sharingMode; uint32_t queueFamilyIndexCount;
  const uint32_t* pQueueFamilyIndices;
} VkBufferCreateInfo;
typedef struct VkMemoryRequirements {
  VkDeviceSize size; VkDeviceSize alignment; uint32_t memoryTypeBits;
} VkMemoryRequirements;
typedef struct VkMemoryAllocateInfo {
  VkStructureType sType; const void* pNext; VkDeviceSize allocationSize;
  uint32_t memoryTypeIndex;
} VkMemoryAllocateInfo;
typedef struct VkShaderModuleCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; size_t codeSize;
  const uint32_t* pCode;
} VkShaderModuleCreateInfo;
typedef struct VkDescriptorSetLayoutBinding {
  uint32_t binding; int32_t descriptorType; uint32_t descriptorCount;
  VkFlags stageFlags; const void* pImmutableSamplers;
} VkDescriptorSetLayoutBinding;
typedef struct VkDescriptorSetLayoutCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags;
  uint32_t bindingCount; const VkDescriptorSetLayoutBinding* pBindings;
} VkDescriptorSetLayoutCreateInfo;
typedef struct VkPushConstantRange {
  VkFlags stageFlags; uint32_t offset; uint32_t size;
} VkPushConstantRange;
typedef struct VkPipelineLayoutCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags;
  uint32_t setLayoutCount; const VkDescriptorSetLayout* pSetLayouts;
  uint32_t pushConstantRangeCount; const void* pPushConstantRanges;
} VkPipelineLayoutCreateInfo;
typedef struct VkDescriptorPoolSize { int32_t type; uint32_t descriptorCount; } VkDescriptorPoolSize;
typedef struct VkDescriptorPoolCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; uint32_t maxSets;
  uint32_t poolSizeCount; const VkDescriptorPoolSize* pPoolSizes;
} VkDescriptorPoolCreateInfo;
typedef struct VkDescriptorSetAllocateInfo {
  VkStructureType sType; const void* pNext; VkDescriptorPool descriptorPool;
  uint32_t descriptorSetCount; const VkDescriptorSetLayout* pSetLayouts;
} VkDescriptorSetAllocateInfo;
typedef struct VkDescriptorBufferInfo {
  VkBuffer buffer; VkDeviceSize offset; VkDeviceSize range;
} VkDescriptorBufferInfo;
typedef struct VkWriteDescriptorSet {
  VkStructureType sType; const void* pNext; VkDescriptorSet dstSet;
  uint32_t dstBinding; uint32_t dstArrayElement; uint32_t descriptorCount;
  int32_t descriptorType; const void* pImageInfo;
  const VkDescriptorBufferInfo* pBufferInfo; const void* pTexelBufferView;
} VkWriteDescriptorSet;
typedef struct VkPipelineShaderStageCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; int32_t stage;
  VkShaderModule module; const char* pName; const void* pSpecializationInfo;
} VkPipelineShaderStageCreateInfo;
typedef struct VkComputePipelineCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags;
  VkPipelineShaderStageCreateInfo stage; VkPipelineLayout layout;
  VkPipeline basePipelineHandle; int32_t basePipelineIndex;
} VkComputePipelineCreateInfo;
typedef struct VkCommandPoolCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags;
  uint32_t queueFamilyIndex;
} VkCommandPoolCreateInfo;
typedef struct VkCommandBufferAllocateInfo {
  VkStructureType sType; const void* pNext; VkCommandPool commandPool;
  int32_t level; uint32_t commandBufferCount;
} VkCommandBufferAllocateInfo;
typedef struct VkCommandBufferBeginInfo {
  VkStructureType sType; const void* pNext; VkFlags flags;
  const void* pInheritanceInfo;
} VkCommandBufferBeginInfo;
typedef struct VkFenceCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags;
} VkFenceCreateInfo;
typedef struct VkSubmitInfo {
  VkStructureType sType; const void* pNext; uint32_t waitSemaphoreCount;
  const VkSemaphore* pWaitSemaphores; const VkFlags* pWaitDstStageMask;
  uint32_t commandBufferCount; const VkCommandBuffer* pCommandBuffers;
  uint32_t signalSemaphoreCount; const VkSemaphore* pSignalSemaphores;
} VkSubmitInfo;

}  // extern "C"
#endif  // PNR_USE_SYSTEM_VULKAN
#endif  // PNR_GRAPHICS_VULKAN_SHIM_H
