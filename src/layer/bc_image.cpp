// BC emulation, device level: image/view/buffer substitution, compute-state shadowing and the
// decode-on-upload path. See docs/BCN_DESIGN.md.

#include "bc_image.h"

#include <algorithm>
#include <cstring>

#include "bc_caps.h"
#include "shaders_spv.h"

namespace exynos {

namespace {

constexpr VkFormat kFirstBc = VK_FORMAT_BC4_UNORM_BLOCK;
constexpr VkFormat kLastBc = VK_FORMAT_BC7_SRGB_BLOCK;

bool is_emulatable(VkFormat f) { return f >= kFirstBc && f <= kLastBc; }
uint32_t bit_of(VkFormat f) { return 1u << (f - kFirstBc); }

struct Variant {
    int id;                // index into DecodeResources::pipelines
    uint32_t block_bytes;  // 8 for BC4, 16 for BC5
};

// -1 id: no decoder for this format yet.
Variant variant_of(VkFormat f) {
    switch (f) {
        case VK_FORMAT_BC4_UNORM_BLOCK: return {0, 8};
        case VK_FORMAT_BC4_SNORM_BLOCK: return {1, 8};
        case VK_FORMAT_BC5_UNORM_BLOCK: return {2, 16};
        case VK_FORMAT_BC5_SNORM_BLOCK: return {3, 16};
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK: return {4, 16};
        case VK_FORMAT_BC6H_UFLOAT_BLOCK: return {5, 16};
        case VK_FORMAT_BC6H_SFLOAT_BLOCK: return {6, 16};
        default: return {-1, 0};
    }
}

struct SpvBlob {
    const uint32_t* code;
    size_t bytes;
};
const SpvBlob kSpv[] = {
    {spv::bc4_unorm, sizeof(spv::bc4_unorm)},
    {spv::bc4_snorm, sizeof(spv::bc4_snorm)},
    {spv::bc5_unorm, sizeof(spv::bc5_unorm)},
    {spv::bc5_snorm, sizeof(spv::bc5_snorm)},
    {spv::bc7, sizeof(spv::bc7)},
    {spv::bc6h_unsigned, sizeof(spv::bc6h_unsigned)},
    {spv::bc6h_signed, sizeof(spv::bc6h_signed)},
};
constexpr int kVariantCount = 7;

struct PushBlock {
    uint32_t word_base, row_pitch, slice_pitch;
    uint32_t off_x, off_y, ext_x, ext_y, mip_x, mip_y;
};
static_assert(sizeof(PushBlock) == 36, "push block must match the shader");

std::mutex g_pd_mutex;
std::unordered_map<VkPhysicalDevice, bool> g_pd_can_emulate;

uint64_t handle_of(VkImage i) { return reinterpret_cast<uint64_t>(i); }
uint64_t handle_of(VkBuffer b) { return reinterpret_cast<uint64_t>(b); }

EmuImage* find_image(DeviceData* d, VkImage img) {
    std::lock_guard<std::mutex> lock(d->mutex);
    auto it = d->images.find(handle_of(img));
    return it == d->images.end() ? nullptr : it->second.get();
}

template <class T>
T init(VkStructureType type) {
    T s{};
    s.sType = type;
    return s;
}

bool chain_has(const void* p_next, VkStructureType type) {
    for (auto* s = static_cast<const VkBaseInStructure*>(p_next); s; s = s->pNext)
        if (s->sType == type) return true;
    return false;
}

}  // namespace

// ---- capability helpers ------------------------------------------------------------------

bool bc_pd_can_emulate(InstanceData* inst, VkPhysicalDevice pd) {
    {
        std::lock_guard<std::mutex> lock(g_pd_mutex);
        auto it = g_pd_can_emulate.find(pd);
        if (it != g_pd_can_emulate.end()) return it->second;
    }
    bool ok = false;
    if (inst->EnumerateDeviceExtensionProperties && inst->GetPhysicalDeviceFeatures) {
        uint32_t n = 0;
        inst->EnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> exts(n);
        inst->EnumerateDeviceExtensionProperties(pd, nullptr, &n, exts.data());
        bool push = false;
        for (uint32_t i = 0; i < n; i++)
            if (std::strcmp(exts[i].extensionName, "VK_KHR_push_descriptor") == 0) push = true;
        VkPhysicalDeviceFeatures f{};
        inst->GetPhysicalDeviceFeatures(pd, &f);
        ok = push && f.shaderStorageImageExtendedFormats;
    }
    std::lock_guard<std::mutex> lock(g_pd_mutex);
    g_pd_can_emulate[pd] = ok;
    return ok;
}

bool bc_emulated_format_props(InstanceData* inst, VkPhysicalDevice pd, VkFormat f,
                              const VkFormatProperties& down, VkFormatProperties* out) {
    const BcMode mode = bc_mode();
    if (mode == BcMode::Off || !is_emulatable(f)) return false;
    if (!bc_is_force(mode) && down.optimalTilingFeatures != 0) return false;  // native
    if (bc_is_emulate(mode)) {
        if (!bc_decode_implemented(f) || !bc_pd_can_emulate(inst, pd)) return false;
        VkFormatProperties st{};
        inst->GetPhysicalDeviceFormatProperties(pd, bc_storage_format(f), &st);
        if (!(st.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) return false;
    }
    VkFormatProperties sub{};
    inst->GetPhysicalDeviceFormatProperties(pd, bc_substitute(f), &sub);
    const VkFormatFeatureFlags feat = bc_emulated_features(sub.optimalTilingFeatures);
    if (!feat) return false;
    out->linearTilingFeatures = 0;
    out->optimalTilingFeatures = feat;
    out->bufferFeatures = 0;
    return true;
}

bool bc_fully_available(InstanceData* inst, VkPhysicalDevice pd) {
    for (int f = VK_FORMAT_BC1_RGB_UNORM_BLOCK; f <= VK_FORMAT_BC7_SRGB_BLOCK; f++) {
        VkFormatProperties down{}, emu{};
        inst->GetPhysicalDeviceFormatProperties(pd, static_cast<VkFormat>(f), &down);
        if (!bc_is_force(bc_mode()) && (down.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) continue;
        if (!bc_emulated_format_props(inst, pd, static_cast<VkFormat>(f), down, &emu)) return false;
    }
    return true;
}

// ---- device creation ---------------------------------------------------------------------

void DeviceCreatePatch::restore() {
    if (features2) features2->features.shaderStorageImageExtendedFormats = features2_old;
    features2 = nullptr;
}

void bc_patch_device_create(InstanceData* inst, VkPhysicalDevice pd, const VkDeviceCreateInfo* in,
                            DeviceCreatePatch* out) {
    out->info = *in;
    out->active = false;
    if (!bc_is_emulate(bc_mode())) return;
    if (!bc_pd_can_emulate(inst, pd)) {
        logf("BC emulation requested but the device lacks push descriptors or extended storage formats");
        return;
    }
    out->active = true;

    bool has_push = false;
    out->extensions.assign(in->ppEnabledExtensionNames, in->ppEnabledExtensionNames + in->enabledExtensionCount);
    for (const char* e : out->extensions)
        if (std::strcmp(e, "VK_KHR_push_descriptor") == 0) has_push = true;
    if (!has_push) out->extensions.push_back("VK_KHR_push_descriptor");
    out->info.enabledExtensionCount = static_cast<uint32_t>(out->extensions.size());
    out->info.ppEnabledExtensionNames = out->extensions.data();

    VkPhysicalDeviceFeatures2* f2 = nullptr;
    for (auto* s = static_cast<const VkBaseInStructure*>(in->pNext); s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
            f2 = const_cast<VkPhysicalDeviceFeatures2*>(reinterpret_cast<const VkPhysicalDeviceFeatures2*>(s));
    if (f2) {
        out->features2 = f2;
        out->features2_old = f2->features.shaderStorageImageExtendedFormats;
        f2->features.shaderStorageImageExtendedFormats = VK_TRUE;
    } else {
        out->features = in->pEnabledFeatures ? *in->pEnabledFeatures : VkPhysicalDeviceFeatures{};
        out->features.shaderStorageImageExtendedFormats = VK_TRUE;
        out->info.pEnabledFeatures = &out->features;
    }
}

void bc_device_created(DeviceData* d) {
    d->bc_emulated_mask = 0;
    if (!d->emulation_active) return;
    InstanceData* inst = d->instance;
    for (int f = kFirstBc; f <= kLastBc; f++) {
        VkFormatProperties down{}, emu{};
        inst->GetPhysicalDeviceFormatProperties(d->physical_device, static_cast<VkFormat>(f), &down);
        if (bc_emulated_format_props(inst, d->physical_device, static_cast<VkFormat>(f), down, &emu))
            d->bc_emulated_mask |= bit_of(static_cast<VkFormat>(f));
    }
    if (d->bc_emulated_mask == 0) {
        d->emulation_active = false;
        logf("BC emulation: no format can be emulated on this device");
        return;
    }
    VkPhysicalDeviceProperties props{};
    inst->GetPhysicalDeviceProperties(d->physical_device, &props);
    d->min_storage_alignment = std::max<VkDeviceSize>(props.limits.minStorageBufferOffsetAlignment, 4);
    d->max_storage_range = props.limits.maxStorageBufferRange;
    inst->GetPhysicalDeviceMemoryProperties(d->physical_device, &d->memory_properties);
    logf("BC emulation active (mask 0x%x)", d->bc_emulated_mask);
}

void bc_device_destroying(DeviceData* d) {
    std::lock_guard<std::mutex> lock(d->mutex);
    for (auto& kv : d->images)
        for (auto& v : kv.second->storage_views) d->DestroyImageView(d->device, v.second, nullptr);
    d->images.clear();
    DecodeResources& r = d->decode;
    for (int i = 0; i < 8; i++) {
        if (r.pipelines[i]) d->DestroyPipeline(d->device, r.pipelines[i], nullptr);
        if (r.modules[i]) d->DestroyShaderModule(d->device, r.modules[i], nullptr);
    }
    if (r.layout) d->DestroyPipelineLayout(d->device, r.layout, nullptr);
    if (r.set_layout) d->DestroyDescriptorSetLayout(d->device, r.set_layout, nullptr);
    if (r.scratch) d->DestroyBuffer(d->device, r.scratch, nullptr);
    if (r.scratch_mem) d->FreeMemory(d->device, r.scratch_mem, nullptr);
    r = DecodeResources{};
}

// ---- decode resources ----------------------------------------------------------------------

namespace {

bool ensure_decode(DeviceData* d) {
    std::lock_guard<std::mutex> lock(d->mutex);
    DecodeResources& r = d->decode;
    if (r.ready) return true;
    if (r.failed) return false;
    auto fail = [&](const char* what) {
        logf("BC emulation: failed to create %s; emulated uploads will be skipped", what);
        r.failed = true;
        return false;
    };

    VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dsl = init<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    dsl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dsl.bindingCount = 2;
    dsl.pBindings = b;
    if (d->CreateDescriptorSetLayout(d->device, &dsl, nullptr, &r.set_layout) != VK_SUCCESS)
        return fail("descriptor set layout");

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushBlock)};
    VkPipelineLayoutCreateInfo pl = init<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &r.set_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    if (d->CreatePipelineLayout(d->device, &pl, nullptr, &r.layout) != VK_SUCCESS) return fail("pipeline layout");

    for (int i = 0; i < kVariantCount; i++) {
        VkShaderModuleCreateInfo sm = init<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        sm.codeSize = kSpv[i].bytes;
        sm.pCode = kSpv[i].code;
        if (d->CreateShaderModule(d->device, &sm, nullptr, &r.modules[i]) != VK_SUCCESS) return fail("shader module");
        VkComputePipelineCreateInfo cp = init<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
        cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                    r.modules[i], "main", nullptr};
        cp.layout = r.layout;
        if (d->CreateComputePipelines(d->device, VK_NULL_HANDLE, 1, &cp, nullptr, &r.pipelines[i]) != VK_SUCCESS)
            return fail("compute pipeline");
    }

    // 16-byte scratch buffer: a transfer-stage fill after each decode makes the app's own
    // "transfer write -> consumer" barriers order after our compute work.
    VkBufferCreateInfo bi = init<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    bi.size = 16;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (d->CreateBuffer(d->device, &bi, nullptr, &r.scratch) != VK_SUCCESS) return fail("scratch buffer");
    VkMemoryRequirements mr{};
    d->GetBufferMemoryRequirements(d->device, r.scratch, &mr);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < d->memory_properties.memoryTypeCount; i++) {
        if (!(mr.memoryTypeBits & (1u << i))) continue;
        if (d->memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            type = i;
            break;
        }
        if (type == UINT32_MAX) type = i;
    }
    if (type == UINT32_MAX) return fail("scratch memory type");
    VkMemoryAllocateInfo mai = init<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = type;
    if (d->AllocateMemory(d->device, &mai, nullptr, &r.scratch_mem) != VK_SUCCESS) return fail("scratch memory");
    if (d->BindBufferMemory(d->device, r.scratch, r.scratch_mem, 0) != VK_SUCCESS) return fail("scratch bind");

    r.ready = true;
    return true;
}

VkImageView storage_view(DeviceData* d, VkImage image, EmuImage* e, uint32_t mip, uint32_t base, uint32_t count) {
    const uint64_t key = mip | (static_cast<uint64_t>(base) << 20) | (static_cast<uint64_t>(count) << 40);
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        auto it = e->storage_views.find(key);
        if (it != e->storage_views.end()) return it->second;
    }
    VkImageViewCreateInfo vi = init<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = e->storage_format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, base, count};
    VkImageView view = VK_NULL_HANDLE;
    if (d->CreateImageView(d->device, &vi, nullptr, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    std::lock_guard<std::mutex> lock(d->mutex);
    e->storage_views[key] = view;
    return view;
}

// ---- compute state shadow ------------------------------------------------------------------

void restore_state(DeviceData* d, VkCommandBuffer cb, const CbState& st) {
    if (st.compute_pipeline) d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, st.compute_pipeline);
    for (const CbState::BindCall& b : st.binds)
        d->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, b.layout, b.first,
                                 static_cast<uint32_t>(b.sets.size()), b.sets.data(),
                                 static_cast<uint32_t>(b.dynamic_offsets.size()), b.dynamic_offsets.data());
    if (st.push.hi > st.push.lo && st.push.layout)
        d->CmdPushConstants(cb, st.push.layout, st.push.stages, st.push.lo, st.push.hi - st.push.lo,
                            st.push.bytes + st.push.lo);
}

