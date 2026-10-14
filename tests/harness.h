// Small Vulkan test harness: optional embedded layer (see vk_probe.c), instance/device setup,
// buffer/image helpers. Header-only on purpose.
#pragma once

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#define H_CHECK(call)                                                                        \
    do {                                                                                     \
        VkResult r_ = (call);                                                                \
        if (r_ != VK_SUCCESS) {                                                              \
            fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #call, (int)r_);         \
            exit(1);                                                                         \
        }                                                                                    \
    } while (0)

static PFN_vkGetInstanceProcAddr g_gipa;
static PFN_vkGetDeviceProcAddr g_gdpa;
static int g_embedded;

static VkResult h_shim_CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out) {
    VkInstanceCreateInfo copy = *ci;
    const VkLayerInstanceCreateInfo* head = (const VkLayerInstanceCreateInfo*)ci->pNext;
    if (head && head->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) copy.pNext = head->pNext;
    return vkCreateInstance(&copy, a, out);
}
static VkResult h_shim_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a,
                                    VkDevice* out) {
    VkDeviceCreateInfo copy = *ci;
    const VkLayerDeviceCreateInfo* head = (const VkLayerDeviceCreateInfo*)ci->pNext;
    while (head && head->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
        copy.pNext = head->pNext;
        head = (const VkLayerDeviceCreateInfo*)head->pNext;
    }
    return vkCreateDevice(pd, &copy, a, out);
}
static PFN_vkVoidFunction h_shim_gipa(VkInstance inst, const char* name) {
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)h_shim_CreateInstance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)h_shim_CreateDevice;
    return vkGetInstanceProcAddr(inst, name);
}
static PFN_vkVoidFunction h_shim_gdpa(VkDevice dev, const char* name) { return vkGetDeviceProcAddr(dev, name); }

static int h_load_layer(const char* path) {
    void* lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen(%s): %s\n", path, dlerror()); return 0; }
    PFN_vkNegotiateLoaderLayerInterfaceVersion neg =
        (PFN_vkNegotiateLoaderLayerInterfaceVersion)dlsym(lib, "vkNegotiateLoaderLayerInterfaceVersion");
    VkNegotiateLayerInterface iface = {.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT, .loaderLayerInterfaceVersion = 2};
    if (!neg || neg(&iface) != VK_SUCCESS) return 0;
    g_gipa = iface.pfnGetInstanceProcAddr;
    g_gdpa = iface.pfnGetDeviceProcAddr;
    g_embedded = 1;
    return 1;
}

#define H_INST(inst, name) ((PFN_##name)g_gipa((inst), #name))
#define H_DEV(dev, name) ((PFN_##name)g_gdpa((dev), #name))

typedef struct {
    VkInstance instance;
    VkPhysicalDevice pd;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkPhysicalDeviceMemoryProperties mem;
    PFN_vkGetPhysicalDeviceFormatProperties GetFmt;
    PFN_vkDestroyInstance DestroyInstance;
} HCtx;

static void h_init(HCtx* c) {
    g_gipa = vkGetInstanceProcAddr;
    g_gdpa = vkGetDeviceProcAddr;
    const char* layer = getenv("EXYNOS_PROBE_LAYER");
    if (layer && !h_load_layer(layer)) exit(2);

    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    if (g_embedded) {
        VkLayerInstanceLink link = {.pfnNextGetInstanceProcAddr = h_shim_gipa};
        VkLayerInstanceCreateInfo lici = {.sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO,
                                          .function = VK_LAYER_LINK_INFO, .u.pLayerInfo = &link};
        ici.pNext = &lici;
        H_CHECK(((PFN_vkCreateInstance)g_gipa(NULL, "vkCreateInstance"))(&ici, NULL, &c->instance));
    } else {
        H_CHECK(vkCreateInstance(&ici, NULL, &c->instance));
    }
    uint32_t n = 1;
    PFN_vkEnumeratePhysicalDevices enumpd = H_INST(c->instance, vkEnumeratePhysicalDevices);
    VkResult r = enumpd(c->instance, &n, &c->pd);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) { fprintf(stderr, "no device\n"); exit(1); }
    H_INST(c->instance, vkGetPhysicalDeviceMemoryProperties)(c->pd, &c->mem);
    c->GetFmt = H_INST(c->instance, vkGetPhysicalDeviceFormatProperties);
    c->DestroyInstance = H_INST(c->instance, vkDestroyInstance);

    PFN_vkGetPhysicalDeviceQueueFamilyProperties qf = H_INST(c->instance, vkGetPhysicalDeviceQueueFamilyProperties);
    uint32_t qn = 0;
    qf(c->pd, &qn, NULL);
    VkQueueFamilyProperties* qp = malloc(qn * sizeof(*qp));
    qf(c->pd, &qn, qp);
    c->family = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++)
        if ((qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && (qp[i].queueFlags & VK_QUEUE_TRANSFER_BIT)) { c->family = i; break; }
    free(qp);
    if (c->family == UINT32_MAX) { fprintf(stderr, "no compute queue\n"); exit(1); }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = c->family,
                                   .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci};
    if (g_embedded) {
        VkLayerDeviceLink dlink = {.pfnNextGetInstanceProcAddr = h_shim_gipa, .pfnNextGetDeviceProcAddr = h_shim_gdpa};
        VkLayerDeviceCreateInfo ldci = {.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,
                                        .function = VK_LAYER_LINK_INFO, .u.pLayerInfo = &dlink};
        dci.pNext = &ldci;
        H_CHECK(H_INST(c->instance, vkCreateDevice)(c->pd, &dci, NULL, &c->device));
    } else {
        H_CHECK(H_INST(c->instance, vkCreateDevice)(c->pd, &dci, NULL, &c->device));
    }
    H_DEV(c->device, vkGetDeviceQueue)(c->device, c->family, 0, &c->queue);
}

static uint32_t h_mem_type(const HCtx* c, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < c->mem.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (c->mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static void h_make_buffer(const HCtx* c, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                          VkBuffer* buf, VkDeviceMemory* mem) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    H_CHECK(H_DEV(c->device, vkCreateBuffer)(c->device, &bi, NULL, buf));
    VkMemoryRequirements mr;
    H_DEV(c->device, vkGetBufferMemoryRequirements)(c->device, *buf, &mr);
    uint32_t t = h_mem_type(c, mr.memoryTypeBits, props);
    if (t == UINT32_MAX) { fprintf(stderr, "no memory type\n"); exit(1); }
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                               .memoryTypeIndex = t};
    H_CHECK(H_DEV(c->device, vkAllocateMemory)(c->device, &ai, NULL, mem));
    H_CHECK(H_DEV(c->device, vkBindBufferMemory)(c->device, *buf, *mem, 0));
}
