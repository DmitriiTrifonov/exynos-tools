#include "bc_caps.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace exynos {

// Android: a value can also come from /storage/emulated/0/wrapper/exynos.conf (lines KEY=VALUE,
// '#' starts a comment), so a mode can be switched from outside the launcher's sandbox without
// editing its settings. The environment variable wins.
static std::string conf_value(const char* key) {
#ifdef __ANDROID__
    std::FILE* f = std::fopen("/storage/emulated/0/wrapper/exynos.conf", "r");
    if (!f) return {};
    std::string result;
    char line[256];
    const size_t n = std::strlen(key);
    while (std::fgets(line, sizeof line, f)) {
        if (std::strncmp(line, key, n) != 0 || line[n] != '=') continue;
        result = line + n + 1;
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ')) result.pop_back();
    }
    std::fclose(f);
    return result;
#else
    (void)key;
    return {};
#endif
}

BcMode bc_mode() {
    static const BcMode mode = [] {
        const char* env = std::getenv("EXYNOS_TOOLS_BC");
        static std::string from_conf;
        if (!env) {
            from_conf = conf_value("EXYNOS_TOOLS_BC");
            if (!from_conf.empty()) env = from_conf.c_str();
        }
        const char* v = env;
        if (!v) {
            // Loaded as the "BCn layer" of a Winlator-style launcher (docs/LAUNCHERS.md): the launcher
            // switches that layer on with ENABLE_BCN_COMPUTE=1 and knows nothing about our variables.
            const char* e = std::getenv("ENABLE_BCN_COMPUTE");
            return (e && std::strcmp(e, "1") == 0) ? BcMode::Emulate : BcMode::Off;
        }
        if (std::strcmp(v, "advertise") == 0) return BcMode::Advertise;
        if (std::strcmp(v, "advertise-force") == 0) return BcMode::AdvertiseForce;
        if (std::strcmp(v, "emulate") == 0) return BcMode::Emulate;
        if (std::strcmp(v, "emulate-force") == 0) return BcMode::EmulateForce;
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

VkFormat bc_storage_format(VkFormat bc) {
    return bc == VK_FORMAT_BC7_SRGB_BLOCK ? VK_FORMAT_R8G8B8A8_UNORM : bc_substitute(bc);
}

bool bc_decode_implemented(VkFormat bc) {
    switch (bc) {
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_BC4_SNORM_BLOCK:
        case VK_FORMAT_BC5_UNORM_BLOCK:
        case VK_FORMAT_BC5_SNORM_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:
        case VK_FORMAT_BC6H_SFLOAT_BLOCK: return true;
        default: return false;
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
