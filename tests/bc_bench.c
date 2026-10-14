// bc_bench: measures how fast uploads of BC data are with the layer. For every format it runs
//   large:  one 4096x4096 upload (the decode shader's throughput)
//   tiny:   256 uploads of 64x64 (fixed per-copy cost)
//   many:   256 uploads of 256x256, one per array layer, in one command buffer (per-copy overhead:
//           barriers, push descriptors, state restore)
// and reports GPU time (timestamps) and CPU recording time. BC3 is native on Xclipse and serves as
// the no-emulation baseline for the transfer path.
//   EXYNOS_TOOLS_BC=emulate EXYNOS_PROBE_LAYER=build/libVkLayer_exynos_tools.so build/bc_bench

#include <time.h>

#include "harness.h"

typedef struct {
    VkFormat f;
    const char* name;
    uint32_t block_bytes;
    int kind;  // 0 plain, 6 = BC6H mode spreading, 7 = BC7 mode spreading
} BF;

static const BF kFmts[] = {
    {VK_FORMAT_BC3_UNORM_BLOCK, "BC3 (native)", 16, 0},
    {VK_FORMAT_BC4_UNORM_BLOCK, "BC4_UNORM", 8, 0},
    {VK_FORMAT_BC5_UNORM_BLOCK, "BC5_UNORM", 16, 0},
    {VK_FORMAT_BC6H_UFLOAT_BLOCK, "BC6H_UFLOAT", 16, 6},
    {VK_FORMAT_BC7_UNORM_BLOCK, "BC7_UNORM", 16, 7},
};

static double now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}
static int cmpd(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return (x > y) - (x < y); }
static double median(double* v, int n) { qsort(v, n, sizeof(double), cmpd); return v[n / 2]; }

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }

static void fill(const BF* f, uint8_t* p, size_t bytes) {
    for (size_t i = 0; i < bytes; i++) p[i] = (uint8_t)rnd();
    if (f->kind == 7) {
        for (size_t b = 0; b < bytes / 16; b++) { uint32_t m = (uint32_t)(b % 8); p[b * 16] = (uint8_t)((p[b * 16] & ~((2u << m) - 1u)) | (1u << m)); }
    } else if (f->kind == 6) {
        static const uint8_t codes[14][2] = {{0x00, 3}, {0x01, 3}, {0x02, 31}, {0x06, 31}, {0x0a, 31}, {0x0e, 31}, {0x12, 31},
                                             {0x16, 31}, {0x1a, 31}, {0x1e, 31}, {0x03, 31}, {0x07, 31}, {0x0b, 31}, {0x0f, 31}};
        for (size_t b = 0; b < bytes / 16; b++) p[b * 16] = (uint8_t)((p[b * 16] & ~codes[b % 14][1]) | codes[b % 14][0]);
    }
}

typedef struct { double gpu_ms, cpu_us; } Res;