// ---- upload --------------------------------------------------------------------------------

struct Region {
    VkDeviceSize buffer_offset;
    uint32_t row_length, image_height;
    VkImageSubresourceLayers sub;
    VkOffset3D offset;
    VkExtent3D extent;
};

uint32_t div_up(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

void record_decode(DeviceData* d, VkCommandBuffer cb, VkBuffer buffer, VkImage image, VkImageLayout layout,
                   EmuImage* e, const std::vector<Region>& regions) {
    const Variant var = variant_of(e->bc_format);
    if (var.id < 0 || !ensure_decode(d)) {
        logf("BC upload skipped: no decoder for format %d", static_cast<int>(e->bc_format));
        return;
    }
    VkDeviceSize buf_size = 0;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        auto it = d->buffer_sizes.find(handle_of(buffer));
        if (it != d->buffer_sizes.end()) buf_size = it->second;
    }

    CbState saved;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        auto it = d->cmdbufs.find(cb);
        if (it != d->cmdbufs.end()) saved = it->second;
    }

    // Validate and describe the regions that will be decoded.
    struct Job {
        Region r;
        VkDeviceSize aligned_offset;
        VkDeviceSize range;
        PushBlock pc;
        uint32_t groups_x, groups_y;
    };
    std::vector<Job> jobs;
    for (const Region& r : regions) {
        if ((r.offset.x | r.offset.y) & 3 || r.extent.depth != 1 || r.offset.z != 0) {
            logf("BC upload: region with unaligned offset or depth skipped");
            continue;
        }
        const uint32_t bx = div_up(r.extent.width, 4), by = div_up(r.extent.height, 4);
        const uint32_t row_blocks = div_up(r.row_length ? r.row_length : r.extent.width, 4);
        const uint32_t rows_per_slice = div_up(r.image_height ? r.image_height : r.extent.height, 4);
        const VkDeviceSize slice_blocks = static_cast<VkDeviceSize>(row_blocks) * rows_per_slice;
        const VkDeviceSize last_block =
            (r.sub.layerCount - 1) * slice_blocks + static_cast<VkDeviceSize>(by - 1) * row_blocks + bx;
        const VkDeviceSize end = r.buffer_offset + last_block * var.block_bytes;
        if (buf_size && end > buf_size) {
            logf("BC upload: region reads past the end of the buffer, skipped");
            continue;
        }
        Job j{};
        j.r = r;
        j.aligned_offset = r.buffer_offset & ~(d->min_storage_alignment - 1);
        const VkDeviceSize rest = r.buffer_offset - j.aligned_offset;
        j.range = (buf_size ? buf_size : end) - j.aligned_offset;
        if (j.range > d->max_storage_range || (rest & 3)) {
            logf("BC upload: buffer range not addressable by the decoder, skipped");
            continue;
        }
        const uint32_t mip_w = std::max(1u, e->extent.width >> r.sub.mipLevel);
        const uint32_t mip_h = std::max(1u, e->extent.height >> r.sub.mipLevel);
        j.pc = {static_cast<uint32_t>(rest / 4),
                row_blocks,
                static_cast<uint32_t>(slice_blocks),
                static_cast<uint32_t>(r.offset.x),
                static_cast<uint32_t>(r.offset.y),
                r.extent.width,
                r.extent.height,
                mip_w,
                mip_h};
        j.groups_x = div_up(bx, 8);
        j.groups_y = div_up(by, 8);
        jobs.push_back(j);
    }
    if (jobs.empty()) return;

    std::vector<VkImageMemoryBarrier> to_general, back;
    for (const Job& j : jobs) {
        VkImageMemoryBarrier b = init<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
        b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.oldLayout = layout;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, j.r.sub.mipLevel, 1, j.r.sub.baseArrayLayer,
                              j.r.sub.layerCount};
        to_general.push_back(b);
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout = layout;
        back.push_back(b);
    }
    VkMemoryBarrier mb = init<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                          nullptr, static_cast<uint32_t>(to_general.size()), to_general.data());

    d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d->decode.pipelines[var.id]);
    for (const Job& j : jobs) {
        VkImageView view = storage_view(d, image, e, j.r.sub.mipLevel, j.r.sub.baseArrayLayer, j.r.sub.layerCount);
        if (!view) {
            logf("BC upload: could not create a storage view");
            continue;
        }
        VkDescriptorBufferInfo bi{buffer, j.aligned_offset, j.range};
        VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[2] = {};
        w[0].sType = w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[0].dstBinding = 0;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[0].pBufferInfo = &bi;
        w[1].dstBinding = 1;
        w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[1].pImageInfo = &ii;
        d->CmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d->decode.layout, 0, 2, w);
        d->CmdPushConstants(cb, d->decode.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushBlock), &j.pc);
        d->CmdDispatch(cb, j.groups_x, j.groups_y, j.r.sub.layerCount);
    }

    d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                          nullptr, static_cast<uint32_t>(back.size()), back.data());
    d->CmdFillBuffer(cb, d->decode.scratch, 0, 4, 0);
    restore_state(d, cb, saved);
}

