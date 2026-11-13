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
typedef uint64_t VkImage;
typedef uint64_t VkImageView;
typedef uint64_t VkRenderPass;
typedef uint64_t VkFramebuffer;

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
  VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO = 14,
  VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO = 15,
  VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO = 16,
  VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO = 19,
  VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO = 20,
  VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO = 22,
  VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO = 23,
  VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO = 24,
  VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO = 25,
  VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO = 26,
  VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO = 28,
  VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO = 37,
  VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO = 38,
  VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO = 43,
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
  VK_QUEUE_GRAPHICS_BIT = 0x1,
  VK_BUFFER_USAGE_INDEX_BUFFER_BIT = 0x40,
  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT = 0x20,
  VK_IMAGE_ASPECT_DEPTH_BIT = 0x2,
  VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT = 0x200,
  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT = 0x80,
  VK_QUEUE_COMPUTE_BIT = 0x2,
  VK_IMAGE_USAGE_TRANSFER_SRC_BIT = 0x1,
  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT = 0x10,
  VK_BUFFER_USAGE_TRANSFER_DST_BIT = 0x2,
  VK_SHADER_STAGE_VERTEX_BIT = 0x1,
  VK_SHADER_STAGE_FRAGMENT_BIT = 0x10,
  VK_IMAGE_ASPECT_COLOR_BIT = 0x1,
  VK_COLOR_COMPONENT_RGBA = 0xF,
  VK_SAMPLE_COUNT_1_BIT = 0x1,
  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT = 0x1,
  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT = 0x400,
  VK_PIPELINE_STAGE_TRANSFER_BIT = 0x1000,
  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT = 0x100,
  VK_ACCESS_TRANSFER_READ_BIT = 0x800,
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
  VK_PIPELINE_BIND_POINT_GRAPHICS = 0,
  VK_FORMAT_D32_SFLOAT = 126,
  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL = 3,
  VK_COMPARE_OP_LESS = 1,
  VK_INDEX_TYPE_UINT32 = 1,
  VK_FORMAT_R32G32_SFLOAT = 103,
  VK_FORMAT_R32G32B32_SFLOAT = 106,
  VK_VERTEX_INPUT_RATE_VERTEX = 0,
  VK_FORMAT_R8G8B8A8_UNORM = 37,
  VK_IMAGE_TYPE_2D = 1,
  VK_IMAGE_TILING_OPTIMAL = 0,
  VK_IMAGE_LAYOUT_UNDEFINED = 0,
  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL = 2,
  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL = 6,
  VK_IMAGE_VIEW_TYPE_2D = 1,
  VK_ATTACHMENT_LOAD_OP_CLEAR = 1,
  VK_ATTACHMENT_LOAD_OP_DONT_CARE = 2,
  VK_ATTACHMENT_STORE_OP_STORE = 0,
  VK_ATTACHMENT_STORE_OP_DONT_CARE = 1,
  VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST = 3,
  VK_POLYGON_MODE_FILL = 0,
  VK_CULL_MODE_NONE = 0,
  VK_FRONT_FACE_COUNTER_CLOCKWISE = 0,
  VK_SUBPASS_CONTENTS_INLINE = 0,
};
inline constexpr uint32_t VK_SUBPASS_EXTERNAL = ~0u;
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
typedef struct VkVertexInputBindingDescription { uint32_t binding, stride; int32_t inputRate; } VkVertexInputBindingDescription;
typedef struct VkVertexInputAttributeDescription { uint32_t location, binding; int32_t format; uint32_t offset; } VkVertexInputAttributeDescription;
typedef struct VkFormatProperties { VkFlags linearTilingFeatures, optimalTilingFeatures, bufferFeatures; } VkFormatProperties;
typedef struct VkStencilOpState {
  int32_t failOp, passOp, depthFailOp, compareOp; uint32_t compareMask, writeMask, reference;
} VkStencilOpState;
typedef struct VkPipelineDepthStencilStateCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; VkBool32 depthTestEnable;
  VkBool32 depthWriteEnable; int32_t depthCompareOp; VkBool32 depthBoundsTestEnable;
  VkBool32 stencilTestEnable; VkStencilOpState front, back;
  float minDepthBounds, maxDepthBounds;
} VkPipelineDepthStencilStateCreateInfo;
typedef struct VkOffset2D { int32_t x, y; } VkOffset2D;
typedef struct VkOffset3D { int32_t x, y, z; } VkOffset3D;
typedef struct VkExtent2D { uint32_t width, height; } VkExtent2D;
typedef struct VkRect2D { VkOffset2D offset; VkExtent2D extent; } VkRect2D;
typedef struct VkViewport { float x, y, width, height, minDepth, maxDepth; } VkViewport;
typedef struct VkImageCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; int32_t imageType;
  int32_t format; VkExtent3D extent; uint32_t mipLevels; uint32_t arrayLayers;
  VkFlags samples; int32_t tiling; VkFlags usage; int32_t sharingMode;
  uint32_t queueFamilyIndexCount; const uint32_t* pQueueFamilyIndices;
  int32_t initialLayout;
} VkImageCreateInfo;
typedef struct VkComponentMapping { int32_t r, g, b, a; } VkComponentMapping;
typedef struct VkImageSubresourceRange {
  VkFlags aspectMask; uint32_t baseMipLevel, levelCount, baseArrayLayer, layerCount;
} VkImageSubresourceRange;
typedef struct VkImageViewCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; VkImage image;
  int32_t viewType; int32_t format; VkComponentMapping components;
  VkImageSubresourceRange subresourceRange;
} VkImageViewCreateInfo;
typedef struct VkAttachmentDescription {
  VkFlags flags; int32_t format; VkFlags samples; int32_t loadOp; int32_t storeOp;
  int32_t stencilLoadOp; int32_t stencilStoreOp; int32_t initialLayout;
  int32_t finalLayout;
} VkAttachmentDescription;
typedef struct VkAttachmentReference { uint32_t attachment; int32_t layout; } VkAttachmentReference;
typedef struct VkSubpassDescription {
  VkFlags flags; int32_t pipelineBindPoint; uint32_t inputAttachmentCount;
  const VkAttachmentReference* pInputAttachments; uint32_t colorAttachmentCount;
  const VkAttachmentReference* pColorAttachments;
  const VkAttachmentReference* pResolveAttachments;
  const VkAttachmentReference* pDepthStencilAttachment;
  uint32_t preserveAttachmentCount; const uint32_t* pPreserveAttachments;
} VkSubpassDescription;
typedef struct VkSubpassDependency {
  uint32_t srcSubpass, dstSubpass; VkFlags srcStageMask, dstStageMask;
  VkFlags srcAccessMask, dstAccessMask, dependencyFlags;
} VkSubpassDependency;
typedef struct VkRenderPassCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags;
  uint32_t attachmentCount; const VkAttachmentDescription* pAttachments;
  uint32_t subpassCount; const VkSubpassDescription* pSubpasses;
  uint32_t dependencyCount; const VkSubpassDependency* pDependencies;
} VkRenderPassCreateInfo;
typedef struct VkFramebufferCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; VkRenderPass renderPass;
  uint32_t attachmentCount; const VkImageView* pAttachments;
  uint32_t width, height, layers;
} VkFramebufferCreateInfo;
typedef struct VkPipelineVertexInputStateCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags;
  uint32_t vertexBindingDescriptionCount; const VkVertexInputBindingDescription* pVertexBindingDescriptions;
  uint32_t vertexAttributeDescriptionCount; const VkVertexInputAttributeDescription* pVertexAttributeDescriptions;
} VkPipelineVertexInputStateCreateInfo;
typedef struct VkPipelineInputAssemblyStateCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; int32_t topology;
  VkBool32 primitiveRestartEnable;
} VkPipelineInputAssemblyStateCreateInfo;
typedef struct VkPipelineViewportStateCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; uint32_t viewportCount;
  const VkViewport* pViewports; uint32_t scissorCount; const VkRect2D* pScissors;
} VkPipelineViewportStateCreateInfo;
typedef struct VkPipelineRasterizationStateCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags;
  VkBool32 depthClampEnable; VkBool32 rasterizerDiscardEnable; int32_t polygonMode;
  VkFlags cullMode; int32_t frontFace; VkBool32 depthBiasEnable;
  float depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor, lineWidth;
} VkPipelineRasterizationStateCreateInfo;
typedef struct VkPipelineMultisampleStateCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; VkFlags rasterizationSamples;
  VkBool32 sampleShadingEnable; float minSampleShading; const uint32_t* pSampleMask;
  VkBool32 alphaToCoverageEnable; VkBool32 alphaToOneEnable;
} VkPipelineMultisampleStateCreateInfo;
typedef struct VkPipelineColorBlendAttachmentState {
  VkBool32 blendEnable; int32_t srcColorBlendFactor, dstColorBlendFactor, colorBlendOp,
      srcAlphaBlendFactor, dstAlphaBlendFactor, alphaBlendOp;
  VkFlags colorWriteMask;
} VkPipelineColorBlendAttachmentState;
typedef struct VkPipelineColorBlendStateCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; VkBool32 logicOpEnable;
  int32_t logicOp; uint32_t attachmentCount;
  const VkPipelineColorBlendAttachmentState* pAttachments; float blendConstants[4];
} VkPipelineColorBlendStateCreateInfo;
typedef struct VkGraphicsPipelineCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; uint32_t stageCount;
  const VkPipelineShaderStageCreateInfo* pStages;
  const VkPipelineVertexInputStateCreateInfo* pVertexInputState;
  const VkPipelineInputAssemblyStateCreateInfo* pInputAssemblyState;
  const void* pTessellationState; const VkPipelineViewportStateCreateInfo* pViewportState;
  const VkPipelineRasterizationStateCreateInfo* pRasterizationState;
  const VkPipelineMultisampleStateCreateInfo* pMultisampleState;
  const void* pDepthStencilState; const VkPipelineColorBlendStateCreateInfo* pColorBlendState;
  const void* pDynamicState; VkPipelineLayout layout; VkRenderPass renderPass;
  uint32_t subpass; VkPipeline basePipelineHandle; int32_t basePipelineIndex;
} VkGraphicsPipelineCreateInfo;
typedef union VkClearColorValue { float float32[4]; int32_t int32[4]; uint32_t uint32[4]; } VkClearColorValue;
typedef struct VkClearDepthStencilValue { float depth; uint32_t stencil; } VkClearDepthStencilValue;
typedef union VkClearValue { VkClearColorValue color; VkClearDepthStencilValue depthStencil; } VkClearValue;
typedef struct VkRenderPassBeginInfo {
  VkStructureType sType; const void* pNext; VkRenderPass renderPass;
  VkFramebuffer framebuffer; VkRect2D renderArea; uint32_t clearValueCount;
  const VkClearValue* pClearValues;
} VkRenderPassBeginInfo;
typedef struct VkImageSubresourceLayers {
  VkFlags aspectMask; uint32_t mipLevel, baseArrayLayer, layerCount;
} VkImageSubresourceLayers;
typedef struct VkBufferImageCopy {
  VkDeviceSize bufferOffset; uint32_t bufferRowLength, bufferImageHeight;
  VkImageSubresourceLayers imageSubresource; VkOffset3D imageOffset;
  VkExtent3D imageExtent;
} VkBufferImageCopy;

}  // extern "C"
#endif  // PNR_USE_SYSTEM_VULKAN
#endif  // PNR_GRAPHICS_VULKAN_SHIM_H