// copies uploads of w x h; if layers > 1 each copy goes to its own layer.
static int run(HCtx* c, const BF* f, uint32_t w, uint32_t h, uint32_t layers, uint32_t copies, float period_ns, Res* out) {
    uint32_t bx = (w + 3) / 4, by = (h + 3) / 4;
    VkDeviceSize bytes = (VkDeviceSize)bx * by * f->block_bytes;
    VkBuffer sbuf;
    VkDeviceMemory smem;
    h_make_buffer(c, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &sbuf, &smem);
    uint8_t* p;
    H_CHECK(H_DEV(c->device, vkMapMemory)(c->device, smem, 0, VK_WHOLE_SIZE, 0, (void**)&p));
    fill(f, p, bytes);

    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f->f,
                             .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = layers, .samples = VK_SAMPLE_COUNT_1_BIT,
                             .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    VkResult r = H_DEV(c->device, vkCreateImage)(c->device, &ici, NULL, &img);
    if (r != VK_SUCCESS) { printf("  %-14s create image failed (%d)\n", f->name, (int)r); return 1; }
    VkMemoryRequirements mr;
    H_DEV(c->device, vkGetImageMemoryRequirements)(c->device, img, &mr);
    uint32_t t = h_mem_type(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t == UINT32_MAX) t = h_mem_type(c, mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t};
    VkDeviceMemory imem;
    H_CHECK(H_DEV(c->device, vkAllocateMemory)(c->device, &mai, NULL, &imem));
    H_CHECK(H_DEV(c->device, vkBindImageMemory)(c->device, img, imem, 0));

    VkQueryPoolCreateInfo qpi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2};
    VkQueryPool qp;
    H_CHECK(H_DEV(c->device, vkCreateQueryPool)(c->device, &qpi, NULL, &qp));
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = c->family,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VkCommandPool pool;
    H_CHECK(H_DEV(c->device, vkCreateCommandPool)(c->device, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb;
    H_CHECK(H_DEV(c->device, vkAllocateCommandBuffers)(c->device, &cai, &cb));

    enum { RUNS = 7 };
    double gpu[RUNS], cpu[RUNS];
    for (int run = 0; run < RUNS; run++) {
        double t0 = now_us();
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        H_CHECK(H_DEV(c->device, vkBeginCommandBuffer)(cb, &bi));
        H_DEV(c->device, vkCmdResetQueryPool)(cb, qp, 0, 2);
        VkImageMemoryBarrier b1 = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
        H_DEV(c->device, vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
        H_DEV(c->device, vkCmdWriteTimestamp)(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
        for (uint32_t i = 0; i < copies; i++) {
            VkBufferImageCopy reg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layers > 1 ? i % layers : 0, 1},
                                     .imageExtent = {w, h, 1}};
            H_DEV(c->device, vkCmdCopyBufferToImage)(cb, sbuf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
        }
        H_DEV(c->device, vkCmdWriteTimestamp)(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
        H_CHECK(H_DEV(c->device, vkEndCommandBuffer)(cb));
        cpu[run] = now_us() - t0;
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
        H_CHECK(H_DEV(c->device, vkQueueSubmit)(c->queue, 1, &si, VK_NULL_HANDLE));
        H_CHECK(H_DEV(c->device, vkQueueWaitIdle)(c->queue));
        uint64_t ts[2];
        H_CHECK(H_DEV(c->device, vkGetQueryPoolResults)(c->device, qp, 0, 2, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        gpu[run] = (double)(ts[1] - ts[0]) * period_ns / 1e6;
    }
    out->gpu_ms = median(gpu, RUNS);
    out->cpu_us = median(cpu, RUNS);

    H_DEV(c->device, vkDestroyCommandPool)(c->device, pool, NULL);
    H_DEV(c->device, vkDestroyQueryPool)(c->device, qp, NULL);
    H_DEV(c->device, vkDestroyImage)(c->device, img, NULL);
    H_DEV(c->device, vkFreeMemory)(c->device, imem, NULL);
    H_DEV(c->device, vkUnmapMemory)(c->device, smem);
    H_DEV(c->device, vkDestroyBuffer)(c->device, sbuf, NULL);
    H_DEV(c->device, vkFreeMemory)(c->device, smem, NULL);
    return 0;
}

int main(void) {
    HCtx c;
    h_init(&c);
    VkPhysicalDeviceProperties props;
    H_INST(c.instance, vkGetPhysicalDeviceProperties)(c.pd, &props);
    printf("device: %s, timestamp period %.2f ns, layer %s, EXYNOS_TOOLS_BC=%s\n", props.deviceName,
           props.limits.timestampPeriod, g_embedded ? "embedded" : "none", getenv("EXYNOS_TOOLS_BC") ? getenv("EXYNOS_TOOLS_BC") : "(unset)");
    printf("%-14s %-22s %10s %12s %12s %14s\n", "format", "scenario", "GPU ms", "MB/s (BC)", "Mtexel/s", "CPU us/copy");
    for (size_t i = 0; i < sizeof(kFmts) / sizeof(kFmts[0]); i++) {
        const BF* f = &kFmts[i];
        Res r;
        // large: 4096x4096, 4 copies of the whole image
        if (!run(&c, f, 4096, 4096, 1, 4, props.limits.timestampPeriod, &r)) {
            double per = r.gpu_ms / 4, mb = (4096.0 / 4) * (4096.0 / 4) * f->block_bytes / 1e6;
            printf("%-14s %-22s %10.3f %12.0f %12.0f %14.1f\n", f->name, "large 4096x4096", per, mb / (per / 1e3),
                   4096.0 * 4096 / 1e6 / (per / 1e3), r.cpu_us / 4);
        }
        // many: 256 copies of 256x256, each into its own layer
        if (!run(&c, f, 256, 256, 256, 256, props.limits.timestampPeriod, &r)) {
            double per = r.gpu_ms / 256, mb = (256.0 / 4) * (256.0 / 4) * f->block_bytes / 1e6;
            printf("%-14s %-22s %10.4f %12.0f %12.0f %14.1f\n", f->name, "many 256x256 x256", per, mb / (per / 1e3),
                   256.0 * 256 / 1e6 / (per / 1e3), r.cpu_us / 256);
        }
        // tiny: 256 copies of 64x64 (per-copy fixed cost, decode time is negligible)
        if (!run(&c, f, 64, 64, 256, 256, props.limits.timestampPeriod, &r)) {
            double per = r.gpu_ms / 256, mb = (64.0 / 4) * (64.0 / 4) * f->block_bytes / 1e6;
            printf("%-14s %-22s %10.4f %12.0f %12.0f %14.1f\n", f->name, "tiny 64x64 x256", per, mb / (per / 1e3),
                   64.0 * 64 / 1e6 / (per / 1e3), r.cpu_us / 256);
        }
    }
    H_DEV(c.device, vkDestroyDevice)(c.device, NULL);
    c.DestroyInstance(c.instance, NULL);
    return 0;
}
