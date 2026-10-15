// exynos-tools: Vulkan layer.
//
// Written from the Vulkan specification and the Khronos loader/layer interface documentation
// (loader interface version 2). Forwards everything to the next layer/driver; with
// EXYNOS_TOOLS_BC set it also reports and emulates the BC formats Xclipse lacks (bc_caps.h,
// bc_image.cpp, docs/BCN_DESIGN.md).

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

#include "bc_caps.h"
#include "bc_image.h"
#include "layer_state.h"

#define EXPORT extern "C" __attribute__((visibility("default")))

namespace exynos {

namespace {

constexpr const char* kLayerName = "VK_LAYER_EXYNOS_TOOLS_xclipse";
constexpr const char* kLayerDesc = "exynos-tools: Xclipse compatibility layer";

bool log_enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("EXYNOS_TOOLS_LOG");
        return !(v && v[0] == '0');
    }();
    return enabled;
}

// ---- dispatch bookkeeping --------------------------------------------------

// The loader stores its dispatch table pointer in the first word of every dispatchable handle;
// handles of one instance (instance + its physical devices) share it, as do a device, its queues
// and command buffers.
void* dispatch_key(const void* handle) { return *static_cast<void* const*>(handle); }

std::mutex g_mutex;
std::unordered_map<void*, std::unique_ptr<InstanceData>> g_instances;
std::unordered_map<void*, std::unique_ptr<DeviceData>> g_devices;

}  // namespace

void logf(const char* fmt, ...) {
    if (!log_enabled()) return;
    va_list ap;
#ifdef __ANDROID__
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, "exynos-tools", fmt, ap);
    va_end(ap);
#endif
    // stderr as well, so the log is visible in a terminal (Termux) without logcat.
    va_start(ap, fmt);
    std::fprintf(stderr, "[exynos-tools] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
    // EXYNOS_TOOLS_LOG_FILE=<path>: also append to a file, e.g. in shared storage where another app
    // (Termux) can read it when the game runs inside a launcher's sandbox.
    static std::FILE* file = [] {
        const char* path = std::getenv("EXYNOS_TOOLS_LOG_FILE");
        return path && path[0] ? std::fopen(path, "a") : nullptr;
    }();
    if (file) {
        va_start(ap, fmt);
        std::fprintf(file, "[exynos-tools pid %d] ", static_cast<int>(getpid()));
        std::vfprintf(file, fmt, ap);
        std::fprintf(file, "\n");
        std::fflush(file);
        va_end(ap);
    }
}

InstanceData* find_instance(const void* handle) {
    if (!handle) return nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_instances.find(dispatch_key(handle));
    return it == g_instances.end() ? nullptr : it->second.get();
}

DeviceData* find_device(const void* handle) {
    if (!handle) return nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(dispatch_key(handle));
    return it == g_devices.end() ? nullptr : it->second.get();
}

namespace {

bool is_bc_format(VkFormat f) { return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK; }
bool is_emulatable_bc(VkFormat f) { return f >= VK_FORMAT_BC4_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK; }

// ---- intercepted: instance level -------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL layer_CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                    const VkAllocationCallbacks* pAllocator,
                                                    VkInstance* pInstance) {
    auto* link = const_cast<VkLayerInstanceCreateInfo*>(
        static_cast<const VkLayerInstanceCreateInfo*>(pCreateInfo->pNext));
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                     link->function == VK_LAYER_LINK_INFO)) {
        link = const_cast<VkLayerInstanceCreateInfo*>(static_cast<const VkLayerInstanceCreateInfo*>(link->pNext));
    }
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    if (!gipa) return VK_ERROR_INITIALIZATION_FAILED;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;  // advance the chain for the next layer

    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult res = create(pCreateInfo, pAllocator, pInstance);
    if (res != VK_SUCCESS) return res;

    auto d = std::make_unique<InstanceData>();
    d->instance = *pInstance;
    d->gipa = gipa;
    auto load = [&](const char* name) { return gipa(*pInstance, name); };
