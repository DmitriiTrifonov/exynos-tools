#include "bc_caps.h"

#include <cstdlib>
#include <cstring>

namespace exynos {

BcMode bc_mode() {
    static const BcMode mode = [] {
        const char* v = std::getenv("EXYNOS_TOOLS_BC");
        if (!v) return BcMode::Off;
        if (std::strcmp(v, "advertise") == 0) return BcMode::Advertise;
        if (std::strcmp(v, "advertise-force") == 0) return BcMode::AdvertiseForce;
        return BcMode::Off;
    }();
    return mode;
}

VkFormat bc_substitute(VkFormat bc) {
    switch (bc) {
        case VK_FORMAT_BC4_UNORM_BLOCK: return VK_FORMAT_R8_UNORM;
        case VK_FORMAT_BC4_SNORM_BLOCK: return VK_FORMAT_R8_SNORM;
        case VK_FORMAT_BC5_UNORM_BLOCK: return VK_FORMAT_R8G8_UNORM;
        case VK_FORMAT_BC5_SNORM_BLOCK: return VK_FORMAT_R8G8_SNORM;
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:
        case VK_FORMAT_BC6H_SFLOAT_BLOCK: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case VK_FORMAT_BC7_UNORM_BLOCK: return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_BC7_SRGB_BLOCK: return VK_FORMAT_R8G8B8A8_SRGB;
        default: return VK_FORMAT_UNDEFINED;
    }
}

VkFormatFeatureFlags bc_emulated_features(VkFormatFeatureFlags sub) {
    // To upload we must be able to write the substitute by transfer, and to use the image we must
    // be able to sample it.
    const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((sub & required) != required) return 0;
    const VkFormatFeatureFlags allowed =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    return sub & allowed;
}

bool bc_usage_supported(VkImageUsageFlags usage) {
    const VkImageUsageFlags allowed =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return usage != 0 && (usage & ~allowed) == 0;
}

}  // namespace exynos
