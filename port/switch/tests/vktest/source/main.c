/* SvR 2011 Switch port, console test 2.
   Part 1 finishes the memory design: one big pool with on-demand pieces at 3
   views, and 2 GB held with the first 1 GB at 3 views.
   Part 2: Vulkan through mesa-switch (NVK). Logs what the driver offers, then
   draws a triangle for 10 seconds and exits by itself (+ exits early).
   There is no text on screen: results are in sdmc:/svr_vktest.log, flushed
   before each step, so after a crash the last line names the step that died. */
#include <switch.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_vi.h>
#include "shaders.h"

#define KB 1024ull
#define MB (1024ull * 1024ull)
#define PAGE 0x1000ull
#define BLOCK (2ull * MB)

static FILE *g_log;
static Handle g_self;

static void say(const char *fmt, ...)
{
    if (!g_log)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    fsync(fileno(g_log));
}

static double ms_since(u64 t0)
{
    return (double)(armGetSystemTick() - t0) * 1000.0 / 19200000.0;
}

static u64 info(u32 id)
{
    u64 v = 0;
    return R_SUCCEEDED(svcGetInfo(&v, id, CUR_PROCESS_HANDLE, 0)) ? v : ~0ull;
}

/* ---- part 1: memory ---- */
static u8 *g_window;
#define WINDOW_SIZE 0x120000000ull

typedef struct Backing {
    void *heap;
    u64 alias, size;
} Backing;

static Result backing_make(Backing *b, u64 size)
{
    Result rc;
    memset(b, 0, sizeof(*b));
    b->size = size;
    b->heap = memalign(BLOCK, size);
    if (!b->heap)
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    memset(b->heap, 0, size);
    virtmemLock();
    b->alias = (u64)virtmemFindCodeMemory(size + BLOCK, PAGE);
    b->alias = (b->alias + BLOCK - 1) & ~(BLOCK - 1);
    rc = svcMapProcessCodeMemory(g_self, b->alias, (u64)b->heap, size);
    virtmemUnlock();
    if (R_FAILED(rc)) {
        free(b->heap);
        b->heap = NULL;
        return rc;
    }
    rc = svcSetProcessMemoryPermission(g_self, b->alias, size, Perm_Rw);
    if (R_FAILED(rc)) {
        svcUnmapProcessCodeMemory(g_self, b->alias, (u64)b->heap, size);
        free(b->heap);
        b->heap = NULL;
    }
    return rc;
}

static void backing_free(Backing *b)
{
    if (b->heap) {
        svcUnmapProcessCodeMemory(g_self, b->alias, (u64)b->heap, b->size);
        free(b->heap);
        b->heap = NULL;
    }
}

static const u64 k_view[3] = { 0, 0x40000000ull, 0x80000000ull };

static int views_agree(u64 off, u32 tag)
{
    for (int w = 0; w < 3; w++) {
        u32 want = tag ^ (u32)(w << 28);
        *(volatile u32 *)(g_window + k_view[w] + off) = want;
        __asm__("dsb sy");
        for (int r = 0; r < 3; r++)
            if (*(volatile u32 *)(g_window + k_view[r] + off) != want)
                return 0;
    }
    return 1;
}