#define LOAD(field, pfn, name) d->field = reinterpret_cast<pfn>(load(name))
    LOAD(DestroyInstance, PFN_vkDestroyInstance, "vkDestroyInstance");
    LOAD(CreateDevice, PFN_vkCreateDevice, "vkCreateDevice");
    LOAD(GetPhysicalDeviceProperties, PFN_vkGetPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties");
    LOAD(GetPhysicalDeviceFeatures, PFN_vkGetPhysicalDeviceFeatures, "vkGetPhysicalDeviceFeatures");
    LOAD(GetPhysicalDeviceFeatures2, PFN_vkGetPhysicalDeviceFeatures2, "vkGetPhysicalDeviceFeatures2");
    if (!d->GetPhysicalDeviceFeatures2)
        LOAD(GetPhysicalDeviceFeatures2, PFN_vkGetPhysicalDeviceFeatures2, "vkGetPhysicalDeviceFeatures2KHR");
    LOAD(GetPhysicalDeviceFormatProperties, PFN_vkGetPhysicalDeviceFormatProperties,
         "vkGetPhysicalDeviceFormatProperties");
    LOAD(GetPhysicalDeviceFormatProperties2, PFN_vkGetPhysicalDeviceFormatProperties2,
         "vkGetPhysicalDeviceFormatProperties2");
    if (!d->GetPhysicalDeviceFormatProperties2)
        LOAD(GetPhysicalDeviceFormatProperties2, PFN_vkGetPhysicalDeviceFormatProperties2,
             "vkGetPhysicalDeviceFormatProperties2KHR");
    LOAD(GetPhysicalDeviceImageFormatProperties, PFN_vkGetPhysicalDeviceImageFormatProperties,
         "vkGetPhysicalDeviceImageFormatProperties");
    LOAD(GetPhysicalDeviceImageFormatProperties2, PFN_vkGetPhysicalDeviceImageFormatProperties2,
         "vkGetPhysicalDeviceImageFormatProperties2");
    if (!d->GetPhysicalDeviceImageFormatProperties2)
        LOAD(GetPhysicalDeviceImageFormatProperties2, PFN_vkGetPhysicalDeviceImageFormatProperties2,
             "vkGetPhysicalDeviceImageFormatProperties2KHR");
    LOAD(GetPhysicalDeviceMemoryProperties, PFN_vkGetPhysicalDeviceMemoryProperties,
         "vkGetPhysicalDeviceMemoryProperties");
    LOAD(EnumerateDeviceExtensionProperties, PFN_vkEnumerateDeviceExtensionProperties,
         "vkEnumerateDeviceExtensionProperties");
#undef LOAD
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_instances[dispatch_key(*pInstance)] = std::move(d);
    }
    const VkApplicationInfo* app = pCreateInfo->pApplicationInfo;
    logf("vkCreateInstance ok (app requested apiVersion %u.%u.%u)", app ? VK_API_VERSION_MAJOR(app->apiVersion) : 1,
         app ? VK_API_VERSION_MINOR(app->apiVersion) : 0, app ? VK_API_VERSION_PATCH(app->apiVersion) : 0);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL layer_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    if (!instance) return;
    std::unique_ptr<InstanceData> d;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_instances.find(dispatch_key(instance));
        if (it == g_instances.end()) return;
        d = std::move(it->second);
        g_instances.erase(it);
    }
    logf("vkDestroyInstance");
    d->DestroyInstance(instance, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL layer_GetPhysicalDeviceProperties(VkPhysicalDevice pd, VkPhysicalDeviceProperties* p) {
    InstanceData* d = find_instance(pd);
    if (!d) return;
    d->GetPhysicalDeviceProperties(pd, p);
    logf("vkGetPhysicalDeviceProperties: \"%s\" api %u.%u.%u driver 0x%x", p->deviceName,
         VK_API_VERSION_MAJOR(p->apiVersion), VK_API_VERSION_MINOR(p->apiVersion), VK_API_VERSION_PATCH(p->apiVersion),
         p->driverVersion);
}

VKAPI_ATTR void VKAPI_CALL layer_GetPhysicalDeviceFeatures(VkPhysicalDevice pd, VkPhysicalDeviceFeatures* f) {
    InstanceData* d = find_instance(pd);
    if (!d) return;
    d->GetPhysicalDeviceFeatures(pd, f);
    if (!f->textureCompressionBC && bc_mode() != BcMode::Off && bc_fully_available(d, pd))
        f->textureCompressionBC = VK_TRUE;
    logf("vkGetPhysicalDeviceFeatures: textureCompressionBC=%u", f->textureCompressionBC);
}

VKAPI_ATTR void VKAPI_CALL layer_GetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2* f) {
    InstanceData* d = find_instance(pd);
    if (!d || !d->GetPhysicalDeviceFeatures2) return;
    d->GetPhysicalDeviceFeatures2(pd, f);
    if (!f->features.textureCompressionBC && bc_mode() != BcMode::Off && bc_fully_available(d, pd))
        f->features.textureCompressionBC = VK_TRUE;
    logf("vkGetPhysicalDeviceFeatures2: textureCompressionBC=%u", f->features.textureCompressionBC);
}

