// exynos-tools: Vulkan layer, phase 3 (pass-through with logging).
//
// Written from the Vulkan specification and the Khronos loader/layer interface
// documentation (loader interface version 2). It forwards everything to the next
// layer/driver and logs a few calls that matter for the BCn work in later phases.

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#ifdef __ANDROID__
#include <android/log.h>
#endif

#define EXPORT extern "C" __attribute__((visibility("default")))

namespace {

constexpr const char* kLayerName = "VK_LAYER_EXYNOS_TOOLS_xclipse";
constexpr const char* kLayerDesc = "exynos-tools: Xclipse compatibility layer (pass-through)";

// ---- logging ---------------------------------------------------------------

bool log_enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("EXYNOS_TOOLS_LOG");
        return !(v && v[0] == '0');
    }();
    return enabled;
}

void logf(const char* fmt, ...) {
    if (!log_enabled()) return;
    va_list ap;
    va_start(ap, fmt);
#ifdef __ANDROID__
    __android_log_vprint(ANDROID_LOG_INFO, "exynos-tools", fmt, ap);
#else
    std::fprintf(stderr, "[exynos-tools] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
#endif
    va_end(ap);
}

// ---- dispatch bookkeeping --------------------------------------------------

// The loader stores its dispatch table pointer in the first word of every
// dispatchable handle; handles of one instance (instance + its physical devices)
// share it, as do a device, its queues and command buffers.
void* dispatch_key(const void* handle) { return *static_cast<void* const*>(handle); }

struct InstanceData {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkCreateDevice CreateDevice = nullptr;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures GetPhysicalDeviceFeatures = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2 = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
};

struct DeviceData {
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkDestroyDevice DestroyDevice = nullptr;
};

std::mutex g_mutex;
std::unordered_map<void*, InstanceData> g_instances;
std::unordered_map<void*, DeviceData> g_devices;

bool get_instance(const void* handle, InstanceData* out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_instances.find(dispatch_key(handle));
    if (it == g_instances.end()) return false;
    *out = it->second;
    return true;
}

bool get_device(const void* handle, DeviceData* out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(dispatch_key(handle));
    if (it == g_devices.end()) return false;
    *out = it->second;
    return true;
}

bool is_bc_format(VkFormat f) {
    return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK;
}

// ---- intercepted: instance level -------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL layer_CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                    const VkAllocationCallbacks* pAllocator,
                                                    VkInstance* pInstance) {
    auto* link = const_cast<VkLayerInstanceCreateInfo*>(
        static_cast<const VkLayerInstanceCreateInfo*>(pCreateInfo->pNext));
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                     link->function == VK_LAYER_LINK_INFO)) {
        link = const_cast<VkLayerInstanceCreateInfo*>(
            static_cast<const VkLayerInstanceCreateInfo*>(link->pNext));
    }
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    if (!gipa) return VK_ERROR_INITIALIZATION_FAILED;
    // Advance the chain for the next layer.
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = create(pCreateInfo, pAllocator, pInstance);
    if (res != VK_SUCCESS) return res;

    InstanceData d;
    d.instance = *pInstance;
    d.gipa = gipa;
    auto load = [&](const char* name) { return gipa(*pInstance, name); };
    d.DestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(load("vkDestroyInstance"));
    d.CreateDevice = reinterpret_cast<PFN_vkCreateDevice>(load("vkCreateDevice"));
    d.GetPhysicalDeviceProperties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(load("vkGetPhysicalDeviceProperties"));
    d.GetPhysicalDeviceFeatures =
        reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(load("vkGetPhysicalDeviceFeatures"));
    d.GetPhysicalDeviceFeatures2 =
        reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(load("vkGetPhysicalDeviceFeatures2"));
    if (!d.GetPhysicalDeviceFeatures2) {
        d.GetPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            load("vkGetPhysicalDeviceFeatures2KHR"));
    }
    d.GetPhysicalDeviceFormatProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
        load("vkGetPhysicalDeviceFormatProperties"));
    d.EnumerateDeviceExtensionProperties = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        load("vkEnumerateDeviceExtensionProperties"));

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_instances[dispatch_key(*pInstance)] = d;
    }
    logf("vkCreateInstance ok (app requested apiVersion %u.%u.%u)",
         pCreateInfo->pApplicationInfo ? VK_API_VERSION_MAJOR(pCreateInfo->pApplicationInfo->apiVersion) : 1,
         pCreateInfo->pApplicationInfo ? VK_API_VERSION_MINOR(pCreateInfo->pApplicationInfo->apiVersion) : 0,
         pCreateInfo->pApplicationInfo ? VK_API_VERSION_PATCH(pCreateInfo->pApplicationInfo->apiVersion) : 0);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL layer_DestroyInstance(VkInstance instance,
                                                 const VkAllocationCallbacks* pAllocator) {
    if (!instance) return;
    InstanceData d;
    if (!get_instance(instance, &d)) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_instances.erase(dispatch_key(instance));
    }
    logf("vkDestroyInstance");
    d.DestroyInstance(instance, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL layer_GetPhysicalDeviceProperties(VkPhysicalDevice pd,
                                                             VkPhysicalDeviceProperties* p) {
    InstanceData d;
    if (!get_instance(pd, &d)) return;
    d.GetPhysicalDeviceProperties(pd, p);
    logf("vkGetPhysicalDeviceProperties: \"%s\" api %u.%u.%u driver 0x%x", p->deviceName,
         VK_API_VERSION_MAJOR(p->apiVersion), VK_API_VERSION_MINOR(p->apiVersion),
         VK_API_VERSION_PATCH(p->apiVersion), p->driverVersion);
}

VKAPI_ATTR void VKAPI_CALL layer_GetPhysicalDeviceFeatures(VkPhysicalDevice pd,
                                                           VkPhysicalDeviceFeatures* f) {
    InstanceData d;
    if (!get_instance(pd, &d)) return;
    d.GetPhysicalDeviceFeatures(pd, f);
    logf("vkGetPhysicalDeviceFeatures: textureCompressionBC=%u", f->textureCompressionBC);
}

VKAPI_ATTR void VKAPI_CALL layer_GetPhysicalDeviceFeatures2(VkPhysicalDevice pd,
                                                            VkPhysicalDeviceFeatures2* f) {
    InstanceData d;
    if (!get_instance(pd, &d) || !d.GetPhysicalDeviceFeatures2) return;
    d.GetPhysicalDeviceFeatures2(pd, f);
    logf("vkGetPhysicalDeviceFeatures2: textureCompressionBC=%u", f->features.textureCompressionBC);
}

VKAPI_ATTR void VKAPI_CALL layer_GetPhysicalDeviceFormatProperties(VkPhysicalDevice pd, VkFormat format,
                                                                   VkFormatProperties* p) {
    InstanceData d;
    if (!get_instance(pd, &d)) return;
    d.GetPhysicalDeviceFormatProperties(pd, format, p);
    if (is_bc_format(format)) {
        logf("vkGetPhysicalDeviceFormatProperties: BC format %d linear=0x%x optimal=0x%x buffer=0x%x",
             static_cast<int>(format), p->linearTilingFeatures, p->optimalTilingFeatures,
             p->bufferFeatures);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL layer_CreateDevice(VkPhysicalDevice pd,
                                                  const VkDeviceCreateInfo* pCreateInfo,
                                                  const VkAllocationCallbacks* pAllocator,
                                                  VkDevice* pDevice) {
    InstanceData inst;
    if (!get_instance(pd, &inst)) return VK_ERROR_INITIALIZATION_FAILED;

    auto* link = const_cast<VkLayerDeviceCreateInfo*>(
        static_cast<const VkLayerDeviceCreateInfo*>(pCreateInfo->pNext));
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                     link->function == VK_LAYER_LINK_INFO)) {
        link = const_cast<VkLayerDeviceCreateInfo*>(
            static_cast<const VkLayerDeviceCreateInfo*>(link->pNext));
    }
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    if (!gipa || !gdpa) return VK_ERROR_INITIALIZATION_FAILED;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(inst.instance, "vkCreateDevice"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = create(pd, pCreateInfo, pAllocator, pDevice);
    if (res != VK_SUCCESS) return res;

    DeviceData d;
    d.gdpa = gdpa;
    d.DestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(gdpa(*pDevice, "vkDestroyDevice"));
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_devices[dispatch_key(*pDevice)] = d;
    }
    logf("vkCreateDevice ok (%u extensions requested)", pCreateInfo->enabledExtensionCount);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL layer_EnumerateDeviceExtensionProperties(
    VkPhysicalDevice pd, const char* pLayerName, uint32_t* pCount, VkExtensionProperties* pProps) {
    if (pLayerName && std::strcmp(pLayerName, kLayerName) == 0) {
        *pCount = 0;  // this layer adds no device extensions (yet)
        return VK_SUCCESS;
    }
    InstanceData d;
    if (!get_instance(pd, &d)) return VK_ERROR_INITIALIZATION_FAILED;
    return d.EnumerateDeviceExtensionProperties(pd, pLayerName, pCount, pProps);
}

// ---- intercepted: device level ---------------------------------------------

VKAPI_ATTR void VKAPI_CALL layer_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    if (!device) return;
    DeviceData d;
    if (!get_device(device, &d)) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_devices.erase(dispatch_key(device));
    }
    logf("vkDestroyDevice");
    d.DestroyDevice(device, pAllocator);
}