void copy_buffer_to_image(DeviceData* d, VkCommandBuffer cb, VkBuffer buf, VkImage img, VkImageLayout layout,
                          const std::vector<Region>& regions, EmuImage* e) {
    record_decode(d, cb, buf, img, layout, e, regions);
}

// ---- hooks ---------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL hook_CreateImage(VkDevice device, const VkImageCreateInfo* ci,
                                                const VkAllocationCallbacks* alloc, VkImage* out) {
    DeviceData* d = find_device(device);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    if (!d->emulation_active || !is_emulatable(ci->format) || !(d->bc_emulated_mask & bit_of(ci->format)))
        return d->CreateImage(device, ci, alloc, out);

    const VkImageUsageFlags allowed =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (ci->imageType != VK_IMAGE_TYPE_2D || ci->tiling != VK_IMAGE_TILING_OPTIMAL || (ci->usage & ~allowed) ||
        ci->samples != VK_SAMPLE_COUNT_1_BIT) {
        logf("vkCreateImage: unsupported emulated BC image (type %d tiling %d usage 0x%x)",
             static_cast<int>(ci->imageType), static_cast<int>(ci->tiling), ci->usage);
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    auto emu = std::make_unique<EmuImage>();
    emu->bc_format = ci->format;
    emu->storage_format = bc_storage_format(ci->format);
    emu->sample_format = bc_substitute(ci->format);
    emu->mip_levels = ci->mipLevels;
    emu->array_layers = ci->arrayLayers;
    emu->extent = ci->extent;

    VkImageCreateInfo n = *ci;
    n.format = emu->storage_format;
    n.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    n.flags = (n.flags & ~VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT) | VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;

    // A format list from the app names BC view formats; replace it with the substitutes.
    std::vector<VkFormat> list{emu->storage_format};
    if (emu->sample_format != emu->storage_format) list.push_back(emu->sample_format);
    VkImageFormatListCreateInfo* fl = nullptr;
    for (auto* s = static_cast<const VkBaseInStructure*>(ci->pNext); s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO)
            fl = const_cast<VkImageFormatListCreateInfo*>(reinterpret_cast<const VkImageFormatListCreateInfo*>(s));
    uint32_t old_count = 0;
    const VkFormat* old_formats = nullptr;
    if (fl) {
        old_count = fl->viewFormatCount;
        old_formats = fl->pViewFormats;
        fl->viewFormatCount = static_cast<uint32_t>(list.size());
        fl->pViewFormats = list.data();
    }
    VkResult r = d->CreateImage(device, &n, alloc, out);
    if (fl) {
        fl->viewFormatCount = old_count;
        fl->pViewFormats = old_formats;
    }
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->images[handle_of(*out)] = std::move(emu);
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL hook_DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* alloc) {
    DeviceData* d = find_device(device);
    if (!d) return;
    if (d->emulation_active && image) {
        std::unique_ptr<EmuImage> e;
        {
            std::lock_guard<std::mutex> lock(d->mutex);
            auto it = d->images.find(handle_of(image));
            if (it != d->images.end()) {
                e = std::move(it->second);
                d->images.erase(it);
            }
        }
        if (e)
            for (auto& v : e->storage_views) d->DestroyImageView(device, v.second, nullptr);
    }
    d->DestroyImage(device, image, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL hook_CreateImageView(VkDevice device, const VkImageViewCreateInfo* ci,
                                                    const VkAllocationCallbacks* alloc, VkImageView* out) {
    DeviceData* d = find_device(device);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    EmuImage* e = d->emulation_active ? find_image(d, ci->image) : nullptr;
    if (!e) return d->CreateImageView(device, ci, alloc, out);

    VkImageViewCreateInfo n = *ci;
    if (is_emulatable(ci->format)) n.format = bc_substitute(ci->format);
    // The image has STORAGE usage but sampled views (e.g. sRGB) need not support it.
    VkImageViewUsageCreateInfo usage = init<VkImageViewUsageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO);
    usage.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!chain_has(ci->pNext, VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO)) {
        usage.pNext = ci->pNext;
        n.pNext = &usage;
    }
    return d->CreateImageView(device, &n, alloc, out);
}

VKAPI_ATTR VkResult VKAPI_CALL hook_CreateBuffer(VkDevice device, const VkBufferCreateInfo* ci,
                                                 const VkAllocationCallbacks* alloc, VkBuffer* out) {
    DeviceData* d = find_device(device);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    if (!d->emulation_active) return d->CreateBuffer(device, ci, alloc, out);
    VkBufferCreateInfo n = *ci;
    if (ci->usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) n.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VkResult r = d->CreateBuffer(device, &n, alloc, out);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->buffer_sizes[handle_of(*out)] = ci->size;
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL hook_DestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* alloc) {
    DeviceData* d = find_device(device);
    if (!d) return;
    if (d->emulation_active && buffer) {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->buffer_sizes.erase(handle_of(buffer));
    }
    d->DestroyBuffer(device, buffer, alloc);
}

// -- command buffer bookkeeping --

VKAPI_ATTR VkResult VKAPI_CALL hook_AllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* ai,
                                                           VkCommandBuffer* out) {
    DeviceData* d = find_device(device);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = d->AllocateCommandBuffers(device, ai, out);
    if (r == VK_SUCCESS && d->emulation_active) {
        std::lock_guard<std::mutex> lock(d->mutex);
        auto& pool = d->pool_cmdbufs[reinterpret_cast<uint64_t>(ai->commandPool)];
        for (uint32_t i = 0; i < ai->commandBufferCount; i++) {
            d->cmdbufs[out[i]] = CbState{};
            pool.push_back(out[i]);
        }
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL hook_FreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t n,
                                                   const VkCommandBuffer* cbs) {
    DeviceData* d = find_device(device);
    if (!d) return;
    if (d->emulation_active) {
        std::lock_guard<std::mutex> lock(d->mutex);
        auto& list = d->pool_cmdbufs[reinterpret_cast<uint64_t>(pool)];
        for (uint32_t i = 0; i < n; i++) {
            d->cmdbufs.erase(cbs[i]);
            list.erase(std::remove(list.begin(), list.end(), cbs[i]), list.end());
        }
    }
    d->FreeCommandBuffers(device, pool, n, cbs);
}

VKAPI_ATTR void VKAPI_CALL hook_DestroyCommandPool(VkDevice device, VkCommandPool pool,
                                                   const VkAllocationCallbacks* alloc) {
    DeviceData* d = find_device(device);
    if (!d) return;
    if (d->emulation_active) {
        std::lock_guard<std::mutex> lock(d->mutex);
        auto it = d->pool_cmdbufs.find(reinterpret_cast<uint64_t>(pool));
        if (it != d->pool_cmdbufs.end()) {
            for (VkCommandBuffer cb : it->second) d->cmdbufs.erase(cb);
            d->pool_cmdbufs.erase(it);
        }
    }
    d->DestroyCommandPool(device, pool, alloc);
}

void reset_cb_state(DeviceData* d, VkCommandBuffer cb) {
    std::lock_guard<std::mutex> lock(d->mutex);
    auto it = d->cmdbufs.find(cb);
    if (it != d->cmdbufs.end()) it->second = CbState{};
}

VKAPI_ATTR VkResult VKAPI_CALL hook_ResetCommandPool(VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags f) {
    DeviceData* d = find_device(device);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    if (d->emulation_active) {
        std::lock_guard<std::mutex> lock(d->mutex);
        auto it = d->pool_cmdbufs.find(reinterpret_cast<uint64_t>(pool));
        if (it != d->pool_cmdbufs.end())
            for (VkCommandBuffer cb : it->second) d->cmdbufs[cb] = CbState{};
    }
    return d->ResetCommandPool(device, pool, f);
}

VKAPI_ATTR VkResult VKAPI_CALL hook_ResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags f) {
    DeviceData* d = find_device(cb);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    if (d->emulation_active) reset_cb_state(d, cb);
    return d->ResetCommandBuffer(cb, f);
}