VKAPI_ATTR void VKAPI_CALL layer_GetPhysicalDeviceFormatProperties(VkPhysicalDevice pd, VkFormat format,
                                                                   VkFormatProperties* p) {
    InstanceData* d = find_instance(pd);
    if (!d) return;
    d->GetPhysicalDeviceFormatProperties(pd, format, p);
    VkFormatProperties emu{};
    if (bc_emulated_format_props(d, pd, format, *p, &emu)) *p = emu;
    if (is_bc_format(format)) {
        logf("vkGetPhysicalDeviceFormatProperties: BC format %d linear=0x%x optimal=0x%x buffer=0x%x",
             static_cast<int>(format), p->linearTilingFeatures, p->optimalTilingFeatures, p->bufferFeatures);
    }
}

VKAPI_ATTR void VKAPI_CALL layer_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice pd, VkFormat format,
                                                                    VkFormatProperties2* p) {
    InstanceData* d = find_instance(pd);
    if (!d || !d->GetPhysicalDeviceFormatProperties2) return;
    d->GetPhysicalDeviceFormatProperties2(pd, format, p);
    VkFormatProperties emu{};
    if (!bc_emulated_format_props(d, pd, format, p->formatProperties, &emu)) return;
    p->formatProperties = emu;
    for (auto* s = static_cast<VkBaseOutStructure*>(p->pNext); s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) {
            auto* p3 = reinterpret_cast<VkFormatProperties3*>(s);
            p3->linearTilingFeatures = emu.linearTilingFeatures;
            p3->optimalTilingFeatures = emu.optimalTilingFeatures;
            p3->bufferFeatures = emu.bufferFeatures;
        }
    }
}

// Whether an image-format query for BC format `format` should be answered by emulation.
bool image_query_emulated(InstanceData* d, VkPhysicalDevice pd, VkFormat format, VkImageType type, VkImageTiling tiling,
                          VkImageUsageFlags usage, VkResult down_result) {
    const BcMode mode = bc_mode();
    if (mode == BcMode::Off || !is_emulatable_bc(format)) return false;
    if (!bc_is_force(mode) && down_result == VK_SUCCESS) return false;
    if (type == VK_IMAGE_TYPE_1D || tiling != VK_IMAGE_TILING_OPTIMAL || !bc_usage_supported(usage)) return false;
    if (bc_is_emulate(mode) && type != VK_IMAGE_TYPE_2D) return false;
    VkFormatProperties none{}, emu{};
    return bc_emulated_format_props(d, pd, format, none, &emu);
}

// Usage/flags/format to ask the driver about when answering for an emulated BC image.
VkImageUsageFlags backing_usage(VkImageUsageFlags usage) {
    return bc_is_emulate(bc_mode()) ? (usage | VK_IMAGE_USAGE_STORAGE_BIT) : usage;
}
VkImageCreateFlags backing_flags(VkImageCreateFlags flags) {
    flags &= ~VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
    return bc_is_emulate(bc_mode()) ? (flags | VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) : flags;
}

VKAPI_ATTR VkResult VKAPI_CALL layer_GetPhysicalDeviceImageFormatProperties(
    VkPhysicalDevice pd, VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage,
    VkImageCreateFlags flags, VkImageFormatProperties* props) {
    InstanceData* d = find_instance(pd);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = d->GetPhysicalDeviceImageFormatProperties(pd, format, type, tiling, usage, flags, props);
    if (!image_query_emulated(d, pd, format, type, tiling, usage, r)) return r;
    return d->GetPhysicalDeviceImageFormatProperties(pd, bc_storage_format(format), type, tiling, backing_usage(usage),
                                                     backing_flags(flags), props);
}

VKAPI_ATTR VkResult VKAPI_CALL layer_GetPhysicalDeviceImageFormatProperties2(
    VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2* info, VkImageFormatProperties2* props) {
    InstanceData* d = find_instance(pd);
    if (!d || !d->GetPhysicalDeviceImageFormatProperties2) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = d->GetPhysicalDeviceImageFormatProperties2(pd, info, props);
    if (!image_query_emulated(d, pd, info->format, info->type, info->tiling, info->usage, r)) return r;
    VkPhysicalDeviceImageFormatInfo2 sub = *info;
    sub.format = bc_storage_format(info->format);
    sub.usage = backing_usage(info->usage);
    sub.flags = backing_flags(info->flags);
    return d->GetPhysicalDeviceImageFormatProperties2(pd, &sub, props);
}

