// bc_upload_test: uploads random BC4/BC5/BC7/BC6H data to emulated images through vkCmdCopyBufferToImage,
// reads the decoded texels back and compares them with a reference decoder (bcdec, run on the CPU).
// Run with the layer embedded and emulation on, e.g.
//   EXYNOS_TOOLS_BC=emulate EXYNOS_PROBE_LAYER=build/libVkLayer_exynos_tools.so build/bc_upload_test
// (emulate-force on drivers that already have BC). Exit status 0 = all cases within tolerance.

#define BCDEC_BC4BC5_PRECISE
#define BCDEC_IMPLEMENTATION
#include "../third_party/bcdec/bcdec.h"

#include <math.h>

#include "harness.h"

typedef struct {
    VkFormat format;
    const char* name;
    int channels;  // texel channels of the backing image (BC4 1, BC5 2, BC7 and BC6H 4)
    int is_signed;
    int texel_bytes;  // bytes per texel of the backing image
    VkFormat backing;  // what the emulated image is stored as
} Fmt;

static const Fmt kFmts[] = {
    {VK_FORMAT_BC4_UNORM_BLOCK, "BC4_UNORM", 1, 0, 1, VK_FORMAT_R8_UNORM},
    {VK_FORMAT_BC4_SNORM_BLOCK, "BC4_SNORM", 1, 1, 1, VK_FORMAT_R8_SNORM},
    {VK_FORMAT_BC5_UNORM_BLOCK, "BC5_UNORM", 2, 0, 2, VK_FORMAT_R8G8_UNORM},
    {VK_FORMAT_BC5_SNORM_BLOCK, "BC5_SNORM", 2, 1, 2, VK_FORMAT_R8G8_SNORM},
    {VK_FORMAT_BC7_UNORM_BLOCK, "BC7_UNORM", 4, 0, 4, VK_FORMAT_R8G8B8A8_UNORM},
    {VK_FORMAT_BC7_SRGB_BLOCK, "BC7_SRGB", 4, 0, 4, VK_FORMAT_R8G8B8A8_UNORM},
    {VK_FORMAT_BC6H_UFLOAT_BLOCK, "BC6H_UFLOAT", 4, 0, 8, VK_FORMAT_R16G16B16A16_SFLOAT},
    {VK_FORMAT_BC6H_SFLOAT_BLOCK, "BC6H_SFLOAT", 4, 1, 8, VK_FORMAT_R16G16B16A16_SFLOAT},
};
#define IS_BC6H(f) ((f)->texel_bytes == 8)

typedef struct { uint32_t w, h, mips, layers; } Case;
static const Case kCases[] = {
    {64, 64, 3, 1},   // plain
    {40, 28, 2, 2},   // non-square, odd mip size (20x14), array
    {8, 8, 4, 1},     // mips smaller than one block (2x2, 1x1)
    {100, 36, 3, 3},  // not a multiple of 8 blocks, array
};

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}
static uint32_t maxu(uint32_t a, uint32_t b) { return a > b ? a : b; }
static uint32_t div4(uint32_t a) { return (a + 3) / 4; }