/* The design we want: one pool, small pieces mapped at 3 views, three rounds. */
static void test_pool(void)
{
    const u64 pool = 1792 * MB, piece = 64 * KB;
    const int want = 8000;
    Backing b;
    u64 t0 = armGetSystemTick();
    Result rc = backing_make(&b, pool);
    say("pool %llu MB rc=0x%x, %.0f ms", pool / MB, rc, ms_since(t0));
    if (R_FAILED(rc))
        return;
    for (int round = 0; round < 3; round++) {
        int done = 0, good = 1;
        t0 = armGetSystemTick();
        for (; done < want; done++) {
            u64 src = b.alias + (u64)done * piece * 3 % pool;
            int v = 0;
            for (; v < 3; v++) {
                rc = svcMapProcessMemory(g_window + k_view[v] + (u64)done * piece * 2, g_self, src, piece);
                if (R_FAILED(rc))
                    break;
            }
            if (v < 3) {
                while (v-- > 0)
                    svcUnmapProcessMemory(g_window + k_view[v] + (u64)done * piece * 2, g_self, src, piece);
                break;
            }
        }
        double ms = ms_since(t0);
        for (int i = 0; i < done; i += 97)
            good &= views_agree((u64)i * piece * 2 + piece - 4, 0x5A000000u + (u32)i);
        for (int i = done - 1; i >= 0; i--)
            for (int v = 0; v < 3; v++)
                svcUnmapProcessMemory(g_window + k_view[v] + (u64)i * piece * 2, g_self,
                                      b.alias + (u64)i * piece * 3 % pool, piece);
        say("[%s] pool round %d: %d of %d pieces at 3 views, %.1f us per map, rc=0x%x, views agree %d",
            done == want && good ? "PASS" : "FAIL", round, done, want, done ? ms * 1000.0 / (done * 3) : 0.0, rc, good);
    }
    backing_free(&b);
}

/* 2 GB of 16 MB chunks. The first 1 GB has 3 views each (the window has no room
   for more); the second 1 GB has one view, above them. */
static void test_big_views(void)
{
    enum { MAX_CHUNKS = 128, PER_ROW = 64 };
    static Backing c[MAX_CHUNKS];
    const u64 chunk = 16 * MB;
    const u64 top = 0xC0000000ull;
    int n = 0;
    Result rc = 0;
    u64 t0 = armGetSystemTick();
    for (; n < MAX_CHUNKS; n++) {
        rc = backing_make(&c[n], chunk);
        if (R_FAILED(rc))
            break;
        if (n < PER_ROW) {
            for (int v = 0; v < 3 && R_SUCCEEDED(rc); v++)
                rc = svcMapProcessMemory(g_window + k_view[v] + (u64)n * chunk, g_self, c[n].alias, chunk);
        } else {
            rc = svcMapProcessMemory(g_window + top + (u64)(n - PER_ROW) * chunk, g_self, c[n].alias, chunk);
        }
        if (R_FAILED(rc))
            break;
    }
    say("[%s] big: %llu MB in 16 MB chunks, first 1 GB at 3 views, %.0f ms, rc=0x%x, used %llu MB",
        n == MAX_CHUNKS ? "PASS" : "FAIL", n * chunk / MB, ms_since(t0), rc, info(InfoType_UsedMemorySize) / MB);
    /* Give it back: Vulkan needs the heap next. Any failure above stops the
       test, so unmap failures for a partly mapped last chunk are harmless. */
    for (int i = 0; i < n; i++) {
        if (i < PER_ROW) {
            for (int v = 0; v < 3; v++)
                svcUnmapProcessMemory(g_window + k_view[v] + (u64)i * chunk, g_self, c[i].alias, chunk);
        } else {
            svcUnmapProcessMemory(g_window + top + (u64)(i - PER_ROW) * chunk, g_self, c[i].alias, chunk);
        }
        backing_free(&c[i]);
    }
}