VKAPI_ATTR VkResult VKAPI_CALL layer_EnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char* pLayerName,
                                                                        uint32_t* pCount, VkExtensionProperties* pProps) {
    if (pLayerName && std::strcmp(pLayerName, kLayerName) == 0) {
        *pCount = 0;  // this layer adds no device extensions
        return VK_SUCCESS;
    }
    InstanceData* d = find_instance(pd);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    return d->EnumerateDeviceExtensionProperties(pd, pLayerName, pCount, pProps);
}

// ---- intercepted: device creation ------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL layer_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* pCreateInfo,
                                                  const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    InstanceData* inst = find_instance(pd);
    if (!inst) return VK_ERROR_INITIALIZATION_FAILED;

    auto* link = const_cast<VkLayerDeviceCreateInfo*>(static_cast<const VkLayerDeviceCreateInfo*>(pCreateInfo->pNext));
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = const_cast<VkLayerDeviceCreateInfo*>(static_cast<const VkLayerDeviceCreateInfo*>(link->pNext));
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    if (!gipa || !gdpa) return VK_ERROR_INITIALIZATION_FAILED;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;  // advance the chain for the next layer
    VkLayerDeviceLink* advanced = link->u.pLayerInfo;

    auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(inst->instance, "vkCreateDevice"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;

    DeviceCreatePatch patch;
    bc_patch_device_create(inst, pd, pCreateInfo, &patch);
    VkResult res = create(pd, &patch.info, pAllocator, pDevice);
    patch.restore();
    if (res != VK_SUCCESS && patch.active) {
        logf("vkCreateDevice with BC emulation additions failed (%d); retrying without", static_cast<int>(res));
        patch.active = false;
        link->u.pLayerInfo = advanced;  // the failed attempt moved the chain on
        res = create(pd, pCreateInfo, pAllocator, pDevice);
    }
    if (res != VK_SUCCESS) return res;

    auto d = std::make_unique<DeviceData>();
    d->device = *pDevice;
    d->physical_device = pd;
    d->instance = inst;
    d->gdpa = gdpa;
#define X(name) d->name = reinterpret_cast<PFN_vk##name>(gdpa(*pDevice, "vk" #name));
    EXYNOS_DEVICE_FNS(X)
#undef X
    if (!d->CmdCopyBufferToImage2)
        d->CmdCopyBufferToImage2 = reinterpret_cast<PFN_vkCmdCopyBufferToImage2>(gdpa(*pDevice, "vkCmdCopyBufferToImage2KHR"));
    d->emulation_active = patch.active && d->CmdPushDescriptorSetKHR != nullptr;
    DeviceData* raw = d.get();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_devices[dispatch_key(*pDevice)] = std::move(d);
    }
    bc_device_created(raw);
    logf("vkCreateDevice ok (%u extensions requested, BC emulation %s)", pCreateInfo->enabledExtensionCount,
         raw->emulation_active ? "on" : "off");
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL layer_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    if (!device) return;
    std::unique_ptr<DeviceData> d;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_devices.find(dispatch_key(device));
        if (it == g_devices.end()) return;
        d = std::move(it->second);
        g_devices.erase(it);
    }
    bc_device_destroying(d.get());
    logf("vkDestroyDevice");
    d->DestroyDevice(device, pAllocator);
}

// ---- proc addr lookup ------------------------------------------------------