VKAPI_ATTR VkResult VKAPI_CALL hook_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* bi) {
    DeviceData* d = find_device(cb);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    if (d->emulation_active) reset_cb_state(d, cb);
    return d->BeginCommandBuffer(cb, bi);
}

// -- compute state tracking --

VKAPI_ATTR void VKAPI_CALL hook_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    DeviceData* d = find_device(cb);
    if (!d) return;
    if (d->emulation_active && bp == VK_PIPELINE_BIND_POINT_COMPUTE) {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->cmdbufs[cb].compute_pipeline = p;
    }
    d->CmdBindPipeline(cb, bp, p);
}

VKAPI_ATTR void VKAPI_CALL hook_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout,
                                                      uint32_t first, uint32_t n, const VkDescriptorSet* sets,
                                                      uint32_t dyn_n, const uint32_t* dyn) {
    DeviceData* d = find_device(cb);
    if (!d) return;
    if (d->emulation_active && bp == VK_PIPELINE_BIND_POINT_COMPUTE) {
        std::lock_guard<std::mutex> lock(d->mutex);
        CbState& st = d->cmdbufs[cb];
        st.binds.erase(std::remove_if(st.binds.begin(), st.binds.end(),
                                      [&](const CbState::BindCall& b) {
                                          return b.first >= first &&
                                                 b.first + b.sets.size() <= static_cast<size_t>(first) + n;
                                      }),
                       st.binds.end());
        CbState::BindCall call;
        call.layout = layout;
        call.first = first;
        call.sets.assign(sets, sets + n);
        if (dyn_n) call.dynamic_offsets.assign(dyn, dyn + dyn_n);
        st.binds.push_back(std::move(call));
    }
    d->CmdBindDescriptorSets(cb, bp, layout, first, n, sets, dyn_n, dyn);
}

