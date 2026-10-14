// Shared state of the layer: per-instance and per-device data, and lookup helpers.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace exynos {

void logf(const char* fmt, ...);

struct InstanceData {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkCreateDevice CreateDevice = nullptr;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures GetPhysicalDeviceFeatures = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2 = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties2 GetPhysicalDeviceFormatProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties GetPhysicalDeviceImageFormatProperties = nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties2 GetPhysicalDeviceImageFormatProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
};

// Device-level entry points the layer calls (next layer / driver). Loaded with the next
// vkGetDeviceProcAddr when the device is created; optional ones may be null.
#define EXYNOS_DEVICE_FNS(X)                                                                       \
    X(DestroyDevice) X(CreateImage) X(DestroyImage) X(CreateImageView) X(DestroyImageView)          \
    X(CreateBuffer) X(DestroyBuffer) X(AllocateCommandBuffers) X(FreeCommandBuffers)                \
    X(ResetCommandBuffer) X(BeginCommandBuffer) X(DestroyCommandPool) X(ResetCommandPool)           \
    X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdCopyBufferToImage)         \
    X(CmdCopyBufferToImage2) X(CmdPipelineBarrier) X(CmdDispatch) X(CmdPushDescriptorSetKHR)        \
    X(CmdFillBuffer) X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout)      \
    X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout)                  \
    X(CreateComputePipelines) X(DestroyPipeline) X(AllocateMemory) X(FreeMemory)                    \
    X(BindBufferMemory) X(GetBufferMemoryRequirements)

// An image created by the layer in place of a BC image.
struct EmuImage {
    VkFormat bc_format = VK_FORMAT_UNDEFINED;
    VkFormat storage_format = VK_FORMAT_UNDEFINED;  // format of the backing image
    VkFormat sample_format = VK_FORMAT_UNDEFINED;   // format sampled views use
    uint32_t mip_levels = 1;
    uint32_t array_layers = 1;
    VkExtent3D extent{};
    std::unordered_map<uint64_t, VkImageView> storage_views;  // key: mip | baseLayer | layerCount
};

// Shadow of the compute-bind-point state the layer must restore after its own dispatches.
struct CbState {
    VkPipeline compute_pipeline = VK_NULL_HANDLE;
    // vkCmdBindDescriptorSets calls on the compute bind point, in order. Earlier calls whose set
    // range is fully covered by a later one are dropped, so the list stays short. Replaying it
    // reproduces the bound sets and their dynamic offsets.
    struct BindCall {
        VkPipelineLayout layout = VK_NULL_HANDLE;
        uint32_t first = 0;
        std::vector<VkDescriptorSet> sets;
        std::vector<uint32_t> dynamic_offsets;
    };
    std::vector<BindCall> binds;
    struct Push {
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkShaderStageFlags stages = 0;
        uint32_t lo = UINT32_MAX, hi = 0;
        uint8_t bytes[256] = {};
    } push;
};

struct DecodeResources {
    bool ready = false;
    bool failed = false;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule modules[8] = {};
    VkPipeline pipelines[8] = {};
    VkBuffer scratch = VK_NULL_HANDLE;  // 16 bytes, target of the trailing transfer-stage fill
    VkDeviceMemory scratch_mem = VK_NULL_HANDLE;
};

struct DeviceData {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    InstanceData* instance = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;

#define X(name) PFN_vk##name name = nullptr;
    EXYNOS_DEVICE_FNS(X)
#undef X

    // BC emulation
    bool emulation_active = false;
    uint32_t bc_emulated_mask = 0;  // bit (format - VK_FORMAT_BC4_UNORM_BLOCK)
    VkDeviceSize min_storage_alignment = 4;
    VkDeviceSize max_storage_range = 0xffffffffu;
    VkPhysicalDeviceMemoryProperties memory_properties{};

    std::mutex mutex;
    std::unordered_map<uint64_t, std::unique_ptr<EmuImage>> images;
    std::unordered_map<uint64_t, VkDeviceSize> buffer_sizes;
    std::unordered_map<VkCommandBuffer, CbState> cmdbufs;
    std::unordered_map<uint64_t, std::vector<VkCommandBuffer>> pool_cmdbufs;
    DecodeResources decode;
};

// Lookup by any dispatchable handle of the instance / device (uses the loader dispatch key).
InstanceData* find_instance(const void* handle);
DeviceData* find_device(const void* handle);

}  // namespace exynos