#define INTERCEPT(fn) \
    if (std::strcmp(name, "vk" #fn) == 0) return reinterpret_cast<PFN_vkVoidFunction>(layer_##fn)

PFN_vkVoidFunction intercepted_instance_proc(const char* name) {
    INTERCEPT(CreateInstance);
    INTERCEPT(DestroyInstance);
    INTERCEPT(CreateDevice);
    INTERCEPT(DestroyDevice);
    INTERCEPT(GetPhysicalDeviceProperties);
    INTERCEPT(GetPhysicalDeviceFeatures);
    INTERCEPT(GetPhysicalDeviceFeatures2);
    INTERCEPT(GetPhysicalDeviceFormatProperties);
    INTERCEPT(GetPhysicalDeviceFormatProperties2);
    INTERCEPT(GetPhysicalDeviceImageFormatProperties);
    INTERCEPT(GetPhysicalDeviceImageFormatProperties2);
    INTERCEPT(EnumerateDeviceExtensionProperties);
    if (std::strcmp(name, "vkGetPhysicalDeviceFeatures2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(layer_GetPhysicalDeviceFeatures2);
    if (std::strcmp(name, "vkGetPhysicalDeviceFormatProperties2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(layer_GetPhysicalDeviceFormatProperties2);
    if (std::strcmp(name, "vkGetPhysicalDeviceImageFormatProperties2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(layer_GetPhysicalDeviceImageFormatProperties2);
    return nullptr;
}

}  // namespace

}  // namespace exynos

// ---- entry points ----------------------------------------------------------
//
// Only the names the loader looks up by dlsym are exported. Anything named exactly like a loader
// function (vkGetInstanceProcAddr, ...) must not be referenced by symbol inside this library:
// with a global libvulkan loaded first, such a reference would bind to the loader's own function
// (symbol interposition) and recurse forever. Internal code uses the static functions below; the
// exported wrappers are only reached through dlsym on our handle.

static PFN_vkVoidFunction layer_GetDeviceProcAddr(VkDevice device, const char* name) {
    using namespace exynos;
    if (!name) return nullptr;
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(layer_GetDeviceProcAddr);
    if (std::strcmp(name, "vkDestroyDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(layer_DestroyDevice);
    DeviceData* d = find_device(device);
    bool hot = false;
    if (PFN_vkVoidFunction f = bc_device_proc(name, &hot)) {
        // Per-command hooks only cost anything when emulation is on for this device.
        if (hot && d && !d->emulation_active) return d->gdpa(device, name);
        return f;
    }
    if (!d || !d->gdpa) return nullptr;
    return d->gdpa(device, name);
}

static PFN_vkVoidFunction layer_GetInstanceProcAddr(VkInstance instance, const char* name) {
    using namespace exynos;
    if (!name) return nullptr;
    if (std::strcmp(name, "vkGetInstanceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(layer_GetInstanceProcAddr);
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(layer_GetDeviceProcAddr);
    if (PFN_vkVoidFunction f = intercepted_instance_proc(name)) return f;
    bool hot = false;
    if (PFN_vkVoidFunction f = bc_device_proc(name, &hot)) return f;  // hooks pass through if emulation is off
    InstanceData* d = find_instance(instance);
    if (!d || !d->gipa) return nullptr;
    return d->gipa(instance, name);
}

static VkResult layer_EnumerateLayerProperties(uint32_t* pCount, VkLayerProperties* pProps) {
    if (!pCount) return VK_INCOMPLETE;
    if (!pProps) {
        *pCount = 1;
        return VK_SUCCESS;
    }
    if (*pCount < 1) return VK_INCOMPLETE;
    VkLayerProperties p{};
    std::strncpy(p.layerName, exynos::kLayerName, VK_MAX_EXTENSION_NAME_SIZE - 1);
    std::strncpy(p.description, exynos::kLayerDesc, VK_MAX_DESCRIPTION_SIZE - 1);
    p.specVersion = VK_API_VERSION_1_3;
    p.implementationVersion = 1;
    pProps[0] = p;
    *pCount = 1;
    return VK_SUCCESS;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    if (!pVersionStruct || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
    // We implement interface version 2; accept anything the loader offers above that.
    if (pVersionStruct->loaderLayerInterfaceVersion > 2) pVersionStruct->loaderLayerInterfaceVersion = 2;
    if (pVersionStruct->loaderLayerInterfaceVersion >= 2) {
        pVersionStruct->pfnGetInstanceProcAddr = layer_GetInstanceProcAddr;
        pVersionStruct->pfnGetDeviceProcAddr = layer_GetDeviceProcAddr;
        pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    }
    return VK_SUCCESS;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t* pCount, VkLayerProperties* pProps) {
    return layer_EnumerateLayerProperties(pCount, pProps);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pCount,
                                                                             VkExtensionProperties*) {
    if (pLayerName && std::strcmp(pLayerName, exynos::kLayerName) == 0) {
        if (pCount) *pCount = 0;
        return VK_SUCCESS;
    }
    return VK_ERROR_LAYER_NOT_PRESENT;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice, uint32_t* pCount,
                                                                       VkLayerProperties* pProps) {
    return layer_EnumerateLayerProperties(pCount, pProps);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char* pLayerName,
                                                                           uint32_t* pCount, VkExtensionProperties* pProps) {
    return exynos::layer_EnumerateDeviceExtensionProperties(pd, pLayerName, pCount, pProps);
}