// ---- proc addr lookup ------------------------------------------------------

#define INTERCEPT(fn) \
    if (std::strcmp(name, "vk" #fn) == 0) return reinterpret_cast<PFN_vkVoidFunction>(layer_##fn)

PFN_vkVoidFunction intercepted_device_proc(const char* name) {
    INTERCEPT(DestroyDevice);
    return nullptr;
}

PFN_vkVoidFunction intercepted_instance_proc(const char* name) {
    INTERCEPT(CreateInstance);
    INTERCEPT(DestroyInstance);
    INTERCEPT(CreateDevice);
    INTERCEPT(GetPhysicalDeviceProperties);
    INTERCEPT(GetPhysicalDeviceFeatures);
    INTERCEPT(GetPhysicalDeviceFeatures2);
    INTERCEPT(GetPhysicalDeviceFormatProperties);
    INTERCEPT(EnumerateDeviceExtensionProperties);
    if (std::strcmp(name, "vkGetPhysicalDeviceFeatures2KHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(layer_GetPhysicalDeviceFeatures2);
    }
    return nullptr;
}

}  // namespace

// ---- entry points ----------------------------------------------------------
//
// Only the names the loader looks up by dlsym are exported. Anything named exactly like a
// loader function (vkGetInstanceProcAddr, ...) must not be referenced by symbol inside this
// library: with a global libvulkan loaded first, such a reference would bind to the loader's
// own function (symbol interposition) and recurse forever. Internal code uses the static
// functions below; the exported wrappers are only reached through dlsym on our handle.

static PFN_vkVoidFunction layer_GetDeviceProcAddr(VkDevice device, const char* name);
static PFN_vkVoidFunction layer_GetInstanceProcAddr(VkInstance instance, const char* name);

static PFN_vkVoidFunction layer_GetDeviceProcAddr(VkDevice device, const char* name) {
    if (!name) return nullptr;
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(layer_GetDeviceProcAddr);
    }
    if (PFN_vkVoidFunction f = intercepted_device_proc(name)) return f;
    DeviceData d;
    if (!device || !get_device(device, &d) || !d.gdpa) return nullptr;
    return d.gdpa(device, name);
}

static PFN_vkVoidFunction layer_GetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!name) return nullptr;
    if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(layer_GetInstanceProcAddr);
    }
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(layer_GetDeviceProcAddr);
    }
    if (PFN_vkVoidFunction f = intercepted_instance_proc(name)) return f;
    if (PFN_vkVoidFunction f = intercepted_device_proc(name)) return f;
    InstanceData d;
    if (!instance || !get_instance(instance, &d) || !d.gipa) return nullptr;
    return d.gipa(instance, name);
}