/* ---- part 2: Vulkan ---- */
#define CHECK(x)                                 \
    do {                                         \
        say("%s", #x);                           \
        VkResult r_ = (x);                       \
        if (r_ != VK_SUCCESS) {                  \
            say("  FAILED: VkResult %d", (int)r_); \
            return 0;                            \
        }                                        \
    } while (0)

static void log_device(VkPhysicalDevice gpu)
{
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(gpu, &p);
    say("device: %s, Vulkan %u.%u.%u, driver 0x%x, vendor 0x%x", p.deviceName, VK_VERSION_MAJOR(p.apiVersion),
        VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion), p.driverVersion, p.vendorID);
    say("limits: push constants %u, bound sets %u, stage samplers %u, stage sampled images %u, stage ubos %u, stage ssbos %u",
        p.limits.maxPushConstantsSize, p.limits.maxBoundDescriptorSets, p.limits.maxPerStageDescriptorSamplers,
        p.limits.maxPerStageDescriptorSampledImages, p.limits.maxPerStageDescriptorUniformBuffers,
        p.limits.maxPerStageDescriptorStorageBuffers);
    say("limits: ubo align %llu, ssbo align %llu, max ubo %u, max ssbo %u, timestamp period %.3f ns, color samples 0x%x",
        (unsigned long long)p.limits.minUniformBufferOffsetAlignment,
        (unsigned long long)p.limits.minStorageBufferOffsetAlignment, p.limits.maxUniformBufferRange,
        p.limits.maxStorageBufferRange, p.limits.timestampPeriod, p.limits.framebufferColorSampleCounts);

    VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan11Features f11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12 };
    VkPhysicalDeviceFeatures2 f = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11 };
    vkGetPhysicalDeviceFeatures2(gpu, &f);
    say("features: int64 %d, int16 %d, geometry %d, tess %d, bc %d, depth clamp %d, dual src %d, indep blend %d, logic op %d",
        f.features.shaderInt64, f.features.shaderInt16, f.features.geometryShader, f.features.tessellationShader,
        f.features.textureCompressionBC, f.features.depthClamp, f.features.dualSrcBlend, f.features.independentBlend,
        f.features.logicOp);
    say("features 1.2: bda %d, descriptor indexing %d, runtime arrays %d, partially bound %d, update after bind %d/%d, non-uniform %d, timeline %d, 8bit storage %d",
        f12.bufferDeviceAddress, f12.descriptorIndexing, f12.runtimeDescriptorArray, f12.descriptorBindingPartiallyBound,
        f12.descriptorBindingSampledImageUpdateAfterBind, f12.descriptorBindingUniformBufferUpdateAfterBind,
        f12.shaderSampledImageArrayNonUniformIndexing, f12.timelineSemaphore, f12.storageBuffer8BitAccess);

    VkPhysicalDeviceMemoryProperties m;
    vkGetPhysicalDeviceMemoryProperties(gpu, &m);
    for (u32 i = 0; i < m.memoryHeapCount; i++)
        say("heap %u: %llu MB flags 0x%x", i, (unsigned long long)(m.memoryHeaps[i].size / MB), m.memoryHeaps[i].flags);
    for (u32 i = 0; i < m.memoryTypeCount; i++)
        say("type %u: heap %u flags 0x%x", i, m.memoryTypes[i].heapIndex, m.memoryTypes[i].propertyFlags);

    u32 n = 0;
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &n, NULL);
    VkExtensionProperties *e = calloc(n, sizeof(*e));
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &n, e);
    say("%u device extensions:", n);
    for (u32 i = 0; i < n; i++)
        say("  %s", e[i].extensionName);
    free(e);
}

