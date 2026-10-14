// vk_probe: prints what the Vulkan implementation (with or without our layer) reports.
// Everything goes to stdout in a stable format so two runs can be diffed.
// Layers are chosen from outside (VK_INSTANCE_LAYERS / VK_LOADER_LAYERS_ENABLE).

#include <stdio.h>
#include <stdlib.h>
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

int main(void) {
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "vk_probe",
                             .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &app};
    VkInstance instance;
    CHECK(vkCreateInstance(&ici, NULL, &instance));

    uint32_t n = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &n, NULL));
    printf("physical devices: %u\n", n);
    if (n == 0) return 1;
    VkPhysicalDevice* pds = malloc(n * sizeof(*pds));
    CHECK(vkEnumeratePhysicalDevices(instance, &n, pds));

    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDevice pd = pds[i];
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        printf("device %u: %s\n  api %u.%u.%u vendor 0x%x device 0x%x type %d\n", i, props.deviceName,
               VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
               VK_API_VERSION_PATCH(props.apiVersion), props.vendorID, props.deviceID,
               (int)props.deviceType);

        VkPhysicalDeviceFeatures feat;
        vkGetPhysicalDeviceFeatures(pd, &feat);
        printf("  textureCompressionBC=%u ETC2=%u ASTC_LDR=%u shaderFloat64=%u\n",
               feat.textureCompressionBC, feat.textureCompressionETC2, feat.textureCompressionASTC_LDR,
               feat.shaderFloat64);

        VkPhysicalDeviceFeatures2 feat2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        vkGetPhysicalDeviceFeatures2(pd, &feat2);
        printf("  features2 textureCompressionBC=%u\n", feat2.features.textureCompressionBC);

        uint32_t ne = 0;
        CHECK(vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL));
        printf("  device extensions: %u\n", ne);

        printf("  formats (optimalTilingFeatures, linear, buffer):\n");
        for (size_t k = 0; k < sizeof(kFormats) / sizeof(kFormats[0]); k++) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(pd, kFormats[k].format, &fp);
            printf("    %-28s 0x%08x 0x%08x 0x%08x\n", kFormats[k].name, fp.optimalTilingFeatures,
                   fp.linearTilingFeatures, fp.bufferFeatures);
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
        CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
        VkQueue q;
        vkGetDeviceQueue(dev, 0, 0, &q);
        CHECK(vkDeviceWaitIdle(dev));
        printf("  device create/queue/waitidle: ok\n");
        vkDestroyDevice(dev, NULL);
    }
    free(pds);
    vkDestroyInstance(instance, NULL);
    printf("done\n");
    return 0;
}