VKAPI_ATTR void VKAPI_CALL hook_CmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages,
                                                 uint32_t offset, uint32_t size, const void* values) {
    DeviceData* d = find_device(cb);
    if (!d) return;
    if (d->emulation_active && (stages & VK_SHADER_STAGE_COMPUTE_BIT) && offset + size <= sizeof(CbState::Push::bytes)) {
        std::lock_guard<std::mutex> lock(d->mutex);
        CbState::Push& p = d->cmdbufs[cb].push;
        std::memcpy(p.bytes + offset, values, size);
        p.layout = layout;
        p.stages = stages;
        p.lo = std::min(p.lo, offset);
        p.hi = std::max(p.hi, offset + size);
    }
    d->CmdPushConstants(cb, layout, stages, offset, size, values);
}

// -- uploads --

VKAPI_ATTR void VKAPI_CALL hook_CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer buf, VkImage img, VkImageLayout layout,
                                                     uint32_t n, const VkBufferImageCopy* regions) {
    DeviceData* d = find_device(cb);
    if (!d) return;
    EmuImage* e = d->emulation_active ? find_image(d, img) : nullptr;
    if (!e) {
        d->CmdCopyBufferToImage(cb, buf, img, layout, n, regions);
        return;
    }
    std::vector<Region> rs;
    for (uint32_t i = 0; i < n; i++) {
        const VkBufferImageCopy& c = regions[i];
        rs.push_back({c.bufferOffset, c.bufferRowLength, c.bufferImageHeight, c.imageSubresource, c.imageOffset,
                      c.imageExtent});
    }
    copy_buffer_to_image(d, cb, buf, img, layout, rs, e);
}

