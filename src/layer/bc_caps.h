// BCn capability policy: which BC formats this layer emulates, what they are backed by, and
// what features it may honestly advertise for them (phase 4: reporting only, no image creation yet).
#pragma once

#include <vulkan/vulkan.h>

#include <string>

namespace exynos {

// EXYNOS_TOOLS_BC selects the behaviour:
//   off (default)     pure pass-through, nothing is advertised or changed
//   advertise         advertise BC4/BC5/BC6H/BC7 where the driver does not support them
//   advertise-force   same, but pretend the driver supports none of them (host testing)
//   emulate           really decode, for the formats implemented so far (bc_decode_implemented)
//   emulate-force     same, but pretend the driver supports none of BC4-BC7 (host testing)
enum class BcMode { Off, Advertise, AdvertiseForce, Emulate, EmulateForce };

inline bool bc_is_emulate(BcMode m) { return m == BcMode::Emulate || m == BcMode::EmulateForce; }
inline bool bc_is_force(BcMode m) { return m == BcMode::AdvertiseForce || m == BcMode::EmulateForce; }

BcMode bc_mode();

// Value of a layer setting: the environment variable, else (Android) the line KEY=VALUE of
// /storage/emulated/0/wrapper/exynos.conf. Empty if unset.
std::string config_value(const char* key);

// Whether device extension `name` is hidden from the app (EXYNOS_TOOLS_HIDE_EXT, comma-separated
// short names: present_wait, present_id).
bool ext_hidden(const char* name);

// Uncompressed format that backs an emulated BC format; VK_FORMAT_UNDEFINED if the format is not
// emulated (BC1-BC3 are expected to be native and are never emulated).
VkFormat bc_substitute(VkFormat bc);

// Format of the backing image of an emulated BC image (what the decoder writes). Same as
// bc_substitute() except for BC7_SRGB, which is stored as UNORM and sampled through an sRGB view.
VkFormat bc_storage_format(VkFormat bc);

// Whether the decode shader for this format exists yet.
bool bc_decode_implemented(VkFormat bc);

// Features that may be advertised for an emulated BC format, given the optimal-tiling features
// of its substitute. Returns 0 if the substitute cannot back the format.
VkFormatFeatureFlags bc_emulated_features(VkFormatFeatureFlags substitute_optimal);

// Usage an emulated BC image may be created with (sampling and transfers only).
bool bc_usage_supported(VkImageUsageFlags usage);

}  // namespace exynos
