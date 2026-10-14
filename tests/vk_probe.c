// vk_probe: prints what the Vulkan implementation (with or without our layer) reports.
// Everything goes to stdout in a stable format so two runs can be diffed.
//
// Layer selection:
//  - loader mode: pick layers from outside (VK_INSTANCE_LAYERS / VK_LOADER_LAYERS_ENABLE) on
//    loaders that support it (Khronos loader). The Android system loader does not for ordinary apps.
//  - embedded mode: EXYNOS_PROBE_LAYER=/path/to/libVkLayer_exynos_tools.so. The probe loads the
//    library itself, negotiates the layer interface and builds the call chain by hand, the way an
//    application or a launcher that embeds the layer would. Works anywhere dlopen works.

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#define CHECK(call)                                                      \
    do {                                                                 \
        VkResult r_ = (call);                                            \
        if (r_ != VK_SUCCESS) {                                          \
            printf("FAILED %s = %d\n", #call, (int)r_);                  \
            return 1;                                                    \
        }                                                                \
    } while (0)

struct NamedFormat {
    VkFormat format;
    const char* name;
};

#define F(x) {VK_FORMAT_##x, #x}
static const struct NamedFormat kFormats[] = {
    F(BC1_RGB_UNORM_BLOCK),  F(BC1_RGB_SRGB_BLOCK),  F(BC1_RGBA_UNORM_BLOCK), F(BC1_RGBA_SRGB_BLOCK),
    F(BC2_UNORM_BLOCK),      F(BC2_SRGB_BLOCK),      F(BC3_UNORM_BLOCK),      F(BC3_SRGB_BLOCK),
    F(BC4_UNORM_BLOCK),      F(BC4_SNORM_BLOCK),     F(BC5_UNORM_BLOCK),      F(BC5_SNORM_BLOCK),
    F(BC6H_UFLOAT_BLOCK),    F(BC6H_SFLOAT_BLOCK),   F(BC7_UNORM_BLOCK),      F(BC7_SRGB_BLOCK),
    // Formats D3D11/D3D12 translation layers commonly rely on.
    F(B8G8R8A8_UNORM),       F(B8G8R8A8_SRGB),       F(R8G8B8A8_UNORM),       F(R8G8B8A8_SRGB),
    F(A2B10G10R10_UNORM_PACK32), F(R16G16B16A16_UNORM), F(R16G16B16A16_SFLOAT),
    F(R32G32B32A32_SFLOAT),  F(B10G11R11_UFLOAT_PACK32), F(E5B9G9R9_UFLOAT_PACK32),
    F(D16_UNORM),            F(D24_UNORM_S8_UINT),   F(D32_SFLOAT),           F(D32_SFLOAT_S8_UINT),
    F(X8_D24_UNORM_PACK32),  F(R5G6B5_UNORM_PACK16), F(B4G4R4A4_UNORM_PACK16),
};
#undef F

// ---- call chain ------------------------------------------------------------

// Entry points are always fetched through g_gipa/g_gdpa so that, in embedded mode, every call
// goes through the layer.
static PFN_vkGetInstanceProcAddr g_gipa;
static PFN_vkGetDeviceProcAddr g_gdpa;

// Embedded mode: the "next" link below the layer is the system loader. Its vkCreateInstance and
// vkCreateDevice must not see the layer-link structs we put in pNext, so these shims strip them.
static VkResult shim_CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a,
                                    VkInstance* out) {
    VkInstanceCreateInfo copy = *ci;
    const VkLayerInstanceCreateInfo* head = (const VkLayerInstanceCreateInfo*)ci->pNext;
    if (head && head->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) copy.pNext = head->pNext;
    return vkCreateInstance(&copy, a, out);
}

static VkResult shim_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
                                  const VkAllocationCallbacks* a, VkDevice* out) {
    VkDeviceCreateInfo copy = *ci;
    const VkLayerDeviceCreateInfo* head = (const VkLayerDeviceCreateInfo*)ci->pNext;
    if (head && head->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) copy.pNext = head->pNext;
    return vkCreateDevice(pd, &copy, a, out);
}

static PFN_vkVoidFunction shim_gipa(VkInstance inst, const char* name) {
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)shim_CreateInstance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)shim_CreateDevice;
    return vkGetInstanceProcAddr(inst, name);
}

static PFN_vkVoidFunction shim_gdpa(VkDevice dev, const char* name) {
    return vkGetDeviceProcAddr(dev, name);
}

static int g_embedded;

