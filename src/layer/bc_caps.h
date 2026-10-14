// BCn capability policy: which BC formats this layer emulates, what they are backed by, and
// what features it may honestly advertise for them (phase 4: reporting only, no image creation yet).
#pragma once

#include <vulkan/vulkan.h>

namespace exynos {

// EXYNOS_TOOLS_BC selects the behaviour:
//   off (default)     pure pass-through, nothing is advertised or changed
//   advertise         advertise BC4/BC5/BC6H/BC7 where the driver does not support them
//   advertise-force   same, but pretend the driver supports none of them (host testing)
enum class BcMode { Off, Advertise, AdvertiseForce };

BcMode bc_mode();

// Uncompressed format that backs an emulated BC format; VK_FORMAT_UNDEFINED if the format is not
// emulated (BC1-BC3 are expected to be native and are never emulated).
VkFormat bc_substitute(VkFormat bc);

// Features that may be advertised for an emulated BC format, given the optimal-tiling features
// of its substitute. Returns 0 if the substitute cannot back the format.
VkFormatFeatureFlags bc_emulated_features(VkFormatFeatureFlags substitute_optimal);

// Usage an emulated BC image may be created with (sampling and transfers only).
bool bc_usage_supported(VkImageUsageFlags usage);

}  // namespace exynos