static VkResult layer_EnumerateLayerProperties(uint32_t* pCount, VkLayerProperties* pProps) {
    if (!pCount) return VK_INCOMPLETE;
    if (!pProps) {
        *pCount = 1;
        return VK_SUCCESS;
    }
    if (*pCount < 1) return VK_INCOMPLETE;
    VkLayerProperties p{};
    std::strncpy(p.layerName, kLayerName, VK_MAX_EXTENSION_NAME_SIZE - 1);
    std::strncpy(p.description, kLayerDesc, VK_MAX_DESCRIPTION_SIZE - 1);
    p.specVersion = VK_API_VERSION_1_3;
    p.implementationVersion = 1;
    pProps[0] = p;
    *pCount = 1;
    return VK_SUCCESS;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    if (!pVersionStruct || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    // We implement interface version 2; accept anything the loader offers above that.
    if (pVersionStruct->loaderLayerInterfaceVersion > 2) pVersionStruct->loaderLayerInterfaceVersion = 2;
    if (pVersionStruct->loaderLayerInterfaceVersion >= 2) {
        pVersionStruct->pfnGetInstanceProcAddr = layer_GetInstanceProcAddr;
        pVersionStruct->pfnGetDeviceProcAddr = layer_GetDeviceProcAddr;
        pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    }
    return VK_SUCCESS;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t* pCount,
                                                                         VkLayerProperties* pProps) {
    return layer_EnumerateLayerProperties(pCount, pProps);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char* pLayerName,
                                                                             uint32_t* pCount,
                                                                             VkExtensionProperties*) {
    if (pLayerName && std::strcmp(pLayerName, kLayerName) == 0) {
        if (pCount) *pCount = 0;
        return VK_SUCCESS;
    }
    return VK_ERROR_LAYER_NOT_PRESENT;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice, uint32_t* pCount,
                                                                       VkLayerProperties* pProps) {
    return layer_EnumerateLayerProperties(pCount, pProps);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice pd,
                                                                           const char* pLayerName,
                                                                           uint32_t* pCount,
                                                                           VkExtensionProperties* pProps) {
    return layer_EnumerateDeviceExtensionProperties(pd, pLayerName, pCount, pProps);
}