static int load_layer(const char* path) {
    void* lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "vk_probe: dlopen(%s) failed: %s\n", path, dlerror());
        return 0;
    }
    PFN_vkNegotiateLoaderLayerInterfaceVersion negotiate =
        (PFN_vkNegotiateLoaderLayerInterfaceVersion)dlsym(lib, "vkNegotiateLoaderLayerInterfaceVersion");
    if (!negotiate) {
        fprintf(stderr, "vk_probe: layer has no vkNegotiateLoaderLayerInterfaceVersion\n");
        return 0;
    }
    VkNegotiateLayerInterface iface = {.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT,
                                       .loaderLayerInterfaceVersion = 2};
    if (negotiate(&iface) != VK_SUCCESS || !iface.pfnGetInstanceProcAddr || !iface.pfnGetDeviceProcAddr) {
        fprintf(stderr, "vk_probe: layer interface negotiation failed\n");
        return 0;
    }
    g_gipa = iface.pfnGetInstanceProcAddr;
    g_gdpa = iface.pfnGetDeviceProcAddr;
    g_embedded = 1;
    fprintf(stderr, "vk_probe: embedded layer loaded from %s (interface v%u)\n", path,
            iface.loaderLayerInterfaceVersion);
    return 1;
}

#define INST(inst, name) ((PFN_##name)g_gipa((inst), #name))
#define DEV(dev, name) ((PFN_##name)g_gdpa((dev), #name))

