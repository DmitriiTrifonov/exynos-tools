// Image/buffer/command hooks that implement BC emulation on the device level.
#pragma once

#include <vulkan/vulkan.h>

#include <vector>

#include "layer_state.h"

namespace exynos {

// Whether the physical device offers what the decoder needs (push descriptors, extended storage
// formats). Cached per physical device.
bool bc_pd_can_emulate(InstanceData* inst, VkPhysicalDevice pd);

// Whether BC format `f` is reported as supported-by-emulation on `pd`. `down` is what the driver
// says; on success `out` holds the properties to report.
bool bc_emulated_format_props(InstanceData* inst, VkPhysicalDevice pd, VkFormat f, const VkFormatProperties& down,
                              VkFormatProperties* out);

// True if every BC format is usable on `pd`, natively or by emulation.
bool bc_fully_available(InstanceData* inst, VkPhysicalDevice pd);

// A (possibly) modified copy of the app's VkDeviceCreateInfo that enables what the decoder needs.
struct DeviceCreatePatch {
    bool active = false;
    VkDeviceCreateInfo info{};
    std::vector<const char*> extensions;
    VkPhysicalDeviceFeatures features{};
    VkPhysicalDeviceFeatures2* features2 = nullptr;  // patched in place, restored by restore()
    VkBool32 features2_old = VK_FALSE;
    void restore();
};
void bc_patch_device_create(InstanceData* inst, VkPhysicalDevice pd, const VkDeviceCreateInfo* in,
                            DeviceCreatePatch* out);

// Called right after the device was created / right before it is destroyed.
void bc_device_created(DeviceData* d);
void bc_device_destroying(DeviceData* d);

// Device-level hooks. `hot` is set for per-command hooks that are only worth installing when
// emulation is active for the device.
PFN_vkVoidFunction bc_device_proc(const char* name, bool* hot);

}  // namespace exynos