static int convert(float f, int is_signed) {
    if (is_signed) { int v = (int)lroundf(f * 127.0f); return v < -127 ? -127 : v > 127 ? 127 : v; }
    int v = (int)lroundf(f * 255.0f);
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

static int run_case(HCtx* c, const Fmt* f, const Case* cs) {
    PFN_vkCreateImage CreateImage = H_DEV(c->device, vkCreateImage);
    uint32_t bb = f->channels == 1 ? 8 : 16;  // block bytes: BC4 8, BC5/BC7/BC6H 16

    // Layout of the staging data: per mip, one slice per layer, tightly packed.
    VkDeviceSize src_off[16], dst_off[16], src_total = 0, dst_total = 0;
    for (uint32_t m = 0; m < cs->mips; m++) {
        uint32_t mw = maxu(1, cs->w >> m), mh = maxu(1, cs->h >> m);
        src_off[m] = src_total;
        src_total += (VkDeviceSize)div4(mw) * div4(mh) * bb * cs->layers;
        dst_off[m] = dst_total;
        dst_total += (VkDeviceSize)mw * mh * f->texel_bytes * cs->layers;
        dst_total = (dst_total + 3) & ~3ull;
    }

    VkBuffer sbuf, rbuf;
    VkDeviceMemory smem, rmem;
    h_make_buffer(c, src_total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &sbuf, &smem);
    h_make_buffer(c, dst_total, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &rbuf, &rmem);
    uint8_t* src;
    H_CHECK(H_DEV(c->device, vkMapMemory)(c->device, smem, 0, VK_WHOLE_SIZE, 0, (void**)&src));
    for (VkDeviceSize i = 0; i < src_total; i++) src[i] = (uint8_t)rnd();
    if (IS_BC6H(f)) {
        // Spread the BC6H modes evenly: the 14 valid mode codes and the 4 reserved ones.
        static const uint8_t codes[18][2] = {{0x00, 3}, {0x01, 3}, {0x02, 31}, {0x06, 31}, {0x0a, 31}, {0x0e, 31},
                                             {0x12, 31}, {0x16, 31}, {0x1a, 31}, {0x1e, 31}, {0x03, 31}, {0x07, 31},
                                             {0x0b, 31}, {0x0f, 31}, {0x13, 31}, {0x17, 31}, {0x1b, 31}, {0x1f, 31}};
        for (VkDeviceSize blk = 0; blk < src_total / 16; blk++) {
            uint8_t* p = src + blk * 16;
            const uint8_t* c = codes[blk % 18];
            p[0] = (uint8_t)((p[0] & ~c[1]) | c[0]);
        }
    } else if (f->channels == 4) {
        // Spread the BC7 modes evenly (mode m = lowest set bit of byte 0; 8 = reserved, decodes to 0).
        for (VkDeviceSize blk = 0; blk < src_total / 16; blk++) {
            uint32_t m = (uint32_t)(blk % 9);
            uint8_t* p = src + blk * 16;
            p[0] = m == 8 ? 0 : (uint8_t)((p[0] & ~((2u << m) - 1u)) | (1u << m));
        }
    }

    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                             .format = f->format, .extent = {cs->w, cs->h, 1}, .mipLevels = cs->mips,
                             .arrayLayers = cs->layers, .samples = VK_SAMPLE_COUNT_1_BIT,
                             .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    VkResult cr = CreateImage(c->device, &ici, NULL, &img);
    if (cr != VK_SUCCESS) { printf("FAIL %s %ux%u: vkCreateImage = %d\n", f->name, cs->w, cs->h, (int)cr); return 1; }
    VkMemoryRequirements mr;
    H_DEV(c->device, vkGetImageMemoryRequirements)(c->device, img, &mr);
    uint32_t t = h_mem_type(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t == UINT32_MAX) t = h_mem_type(c, mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t};
    VkDeviceMemory imem;
    H_CHECK(H_DEV(c->device, vkAllocateMemory)(c->device, &mai, NULL, &imem));
    H_CHECK(H_DEV(c->device, vkBindImageMemory)(c->device, img, imem, 0));

    // A sampled view in the BC format itself (BC7_SRGB becomes an sRGB view of the UNORM backing image).
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY, .format = f->format,
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, cs->mips, 0, cs->layers}};
    VkImageView view;
    VkResult vr = H_DEV(c->device, vkCreateImageView)(c->device, &vci, NULL, &view);
    if (vr != VK_SUCCESS) { printf("FAIL %s: vkCreateImageView = %d\n", f->name, (int)vr); return 1; }
    H_DEV(c->device, vkDestroyImageView)(c->device, view, NULL);

    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = c->family};
    VkCommandPool pool;
    H_CHECK(H_DEV(c->device, vkCreateCommandPool)(c->device, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb;
    H_CHECK(H_DEV(c->device, vkAllocateCommandBuffers)(c->device, &cai, &cb));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    H_CHECK(H_DEV(c->device, vkBeginCommandBuffer)(cb, &bi));

    PFN_vkCmdPipelineBarrier Barrier = H_DEV(c->device, vkCmdPipelineBarrier);
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, cs->mips, 0, cs->layers};
    VkImageMemoryBarrier b1 = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = img, .subresourceRange = all};
    Barrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);

    VkBufferImageCopy up[16];
    for (uint32_t m = 0; m < cs->mips; m++) {
        up[m] = (VkBufferImageCopy){.bufferOffset = src_off[m],
                                    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, cs->layers},
                                    .imageExtent = {maxu(1, cs->w >> m), maxu(1, cs->h >> m), 1}};
    }
    H_DEV(c->device, vkCmdCopyBufferToImage)(cb, sbuf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, cs->mips, up);

    // The app-style barrier: transfer write -> transfer read (the readback below).
    VkImageMemoryBarrier b2 = b1;
    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    Barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);

    VkBufferImageCopy down[16];
    for (uint32_t m = 0; m < cs->mips; m++) {
        down[m] = (VkBufferImageCopy){.bufferOffset = dst_off[m],
                                      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, cs->layers},
                                      .imageExtent = {maxu(1, cs->w >> m), maxu(1, cs->h >> m), 1}};
    }
    H_DEV(c->device, vkCmdCopyImageToBuffer)(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, cs->mips, down);
    H_CHECK(H_DEV(c->device, vkEndCommandBuffer)(cb));

    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    H_CHECK(H_DEV(c->device, vkQueueSubmit)(c->queue, 1, &si, VK_NULL_HANDLE));
    H_CHECK(H_DEV(c->device, vkQueueWaitIdle)(c->queue));

    uint8_t* out;
    H_CHECK(H_DEV(c->device, vkMapMemory)(c->device, rmem, 0, VK_WHOLE_SIZE, 0, (void**)&out));
    long bad = 0, total = 0;
    int maxdiff = 0;
    for (uint32_t m = 0; m < cs->mips; m++) {
        uint32_t mw = maxu(1, cs->w >> m), mh = maxu(1, cs->h >> m);
        uint32_t bx = div4(mw), by = div4(mh);
        for (uint32_t l = 0; l < cs->layers; l++) {
            const uint8_t* blocks = src + src_off[m] + (VkDeviceSize)l * bx * by * bb;
            const uint8_t* got = out + dst_off[m] + (VkDeviceSize)l * mw * mh * f->texel_bytes;
            for (uint32_t y = 0; y < by; y++)
                for (uint32_t x = 0; x < bx; x++) {
                    float ref[16 * 2];
                    uint8_t ref7[16 * 4];
                    uint16_t ref6[16 * 3];
                    const uint8_t* blk = blocks + ((VkDeviceSize)y * bx + x) * bb;
                    if (f->channels == 1) bcdec_bc4_float(blk, ref, 4, f->is_signed);
                    else if (f->channels == 2) bcdec_bc5_float(blk, ref, 4 * 2, f->is_signed);
                    else if (IS_BC6H(f)) bcdec_bc6h_half(blk, ref6, 4 * 3, f->is_signed);
                    else bcdec_bc7(blk, ref7, 4 * 4);
                    for (int j = 0; j < 4; j++)
                        for (int i = 0; i < 4; i++) {
                            uint32_t px = x * 4 + i, py = y * 4 + j;
                            if (px >= mw || py >= mh) continue;
                            if (IS_BC6H(f)) {
                                const uint16_t* gp = (const uint16_t*)(got + ((VkDeviceSize)py * mw + px) * 8);
                                for (int ch = 0; ch < 4; ch++) {
                                    int want = ch < 3 ? ref6[(j * 4 + i) * 3 + ch] : 0x3c00;  // alpha = 1.0
                                    int v = gp[ch];
                                    int diff = v == want ? 0 : 999;  // half bit patterns must match exactly
                                    if (diff > maxdiff) maxdiff = diff;
                                    if (diff && bad < 4)
                                        printf("  mismatch mip %u layer %u texel (%u,%u) ch %d: got 0x%04x want 0x%04x, block %02x %02x ...\n",
                                               m, l, px, py, ch, v, want, blk[0], blk[1]);
                                    if (diff) bad++;
                                    total++;
                                }
                                continue;
                            }
                            for (int ch = 0; ch < f->channels; ch++) {
                                int want = f->channels == 4 ? ref7[(j * 4 + i) * 4 + ch]
                                                            : convert(ref[(j * 4 + i) * f->channels + ch], f->is_signed);
                                int v = got[((VkDeviceSize)py * mw + px) * f->channels + ch];
                                if (f->is_signed) v = (int8_t)v;
                                int diff = abs(v - want);
                                if (diff > maxdiff) maxdiff = diff;
                                if (diff > 1 && bad < 4)
                                    printf("  mismatch mip %u layer %u texel (%u,%u) ch %d: got %d want %d, block %02x %02x ...\n",
                                           m, l, px, py, ch, v, want, blk[0], blk[1]);
                                if (diff > 1) bad++;
                                total++;
                            }
                        }
                }
        }
    }
    printf("%s %-10s %3ux%-3u mips=%u layers=%u: %ld texel values, max diff %d, %ld outside tolerance\n",
           bad ? "FAIL" : "ok  ", f->name, cs->w, cs->h, cs->mips, cs->layers, total, maxdiff, bad);

    H_DEV(c->device, vkUnmapMemory)(c->device, smem);
    H_DEV(c->device, vkUnmapMemory)(c->device, rmem);
    H_DEV(c->device, vkDestroyCommandPool)(c->device, pool, NULL);
    H_DEV(c->device, vkDestroyImage)(c->device, img, NULL);
    H_DEV(c->device, vkFreeMemory)(c->device, imem, NULL);
    H_DEV(c->device, vkDestroyBuffer)(c->device, sbuf, NULL);
    H_DEV(c->device, vkFreeMemory)(c->device, smem, NULL);
    H_DEV(c->device, vkDestroyBuffer)(c->device, rbuf, NULL);
    H_DEV(c->device, vkFreeMemory)(c->device, rmem, NULL);
    return bad != 0;
}

int main(void) {
    const char* mode = getenv("EXYNOS_TOOLS_BC");
    if (!getenv("EXYNOS_PROBE_LAYER") || !mode || strncmp(mode, "emulate", 7) != 0) {
        printf("SKIP: needs EXYNOS_PROBE_LAYER and EXYNOS_TOOLS_BC=emulate|emulate-force\n");
        return 77;
    }
    HCtx c;
    h_init(&c);
    int failed = 0;
    for (size_t i = 0; i < sizeof(kFmts) / sizeof(kFmts[0]); i++)
        for (size_t k = 0; k < sizeof(kCases) / sizeof(kCases[0]); k++) failed += run_case(&c, &kFmts[i], &kCases[k]);
    H_DEV(c.device, vkDestroyDevice)(c.device, NULL);
    c.DestroyInstance(c.instance, NULL);
    printf(failed ? "FAILED: %d case(s)\n" : "all cases passed\n", failed);
    return failed ? 1 : 0;
}