int main(void) {
    g_gipa = vkGetInstanceProcAddr;
    g_gdpa = vkGetDeviceProcAddr;
    const char* layer = getenv("EXYNOS_PROBE_LAYER");
    if (layer && !load_layer(layer)) return 2;

    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "vk_probe",
                             .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &app};
    VkInstance instance;
    if (g_embedded) {
        VkLayerInstanceLink link = {.pNext = NULL,
                                    .pfnNextGetInstanceProcAddr = shim_gipa,
                                    .pfnNextGetPhysicalDeviceProcAddr = NULL};
        VkLayerInstanceCreateInfo lici = {.sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO,
                                          .function = VK_LAYER_LINK_INFO,
                                          .u.pLayerInfo = &link};
        ici.pNext = &lici;
        PFN_vkCreateInstance create = (PFN_vkCreateInstance)g_gipa(NULL, "vkCreateInstance");
        CHECK(create(&ici, NULL, &instance));
    } else {
        CHECK(vkCreateInstance(&ici, NULL, &instance));
    }

    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = INST(instance, vkEnumeratePhysicalDevices);
    PFN_vkGetPhysicalDeviceProperties GetProps = INST(instance, vkGetPhysicalDeviceProperties);
    PFN_vkGetPhysicalDeviceFeatures GetFeat = INST(instance, vkGetPhysicalDeviceFeatures);
    PFN_vkGetPhysicalDeviceFeatures2 GetFeat2 = INST(instance, vkGetPhysicalDeviceFeatures2);
    PFN_vkEnumerateDeviceExtensionProperties EnumDevExt = INST(instance, vkEnumerateDeviceExtensionProperties);
    PFN_vkGetPhysicalDeviceFormatProperties GetFmt = INST(instance, vkGetPhysicalDeviceFormatProperties);
    PFN_vkGetPhysicalDeviceImageFormatProperties GetImgFmt = INST(instance, vkGetPhysicalDeviceImageFormatProperties);
    PFN_vkGetPhysicalDeviceImageFormatProperties2 GetImgFmt2 =
        INST(instance, vkGetPhysicalDeviceImageFormatProperties2);
    PFN_vkGetPhysicalDeviceFormatProperties2 GetFmt2 = INST(instance, vkGetPhysicalDeviceFormatProperties2);
    PFN_vkCreateDevice CreateDevice = INST(instance, vkCreateDevice);
    PFN_vkDestroyInstance DestroyInstance = INST(instance, vkDestroyInstance);

    uint32_t n = 0;
    CHECK(EnumeratePhysicalDevices(instance, &n, NULL));
    printf("physical devices: %u\n", n);
    if (n == 0) return 1;
    VkPhysicalDevice* pds = malloc(n * sizeof(*pds));
    CHECK(EnumeratePhysicalDevices(instance, &n, pds));

    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDevice pd = pds[i];
        VkPhysicalDeviceProperties props;
        GetProps(pd, &props);
        printf("device %u: %s\n  api %u.%u.%u vendor 0x%x device 0x%x type %d\n", i, props.deviceName,
               VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
               VK_API_VERSION_PATCH(props.apiVersion), props.vendorID, props.deviceID,
               (int)props.deviceType);

        VkPhysicalDeviceFeatures feat;
        GetFeat(pd, &feat);
        printf("  textureCompressionBC=%u ETC2=%u ASTC_LDR=%u shaderFloat64=%u\n",
               feat.textureCompressionBC, feat.textureCompressionETC2, feat.textureCompressionASTC_LDR,
               feat.shaderFloat64);

        VkPhysicalDeviceFeatures2 feat2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        GetFeat2(pd, &feat2);
        printf("  features2 textureCompressionBC=%u\n", feat2.features.textureCompressionBC);

        uint32_t ne = 0;
        CHECK(EnumDevExt(pd, NULL, &ne, NULL));
        printf("  device extensions: %u\n", ne);

        printf("  formats (optimalTilingFeatures, linear, buffer):\n");
        for (size_t k = 0; k < sizeof(kFormats) / sizeof(kFormats[0]); k++) {
            VkFormatProperties fp;
            GetFmt(pd, kFormats[k].format, &fp);
            printf("    %-28s 0x%08x 0x%08x 0x%08x\n", kFormats[k].name, fp.optimalTilingFeatures,
                   fp.linearTilingFeatures, fp.bufferFeatures);
        }

        printf("  bc image-format queries (2D optimal SAMPLED|TRANSFER_DST), FormatProperties2 vs 1:\n");
        for (size_t k = 0; k < 16; k++) {
            VkImageFormatProperties ifp = {0};
            VkResult r = GetImgFmt(pd, kFormats[k].format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &ifp);
            int r2 = 99;
            if (GetImgFmt2) {
                VkPhysicalDeviceImageFormatInfo2 info = {
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
                    .format = kFormats[k].format,
                    .type = VK_IMAGE_TYPE_2D,
                    .tiling = VK_IMAGE_TILING_OPTIMAL,
                    .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
                VkImageFormatProperties2 out = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
                r2 = (int)GetImgFmt2(pd, &info, &out);
            }
            int fp2same = -1;
            if (GetFmt2) {
                VkFormatProperties a, b;
                VkFormatProperties2 fp2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
                GetFmt(pd, kFormats[k].format, &a);
                GetFmt2(pd, kFormats[k].format, &fp2);
                b = fp2.formatProperties;
                fp2same = a.optimalTilingFeatures == b.optimalTilingFeatures &&
                          a.linearTilingFeatures == b.linearTilingFeatures &&
                          a.bufferFeatures == b.bufferFeatures;
            }
            printf("    %-28s query=%d query2=%d maxExtent=%u fp2same=%d\n", kFormats[k].name, (int)r, r2,
                   r == VK_SUCCESS ? ifp.maxExtent.width : 0, fp2same);
        }

        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                       .queueFamilyIndex = 0,
                                       .queueCount = 1,
                                       .pQueuePriorities = &prio};
        VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                  .queueCreateInfoCount = 1,
                                  .pQueueCreateInfos = &qci};
        VkDevice dev;
        if (g_embedded) {
            VkLayerDeviceLink dlink = {.pNext = NULL,
                                       .pfnNextGetInstanceProcAddr = shim_gipa,
                                       .pfnNextGetDeviceProcAddr = shim_gdpa};
            VkLayerDeviceCreateInfo ldci = {.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,
                                            .function = VK_LAYER_LINK_INFO,
                                            .u.pLayerInfo = &dlink};
            dci.pNext = &ldci;
            CHECK(CreateDevice(pd, &dci, NULL, &dev));
        } else {
            CHECK(CreateDevice(pd, &dci, NULL, &dev));
        }
        PFN_vkGetDeviceQueue GetQueue = DEV(dev, vkGetDeviceQueue);
        PFN_vkDeviceWaitIdle WaitIdle = DEV(dev, vkDeviceWaitIdle);
        PFN_vkDestroyDevice DestroyDevice = DEV(dev, vkDestroyDevice);
        PFN_vkCreateImage CreateImage = DEV(dev, vkCreateImage);
        PFN_vkDestroyImage DestroyImage = DEV(dev, vkDestroyImage);
        printf("  create 64x64 BC images:\n");
        for (size_t k = 0; k < 16; k++) {
            VkImageCreateInfo imci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                      .imageType = VK_IMAGE_TYPE_2D,
                                      .format = kFormats[k].format,
                                      .extent = {64, 64, 1},
                                      .mipLevels = 1,
                                      .arrayLayers = 1,
                                      .samples = VK_SAMPLE_COUNT_1_BIT,
                                      .tiling = VK_IMAGE_TILING_OPTIMAL,
                                      .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
            VkImage img = VK_NULL_HANDLE;
            VkResult cr = CreateImage(dev, &imci, NULL, &img);
            printf("    %-28s result=%d\n", kFormats[k].name, (int)cr);
            if (cr == VK_SUCCESS) DestroyImage(dev, img, NULL);
        }
        VkQueue q;
        GetQueue(dev, 0, 0, &q);
        CHECK(WaitIdle(dev));
        printf("  device create/queue/waitidle: ok\n");
        DestroyDevice(dev, NULL);
    }
    free(pds);
    DestroyInstance(instance, NULL);
    printf("done\n");
    return 0;
}