static int run_vulkan(PadState *pad)
{
    u64 t0 = armGetSystemTick();
    const char *inst_ext[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_NN_VI_SURFACE_EXTENSION_NAME };
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "svr_vktest", .apiVersion = VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                 .enabledExtensionCount = 2, .ppEnabledExtensionNames = inst_ext };
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    say("instance took %.1f ms, used %llu MB", ms_since(t0), info(InfoType_UsedMemorySize) / MB);

    u32 count = 1;
    VkPhysicalDevice gpu;
    VkResult er = vkEnumeratePhysicalDevices(inst, &count, &gpu);
    say("devices: %u, result %d", count, er);
    if (!count)
        return 0;
    log_device(gpu);

    NWindow *win = nwindowGetDefault();
    u32 w = 0, h = 0;
    nwindowGetDimensions(win, &w, &h);
    say("window %ux%u", w, h);
    VkViSurfaceCreateInfoNN sci = { .sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN, .window = win };
    VkSurfaceKHR surf;
    CHECK(vkCreateViSurfaceNN(inst, &sci, NULL, &surf));

    VkBool32 can = 0;
    vkGetPhysicalDeviceSurfaceSupportKHR(gpu, 0, surf, &can);
    say("queue 0 can present: %d", can);

    float prio = 1;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
    const char *dev_ext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = 1, .ppEnabledExtensionNames = dev_ext };
    VkDevice dev;
    t0 = armGetSystemTick();
    CHECK(vkCreateDevice(gpu, &dci, NULL, &dev));
    say("device took %.1f ms, used %llu MB", ms_since(t0), info(InfoType_UsedMemorySize) / MB);
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surf, &caps);
    u32 fn = 0, pn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surf, &fn, NULL);
    VkSurfaceFormatKHR *fmts = calloc(fn, sizeof(*fmts));
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surf, &fn, fmts);
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surf, &pn, NULL);
    VkPresentModeKHR modes[8] = { 0 };
    if (pn > 8)
        pn = 8;
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surf, &pn, modes);
    say("surface: images %u-%u, extent %ux%u, %u formats (first %d), present modes %u: %d %d %d", caps.minImageCount,
        caps.maxImageCount, caps.currentExtent.width, caps.currentExtent.height, fn, fmts[0].format, pn, modes[0], modes[1], modes[2]);

    VkExtent2D ext = { w ? w : 1280, h ? h : 720 };
    u32 want_images = caps.minImageCount + 1;
    if (caps.maxImageCount && want_images > caps.maxImageCount)
        want_images = caps.maxImageCount;
    VkSwapchainCreateInfoKHR sw = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surf, .minImageCount = want_images,
        .imageFormat = fmts[0].format, .imageColorSpace = fmts[0].colorSpace, .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE };
    VkSwapchainKHR chain;
    CHECK(vkCreateSwapchainKHR(dev, &sw, NULL, &chain));
    u32 nimg = 0;
    vkGetSwapchainImagesKHR(dev, chain, &nimg, NULL);
    VkImage imgs[8];
    if (nimg > 8)
        nimg = 8;
    vkGetSwapchainImagesKHR(dev, chain, &nimg, imgs);
    say("swap chain: %u images %ux%u", nimg, ext.width, ext.height);

    VkAttachmentDescription att = { .format = fmts[0].format, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ref };
    VkSubpassDependency dep = { VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0 };
    VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att,
                                   .subpassCount = 1, .pSubpasses = &sub, .dependencyCount = 1, .pDependencies = &dep };
    VkRenderPass rp;
    CHECK(vkCreateRenderPass(dev, &rpi, NULL, &rp));

    VkFramebuffer fbs[8];
    for (u32 i = 0; i < nimg; i++) {
        VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = imgs[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = fmts[0].format, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        VkImageView view;
        CHECK(vkCreateImageView(dev, &vi, NULL, &view));
        VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1,
                                       .pAttachments = &view, .width = ext.width, .height = ext.height, .layers = 1 };
        CHECK(vkCreateFramebuffer(dev, &fi, NULL, &fbs[i]));
    }

    VkShaderModuleCreateInfo mi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(tri_vert), .pCode = tri_vert };
    VkShaderModule vs, fs;
    CHECK(vkCreateShaderModule(dev, &mi, NULL, &vs));
    mi.codeSize = sizeof(tri_frag);
    mi.pCode = tri_frag;
    CHECK(vkCreateShaderModule(dev, &mi, NULL, &fs));
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { 0, 0, (float)ext.width, (float)ext.height, 0, 1 };
    VkRect2D sc = { { 0, 0 }, ext };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1 };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState ba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
    VkGraphicsPipelineCreateInfo gp = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st, .pVertexInputState = &vin,
        .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = pl, .renderPass = rp };
    VkPipeline pipe;
    t0 = armGetSystemTick();
    CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipe));
    say("pipeline compile %.1f ms", ms_since(t0));

    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(dev, &cai, &cmd));
    VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore acquired, finished;
    CHECK(vkCreateSemaphore(dev, &si, NULL, &acquired));
    CHECK(vkCreateSemaphore(dev, &si, NULL, &finished));
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    VkFence fence;
    CHECK(vkCreateFence(dev, &fci, NULL, &fence));
    say("used %llu MB before the first frame", info(InfoType_UsedMemorySize) / MB);

    u64 start = armGetSystemTick(), last = start, worst = 0;
    int frames = 0;
    while (appletMainLoop() && ms_since(start) < 10000.0) {
        padUpdate(pad);
        if (padGetButtonsDown(pad) & HidNpadButton_Plus)
            break;
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &fence);
        u32 idx = 0;
        VkResult ar = vkAcquireNextImageKHR(dev, chain, UINT64_MAX, acquired, VK_NULL_HANDLE, &idx);
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
            say("acquire failed %d at frame %d", ar, frames);
            return 0;
        }
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &bi);
        float t = (float)(frames % 240) / 240.0f;
        VkClearValue clear = { .color = { .float32 = { 0.05f, 0.05f + 0.25f * (t < 0.5f ? t * 2 : 2 - t * 2), 0.15f, 1 } } };
        VkRenderPassBeginInfo rb = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fbs[idx],
                                     .renderArea = { { 0, 0 }, ext }, .clearValueCount = 1, .pClearValues = &clear };
        vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo sub2 = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &acquired, .pWaitDstStageMask = &stage,
                              .commandBufferCount = 1, .pCommandBuffers = &cmd, .signalSemaphoreCount = 1, .pSignalSemaphores = &finished };
        VkResult sr = vkQueueSubmit(q, 1, &sub2, fence);
        if (sr != VK_SUCCESS) {
            say("submit failed %d at frame %d", sr, frames);
            return 0;
        }
        VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &finished,
                                .swapchainCount = 1, .pSwapchains = &chain, .pImageIndices = &idx };
        VkResult pr = vkQueuePresentKHR(q, &pi);
        if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) {
            say("present failed %d at frame %d", pr, frames);
            return 0;
        }
        u64 now = armGetSystemTick();
        if (frames > 0 && now - last > worst)
            worst = now - last;
        last = now;
        if (++frames == 1)
            say("first frame presented, %.1f ms after the loop started", ms_since(start));
    }
    double total = ms_since(start);
    say("[PASS] vulkan: %d frames in %.0f ms = %.1f fps, worst frame %.1f ms, used %llu MB", frames, total,
        frames * 1000.0 / total, (double)worst * 1000.0 / 19200000.0, info(InfoType_UsedMemorySize) / MB);
    vkDeviceWaitIdle(dev);
    return 1;
}

int main(void)
{
    g_log = fopen("sdmc:/svr_vktest.log", "w");
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    say("SvR 2011 Switch test 2: applet type %d, total %llu MB, used %llu MB", appletGetAppletType(),
        info(InfoType_TotalMemorySize) / MB, info(InfoType_UsedMemorySize) / MB);
    if (appletGetAppletType() != AppletType_Application) {
        say("NOT TITLE MODE: hold R while starting a game.");
    } else {
        g_self = envGetOwnProcessHandle();
        virtmemLock();
        u8 *w = (u8 *)virtmemFindAslr(WINDOW_SIZE + BLOCK, PAGE);
        if (w)
            virtmemAddReservation(w, WINDOW_SIZE + BLOCK);
        virtmemUnlock();
        g_window = (u8 *)(((u64)w + BLOCK - 1) & ~(BLOCK - 1));
        test_pool();
        test_big_views();
        say("memory done, used %llu MB; starting Vulkan", info(InfoType_UsedMemorySize) / MB);
        int ok = run_vulkan(&pad);
        say("DONE: vulkan %s", ok ? "ok" : "FAILED");
    }
    if (g_log)
        fclose(g_log);
    return 0;
}