VKAPI_ATTR void VKAPI_CALL hook_CmdCopyBufferToImage2(VkCommandBuffer cb, const VkCopyBufferToImageInfo2* info) {
    DeviceData* d = find_device(cb);
    if (!d) return;
    EmuImage* e = d->emulation_active ? find_image(d, info->dstImage) : nullptr;
    if (!e) {
        d->CmdCopyBufferToImage2(cb, info);
        return;
    }
    std::vector<Region> rs;
    for (uint32_t i = 0; i < info->regionCount; i++) {
        const VkBufferImageCopy2& c = info->pRegions[i];
        rs.push_back({c.bufferOffset, c.bufferRowLength, c.bufferImageHeight, c.imageSubresource, c.imageOffset,
                      c.imageExtent});
    }
    copy_buffer_to_image(d, cb, info->srcBuffer, info->dstImage, info->dstImageLayout, rs, e);
}

struct HookEntry {
    const char* name;
    PFN_vkVoidFunction fn;
    bool hot;
};

#define HOOK(n, hot_) {"vk" #n, reinterpret_cast<PFN_vkVoidFunction>(hook_##n), hot_}
const HookEntry kHooks[] = {
    HOOK(CreateImage, false),         HOOK(DestroyImage, false),        HOOK(CreateImageView, false),
    HOOK(CreateBuffer, false),        HOOK(DestroyBuffer, false),       HOOK(AllocateCommandBuffers, true),
    HOOK(FreeCommandBuffers, true),   HOOK(DestroyCommandPool, true),   HOOK(ResetCommandPool, true),
    HOOK(ResetCommandBuffer, true),   HOOK(BeginCommandBuffer, true),   HOOK(CmdBindPipeline, true),
    HOOK(CmdBindDescriptorSets, true), HOOK(CmdPushConstants, true),    HOOK(CmdCopyBufferToImage, true),
    HOOK(CmdCopyBufferToImage2, true),
};
#undef HOOK

}  // namespace

PFN_vkVoidFunction bc_device_proc(const char* name, bool* hot) {
    for (const HookEntry& h : kHooks) {
        if (std::strcmp(name, h.name) == 0) {
            *hot = h.hot;
            return h.fn;
        }
    }
    if (std::strcmp(name, "vkCmdCopyBufferToImage2KHR") == 0) {
        *hot = true;
        return reinterpret_cast<PFN_vkVoidFunction>(hook_CmdCopyBufferToImage2);
    }
    return nullptr;
}

}  // namespace exynos
