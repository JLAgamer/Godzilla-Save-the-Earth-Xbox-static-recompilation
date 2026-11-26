/*
 * Vulkan render back end for the NV2A pushbuffer executor (Linux / Steam
 * Deck, and Windows with GZ_GPU=vulkan for testing).
 *
 * The same design as gpu_d3d11.c, which it follows function for function:
 *
 *   - one render target per guest colour surface address and one depth/
 *     stencil buffer per depth surface, at real pixels times GZ_RENDER_SCALE;
 *     both stay in one layout for their whole life (GENERAL / depth-stencil
 *     attachment), with barriers between drawing into a target and sampling it;
 *   - the shaders are the D3D11 back end's own HLSL (s_hlsl), compiled to
 *     SPIR-V by recomp_config/tools/build_vk_shaders.py (gpu_vk_spirv.h);
 *   - dynamic rendering and the Vulkan 1.3 dynamic depth/stencil state, so a
 *     pipeline is only a blend state and a colour write mask (cached);
 *   - textures decoded by nv2a_backend_decode_texture, cached and re-hashed
 *     as in gpu_d3d11.c (texture_srv), with its replacement textures
 *     (textures_ps/) and generated mips for anisotropic filtering;
 *   - flip blits the most recent full-size surface into the window,
 *     letterboxed to 4:3 or 16:9.
 *
 * Not yet: GPU vertex programs (draw_program: the executor runs them on the
 * CPU), the specialised pixel shader variants, frame readback (RECOMP_FB_DUMP),
 * screenshots, traces and the debug hotkeys. See docs/LINUX_PORT.md.
 *
 * Every call arrives on the NV2A poll thread, one at a time. The window is
 * created there too and its events are pumped from flip(): a Win32 window on
 * Windows, an SDL2 window on POSIX (where the toolkit's own framebuffer window
 * then stays closed, see main.c).
 *
 * Switches as in gpu_d3d11.c: GZ_RENDER_SCALE, GZ_WINDOW_SCALE, GZ_FULLSCREEN,
 * GZ_VSYNC, GZ_ANISO, GZ_GPU_CULL. GZ_VK_DEVICE=<n> picks the n-th GPU.
 */
#ifdef GZ_HAVE_VULKAN

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#else
#include <SDL.h>
#include <SDL_vulkan.h>
#include <dlfcn.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include "keyboard_sdl.h"
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_backend.h"
#include "nv2a_vsh_interp.h"
#include "gpu_vk_spirv.h"

extern ptrdiff_t g_xbox_mem_offset;
#define GUEST_PTR(va) ((uint8_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

int      gz_opt_wide(void);
int      gz_wide_fight_active(void);
int      gz_opt_ps_prompts(void);
unsigned gz_opt_generation(void);

#define LOG(...) fprintf(stderr, "[GPU-VK] " __VA_ARGS__)

/* ── Small host helpers ───────────────────────────────────── */

static uint64_t now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER q;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q);
    return (uint64_t)(q.QuadPart * 1000000 / f.QuadPart);
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
#endif
}
static uint32_t now_ms(void) { return (uint32_t)(now_us() / 1000u); }

static int env_int(const char *name, int def, int lo, int hi)
{
    const char *e = getenv(name);
    int v = (e && *e) ? atoi(e) : def;
    return v < lo ? lo : v > hi ? hi : v;
}

/* ── Vulkan entry points (loaded at run time: no link dependency) ─────── */

#define VK_GLOBAL_FNS(X) \
    X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceVersion)

#define VK_INSTANCE_FNS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties) \
    X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)

#define VK_DEVICE_FNS(X) \
    X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) \
    X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkQueueSubmit) \
    X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCreateFence) X(vkWaitForFences) \
    X(vkResetFences) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateImage) X(vkDestroyImage) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) \
    X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateBuffer) X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateSampler) \
    X(vkCreateShaderModule) X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkCreatePipelineCache) X(vkGetPipelineCacheData) \
    X(vkCreateDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) \
    X(vkUpdateDescriptorSets) X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdPipelineBarrier) \
    X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdBlitImage) X(vkCmdCopyImage) X(vkCmdClearColorImage) \
    X(vkCmdClearDepthStencilImage) X(vkCmdClearAttachments) X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdDraw) X(vkCmdSetViewport) \
    X(vkCmdSetScissor) X(vkCmdSetBlendConstants) X(vkCmdSetDepthTestEnable) \
    X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetStencilTestEnable) \
    X(vkCmdSetStencilOp) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) \
    X(vkCmdSetStencilReference) X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) X(vkCmdSetCullMode) \
    X(vkCmdSetFrontFace)

#define DECL(f) static PFN_##f f;
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
VK_GLOBAL_FNS(DECL)
VK_INSTANCE_FNS(DECL)
VK_DEVICE_FNS(DECL)
#ifdef _WIN32
static PFN_vkCreateWin32SurfaceKHR vkCreateWin32SurfaceKHR;
#endif
#undef DECL

static void *s_vk_lib;

static int vk_load_library(void)
{
    if (vkGetInstanceProcAddr) return 1;
#ifdef _WIN32
    s_vk_lib = (void *)LoadLibraryA("vulkan-1.dll");
    if (s_vk_lib)
        vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress((HMODULE)s_vk_lib, "vkGetInstanceProcAddr");
#else
    s_vk_lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!s_vk_lib) s_vk_lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (s_vk_lib) vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(s_vk_lib, "vkGetInstanceProcAddr");
#endif
    if (!vkGetInstanceProcAddr) return 0;
#define LOADG(f) f = (PFN_##f)vkGetInstanceProcAddr(NULL, #f);
    VK_GLOBAL_FNS(LOADG)
#undef LOADG
    return vkCreateInstance && vkEnumerateInstanceExtensionProperties;
}

/* ── State ─────────────────────────────────────────────────── */

typedef struct { VkImage img; VkImageView view; VkDeviceMemory mem; } VkImg;
typedef struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *map; VkDeviceSize size; } VkBuf;

typedef struct {
    uint32_t va, w, h;                  /* guest address, real pixels */
    uint32_t pw, ph;                    /* image size (scaled) */
    VkImg    im;
    uint32_t pitch, bpp, last_frame;
    int      drawn;                     /* written since the last barrier to sample it */
    int      sampled;                   /* sampled since it was last drawn into */
} Rt;

typedef struct { uint32_t va, w, h, pw, ph; VkImg im; } Ds;

typedef struct {
    uint32_t offset, w, h, color, hash, frame, used, cube, tick;
    unsigned gen;
    uint32_t iw, ih, mips;
    VkImg    im;
} Tex;

#define MAX_RT  24
#define MAX_DS  12
#define MAX_TEX 1536
#define FRAMES  2

static Rt  s_rts[MAX_RT];
static Ds  s_dss[MAX_DS];
static Tex s_texs[MAX_TEX];

/* Matches cbuffer CB in s_hlsl (gpu_d3d11.c's Consts), row for row. */
typedef struct {
    float    inv_size[2], tex0_inv[2];
    float    zscale, alpha_ref, alpha_func, zclip_on;
    float    zmin, zmax, rc_on, tex0_on;
    uint32_t fin[4];
    uint32_t misc[4];
    float    fog_color[4], final_c0[4], final_c1[4];
    float    tex_scale[4][4];
    uint32_t stage[8][4];
    float    factor0[8][4], factor1[8][4];
    float    wide[4];
    float    dbg[4];
} Consts;

/* Settings */
static int s_scale = 1, s_win_scale = 2, s_vsync, s_cull, s_fullscreen, s_aniso = 16, s_wide;
#define WIDE_SURF(w, h) (s_wide && (w) == 640u && (h) == 480u)
#define PX_W(w, h) (WIDE_SURF(w, h) ? ((w) * (uint32_t)s_scale * 4u + 2u) / 3u : (w) * (uint32_t)s_scale)

static uint32_t s_frame = 1, s_frame_tick, s_overlay_frame;
static float    s_draw_squeeze, s_draw_anchor, s_draw_yscale = 1.0f;
static Rt      *s_present_rt;
static int      s_ready, s_failed;

/* Device */
static VkInstance       s_inst;
static VkPhysicalDevice s_phys;
static VkDevice         s_dev;
static VkQueue          s_queue;
static uint32_t         s_qfam;
static VkPhysicalDeviceMemoryProperties s_memprops;
static VkDeviceSize     s_ubo_align = 256;
static float            s_max_aniso = 1.0f;
static VkFormat         s_ds_format = VK_FORMAT_D24_UNORM_S8_UINT;
static int              s_clip_dist;      /* shaderClipDistance: GPU vertex programs */

/* Window and swap chain */
static VkSurfaceKHR   s_surface;
static VkSwapchainKHR s_swap;
static VkFormat       s_swap_format;
static VkExtent2D     s_swap_ext;
static VkImage        s_swap_img[8];
static VkSemaphore    s_swap_done[8];
static uint32_t       s_swap_n;
static int            s_swap_stale;
static uint32_t       s_win_w, s_win_h;
#ifdef _WIN32
static HWND           s_hwnd;
static volatile LONG  s_quit;
#else
static SDL_Window    *s_win;
#endif

/* Pipelines */
static VkDescriptorSetLayout s_dsl;
static VkPipelineLayout      s_pl;
static VkPipelineCache       s_pcache;
static VkShaderModule        s_sm_vs, s_sm_ps, s_sm_vs_blit, s_sm_ps_one, s_sm_vs_prog, s_sm_ps_spec;
static VkSampler             s_smp_default;
static VkImg                 s_white2d, s_whitecube;

typedef struct { uint64_t key; VkPipeline p; } PipeEnt;
#define MAX_PIPES 1024
static PipeEnt s_pipes[MAX_PIPES];
typedef struct { uint64_t key; VkSampler s; } SmpEnt;
static SmpEnt  s_smps[256];
static int     s_nsmps;

/* Per-frame resources */
typedef struct { VkBuf b; VkDeviceSize pos; } Chunk;
typedef struct { VkImage img; VkImageView view; VkBuffer buf; VkDeviceMemory mem; } Junk;
typedef struct {
    VkCommandBuffer  cb;
    VkFence          fence;
    VkSemaphore      acquire;
    VkDescriptorPool pool;
    Chunk           *chunks;
    int              nchunks, cur;
    Junk            *junk;
    int              njunk, capjunk;
} Frame;
static VkCommandPool s_cpool;
static Frame         s_fr[FRAMES];
static int           s_fi;              /* current frame slot */
static int           s_recording;

/* What the command buffer is doing right now */
static Rt  *s_bound_rt;                 /* rendering into it (dynamic rendering active) */
static Ds  *s_bound_ds;
static int  s_rendering;

/* Stats */
static uint32_t s_st_draws, s_st_uploads;

/* ── Memory, images, buffers ──────────────────────────────── */

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    uint32_t i;
    for (i = 0; i < s_memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (s_memprops.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static void junk_img(VkImg *im)
{
    Frame *f = &s_fr[s_fi];
    if (!im->img) return;
    if (f->njunk == f->capjunk) {
        int c = f->capjunk ? f->capjunk * 2 : 64;
        Junk *n = (Junk *)realloc(f->junk, (size_t)c * sizeof *n);
        if (!n) return;
        f->junk = n; f->capjunk = c;
    }
    memset(&f->junk[f->njunk], 0, sizeof f->junk[0]);
    f->junk[f->njunk].img = im->img; f->junk[f->njunk].view = im->view; f->junk[f->njunk].mem = im->mem;
    f->njunk++;
    memset(im, 0, sizeof *im);
}

static void junk_free(Frame *f)
{
    int i;
    for (i = 0; i < f->njunk; i++) {
        if (f->junk[i].view) vkDestroyImageView(s_dev, f->junk[i].view, NULL);
        if (f->junk[i].img) vkDestroyImage(s_dev, f->junk[i].img, NULL);
        if (f->junk[i].buf) vkDestroyBuffer(s_dev, f->junk[i].buf, NULL);
        if (f->junk[i].mem) vkFreeMemory(s_dev, f->junk[i].mem, NULL);
    }
    f->njunk = 0;
}

static int img_create(VkImg *im, uint32_t w, uint32_t h, uint32_t mips, uint32_t layers, VkFormat fmt,
                      VkImageUsageFlags usage, int cube)
{
    VkImageCreateInfo ci;
    VkImageViewCreateInfo vi;
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai;
    int depth = fmt == s_ds_format;

    memset(im, 0, sizeof *im);
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt;
    ci.extent.width = w; ci.extent.height = h; ci.extent.depth = 1;
    ci.mipLevels = mips; ci.arrayLayers = layers; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(s_dev, &ci, NULL, &im->img) != VK_SUCCESS) { im->img = VK_NULL_HANDLE; return 0; }
    vkGetImageMemoryRequirements(s_dev, im->img, &mr);
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, 0);
    if (vkAllocateMemory(s_dev, &ai, NULL, &im->mem) != VK_SUCCESS) {
        vkDestroyImage(s_dev, im->img, NULL); memset(im, 0, sizeof *im); return 0;
    }
    vkBindImageMemory(s_dev, im->img, im->mem, 0);
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = im->img;
    vi.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                           : VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = mips;
    vi.subresourceRange.layerCount = layers;
    if (vkCreateImageView(s_dev, &vi, NULL, &im->view) != VK_SUCCESS) {
        vkDestroyImage(s_dev, im->img, NULL); vkFreeMemory(s_dev, im->mem, NULL);
        memset(im, 0, sizeof *im); return 0;
    }
    return 1;
}

static int buf_create(VkBuf *b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo ci;
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai;
    memset(b, 0, sizeof *b);
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = size; ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(s_dev, &ci, NULL, &b->buf) != VK_SUCCESS) return 0;
    vkGetBufferMemoryRequirements(s_dev, b->buf, &mr);
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mem_type(mr.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(s_dev, &ai, NULL, &b->mem) != VK_SUCCESS) {
        vkDestroyBuffer(s_dev, b->buf, NULL); memset(b, 0, sizeof *b); return 0;
    }
    vkBindBufferMemory(s_dev, b->buf, b->mem, 0);
    if (vkMapMemory(s_dev, b->mem, 0, VK_WHOLE_SIZE, 0, (void **)&b->map) != VK_SUCCESS) return 0;
    b->size = size;
    return 1;
}

/* Frame-local upload memory (vertices, constants, staging): chunks reused
 * every time this frame slot comes round. */
typedef struct { VkBuffer buf; VkDeviceSize off; uint8_t *ptr; } Alloc;

static int frame_alloc(VkDeviceSize bytes, VkDeviceSize align, Alloc *out)
{
    Frame *f = &s_fr[s_fi];
    for (;;) {
        if (f->cur < f->nchunks) {
            Chunk *c = &f->chunks[f->cur];
            VkDeviceSize at = (c->pos + align - 1) & ~(align - 1);
            if (at + bytes <= c->b.size) {
                c->pos = at + bytes;
                out->buf = c->b.buf; out->off = at; out->ptr = c->b.map + at;
                return 1;
            }
            f->cur++;
            continue;
        }
        {
            VkDeviceSize size = 16u << 20;
            Chunk *n;
            while (size < bytes + align) size *= 2;
            n = (Chunk *)realloc(f->chunks, (size_t)(f->nchunks + 1) * sizeof *n);
            if (!n) return 0;
            f->chunks = n;
            memset(&n[f->nchunks], 0, sizeof n[0]);
            if (!buf_create(&n[f->nchunks].b, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                            | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
                return 0;
            f->nchunks++;
        }
    }
}

/* ── Command buffer and barriers ──────────────────────────── */

static VkCommandBuffer cb(void) { return s_fr[s_fi].cb; }

static void end_rendering(void)
{
    if (s_rendering) { vkCmdEndRendering(cb()); s_rendering = 0; }
    s_bound_rt = NULL; s_bound_ds = NULL;
}

/* Everything drawn so far is visible to everything after: one coarse barrier
 * between a target being drawn and being sampled or copied. */
static void barrier_all(void)
{
    VkMemoryBarrier mb;
    memset(&mb, 0, sizeof mb);
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                     | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT
                     | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                     | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cb(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
}

static void img_barrier(VkImage img, VkImageAspectFlags aspect, uint32_t mips, uint32_t layers,
                        VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.levelCount = mips;
    b.subresourceRange.layerCount = layers;
    vkCmdPipelineBarrier(cb(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
}

static void frame_begin(void)
{
    Frame *f = &s_fr[s_fi];
    VkCommandBufferBeginInfo bi;
    int i;
    vkWaitForFences(s_dev, 1, &f->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(s_dev, 1, &f->fence);
    junk_free(f);
    for (i = 0; i < f->nchunks; i++) f->chunks[i].pos = 0;
    f->cur = 0;
    vkResetDescriptorPool(s_dev, f->pool, 0);
    vkResetCommandBuffer(f->cb, 0);
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f->cb, &bi);
    s_recording = 1;
    s_rendering = 0; s_bound_rt = NULL; s_bound_ds = NULL;
}

/* ── Window ───────────────────────────────────────────────── */

#ifdef _WIN32
static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CLOSE: case WM_DESTROY: InterlockedExchange(&s_quit, 1); return 0;
    case WM_SIZE: s_swap_stale = 1; break;
    }
    return DefWindowProcW(h, m, w, l);
}

static int window_create(void)
{
    WNDCLASSW wc;
    RECT r;
    VkWin32SurfaceCreateInfoKHR si;
    SetProcessDPIAware();
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc; wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"GodzillaSTERecompVk"; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    if (s_fullscreen) {
        s_win_w = (uint32_t)GetSystemMetrics(SM_CXSCREEN);
        s_win_h = (uint32_t)GetSystemMetrics(SM_CYSCREEN);
        s_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Godzilla: Save the Earth (recomp, Vulkan)",
                                 WS_POPUP | WS_VISIBLE, 0, 0, (int)s_win_w, (int)s_win_h, NULL, NULL, wc.hInstance, NULL);
    } else {
        r.left = 0; r.top = 0; r.right = (LONG)s_win_w; r.bottom = (LONG)s_win_h;
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        s_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Godzilla: Save the Earth (recomp, Vulkan)",
                                 WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                 r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    }
    if (!s_hwnd) { LOG("CreateWindow failed (%lu)\n", GetLastError()); return 0; }
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    si.hinstance = wc.hInstance; si.hwnd = s_hwnd;
    return vkCreateWin32SurfaceKHR(s_inst, &si, NULL, &s_surface) == VK_SUCCESS;
}

static void window_title(const char *t)
{
    wchar_t w[200];
    MultiByteToWideChar(CP_UTF8, 0, t, -1, w, 200);
    SetWindowTextW(s_hwnd, w);
}

static void window_pump(void)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (s_quit) { LOG("window closed; exiting\n"); ExitProcess(0); }
}

static void window_size(uint32_t *w, uint32_t *h)
{
    RECT r;
    GetClientRect(s_hwnd, &r);
    *w = (uint32_t)(r.right - r.left); *h = (uint32_t)(r.bottom - r.top);
}

static const char *const s_inst_exts[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
static uint32_t s_ninst_exts = 2;
static int window_extensions(void) { return 1; }
#else
static const char *s_inst_exts[16];
static uint32_t s_ninst_exts;

static int window_extensions(void)
{
    unsigned n = 16;
    if (!SDL_Vulkan_GetInstanceExtensions(s_win, &n, s_inst_exts)) {
        LOG("SDL_Vulkan_GetInstanceExtensions: %s\n", SDL_GetError());
        return 0;
    }
    s_ninst_exts = n;
    return 1;
}

/* The window comes before the instance on POSIX: SDL names the surface
 * extensions the window system needs (X11 or Wayland). */
static int window_open(void)
{
    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        LOG("SDL video unavailable: %s\n", SDL_GetError());
        return 0;
    }
    if (s_fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    s_win = SDL_CreateWindow("Godzilla: Save the Earth (recomp, Vulkan)", SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED, (int)s_win_w, (int)s_win_h, flags);
    if (!s_win) { LOG("SDL_CreateWindow: %s\n", SDL_GetError()); return 0; }
    xbox_InputSdlInit();
    return 1;
}

static int window_create(void)
{
    if (!SDL_Vulkan_CreateSurface(s_win, s_inst, &s_surface)) {
        LOG("SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
        return 0;
    }
    return 1;
}

static void window_title(const char *t) { SDL_SetWindowTitle(s_win, t); }

static void window_pump(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) { LOG("window closed; exiting\n"); exit(0); }
        if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) s_swap_stale = 1;
        xbox_InputSdlEvent(&ev);
    }
}

static void window_size(uint32_t *w, uint32_t *h)
{
    int iw = 0, ih = 0;
    SDL_Vulkan_GetDrawableSize(s_win, &iw, &ih);
    *w = (uint32_t)iw; *h = (uint32_t)ih;
}
#endif

/* ── Swap chain ───────────────────────────────────────────── */

static int swap_create(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR fmts[64];
    VkPresentModeKHR modes[16], mode = VK_PRESENT_MODE_FIFO_KHR;
    VkSwapchainCreateInfoKHR ci;
    VkSwapchainKHR old = s_swap;
    uint32_t n = 64, nm = 16, i, w, h;

    window_size(&w, &h);
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_phys, s_surface, &caps);
    if (caps.currentExtent.width != UINT32_MAX) { w = caps.currentExtent.width; h = caps.currentExtent.height; }
    if (!w || !h) return 0;                 /* minimised */
    vkGetPhysicalDeviceSurfaceFormatsKHR(s_phys, s_surface, &n, fmts);
    s_swap_format = fmts[0].format;
    for (i = 0; i < n; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            s_swap_format = fmts[i].format;
            break;
        }
    vkGetPhysicalDeviceSurfacePresentModesKHR(s_phys, s_surface, &nm, modes);
    if (!s_vsync)
        for (i = 0; i < nm; i++) {
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];
            if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR && mode != VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];
        }
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = s_surface;
    ci.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount) ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = s_swap_format;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent.width = w; ci.imageExtent.height = h;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & ci.compositeAlpha)) ci.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    if (vkCreateSwapchainKHR(s_dev, &ci, NULL, &s_swap) != VK_SUCCESS) { s_swap = VK_NULL_HANDLE; return 0; }
    if (old) vkDestroySwapchainKHR(s_dev, old, NULL);
    s_swap_n = 8;
    vkGetSwapchainImagesKHR(s_dev, s_swap, &s_swap_n, s_swap_img);
    for (i = 0; i < s_swap_n; i++)
        if (!s_swap_done[i]) {
            VkSemaphoreCreateInfo sc;
            memset(&sc, 0, sizeof sc);
            sc.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            vkCreateSemaphore(s_dev, &sc, NULL, &s_swap_done[i]);
        }
    s_swap_ext = ci.imageExtent;
    s_swap_stale = 0;
    return 1;
}

/* ── Pipelines ─────────────────────────────────────────────── */

static VkShaderModule shader(const uint32_t *code, size_t bytes)
{
    VkShaderModuleCreateInfo ci;
    VkShaderModule m = VK_NULL_HANDLE;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes; ci.pCode = code;
    vkCreateShaderModule(s_dev, &ci, NULL, &m);
    return m;
}

static VkBlendFactor map_blend(uint32_t f, int alpha_slot)
{
    switch (f) {
    case 0:      return VK_BLEND_FACTOR_ZERO;
    case 1:      return VK_BLEND_FACTOR_ONE;
    case 0x300:  return alpha_slot ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
    case 0x301:  return alpha_slot ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x302:  return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x303:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x304:  return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x305:  return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x306:  return alpha_slot ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
    case 0x307:  return alpha_slot ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x308:  return alpha_slot ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: case 0x8003: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    }
    return VK_BLEND_FACTOR_ONE;
}

static VkBlendOp map_blend_op(uint32_t e)
{
    switch (e) {
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}

static VkColorComponentFlags map_mask(uint32_t m)
{
    VkColorComponentFlags w = 0;
    if (m & 0x00010000u) w |= VK_COLOR_COMPONENT_R_BIT;
    if (m & 0x00000100u) w |= VK_COLOR_COMPONENT_G_BIT;
    if (m & 0x00000001u) w |= VK_COLOR_COMPONENT_B_BIT;
    if (m & 0x01000000u) w |= VK_COLOR_COMPONENT_A_BIT;
    return w;
}

static VkCompareOp map_cmp(uint32_t f)
{
    return (f >= 0x200 && f <= 0x207) ? (VkCompareOp)(f - 0x200) : VK_COMPARE_OP_ALWAYS;
}

static VkStencilOp map_sop(uint32_t o)
{
    switch (o) {
    case 0:      return VK_STENCIL_OP_ZERO;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    }
    return VK_STENCIL_OP_KEEP;
}

/* kind 0: vs_main/ps_main with Nv2aVertex input; 1: masked clear (vs_blit,
 * ps_one, blend factor x 1, no vertex input). */
/* A pixel shader setup baked in (ps_spec's 48 specialisation constants,
 * ids as in build_vk_shaders.py); gpu_d3d11.c's PsKey. */
typedef struct { uint32_t v[48]; } VkSpec;

static VkPipeline pipeline_build(uint64_t key, const VkSpec *spec);

static VkPipeline pipeline(int kind, int has_ds, VkBool32 blend, VkBlendFactor sc, VkBlendFactor dc,
                           VkBlendFactor sa, VkBlendFactor da, VkBlendOp op, VkColorComponentFlags mask,
                           uint32_t vmask)
{
    uint64_t key = (uint64_t)kind | (uint64_t)has_ds << 2 | (uint64_t)blend << 3 | (uint64_t)sc << 4
                 | (uint64_t)dc << 9 | (uint64_t)sa << 14 | (uint64_t)da << 19 | (uint64_t)op << 24
                 | (uint64_t)mask << 27 | (uint64_t)(vmask & 0xFFFFu) << 32;
    uint32_t at = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 54) % MAX_PIPES, i;
    VkPipeline p;
    for (i = 0; i < 16; i++, at = (at + 1) % MAX_PIPES) {
        if (s_pipes[at].p && s_pipes[at].key == key) return s_pipes[at].p;
        if (!s_pipes[at].p) break;
    }
    p = pipeline_build(key, NULL);
    if (p && i < 16) { s_pipes[at].key = key; s_pipes[at].p = p; }
    return p;
}

/* Builds the pipeline a key describes (see pipeline()); with spec, the
 * pixel shader is ps_spec specialised to it. Safe on any thread. */
static VkPipeline pipeline_build(uint64_t key, const VkSpec *spec)
{
    int kind = (int)(key & 3), has_ds = (int)((key >> 2) & 1);
    VkBool32 blend = (VkBool32)((key >> 3) & 1);
    VkBlendFactor sc = (VkBlendFactor)((key >> 4) & 31), dc = (VkBlendFactor)((key >> 9) & 31);
    VkBlendFactor sa = (VkBlendFactor)((key >> 14) & 31), da = (VkBlendFactor)((key >> 19) & 31);
    VkBlendOp op = (VkBlendOp)((key >> 24) & 7);
    VkColorComponentFlags mask = (VkColorComponentFlags)((key >> 27) & 15);
    uint32_t vmask = (uint32_t)(key >> 32) & 0xFFFFu, i;
    VkSpecializationMapEntry sme[48];
    VkSpecializationInfo si;
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb, vbp[2];
    VkVertexInputAttributeDescription va[16];
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo dss;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cbs;
    VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
    };
    VkPipelineDynamicStateCreateInfo ds;
    VkPipelineRenderingCreateInfo ri;
    VkGraphicsPipelineCreateInfo ci;
    VkFormat cfmt = VK_FORMAT_B8G8R8A8_UNORM;
    VkPipeline p = VK_NULL_HANDLE;

    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = kind == 1 ? s_sm_vs_blit : kind == 2 ? s_sm_vs_prog : s_sm_vs;
    st[0].pName = kind == 1 ? "vs_blit" : kind == 2 ? "vs_prog" : "vs_main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = kind == 1 ? s_sm_ps_one : s_sm_ps;
    st[1].pName = kind == 1 ? "ps_one" : "ps_main";
    if (spec && kind != 1) {
        for (i = 0; i < 48; i++) { sme[i].constantID = i; sme[i].offset = i * 4u; sme[i].size = 4; }
        si.mapEntryCount = 48; si.pMapEntries = sme; si.dataSize = sizeof *spec; si.pData = spec;
        st[1].module = s_sm_ps_spec; st[1].pSpecializationInfo = &si;
    }

    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    if (kind == 2) {
        /* Packed program inputs (Nv2aProgramDraw.packed): binding 0 holds the
         * inputs named in vmask, 16 bytes each, per vertex; binding 1 holds
         * all 16 once (one instance) for the rest. As gpu_d3d11.c prog_layout. */
        uint32_t rank = 0, nact = 0;
        for (i = 0; i < 16; i++) if ((vmask >> i) & 1) nact++;
        for (i = 0; i < 16; i++) {
            int per_vertex = (vmask >> i) & 1;
            va[i].location = i; va[i].binding = per_vertex ? 0 : 1;
            va[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            va[i].offset = 16u * (per_vertex ? rank++ : i);
        }
        vbp[0].binding = 0; vbp[0].stride = nact ? nact * 16u : 16u; vbp[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        vbp[1].binding = 1; vbp[1].stride = 256u; vbp[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
        vi.vertexBindingDescriptionCount = 2; vi.pVertexBindingDescriptions = vbp;
        vi.vertexAttributeDescriptionCount = 16; vi.pVertexAttributeDescriptions = va;
    } else if (!kind) {
        static const struct { VkFormat f; uint32_t off; } el[10] = {
            { VK_FORMAT_R32G32B32_SFLOAT,     offsetof(Nv2aVertex, x) },
            { VK_FORMAT_R32_SFLOAT,           offsetof(Nv2aVertex, rhw) },
            { VK_FORMAT_B8G8R8A8_UNORM,      offsetof(Nv2aVertex, diffuse) },
            { VK_FORMAT_R32G32_SFLOAT,        offsetof(Nv2aVertex, u) },
            { VK_FORMAT_B8G8R8A8_UNORM,      offsetof(Nv2aVertex, specular) },
            { VK_FORMAT_R32_SFLOAT,           offsetof(Nv2aVertex, fog) },
            { VK_FORMAT_R32G32B32A32_SFLOAT,  offsetof(Nv2aVertex, tc[0]) },
            { VK_FORMAT_R32G32B32A32_SFLOAT,  offsetof(Nv2aVertex, tc[1]) },
            { VK_FORMAT_R32G32B32A32_SFLOAT,  offsetof(Nv2aVertex, tc[2]) },
            { VK_FORMAT_R32G32B32A32_SFLOAT,  offsetof(Nv2aVertex, tc[3]) },
        };
        for (i = 0; i < 10; i++) {
            va[i].location = i; va[i].binding = 0; va[i].format = el[i].f; va[i].offset = el[i].off;
        }
        vb.binding = 0; vb.stride = sizeof(Nv2aVertex); vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
        vi.vertexAttributeDescriptionCount = 10; vi.pVertexAttributeDescriptions = va;
    }
    memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    memset(&vp, 0, sizeof vp);
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1; vp.scissorCount = 1;
    memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.depthClampEnable = VK_TRUE;          /* D3D11 DepthClipEnable = FALSE: z compared as given */
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;        /* culling per triangle, as gpu_d3d11.c */
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;
    memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&dss, 0, sizeof dss);
    dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    memset(&cba, 0, sizeof cba);
    cba.blendEnable = blend;
    cba.srcColorBlendFactor = sc; cba.dstColorBlendFactor = dc; cba.colorBlendOp = op;
    cba.srcAlphaBlendFactor = sa; cba.dstAlphaBlendFactor = da; cba.alphaBlendOp = op;
    cba.colorWriteMask = mask;
    memset(&cbs, 0, sizeof cbs);
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = 1; cbs.pAttachments = &cba;
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = sizeof dyn / sizeof dyn[0]; ds.pDynamicStates = dyn;
    memset(&ri, 0, sizeof ri);
    ri.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &cfmt;
    if (has_ds) { ri.depthAttachmentFormat = s_ds_format; ri.stencilAttachmentFormat = s_ds_format; }
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext = &ri;
    ci.stageCount = 2; ci.pStages = st;
    ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia; ci.pViewportState = &vp;
    ci.pRasterizationState = &rs; ci.pMultisampleState = &ms; ci.pDepthStencilState = &dss;
    ci.pColorBlendState = &cbs; ci.pDynamicState = &ds; ci.layout = s_pl;
    if (vkCreateGraphicsPipelines(s_dev, s_pcache, 1, &ci, NULL, &p) != VK_SUCCESS) {
        static int warned;
        if (warned++ < 4) LOG("pipeline creation failed%s\n", spec ? " (specialised)" : "");
        return VK_NULL_HANDLE;
    }
    return p;
}

static VkSampler sampler(const Nv2aTexture *t)
{
    uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
    int linear = (mag == 2) || (min != 1 && min != 0);
    int aniso = linear && s_aniso > 1 && !t->cube && s_max_aniso > 1.0f;
    uint64_t key = (uint64_t)t->addr_u | (uint64_t)t->addr_v << 8 | (uint64_t)linear << 16 | (uint64_t)aniso << 17;
    VkSamplerCreateInfo ci;
    VkSampler s = VK_NULL_HANDLE;
    int i;
    static const VkSamplerAddressMode modes[6] = {
        VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    for (i = 0; i < s_nsmps; i++) if (s_smps[i].key == key) return s_smps[i].s;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = ci.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = modes[t->addr_u < 6 ? t->addr_u : 1];
    ci.addressModeV = modes[t->addr_v < 6 ? t->addr_v : 1];
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.anisotropyEnable = aniso ? VK_TRUE : VK_FALSE;
    ci.maxAnisotropy = aniso ? ((float)s_aniso < s_max_aniso ? (float)s_aniso : s_max_aniso) : 1.0f;
    ci.maxLod = VK_LOD_CLAMP_NONE;
    ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (vkCreateSampler(s_dev, &ci, NULL, &s) != VK_SUCCESS) return s_smp_default;
    if (s_nsmps < 256) { s_smps[s_nsmps].key = key; s_smps[s_nsmps].s = s; s_nsmps++; }
    return s;
}

/* ── Device ────────────────────────────────────────────────── */

static int instance_create(void)
{
    VkApplicationInfo app;
    VkInstanceCreateInfo ci;
    uint32_t ver = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&ver);
    if (VK_API_VERSION_MINOR(ver) < 3 && VK_API_VERSION_MAJOR(ver) == 1) {
        LOG("the Vulkan loader is %u.%u; 1.3 is needed\n", VK_API_VERSION_MAJOR(ver), VK_API_VERSION_MINOR(ver));
        return 0;
    }
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Godzilla: Save the Earth (recomp)";
    app.pEngineName = "xboxrecomp";
    app.apiVersion = VK_API_VERSION_1_3;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = s_ninst_exts; ci.ppEnabledExtensionNames = s_inst_exts;
    if (vkCreateInstance(&ci, NULL, &s_inst) != VK_SUCCESS) { LOG("vkCreateInstance failed\n"); return 0; }
#define LOADI(f) f = (PFN_##f)vkGetInstanceProcAddr(s_inst, #f);
    VK_INSTANCE_FNS(LOADI)
#ifdef _WIN32
    LOADI(vkCreateWin32SurfaceKHR)
#endif
#undef LOADI
    return 1;
}

/* A device that runs Vulkan 1.3 with what the back end uses. */
static int device_usable(VkPhysicalDevice pd, uint32_t *qfam, int need_present)
{
    VkPhysicalDeviceProperties pp;
    VkQueueFamilyProperties qf[16];
    uint32_t n = 16, i;
    VkPhysicalDeviceVulkan13Features f13;
    VkPhysicalDeviceFeatures2 f2;
    vkGetPhysicalDeviceProperties(pd, &pp);
    if (VK_API_VERSION_MAJOR(pp.apiVersion) == 1 && VK_API_VERSION_MINOR(pp.apiVersion) < 3) return 0;
    memset(&f13, 0, sizeof f13); f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    memset(&f2, 0, sizeof f2); f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2; f2.pNext = &f13;
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    if (!f13.dynamicRendering || !f2.features.depthClamp) return 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, qf);
    for (i = 0; i < n; i++) {
        VkBool32 pres = VK_TRUE;
        if (!(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        if (need_present) vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, s_surface, &pres);
        if (pres) { *qfam = i; return 1; }
    }
    return 0;
}

static int device_create(void)
{
    VkPhysicalDevice pds[16];
    uint32_t n = 16, i, pick = UINT32_MAX, q = 0;
    int want = env_int("GZ_VK_DEVICE", -1, -1, 15), best = -1;
    VkPhysicalDeviceProperties pp;
    VkPhysicalDeviceVulkan13Features f13;
    VkPhysicalDeviceFeatures2 f2, have;
    VkDeviceQueueCreateInfo qi;
    VkDeviceCreateInfo ci;
    float prio = 1.0f;
    const char *exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkFormatProperties fp;

    vkEnumeratePhysicalDevices(s_inst, &n, pds);
    for (i = 0; i < n; i++) {
        uint32_t qq;
        int score;
        if (!device_usable(pds[i], &qq, 1)) continue;
        vkGetPhysicalDeviceProperties(pds[i], &pp);
        LOG("GPU %u: %s (Vulkan %u.%u)\n", i, pp.deviceName, VK_API_VERSION_MAJOR(pp.apiVersion),
            VK_API_VERSION_MINOR(pp.apiVersion));
        score = pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
              : pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (want == (int)i) score = 100;
        if (score > best) { best = score; pick = i; q = qq; }
    }
    if (pick == UINT32_MAX) { LOG("no GPU with Vulkan 1.3, dynamic rendering and depth clamp\n"); return 0; }
    s_phys = pds[pick]; s_qfam = q;
    vkGetPhysicalDeviceProperties(s_phys, &pp);
    vkGetPhysicalDeviceMemoryProperties(s_phys, &s_memprops);
    s_ubo_align = pp.limits.minUniformBufferOffsetAlignment ? pp.limits.minUniformBufferOffsetAlignment : 256;
    memset(&have, 0, sizeof have); have.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    vkGetPhysicalDeviceFeatures2(s_phys, &have);
    s_max_aniso = have.features.samplerAnisotropy ? pp.limits.maxSamplerAnisotropy : 1.0f;

    /* AMD (the Steam Deck) has no D24S8; D32S8 holds the same depths. */
    vkGetPhysicalDeviceFormatProperties(s_phys, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
        s_ds_format = VK_FORMAT_D32_SFLOAT_S8_UINT;

    memset(&f13, 0, sizeof f13);
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f13.dynamicRendering = VK_TRUE;
    memset(&f2, 0, sizeof f2);
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2; f2.pNext = &f13;
    f2.features.depthClamp = VK_TRUE;
    f2.features.samplerAnisotropy = have.features.samplerAnisotropy;
    f2.features.shaderClipDistance = have.features.shaderClipDistance;
    s_clip_dist = have.features.shaderClipDistance != 0;
    memset(&qi, 0, sizeof qi);
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = s_qfam; qi.queueCount = 1; qi.pQueuePriorities = &prio;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.pNext = &f2;
    ci.queueCreateInfoCount = 1; ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = 1; ci.ppEnabledExtensionNames = exts;
    if (vkCreateDevice(s_phys, &ci, NULL, &s_dev) != VK_SUCCESS) { LOG("vkCreateDevice failed\n"); return 0; }
#define LOADD(f) f = (PFN_##f)vkGetDeviceProcAddr(s_dev, #f); if (!f) { LOG("missing " #f "\n"); return 0; }
    VK_DEVICE_FNS(LOADD)
#undef LOADD
    vkGetDeviceQueue(s_dev, s_qfam, 0, &s_queue);
    LOG("using %s, depth format %s\n", pp.deviceName,
        s_ds_format == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8");
    return 1;
}

static int objects_create(void)
{
    VkDescriptorSetLayoutBinding lb[9];
    VkDescriptorSetLayoutCreateInfo lci;
    VkPipelineLayoutCreateInfo plci;
    VkPipelineCacheCreateInfo pci;
    VkCommandPoolCreateInfo cpi;
    VkSamplerCreateInfo sci;
    int i;

    memset(lb, 0, sizeof lb);
    lb[0].binding = 0; lb[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    lb[0].descriptorCount = 1; lb[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (i = 0; i < 4; i++) {
        lb[1 + i].binding = 16 + (uint32_t)i;      /* tx0..tx3 (cx0..cx3 at 20..23, one array below) */
        lb[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        lb[1 + i].descriptorCount = 1; lb[1 + i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        lb[5 + i].binding = 32 + (uint32_t)i;      /* sm0..sm3 */
        lb[5 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        lb[5 + i].descriptorCount = 1; lb[5 + i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    {
        /* cube maps cx0..cx3: bindings 20..23 */
        VkDescriptorSetLayoutBinding all[16];
        memcpy(all, lb, sizeof lb);
        for (i = 0; i < 4; i++) {
            memset(&all[9 + i], 0, sizeof all[0]);
            all[9 + i].binding = 20 + (uint32_t)i;
            all[9 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            all[9 + i].descriptorCount = 1; all[9 + i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        for (i = 0; i < 3; i++) {          /* vs_prog: VP (program), VC (constants), VD (per draw) */
            memset(&all[13 + i], 0, sizeof all[0]);
            all[13 + i].binding = 1 + (uint32_t)i;
            all[13 + i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            all[13 + i].descriptorCount = 1; all[13 + i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        }
        memset(&lci, 0, sizeof lci);
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 16; lci.pBindings = all;
        if (vkCreateDescriptorSetLayout(s_dev, &lci, NULL, &s_dsl) != VK_SUCCESS) return 0;
    }
    memset(&plci, 0, sizeof plci);
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1; plci.pSetLayouts = &s_dsl;
    if (vkCreatePipelineLayout(s_dev, &plci, NULL, &s_pl) != VK_SUCCESS) return 0;

    memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    {
        /* The driver's compiled pipelines from earlier runs. */
        FILE *f = fopen("shader_cache/vk_pipelines.bin", "rb");
        static uint8_t *data;
        if (f) {
            long sz;
            fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
            if (sz > 0 && sz < (64 << 20) && (data = (uint8_t *)malloc((size_t)sz)) != NULL
                    && fread(data, 1, (size_t)sz, f) == (size_t)sz) {
                pci.initialDataSize = (size_t)sz; pci.pInitialData = data;
            }
            fclose(f);
        }
        if (vkCreatePipelineCache(s_dev, &pci, NULL, &s_pcache) != VK_SUCCESS) {
            pci.initialDataSize = 0; pci.pInitialData = NULL;
            vkCreatePipelineCache(s_dev, &pci, NULL, &s_pcache);
        }
        free(data); data = NULL;
    }

    s_sm_vs = shader(gz_spv_vs_main, sizeof gz_spv_vs_main);
    s_sm_ps = shader(gz_spv_ps_main, sizeof gz_spv_ps_main);
    s_sm_vs_blit = shader(gz_spv_vs_blit, sizeof gz_spv_vs_blit);
    s_sm_ps_one = shader(gz_spv_ps_one, sizeof gz_spv_ps_one);
    s_sm_vs_prog = shader(gz_spv_vs_prog, sizeof gz_spv_vs_prog);
    s_sm_ps_spec = shader(gz_spv_ps_spec, sizeof gz_spv_ps_spec);
    if (!s_sm_vs || !s_sm_ps || !s_sm_vs_blit || !s_sm_ps_one) { LOG("shader modules failed\n"); return 0; }

    memset(&cpi, 0, sizeof cpi);
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = s_qfam;
    if (vkCreateCommandPool(s_dev, &cpi, NULL, &s_cpool) != VK_SUCCESS) return 0;
    for (i = 0; i < FRAMES; i++) {
        VkCommandBufferAllocateInfo ai;
        VkFenceCreateInfo fi;
        VkSemaphoreCreateInfo si;
        VkDescriptorPoolSize ps[3];
        VkDescriptorPoolCreateInfo dpi;
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = s_cpool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(s_dev, &ai, &s_fr[i].cb) != VK_SUCCESS) return 0;
        memset(&fi, 0, sizeof fi);
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(s_dev, &fi, NULL, &s_fr[i].fence);
        memset(&si, 0, sizeof si);
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(s_dev, &si, NULL, &s_fr[i].acquire);
        ps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; ps[0].descriptorCount = 8192 * 4;
        ps[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;          ps[1].descriptorCount = 8192 * 8;
        ps[2].type = VK_DESCRIPTOR_TYPE_SAMPLER;                ps[2].descriptorCount = 8192 * 4;
        memset(&dpi, 0, sizeof dpi);
        dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.maxSets = 8192; dpi.poolSizeCount = 3; dpi.pPoolSizes = ps;
        if (vkCreateDescriptorPool(s_dev, &dpi, NULL, &s_fr[i].pool) != VK_SUCCESS) return 0;
    }
    memset(&sci, 0, sizeof sci);
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(s_dev, &sci, NULL, &s_smp_default);
    return 1;
}

/* 1x1 white stand-ins for unbound texture slots (Vulkan has no null view). */
static void dummies_create(void)
{
    Alloc a;
    VkBufferImageCopy r;
    int k;
    img_create(&s_white2d, 1, 1, 1, 1, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0);
    img_create(&s_whitecube, 1, 1, 1, 6, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1);
    if (!frame_alloc(24, 4, &a)) return;
    for (k = 0; k < 6; k++) ((uint32_t *)a.ptr)[k] = 0xFFFFFFFFu;
    img_barrier(s_white2d.img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    img_barrier(s_whitecube.img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 6, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&r, 0, sizeof r);
    r.bufferOffset = a.off;
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; r.imageSubresource.layerCount = 1;
    r.imageExtent.width = r.imageExtent.height = r.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cb(), a.buf, s_white2d.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    r.imageSubresource.layerCount = 6;
    vkCmdCopyBufferToImage(cb(), a.buf, s_whitecube.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    img_barrier(s_white2d.img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    img_barrier(s_whitecube.img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 6, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

static int init_device(void)
{
    s_scale      = env_int("GZ_RENDER_SCALE", 0, 0, 6);
    s_win_scale  = env_int("GZ_WINDOW_SCALE", 2, 1, 4);
    s_fullscreen = env_int("GZ_FULLSCREEN", 0, 0, 1);
    s_aniso      = env_int("GZ_ANISO", 16, 1, 16);
    s_vsync      = env_int("GZ_VSYNC", 0, 0, 1);
    s_cull       = env_int("GZ_GPU_CULL", 0, 0, 1);
    s_wide = gz_opt_wide();
    s_win_h = 480u * (uint32_t)s_win_scale;
    s_win_w = s_wide ? (s_win_h * 16u + 8u) / 9u : 640u * (uint32_t)s_win_scale;
    if (!vk_load_library()) { LOG("no Vulkan library on this system\n"); return 0; }
#ifndef _WIN32
    if (!window_open() || !window_extensions()) return 0;
    {
        int w, h;
        SDL_Vulkan_GetDrawableSize(s_win, &w, &h);
        if (s_fullscreen && h > 0) { s_win_w = (uint32_t)w; s_win_h = (uint32_t)h; }
    }
#endif
    if (!instance_create() || !window_create() || !device_create() || !objects_create()) return 0;
    if (s_scale == 0) {
        uint32_t w, h;
        window_size(&w, &h);
        if (!h) h = s_win_h;
        s_scale = (int)((h + 479u) / 480u);
        if (s_scale < 1) s_scale = 1;
        if (s_scale > 6) s_scale = 6;
    }
    if (!swap_create()) { LOG("swap chain creation failed\n"); return 0; }
    frame_begin();
    dummies_create();
    LOG("device up: window %ux%u, render scale %d, vsync %d\n", s_win_w, s_win_h, s_scale, s_vsync);
    return 1;
}

static int ready(void)
{
    if (s_ready) return 1;
    if (s_failed) return 0;
    if (!init_device()) {
        s_failed = 1;
        LOG("init failed; nothing will be drawn (GZ_GPU=0 for the CPU rasteriser)\n");
        return 0;
    }
    s_ready = 1;
    return 1;
}

/* ── Render targets and depth buffers ─────────────────────── */

static void begin_rendering(Rt *r, Ds *d)
{
    VkRenderingAttachmentInfo ca, da;
    VkRenderingInfo ri;
    VkViewport vp;
    VkRect2D sc;
    if (s_rendering && s_bound_rt == r && s_bound_ds == d) return;
    end_rendering();
    if (r->sampled) { barrier_all(); r->sampled = 0; }   /* drawing over what was just read */
    memset(&ca, 0, sizeof ca);
    ca.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    ca.imageView = r->im.view; ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    memset(&da, 0, sizeof da);
    da.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    if (d) {
        da.imageView = d->im.view; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    memset(&ri, 0, sizeof ri);
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent.width = d ? (r->pw < d->pw ? r->pw : d->pw) : r->pw;
    ri.renderArea.extent.height = d ? (r->ph < d->ph ? r->ph : d->ph) : r->ph;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    if (d) { ri.pDepthAttachment = &da; ri.pStencilAttachment = &da; }
    vkCmdBeginRendering(cb(), &ri);
    vp.x = 0; vp.y = 0; vp.width = (float)r->pw; vp.height = (float)r->ph; vp.minDepth = 0; vp.maxDepth = 1;
    vkCmdSetViewport(cb(), 0, 1, &vp);
    sc.offset.x = sc.offset.y = 0; sc.extent = ri.renderArea.extent;
    vkCmdSetScissor(cb(), 0, 1, &sc);
    s_rendering = 1; s_bound_rt = r; s_bound_ds = d;
    r->drawn = 1;
}

static void rt_free(Rt *r)
{
    if (s_bound_rt == r) end_rendering();
    junk_img(&r->im);
    memset(r, 0, sizeof *r);
}

static int rt_image(Rt *r, uint32_t w, uint32_t h)
{
    r->pw = PX_W(w, h); r->ph = h * (uint32_t)s_scale;
    if (!img_create(&r->im, r->pw, r->ph, 1, 1, VK_FORMAT_B8G8R8A8_UNORM,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                    | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0))
        return 0;
    {
        VkClearColorValue black = { { 0.0f, 0.0f, 0.0f, 1.0f } };
        VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        img_barrier(r->im.img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdClearColorImage(cb(), r->im.img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &rr);
    }
    return 1;
}

static Rt *rt_get(const Nv2aSurface *s)
{
    int i, oldest = 0;
    Rt *r;
    /* One target per address, grown (contents kept) for a wider clip, never
     * recreated for a narrower one (gpu_d3d11.c rt_get). */
    for (i = 0; i < MAX_RT; i++) {
        r = &s_rts[i];
        if (r->im.img && r->va == s->color_va) {
            if (s->width <= r->w && s->height <= r->h) {
                r->last_frame = s_frame; r->pitch = s->pitch; r->bpp = s->bytes_per_pixel;
                return r;
            }
            {
                Rt old = *r;
                uint32_t w = s->width > old.w ? s->width : old.w, h = s->height > old.h ? s->height : old.h;
                VkImageCopy c;
                end_rendering();
                memset(r, 0, sizeof *r);
                if (!rt_image(r, w, h)) { *r = old; return r; }
                barrier_all();
                memset(&c, 0, sizeof c);
                c.srcSubresource.aspectMask = c.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                c.srcSubresource.layerCount = c.dstSubresource.layerCount = 1;
                c.extent.width = old.pw; c.extent.height = old.ph; c.extent.depth = 1;
                vkCmdCopyImage(cb(), old.im.img, VK_IMAGE_LAYOUT_GENERAL, r->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
                barrier_all();
                r->va = old.va; r->w = w; r->h = h;
                r->pitch = s->pitch; r->bpp = s->bytes_per_pixel; r->last_frame = s_frame;
                if (s_present_rt == &old || s_present_rt == r) s_present_rt = r;
                junk_img(&old.im);
                return r;
            }
        }
    }
    for (i = 0; i < MAX_RT; i++)
        if (!s_rts[i].im.img) { oldest = i; goto create; }
    for (i = 1; i < MAX_RT; i++)
        if (s_rts[i].last_frame < s_rts[oldest].last_frame) oldest = i;
    if (s_present_rt == &s_rts[oldest]) s_present_rt = NULL;
    rt_free(&s_rts[oldest]);
create:
    r = &s_rts[oldest];
    end_rendering();
    if (!rt_image(r, s->width, s->height)) return NULL;
    barrier_all();
    r->va = s->color_va; r->w = s->width; r->h = s->height;
    r->pitch = s->pitch; r->bpp = s->bytes_per_pixel; r->last_frame = s_frame;
    return r;
}

static Ds *ds_get(uint32_t va, uint32_t w, uint32_t h)
{
    int i, slot = -1;
    Ds *d;
    VkImg keep;
    uint32_t kpw = 0, kph = 0;
    VkClearDepthStencilValue cv = { 1.0f, 0 };
    VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };

    if (!va) return NULL;
    memset(&keep, 0, sizeof keep);
    for (i = 0; i < MAX_DS; i++) {
        d = &s_dss[i];
        if (d->im.img && d->va == va) {
            if (w <= d->w && h <= d->h) return d;
            if (w < d->w) w = d->w;
            if (h < d->h) h = d->h;
            slot = i;
            break;
        }
    }
    if (slot < 0)
        for (i = 0; i < MAX_DS; i++)
            if (!s_dss[i].im.img) { slot = i; break; }
    if (slot < 0) slot = (int)(s_frame % MAX_DS);
    d = &s_dss[slot];
    if (d == s_bound_ds || s_rendering) end_rendering();
    if (d->im.img && d->va == va) { keep = d->im; kpw = d->pw; kph = d->ph; }
    else junk_img(&d->im);
    memset(d, 0, sizeof *d);
    d->pw = PX_W(w, h); d->ph = h * (uint32_t)s_scale;
    if (!img_create(&d->im, d->pw, d->ph, 1, 1, s_ds_format,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                    | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0))
        return NULL;
    img_barrier(d->im.img, rr.aspectMask, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdClearDepthStencilImage(cb(), d->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &rr);
    if (keep.img) {
        VkImageCopy c;
        memset(&c, 0, sizeof c);
        c.srcSubresource.aspectMask = c.dstSubresource.aspectMask = rr.aspectMask;
        c.srcSubresource.layerCount = c.dstSubresource.layerCount = 1;
        c.extent.width = kpw; c.extent.height = kph; c.extent.depth = 1;
        img_barrier(keep.img, rr.aspectMask, 1, 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkCmdCopyImage(cb(), keep.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->im.img,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        junk_img(&keep);
    }
    img_barrier(d->im.img, rr.aspectMask, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    d->va = va; d->w = w; d->h = h;
    return d;
}

static void note_present_candidate(Rt *r, const Nv2aSurface *s)
{
    if (s->width / (s->aa_sx ? s->aa_sx : 1) >= 640 && s->height / (s->aa_sy ? s->aa_sy : 1) >= 480)
        s_present_rt = r;
}

/* ── Textures (gpu_d3d11.c texture_srv) ───────────────────── */

static uint32_t tex_hash(const Nv2aTexture *t)
{
    uint32_t bytes = t->width * t->height * 4u, step, h = 2166136261u, i;
    const uint8_t *p = GUEST_PTR(t->offset);
    if (bytes > (4u << 20)) bytes = 4u << 20;
    step = bytes / 2048u;
    if (step < 16) step = 16;
    step &= ~3u;
    for (i = 0; i + 4 <= bytes; i += step) h = (h ^ *(const uint32_t *)(p + i)) * 16777619u;
    if (t->palette && t->color == 0x0B) {
        const uint32_t *pal = (const uint32_t *)GUEST_PTR(t->palette);
        h = (h ^ t->palette) * 16777619u;
        for (i = 0; i < 256; i += 4) h = (h ^ pal[i]) * 16777619u;
    }
    return h;
}

/* Replacement textures (textures_ps/<hash>*.bmp, 32-bit BGRA), as gpu_d3d11.c. */
static uint32_t s_repl_hash[512];
static char     s_repl_file[512][64];
static int      s_repl_n = -1;
static const char *s_repl_dir;

static void repl_add(const char *name)
{
    char *end;
    unsigned long v = strtoul(name, &end, 16);
    size_t n = strlen(name);
    if (end - name == 8 && s_repl_n < 512 && n < 64 && n > 4 && !strcmp(name + n - 4, ".bmp")) {
        s_repl_hash[s_repl_n] = (uint32_t)v;
        strcpy(s_repl_file[s_repl_n++], name);
    }
}

static void repl_scan(void)
{
    s_repl_n = 0;
    s_repl_dir = getenv("GZ_TEX_DIR");
    if (!s_repl_dir || !*s_repl_dir) s_repl_dir = "textures_ps";
    if (getenv("GZ_TEX_REPLACE") && getenv("GZ_TEX_REPLACE")[0] == '0') return;
#ifdef _WIN32
    {
        WIN32_FIND_DATAA fd;
        HANDLE h;
        char pat[MAX_PATH];
        snprintf(pat, sizeof pat, "%s/*.bmp", s_repl_dir);
        if ((h = FindFirstFileA(pat, &fd)) == INVALID_HANDLE_VALUE) return;
        do repl_add(fd.cFileName); while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    {
        DIR *d = opendir(s_repl_dir);
        struct dirent *e;
        if (!d) return;
        while ((e = readdir(d)) != NULL) repl_add(e->d_name);
        closedir(d);
    }
#endif
    LOG("%d replacement texture(s) in %s\n", s_repl_n, s_repl_dir);
}

static uint32_t *repl_load(uint32_t hash, uint32_t *w, uint32_t *h)
{
    char path[600];
    uint8_t hdr[54];
    uint32_t off, y, *px;
    int32_t bw, bh, i;
    FILE *f;
    for (i = 0; i < s_repl_n; i++) if (s_repl_hash[i] == hash) break;
    if (i >= s_repl_n) return NULL;
    snprintf(path, sizeof path, "%s/%s", s_repl_dir, s_repl_file[i]);
    if (!(f = fopen(path, "rb"))) return NULL;
    if (fread(hdr, 1, 54, f) != 54 || hdr[0] != 'B' || hdr[28] != 32) { fclose(f); return NULL; }
    memcpy(&off, hdr + 10, 4); memcpy(&bw, hdr + 18, 4); memcpy(&bh, hdr + 22, 4);
    if (bw <= 0 || bw > 4096 || bh == 0 || bh > 4096 || bh < -4096) { fclose(f); return NULL; }
    *w = (uint32_t)bw; *h = (uint32_t)(bh < 0 ? -bh : bh);
    px = (uint32_t *)malloc((size_t)*w * *h * 4);
    fseek(f, (long)off, SEEK_SET);
    for (y = 0; px && y < *h; y++)
        if (fread(px + (size_t)(bh < 0 ? y : *h - 1 - y) * *w, 4, *w, f) != *w) { free(px); px = NULL; }
    fclose(f);
    return px;
}

static uint32_t *s_decode_buf;
static size_t    s_decode_cap;
extern volatile int32_t gz_stat_decode_us, gz_stat_decode_n, gz_stat_decode_px;

static uint32_t mip_count(uint32_t w, uint32_t h)
{
    uint32_t n = 1;
    while (w > 1 || h > 1) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; n++; }
    return n;
}

/* Upload px (faces x w x h BGRA) into x->im, generating mips for 2D. Called
 * outside rendering. */
static int tex_upload(Tex *x, const uint32_t *px, uint32_t w, uint32_t h, uint32_t faces, int mips)
{
    Alloc a;
    VkBufferImageCopy r;
    size_t bytes = (size_t)w * h * 4 * faces;
    uint32_t levels = mips ? mip_count(w, h) : 1, l;
    int fresh = !x->im.img || x->iw != w || x->ih != h || x->mips != levels;

    if (fresh) {
        junk_img(&x->im);
        if (!img_create(&x->im, w, h, levels, faces, VK_FORMAT_B8G8R8A8_UNORM,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        faces == 6))
            return 0;
        x->iw = w; x->ih = h; x->mips = levels;
    }
    if (!frame_alloc(bytes, 16, &a)) return 0;
    memcpy(a.ptr, px, bytes);
    img_barrier(x->im.img, VK_IMAGE_ASPECT_COLOR_BIT, levels, faces,
                fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&r, 0, sizeof r);
    r.bufferOffset = a.off;
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; r.imageSubresource.layerCount = faces;
    r.imageExtent.width = w; r.imageExtent.height = h; r.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cb(), a.buf, x->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    for (l = 1; l < levels; l++) {
        VkImageMemoryBarrier b;
        VkImageBlit bl;
        int32_t sw = (int32_t)(w >> (l - 1)), sh = (int32_t)(h >> (l - 1));
        memset(&b, 0, sizeof b);
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = x->im.img;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.baseMipLevel = l - 1; b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cb(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        memset(&bl, 0, sizeof bl);
        bl.srcSubresource.aspectMask = bl.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bl.srcSubresource.mipLevel = l - 1; bl.dstSubresource.mipLevel = l;
        bl.srcSubresource.layerCount = bl.dstSubresource.layerCount = 1;
        bl.srcOffsets[1].x = sw < 1 ? 1 : sw; bl.srcOffsets[1].y = sh < 1 ? 1 : sh; bl.srcOffsets[1].z = 1;
        bl.dstOffsets[1].x = sw / 2 < 1 ? 1 : sw / 2; bl.dstOffsets[1].y = sh / 2 < 1 ? 1 : sh / 2; bl.dstOffsets[1].z = 1;
        vkCmdBlitImage(cb(), x->im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, x->im.img,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        vkCmdPipelineBarrier(cb(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    }
    img_barrier(x->im.img, VK_IMAGE_ASPECT_COLOR_BIT, levels, faces, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return 1;
}

/* The image view to sample texture t with (NULL: nothing usable). A texture
 * at a recently drawn render target's address samples that target. Must be
 * called outside rendering: it may upload or barrier. */
static VkImageView texture_view(const Nv2aTexture *t, Rt *drawing)
{
    uint32_t i, slot = MAX_TEX, oldest = 0, h;
    Tex *x;
    static uint16_t ix[4096];
    uint16_t *ixs;

    if (!t->offset || !t->width || !t->height || t->width > 4096 || t->height > 4096) return VK_NULL_HANDLE;
    if (!t->cube)
        for (i = 0; i < MAX_RT; i++)
            if (s_rts[i].im.img && s_rts[i].va == t->offset) {
                if (s_rts[i].last_frame + 4 >= s_frame) {
                    if (&s_rts[i] == drawing) return VK_NULL_HANDLE;    /* can't sample the target being drawn */
                    if (s_rts[i].drawn) { end_rendering(); barrier_all(); s_rts[i].drawn = 0; }
                    s_rts[i].sampled = 1;
                    return s_rts[i].im.view;
                }
                break;
            }
    {
        uint32_t k = ((t->offset >> 5) ^ (t->offset >> 17) ^ t->color) & 4095u, c = ix[k];
        i = 0;
        if (c) {
            x = &s_texs[c - 1];
            if (x->im.img && x->offset == t->offset && x->w == t->width && x->h == t->height
                    && x->color == t->color && x->cube == (t->cube ? 1u : 0u))
                i = c - 1;
        }
        ixs = &ix[k];
    }
    for (; i < MAX_TEX; i++) {
        x = &s_texs[i];
        if (x->im.img && x->offset == t->offset && x->w == t->width && x->h == t->height
                && x->color == t->color && x->cube == (t->cube ? 1u : 0u)) {
            *ixs = (uint16_t)(i + 1);
            if (x->gen != gz_opt_generation()) {
                x->gen = gz_opt_generation();
                x->hash = tex_hash(t); x->frame = s_frame; x->used = s_frame;
                slot = i;
                goto decode;
            }
            if (x->frame == s_frame) { x->used = s_frame; return x->im.view; }
            x->frame = s_frame; x->used = s_frame;
            if (x->tick == s_frame_tick) return x->im.view;
            x->tick = s_frame_tick;
            h = tex_hash(t);
            if (h == x->hash) return x->im.view;
            x->hash = h;
            slot = i;
            goto decode;
        }
        if (!x->im.img) { if (slot == MAX_TEX) slot = i; }
        else if (x->used < s_texs[oldest].used) oldest = i;
    }
    if (slot == MAX_TEX) slot = oldest;
    x = &s_texs[slot];
    junk_img(&x->im);
    memset(x, 0, sizeof *x);
    x->offset = t->offset; x->w = t->width; x->h = t->height; x->color = t->color;
    x->cube = t->cube ? 1u : 0u;
    x->hash = tex_hash(t); x->frame = s_frame; x->used = s_frame;
    x->gen = gz_opt_generation();
decode:
    x = &s_texs[slot];
    {
        uint32_t faces = t->cube ? 6u : 1u, f, uw = t->width, uh = t->height, *rp = NULL;
        uint32_t stride = t->cube ? nv2a_backend_texture_face_stride(t) : 0;
        size_t face_px = (size_t)t->width * t->height, need = face_px * faces;
        const uint32_t *px;
        uint64_t t0 = now_us();
        int ok;
        gz_stat_decode_n++;
        gz_stat_decode_px += (int32_t)need;
        if (need > s_decode_cap) {
            free(s_decode_buf);
            s_decode_buf = (uint32_t *)malloc(need * 4);
            s_decode_cap = s_decode_buf ? need : 0;
            if (!s_decode_buf) return VK_NULL_HANDLE;
        }
        for (f = 0; f < faces; f++) {
            Nv2aTexture ft = *t;
            ft.offset = t->offset + f * stride; ft.cube = 0;
            if (!nv2a_backend_decode_texture(&ft, s_decode_buf + f * face_px)) {
                static int warned;
                if (warned++ < 8)
                    LOG("texture format 0x%02X (%ux%u at %08X) not decodable\n", t->color, t->width, t->height, t->offset);
                junk_img(&x->im);
                memset(x, 0, sizeof *x);
                return VK_NULL_HANDLE;
            }
        }
        px = s_decode_buf;
        if (s_repl_n < 0) repl_scan();
        if (!t->cube && s_repl_n && gz_opt_ps_prompts()) {
            uint32_t ch = 2166136261u, n = t->width * t->height, k;
            for (k = 0; k < n; k++) ch = (ch ^ s_decode_buf[k]) * 16777619u;
            ch = (ch ^ t->width) * 16777619u;
            if ((rp = repl_load(ch, &uw, &uh)) != NULL) px = rp;
        }
        end_rendering();
        ok = tex_upload(x, px, uw, uh, faces, s_aniso > 1 && !t->cube && (uw > 1 || uh > 1));
        free(rp);
        gz_stat_decode_us += (int32_t)(now_us() - t0);
        s_st_uploads++;
        if (!ok) { junk_img(&x->im); memset(x, 0, sizeof *x); return VK_NULL_HANDLE; }
    }
    return x->im.view;
}

/* ── Constants (gpu_d3d11.c set_consts) ───────────────────── */

static void unpack_argb(uint32_t c, float o[4])
{
    o[0] = (float)((c >> 16) & 0xFF) / 255.0f; o[1] = (float)((c >> 8) & 0xFF) / 255.0f;
    o[2] = (float)(c & 0xFF) / 255.0f;         o[3] = (float)(c >> 24) / 255.0f;
}

static VkSpec s_spec;                   /* of the draw being set up (make_consts) */

static void make_consts(Consts *c, const Nv2aSurface *s, const Nv2aBatch *b, Rt *r, int tex0_on, const int bound[4])
{
    const Nv2aRenderState *rs = b->state;
    const Nv2aCombiner *rc = b->combiner;
    float zmax_units = rs->zeta_bits == 16 ? 65535.0f : 16777215.0f;
    int st;
    static float zoom = -1.0f;
    memset(c, 0, sizeof *c);
    c->wide[0] = s_draw_squeeze; c->wide[1] = s_draw_anchor; c->wide[2] = s_draw_yscale;
    if (zoom < 0.0f) { const char *e = getenv("GZ_DEBUG_ZOOM"); zoom = (e && atof(e) > 0.05) ? (float)atof(e) : 1.0f; }
    c->dbg[0] = zoom;
    c->inv_size[0] = 1.0f / (float)(r ? r->w : s->width);
    c->inv_size[1] = 1.0f / (float)(r ? r->h : s->height);
    if (b->texture && b->texture->width && b->texture->height) {
        c->tex0_inv[0] = 1.0f / (float)b->texture->width;
        c->tex0_inv[1] = 1.0f / (float)b->texture->height;
    }
    c->zscale = 1.0f / zmax_units;
    c->tex0_on = tex0_on ? 1.0f : 0.0f;
    if (rc) {
        c->rc_on = 1.0f;
        c->fin[0] = rc->final0; c->fin[1] = rc->final1; c->fin[2] = rc->control; c->fin[3] = rc->stage_program;
        c->misc[0] = b->clip_plane_mode;
        c->misc[2] = b->dot_mapping;
        c->misc[3] = b->other_stage_input & 0x7FFFFFFFu;
        unpack_argb(b->fog_color, c->fog_color);
        unpack_argb(rc->final_c0, c->final_c0);
        unpack_argb(rc->final_c1, c->final_c1);
        for (st = 0; st < 8; st++) {
            c->stage[st][0] = rc->color_icw[st]; c->stage[st][1] = rc->alpha_icw[st];
            c->stage[st][2] = rc->color_ocw[st]; c->stage[st][3] = rc->alpha_ocw[st];
            unpack_argb(rc->factor0[st], c->factor0[st]);
            unpack_argb(rc->factor1[st], c->factor1[st]);
        }
        for (st = 0; st < 4; st++) {
            const Nv2aTexture *t = b->textures[st];
            c->tex_scale[st][0] = c->tex_scale[st][1] = 1.0f;
            if (t && !t->normalized && t->width && t->height) {
                c->tex_scale[st][0] = 1.0f / (float)t->width;
                c->tex_scale[st][1] = 1.0f / (float)t->height;
            }
            c->tex_scale[st][2] = bound[st] ? (t && t->cube ? 2.0f : 1.0f) : 0.0f;
            c->tex_scale[st][3] = (t && t->normalized) ? 1.0f : 0.0f;
        }
    }
    c->alpha_ref  = (float)(rs->alpha_ref & 0xFF) / 255.0f;
    c->alpha_func = rs->alpha_test_enable ? (float)((rs->alpha_func >= 0x200 && rs->alpha_func <= 0x207)
                                                    ? rs->alpha_func - 0x200 : 7) : 7.0f;
    c->zmin = rs->depth_min; c->zmax = rs->depth_max;
    c->zclip_on = (rs->depth_max > rs->depth_min && rs->depth_max > 0.0f) ? 1.0f : 0.0f;
    {
        /* What a specialised shader bakes in: gpu_d3d11.c's PsKey, in
         * ps_spec's constant order. */
        VkSpec *k = &s_spec;
        memset(k, 0, sizeof *k);
        k->v[46] = (uint32_t)c->alpha_func;
        k->v[47] = c->zclip_on > 0.5f;
        if (rc) {
            uint32_t n = c->fin[2] & 255u;
            if (n > 8) n = 8;
            k->v[44] = 1;
            memcpy(&k->v[0], c->fin, 16);
            memcpy(&k->v[4], c->misc, 16);
            memcpy(&k->v[8], c->stage, n * 16u);
            for (st = 0; st < 4; st++) k->v[40 + st] = (uint32_t)c->tex_scale[st][2];
        } else {
            k->v[45] = tex0_on ? 1u : 0u;
        }
    }
}

/* ── Specialised pixel shaders (gpu_d3d11.c ps_variant) ──────
 * The generic ps_main interprets the combiners per pixel. Each distinct
 * setup also gets a pipeline with ps_spec specialised to it, built on a
 * worker thread while the generic one draws, so nothing waits for it.
 * GZ_PS_SPEC=0: generic shader only. */
#define SPEC_VARS 2048
typedef struct { uint64_t key, hash; VkSpec spec; volatile int state; VkPipeline p; } SpecVar;
static SpecVar  s_sv[SPEC_VARS];
static int      s_sv_queue[SPEC_VARS], s_sv_head, s_sv_tail;
static int      s_spec_on = -1;
#ifdef _WIN32
static CRITICAL_SECTION s_sv_lock;
static HANDLE   s_sv_event;
#define SV_LOCK()   EnterCriticalSection(&s_sv_lock)
#define SV_UNLOCK() LeaveCriticalSection(&s_sv_lock)
#else
static pthread_mutex_t s_sv_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_sv_cond = PTHREAD_COND_INITIALIZER;
#define SV_LOCK()   pthread_mutex_lock(&s_sv_lock)
#define SV_UNLOCK() pthread_mutex_unlock(&s_sv_lock)
#endif

static uint64_t fnv64(const void *d, size_t n, uint64_t h)
{
    const uint8_t *q = (const uint8_t *)d;
    while (n--) h = (h ^ *q++) * 1099511628211ull;
    return h;
}

static void spec_work(void)
{
    for (;;) {
        SpecVar *v;
        VkPipeline pl;
        SV_LOCK();
#ifdef _WIN32
        while (s_sv_tail == s_sv_head) { SV_UNLOCK(); WaitForSingleObject(s_sv_event, INFINITE); SV_LOCK(); }
#else
        while (s_sv_tail == s_sv_head) pthread_cond_wait(&s_sv_cond, &s_sv_lock);
#endif
        v = &s_sv[s_sv_queue[s_sv_tail % SPEC_VARS]];
        s_sv_tail++;
        SV_UNLOCK();
        pl = pipeline_build(v->key, &v->spec);
        SV_LOCK();
        v->p = pl;
        v->state = pl ? 2 : 3;
        SV_UNLOCK();
    }
}

#ifdef _WIN32
static DWORD WINAPI spec_thread(LPVOID a) { (void)a; spec_work(); return 0; }
#else
static void *spec_thread(void *a) { (void)a; spec_work(); return NULL; }
#endif

/* The pipeline for the draw whose constants were just made: its
 * specialised one once built, the generic one until then. */
static VkPipeline spec_pipeline(int kind, const Nv2aRenderState *rs, int has_ds, uint32_t vmask, VkPipeline generic)
{
    uint64_t key, h;
    uint32_t i, at;
    VkPipeline out = generic;
    if (s_spec_on < 0) {
        s_spec_on = env_int("GZ_PS_SPEC", 1, 0, 1);
        if (s_spec_on) {
#ifdef _WIN32
            InitializeCriticalSection(&s_sv_lock);
            s_sv_event = CreateEventA(NULL, FALSE, FALSE, NULL);
            if (!s_sv_event || !CreateThread(NULL, 0, spec_thread, NULL, 0, NULL)) s_spec_on = 0;
#else
            pthread_t t;
            if (pthread_create(&t, NULL, spec_thread, NULL) != 0) s_spec_on = 0;
            else pthread_detach(t);
#endif
        }
    }
    if (!s_spec_on || !generic) return generic;
    key = (uint64_t)kind | (uint64_t)has_ds << 2 | (uint64_t)(rs->blend_enable ? 1 : 0) << 3
        | (uint64_t)map_blend(rs->blend_src, 0) << 4 | (uint64_t)map_blend(rs->blend_dst, 0) << 9
        | (uint64_t)map_blend(rs->blend_src, 1) << 14 | (uint64_t)map_blend(rs->blend_dst, 1) << 19
        | (uint64_t)map_blend_op(rs->blend_eq) << 24 | (uint64_t)map_mask(rs->color_mask) << 27
        | (uint64_t)(vmask & 0xFFFFu) << 32;
    h = fnv64(&s_spec, sizeof s_spec, key * 0x9E3779B97F4A7C15ull);
    at = (uint32_t)(h ^ (h >> 32)) % SPEC_VARS;
    SV_LOCK();
    for (i = 0; i < 16; i++, at = (at + 1) % SPEC_VARS) {
        SpecVar *v = &s_sv[at];
        if (v->state == 0) {
            if (s_sv_head - s_sv_tail < SPEC_VARS - 1) {
                v->key = key; v->hash = h; v->spec = s_spec; v->state = 1;
                s_sv_queue[s_sv_head % SPEC_VARS] = (int)at;
                s_sv_head++;
#ifdef _WIN32
                SetEvent(s_sv_event);
#else
                pthread_cond_signal(&s_sv_cond);
#endif
            }
            break;
        }
        if (v->hash == h && v->key == key && !memcmp(&v->spec, &s_spec, sizeof s_spec)) {
            if (v->state == 2) out = v->p;
            break;
        }
    }
    SV_UNLOCK();
    return out;
}

/* ── Widescreen (gpu_d3d11.c, unchanged rules) ────────────── */

static int wide_hud_on(void) { return s_overlay_frame != s_frame && gz_wide_fight_active(); }

static void wide_hud_scale(float anchor, float w, float ymx, float h)
{
    static int mode = -1;
    if (mode < 0) { const char *e = getenv("GZ_WIDE_HUD"); mode = (e && *e) ? atoi(e) : 2; }
    if (mode < 2 || ymx > h * 0.30f || !wide_hud_on()) return;
    if (anchor == w * 0.5f) { s_draw_squeeze = 1.0f; return; }
    s_draw_squeeze = 4.0f / 3.0f; s_draw_anchor = w * 0.5f; s_draw_yscale = 4.0f / 3.0f;
}

static float wide_anchor(float mn, float mx, float ymx, float w, float h)
{
    static int hud = -1;
    if (hud < 0) hud = !(getenv("GZ_WIDE_HUD") && getenv("GZ_WIDE_HUD")[0] == '0');
    if (hud && ymx <= h * 0.30f && wide_hud_on()) {
        float c = (mn + mx) * 0.5f;
        if (c < w * 0.465f) return 0.0f;
        if (c > w * 0.535f) return w;
    }
    return w * 0.5f;
}

static int wide_untextured(const Nv2aBatch *b)
{
    int st;
    if (!b->combiner) return !b->texture && !b->textures[0];
    for (st = 0; st < 4; st++) if (b->textures[st]) return 0;
    return 1;
}

static void wide_setup(Rt *r, const Nv2aBatch *b, const Nv2aVertex *verts, uint32_t count)
{
    s_draw_squeeze = 0.0f;
    if (!WIDE_SURF(r->w, r->h)) return;
    s_draw_squeeze = 0.75f;
    s_draw_anchor = (float)r->w * 0.5f;
    s_draw_yscale = 1.0f;
    if (count <= 96) {
        float mn = 1e9f, mx = -1e9f, ymn = 1e9f, ymx = -1e9f;
        uint32_t k;
        int flat = 1;
        for (k = 0; k < count; k++) {
            if (verts[k].x < mn) mn = verts[k].x;
            if (verts[k].x > mx) mx = verts[k].x;
            if (verts[k].y < ymn) ymn = verts[k].y;
            if (verts[k].y > ymx) ymx = verts[k].y;
            if (verts[k].rhw < 0.99999f || verts[k].rhw > 1.00001f) flat = 0;
        }
        if (flat && count <= 6 && wide_untextured(b) && mn <= 2.0f && mx >= (float)r->w - 2.0f) {
            s_draw_squeeze = 0.0f;
            if (ymn <= 2.0f && ymx >= (float)r->h - 2.0f
                    && ((verts[0].diffuse >> 16) & 0xFF) < 0x60u && ((verts[0].diffuse >> 8) & 0xFF) < 0x60u
                    && (verts[0].diffuse & 0xFF) < 0x60u)
                s_overlay_frame = s_frame;
        } else if (flat) {
            s_draw_anchor = wide_anchor(mn, mx, ymx, (float)r->w, (float)r->h);
            wide_hud_scale(s_draw_anchor, (float)r->w, ymx, (float)r->h);
        }
    }
}

/* ── Back-end callbacks ───────────────────────────────────── */

static Nv2aVertex *s_cull_buf;
static uint32_t    s_cull_cap;

static const Nv2aVertex *cull_tris(const Nv2aBatch *b, uint32_t *count)
{
    const Nv2aRenderState *rs = b->state;
    uint32_t i, n = 0;
    if (!s_cull || !rs->cull_enable) { *count = b->count; return b->vertices; }
    if (b->count > s_cull_cap) {
        free(s_cull_buf);
        s_cull_buf = (Nv2aVertex *)malloc(sizeof(Nv2aVertex) * b->count);
        s_cull_cap = s_cull_buf ? b->count : 0;
        if (!s_cull_buf) { *count = b->count; return b->vertices; }
    }
    for (i = 0; i + 2 < b->count; i += 3) {
        const Nv2aVertex *a = &b->vertices[i], *c = &b->vertices[i + 1], *d = &b->vertices[i + 2];
        float area = (c->x - a->x) * (d->y - a->y) - (d->x - a->x) * (c->y - a->y);
        int front = rs->front_face == 0x900 ? area > 0.0f : !(area > 0.0f);
        int drop = rs->cull_face == 0x408 || (rs->cull_face == 0x405 && !front) || (rs->cull_face == 0x404 && front);
        if (drop) continue;
        s_cull_buf[n++] = *a; s_cull_buf[n++] = *c; s_cull_buf[n++] = *d;
    }
    *count = n;
    return s_cull_buf;
}

static void be_clear(const Nv2aSurface *s, const Nv2aRenderState *rs, uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    Rt *r;
    Ds *d;
    VkClearAttachment ca[2];
    VkClearRect cr;
    uint32_t n = 0;

    if (!ready() || !s->color_va || !s->width || !s->height) return;
    if (!(r = rt_get(s))) return;
    d = ds_get(rs->zeta_va, r->w, r->h);
    begin_rendering(r, d);
    memset(ca, 0, sizeof ca);
    memset(&cr, 0, sizeof cr);
    cr.layerCount = 1;
    if (flags & 0xF0) {
        float c[4];
        unpack_argb(argb, c);
        if ((flags & 0xF0) == 0xF0) {
            ca[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            memcpy(ca[n].clearValue.color.float32, c, sizeof c);
            cr.rect.extent.width = PX_W(s->width, s->height);
            cr.rect.extent.height = s->height * (uint32_t)s_scale;
            if (s->width >= r->w && s->height >= r->h) { cr.rect.extent.width = r->pw; cr.rect.extent.height = r->ph; }
            if (d) {
                if (cr.rect.extent.width > d->pw) cr.rect.extent.width = d->pw;
                if (cr.rect.extent.height > d->ph) cr.rect.extent.height = d->ph;
            }
            vkCmdClearAttachments(cb(), 1, &ca[n], 1, &cr);
        } else {
            /* Per-channel clear (Godzilla clears only alpha before a
             * destination-alpha pass): a full-viewport triangle of the clear
             * colour through a write mask, as gpu_d3d11.c. */
            VkColorComponentFlags m = ((flags & 0x10) ? VK_COLOR_COMPONENT_R_BIT : 0)
                                    | ((flags & 0x20) ? VK_COLOR_COMPONENT_G_BIT : 0)
                                    | ((flags & 0x40) ? VK_COLOR_COMPONENT_B_BIT : 0)
                                    | ((flags & 0x80) ? VK_COLOR_COMPONENT_A_BIT : 0);
            VkPipeline p = pipeline(1, d != NULL, VK_TRUE, VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ZERO,
                                    VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, m, 0);
            if (p) {
                VkViewport vp = { 0, 0, (float)PX_W(s->width, s->height), (float)(s->height * (uint32_t)s_scale), 0, 1 };
                vkCmdBindPipeline(cb(), VK_PIPELINE_BIND_POINT_GRAPHICS, p);
                vkCmdSetViewport(cb(), 0, 1, &vp);
                vkCmdSetBlendConstants(cb(), c);
                vkCmdSetCullMode(cb(), VK_CULL_MODE_NONE); vkCmdSetFrontFace(cb(), VK_FRONT_FACE_CLOCKWISE);
                vkCmdSetDepthTestEnable(cb(), VK_FALSE); vkCmdSetDepthWriteEnable(cb(), VK_FALSE);
                vkCmdSetDepthCompareOp(cb(), VK_COMPARE_OP_ALWAYS); vkCmdSetStencilTestEnable(cb(), VK_FALSE);
                vkCmdSetStencilOp(cb(), VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                                  VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
                vkCmdSetStencilCompareMask(cb(), VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
                vkCmdSetStencilWriteMask(cb(), VK_STENCIL_FACE_FRONT_AND_BACK, 0);
                vkCmdSetStencilReference(cb(), VK_STENCIL_FACE_FRONT_AND_BACK, 0);
                vkCmdDraw(cb(), 3, 1, 0, 0);
                vp.width = (float)r->pw; vp.height = (float)r->ph;
                vkCmdSetViewport(cb(), 0, 1, &vp);
            }
        }
        note_present_candidate(r, s);
    }
    if (d && (flags & 0x3)) {
        memset(&ca[0], 0, sizeof ca[0]);
        if (flags & 0x1) ca[0].aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (flags & 0x2) ca[0].aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        if (rs->zeta_bits == 16) { ca[0].clearValue.depthStencil.depth = (float)(zstencil & 0xFFFF) / 65535.0f; }
        else {
            ca[0].clearValue.depthStencil.depth = (float)(zstencil >> 8) / 16777215.0f;
            ca[0].clearValue.depthStencil.stencil = zstencil & 0xFF;
        }
        cr.rect.offset.x = cr.rect.offset.y = 0;
        cr.rect.extent.width = r->pw < d->pw ? r->pw : d->pw;
        cr.rect.extent.height = r->ph < d->ph ? r->ph : d->ph;
        vkCmdClearAttachments(cb(), 1, &ca[0], 1, &cr);
    }
}

/* What a draw samples: views for t0..t3 (2D) and t4..t7 (cubes), samplers,
 * and which stages have a texture. Resolved before rendering starts, since
 * it may upload or put a barrier in. */
typedef struct { VkImageView views[8]; VkSampler smps[4]; int bound[4]; } PixIn;

static void pix_textures(const Nv2aBatch *b, Rt *r, PixIn *pi)
{
    uint32_t st;
    for (st = 0; st < 4; st++) {
        pi->views[st] = s_white2d.view; pi->views[st + 4] = s_whitecube.view;
        pi->smps[st] = s_smp_default; pi->bound[st] = 0;
    }
    if (b->combiner) {
        for (st = 0; st < 4; st++) {
            VkImageView v;
            if (!b->textures[st] || !(v = texture_view(b->textures[st], r))) continue;
            pi->smps[st] = sampler(b->textures[st]);
            pi->bound[st] = 1;
            if (b->textures[st]->cube) pi->views[st + 4] = v; else pi->views[st] = v;
        }
    } else if (b->texture) {
        VkImageView v = texture_view(b->texture, r);
        if (v) { pi->views[0] = v; pi->smps[0] = sampler(b->texture); pi->bound[0] = 1; }
    }
}

/* vs_prog's buffers (VP program, VC constants, VD per draw). Every set has
 * all four uniform buffers, so they always exist in this frame's memory:
 * uploaded again whenever the contents change, and once per frame. */
typedef struct { uint32_t info[4]; float param[4]; float fog[4]; } ProgDraw;
static uint32_t s_vp_host[272][4];
static float    s_vc_host[192][4];
static ProgDraw s_vd_host;
static uint32_t s_vp_rev = 0xFFFFFFFFu, s_vc_rev = 0xFFFFFFFFu;
static Alloc    s_vp_a, s_vc_a, s_vd_a;
static uint32_t s_vp_frame, s_vc_frame, s_vd_frame;

static int prog_bufs(int vp_dirty, int vc_dirty, int vd_dirty)
{
    if (vp_dirty || s_vp_frame != s_frame) {
        if (!frame_alloc(sizeof s_vp_host, s_ubo_align, &s_vp_a)) return 0;
        memcpy(s_vp_a.ptr, s_vp_host, sizeof s_vp_host); s_vp_frame = s_frame;
    }
    if (vc_dirty || s_vc_frame != s_frame) {
        if (!frame_alloc(sizeof s_vc_host, s_ubo_align, &s_vc_a)) return 0;
        memcpy(s_vc_a.ptr, s_vc_host, sizeof s_vc_host); s_vc_frame = s_frame;
    }
    if (vd_dirty || s_vd_frame != s_frame) {
        if (!frame_alloc(sizeof s_vd_host, s_ubo_align, &s_vd_a)) return 0;
        memcpy(s_vd_a.ptr, &s_vd_host, sizeof s_vd_host); s_vd_frame = s_frame;
    }
    return 1;
}

/* The descriptor set for this draw, reused while nothing it holds changes
 * (the four buffers are bound with dynamic offsets). */
static VkDescriptorSet desc_set(const PixIn *pi, VkBuffer ub)
{
    static VkDescriptorSet last;
    static VkImageView lviews[8];
    static VkSampler lsmps[4];
    static VkBuffer lbuf[4];
    static uint32_t lframe;
    VkBuffer bufs[4];
    VkDescriptorSetAllocateInfo ai;
    VkWriteDescriptorSet w[6];
    VkDescriptorBufferInfo bi[4];
    VkDescriptorImageInfo ii[8], si[4];
    uint32_t st;
    int k;
    static const VkDeviceSize ranges[4] = { sizeof(Consts), sizeof s_vp_host, sizeof s_vc_host, sizeof s_vd_host };

    bufs[0] = ub; bufs[1] = s_vp_a.buf; bufs[2] = s_vc_a.buf; bufs[3] = s_vd_a.buf;
    if (last && lframe == s_frame && !memcmp(lbuf, bufs, sizeof bufs)
            && !memcmp(lviews, pi->views, sizeof lviews) && !memcmp(lsmps, pi->smps, sizeof lsmps))
        return last;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = s_fr[s_fi].pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &s_dsl;
    if (vkAllocateDescriptorSets(s_dev, &ai, &last) != VK_SUCCESS) {
        static int warned;
        if (warned++ < 4) LOG("descriptor pool exhausted this frame\n");
        last = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    for (k = 0; k < 4; k++) { bi[k].buffer = bufs[k]; bi[k].offset = 0; bi[k].range = ranges[k]; }
    for (st = 0; st < 8; st++) {
        ii[st].sampler = VK_NULL_HANDLE; ii[st].imageView = pi->views[st];
        ii[st].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        for (k = 0; k < MAX_RT; k++)                   /* render targets are sampled in GENERAL */
            if (s_rts[k].im.view && s_rts[k].im.view == pi->views[st]) { ii[st].imageLayout = VK_IMAGE_LAYOUT_GENERAL; break; }
    }
    for (st = 0; st < 4; st++) { si[st].sampler = pi->smps[st]; si[st].imageView = VK_NULL_HANDLE; si[st].imageLayout = VK_IMAGE_LAYOUT_UNDEFINED; }
    memset(w, 0, sizeof w);
    for (k = 0; k < 6; k++) { w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[k].dstSet = last; }
    for (k = 0; k < 4; k++) {
        w[k].dstBinding = (uint32_t)k; w[k].descriptorCount = 1;
        w[k].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[k].pBufferInfo = &bi[k];
    }
    w[4].dstBinding = 16; w[4].descriptorCount = 8; w[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; w[4].pImageInfo = ii;
    w[5].dstBinding = 32; w[5].descriptorCount = 4; w[5].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER; w[5].pImageInfo = si;
    vkUpdateDescriptorSets(s_dev, 6, w, 0, NULL);
    memcpy(lviews, pi->views, sizeof lviews); memcpy(lsmps, pi->smps, sizeof lsmps);
    memcpy(lbuf, bufs, sizeof bufs); lframe = s_frame;
    return last;
}

/* Blend constant and depth/stencil state for a draw (rendering is active). */
static void draw_dynamic(const Nv2aRenderState *rs, Ds *d)
{
    float bf[4];
    unpack_argb(rs->blend_color, bf);
    if (rs->blend_src == 0x8003 || rs->blend_src == 0x8004 || rs->blend_dst == 0x8003 || rs->blend_dst == 0x8004)
        bf[0] = bf[1] = bf[2] = bf[3];
    vkCmdSetBlendConstants(cb(), bf);
    if (d) {
        vkCmdSetDepthTestEnable(cb(), rs->depth_test_enable ? VK_TRUE : VK_FALSE);
        vkCmdSetDepthWriteEnable(cb(), rs->depth_write ? VK_TRUE : VK_FALSE);
        vkCmdSetDepthCompareOp(cb(), map_cmp(rs->depth_func));
        vkCmdSetStencilTestEnable(cb(), rs->stencil_enable ? VK_TRUE : VK_FALSE);
        vkCmdSetStencilOp(cb(), VK_STENCIL_FACE_FRONT_AND_BACK, map_sop(rs->stencil_fail), map_sop(rs->stencil_zpass),
                          map_sop(rs->stencil_zfail), map_cmp(rs->stencil_func));
        vkCmdSetStencilCompareMask(cb(), VK_STENCIL_FACE_FRONT_AND_BACK, rs->stencil_func_mask & 0xFF);
        vkCmdSetStencilWriteMask(cb(), VK_STENCIL_FACE_FRONT_AND_BACK, rs->stencil_write_mask & 0xFF);
        vkCmdSetStencilReference(cb(), VK_STENCIL_FACE_FRONT_AND_BACK, rs->stencil_ref & 0xFF);
    } else {
        vkCmdSetDepthTestEnable(cb(), VK_FALSE); vkCmdSetDepthWriteEnable(cb(), VK_FALSE);
        vkCmdSetDepthCompareOp(cb(), VK_COMPARE_OP_ALWAYS); vkCmdSetStencilTestEnable(cb(), VK_FALSE);
        vkCmdSetStencilOp(cb(), VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                          VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
        vkCmdSetStencilCompareMask(cb(), VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
        vkCmdSetStencilWriteMask(cb(), VK_STENCIL_FACE_FRONT_AND_BACK, 0);
        vkCmdSetStencilReference(cb(), VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    }
}

static VkPipeline draw_pipeline(int kind, const Nv2aRenderState *rs, int has_ds, uint32_t vmask)
{
    return pipeline(kind, has_ds, rs->blend_enable ? VK_TRUE : VK_FALSE, map_blend(rs->blend_src, 0),
                    map_blend(rs->blend_dst, 0), map_blend(rs->blend_src, 1), map_blend(rs->blend_dst, 1),
                    map_blend_op(rs->blend_eq), map_mask(rs->color_mask), vmask);
}

static void bind_set(VkDescriptorSet set, const Alloc *ua)
{
    uint32_t dyn[4];
    dyn[0] = (uint32_t)ua->off; dyn[1] = (uint32_t)s_vp_a.off; dyn[2] = (uint32_t)s_vc_a.off; dyn[3] = (uint32_t)s_vd_a.off;
    vkCmdBindDescriptorSets(cb(), VK_PIPELINE_BIND_POINT_GRAPHICS, s_pl, 0, 1, &set, 4, dyn);
}

static void be_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    Rt *r;
    Ds *d;
    const Nv2aVertex *verts;
    uint32_t count;
    PixIn pi;
    Consts c;
    Alloc va, ua;
    VkPipeline p;
    VkDescriptorSet set;
    VkDeviceSize off;

    if (!ready() || !s->color_va || !s->width || !s->height || b->count < 3) return;
    if (!(r = rt_get(s))) return;
    d = ds_get(b->state->zeta_va, r->w, r->h);
    note_present_candidate(r, s);
    verts = cull_tris(b, &count);
    if (count < 3) return;
    pix_textures(b, r, &pi);
    wide_setup(r, b, verts, count);
    make_consts(&c, s, b, r, pi.bound[0] && !b->combiner, pi.bound);
    if (!frame_alloc(count * sizeof(Nv2aVertex), 16, &va) || !frame_alloc(sizeof c, s_ubo_align, &ua)
            || !prog_bufs(0, 0, 0))
        return;
    memcpy(va.ptr, verts, count * sizeof(Nv2aVertex));
    memcpy(ua.ptr, &c, sizeof c);
    if (!(p = spec_pipeline(0, b->state, d != NULL, 0, draw_pipeline(0, b->state, d != NULL, 0)))
            || !(set = desc_set(&pi, ua.buf)))
        return;
    begin_rendering(r, d);
    vkCmdBindPipeline(cb(), VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    bind_set(set, &ua);
    vkCmdSetCullMode(cb(), VK_CULL_MODE_NONE);          /* culled per triangle in cull_tris */
    vkCmdSetFrontFace(cb(), VK_FRONT_FACE_CLOCKWISE);
    draw_dynamic(b->state, d);
    off = va.off;
    vkCmdBindVertexBuffers(cb(), 0, 1, &va.buf, &off);
    vkCmdDraw(cb(), count, 1, 0, 0);
    s_st_draws++;
}

/* ── Vertex programs on the GPU (gpu_d3d11.c be_draw_program) ── */

static uint32_t prog_nact(uint32_t mask)
{
    uint32_t n = 0;
    for (mask &= 0xFFFFu; mask; mask &= mask - 1) n++;
    return n;
}

/* Vertex k of a packed draw as the 16 x 4 floats nv2a_vsh_run takes. */
static const float (*pv(const Nv2aProgramDraw *p, uint32_t k))[4]
{
    static float v[16][4];
    const float *src = p->packed + 64 + (size_t)k * prog_nact(p->packed_mask) * 4;
    int a;
    memcpy(v, p->packed, sizeof v);
    for (a = 0; a < 16; a++)
        if ((p->packed_mask >> a) & 1) { memcpy(v[a], src, 16); src += 4; }
    return (const float (*)[4])v;
}

static int be_draw_program(const Nv2aSurface *s, const Nv2aBatch *b, const Nv2aProgramDraw *p)
{
    static int on = -1, cull_on = -1;
    const Nv2aRenderState *rs = b->state;
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    VkFrontFace front = VK_FRONT_FACE_CLOCKWISE;
    Rt *r;
    Ds *d;
    PixIn pi;
    Consts c;
    Alloc va, ia, ua;
    VkPipeline pl;
    VkDescriptorSet set;
    VkBuffer vbs[2];
    VkDeviceSize offs[2];
    ProgDraw vd;
    uint32_t nact;
    size_t vbytes;

    if (on < 0) on = env_int("GZ_GPU_VSH", 1, 0, 1);
    if (!on || p->ninstr == 0 || p->ninstr > 136 || !p->packed) return 0;
    if (!ready() || !s_clip_dist) return 0;
    if (!s->color_va || !s->width || !s->height || p->nindices < 3) return 1;
    /* Face culling by the rasterizer, as the title asks (gpu_d3d11.c prog_rs).
     * Front is clockwise on the target for GL_CW (0x900). GZ_PROG_CULL=0
     * draws both faces. */
    if (cull_on < 0) cull_on = env_int("GZ_PROG_CULL", 1, 0, 1);
    if (cull_on && rs->cull_enable) {
        if (rs->cull_face == 0x408) return 1;          /* FRONT_AND_BACK: nothing to draw */
        if (rs->cull_face == 0x404) cull = VK_CULL_MODE_FRONT_BIT;
        else if (rs->cull_face == 0x405) cull = VK_CULL_MODE_BACK_BIT;
        front = rs->front_face != 0x900 ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    }
    nact = prog_nact(p->packed_mask);
    if (!(r = rt_get(s))) return 1;
    d = ds_get(rs->zeta_va, r->w, r->h);
    note_present_candidate(r, s);
    pix_textures(b, r, &pi);

    s_draw_squeeze = 0.0f;
    if (WIDE_SURF(r->w, r->h)) {
        /* As be_draw; the positions come from running the program on the CPU
         * for the few vertices of a quad. */
        s_draw_squeeze = 0.75f;
        s_draw_anchor = (float)r->w * 0.5f;
        s_draw_yscale = 1.0f;
        if (p->nverts <= 64) {
            float mn = 1e9f, mx = -1e9f, ymn = 1e9f, ymx = -1e9f, c0[4] = { 1, 1, 1, 1 };
            uint32_t k;
            int flat = 1;
            for (k = 0; k < p->nverts && flat; k++) {
                Nv2aVshOutput o;
                if (!nv2a_vsh_run(pv(p, k), &o)) { flat = 0; break; }
                if (k == 0) memcpy(c0, o.d0, sizeof c0);
                if (o.pos[0] < mn) mn = o.pos[0];
                if (o.pos[0] > mx) mx = o.pos[0];
                if (o.pos[1] < ymn) ymn = o.pos[1];
                if (o.pos[1] > ymx) ymx = o.pos[1];
                if (o.pos[3] < 0.99999f || o.pos[3] > 1.00001f) flat = 0;
            }
            if (flat && p->nverts <= 6 && wide_untextured(b) && mn <= 2.0f && mx >= (float)r->w - 2.0f) {
                s_draw_squeeze = 0.0f;
                if (ymn <= 2.0f && ymx >= (float)r->h - 2.0f && c0[0] < 0.38f && c0[1] < 0.38f && c0[2] < 0.38f)
                    s_overlay_frame = s_frame;
            } else if (flat) {
                s_draw_anchor = wide_anchor(mn, mx, ymx, (float)r->w, (float)r->h);
                wide_hud_scale(s_draw_anchor, (float)r->w, ymx, (float)r->h);
            }
        }
    }
    make_consts(&c, s, b, r, pi.bound[0] && !b->combiner, pi.bound);

    memset(&vd, 0, sizeof vd);
    vd.info[0] = p->ninstr; vd.info[1] = p->near_z; vd.info[2] = p->fog_mode; vd.info[3] = p->fog_enable;
    vd.param[0] = p->aa_sx; vd.param[1] = p->aa_sy; vd.param[2] = p->clip_w;
    vd.param[3] = (b->texture && b->texture->normalized) ? 1.0f : 0.0f;
    vd.fog[0] = p->fog_param[0]; vd.fog[1] = p->fog_param[1];
    {
        int vp_dirty = p->program_rev != s_vp_rev, vc_dirty = p->constants_rev != s_vc_rev;
        int vd_dirty = memcmp(&vd, &s_vd_host, sizeof vd) != 0;
        if (vp_dirty) { memset(s_vp_host, 0, sizeof s_vp_host); memcpy(s_vp_host, p->program, p->ninstr * 32u); s_vp_rev = p->program_rev; }
        if (vc_dirty) { memcpy(s_vc_host, p->constants, sizeof s_vc_host); s_vc_rev = p->constants_rev; }
        if (vd_dirty) s_vd_host = vd;
        if (!prog_bufs(vp_dirty, vc_dirty, vd_dirty)) return 1;
    }
    vbytes = 256u + (size_t)p->nverts * nact * 16u;
    if (!frame_alloc(vbytes, 16, &va) || !frame_alloc((VkDeviceSize)p->nindices * 4u, 4, &ia)
            || !frame_alloc(sizeof c, s_ubo_align, &ua))
        return 1;
    memcpy(va.ptr, p->packed, vbytes);
    memcpy(ia.ptr, p->indices, (size_t)p->nindices * 4u);
    memcpy(ua.ptr, &c, sizeof c);
    if (!(pl = spec_pipeline(2, rs, d != NULL, p->packed_mask, draw_pipeline(2, rs, d != NULL, p->packed_mask)))
            || !(set = desc_set(&pi, ua.buf)))
        return 1;

    begin_rendering(r, d);
    vkCmdBindPipeline(cb(), VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    bind_set(set, &ua);
    vkCmdSetCullMode(cb(), cull);
    vkCmdSetFrontFace(cb(), front);
    draw_dynamic(rs, d);
    vbs[0] = vbs[1] = va.buf;
    offs[0] = va.off + 256u; offs[1] = va.off;
    vkCmdBindVertexBuffers(cb(), 0, 2, vbs, offs);
    vkCmdBindIndexBuffer(cb(), ia.buf, ia.off, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cb(), p->nindices, 1, 0, 0, 0);
    s_st_draws++;
    return 1;
}

/* Drop every target (the aspect changed: frame-size targets have the wrong
 * width now). They come back at the next draw. */
static void targets_reset(void)
{
    int k;
    end_rendering();
    for (k = 0; k < MAX_RT; k++) if (s_rts[k].im.img) rt_free(&s_rts[k]);
    for (k = 0; k < MAX_DS; k++) { junk_img(&s_dss[k].im); memset(&s_dss[k], 0, sizeof s_dss[k]); }
    s_present_rt = NULL;
}

static void save_pipeline_cache(void)
{
    size_t n = 0;
    void *data;
    FILE *f;
    if (!s_pcache || vkGetPipelineCacheData(s_dev, s_pcache, &n, NULL) != VK_SUCCESS || !n) return;
    if (!(data = malloc(n))) return;
    if (vkGetPipelineCacheData(s_dev, s_pcache, &n, data) == VK_SUCCESS) {
#ifdef _WIN32
        CreateDirectoryA("shader_cache", NULL);
#else
        mkdir("shader_cache", 0755);
#endif
        if ((f = fopen("shader_cache/vk_pipelines.bin", "wb")) != NULL) { fwrite(data, 1, n, f); fclose(f); }
    }
    free(data);
}

static void be_flip(void)
{
    Frame *f;
    uint32_t img = 0;
    VkResult ar = VK_ERROR_OUT_OF_DATE_KHR;
    VkSubmitInfo si;
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    int presenting;
    static char shot_path[600];
    static uint32_t shot_w, shot_h;
    static Alloc shot;

    if (!s_ready) return;
    window_pump();
    {
        /* fps in the window title, twice a second */
        static uint32_t t0, n;
        uint32_t now = now_ms();
        n++;
        if (!t0) t0 = now;
        else if (now - t0 >= 500) {
            char title[160];
            snprintf(title, sizeof title, "Godzilla: Save the Earth (recomp, Vulkan) - %.0f fps", n * 1000.0 / (double)(now - t0));
            window_title(title);
            t0 = now; n = 0;
        }
    }
    end_rendering();
    f = &s_fr[s_fi];
    if (s_swap_stale || !s_swap) {
        vkDeviceWaitIdle(s_dev);
        swap_create();
    }
    presenting = s_swap && s_present_rt && s_present_rt->im.img;
    if (presenting) {
        ar = vkAcquireNextImageKHR(s_dev, s_swap, UINT64_MAX, f->acquire, VK_NULL_HANDLE, &img);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR) { s_swap_stale = 1; presenting = 0; }
    }
    if (presenting && (ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR)) {
        VkClearColorValue black = { { 0.0f, 0.0f, 0.0f, 1.0f } };
        VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageBlit bl;
        Rt *r = s_present_rt;
        float aspect = WIDE_SURF(r->w, r->h) ? 16.0f / 9.0f : 4.0f / 3.0f;
        float w = (float)s_swap_ext.width, h = (float)s_swap_ext.height, fw = w, fh = w / aspect;
        if (fh > h) { fh = h; fw = h * aspect; }
        barrier_all();
        img_barrier(s_swap_img[img], VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(cb(), s_swap_img[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
        img_barrier(s_swap_img[img], VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        memset(&bl, 0, sizeof bl);
        bl.srcSubresource.aspectMask = bl.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bl.srcSubresource.layerCount = bl.dstSubresource.layerCount = 1;
        /* the guest surface's own size within the (possibly grown) target */
        bl.srcOffsets[1].x = (int32_t)PX_W(r->w, r->h); bl.srcOffsets[1].y = (int32_t)(r->h * (uint32_t)s_scale);
        if (bl.srcOffsets[1].x > (int32_t)r->pw) bl.srcOffsets[1].x = (int32_t)r->pw;
        if (bl.srcOffsets[1].y > (int32_t)r->ph) bl.srcOffsets[1].y = (int32_t)r->ph;
        bl.srcOffsets[1].z = 1;
        bl.dstOffsets[0].x = (int32_t)((w - fw) * 0.5f); bl.dstOffsets[0].y = (int32_t)((h - fh) * 0.5f);
        bl.dstOffsets[1].x = bl.dstOffsets[0].x + (int32_t)fw; bl.dstOffsets[1].y = bl.dstOffsets[0].y + (int32_t)fh;
        bl.dstOffsets[1].z = 1;
        vkCmdBlitImage(cb(), r->im.img, VK_IMAGE_LAYOUT_GENERAL, s_swap_img[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &bl, VK_FILTER_LINEAR);
        img_barrier(s_swap_img[img], VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        r->drawn = 0;
    } else {
        presenting = 0;
    }
    {
        /* Screenshots (gpu_d3d11.c): GZ_SHOT_FILE=<path> writes <path>.bmp
         * whenever the file <path> exists, then deletes it; F12 on Windows
         * writes screenshots/shot_<n>.bmp. The frame is copied out of the
         * present target here and written once this frame's work is done. */
        static const char *req = (const char *)-1;
        FILE *probe;
        if (req == (const char *)-1) req = getenv("GZ_SHOT_FILE");
        shot_path[0] = 0;
        if (req && (s_frame & 7) == 0 && (probe = fopen(req, "rb")) != NULL) {
            fclose(probe);
            remove(req);
            snprintf(shot_path, sizeof shot_path, "%s.bmp", req);
        }
#ifdef _WIN32
        {
            static int f12_was, n;
            int f12 = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
            if (f12 && !f12_was && GetForegroundWindow() == s_hwnd) {
                CreateDirectoryA("screenshots", NULL);
                do snprintf(shot_path, sizeof shot_path, "screenshots\\shot_%03d.bmp", n++);
                while (GetFileAttributesA(shot_path) != INVALID_FILE_ATTRIBUTES && n < 1000);
            }
            f12_was = f12;
        }
#endif
        if (shot_path[0] && s_present_rt && s_present_rt->im.img) {
            Rt *r = s_present_rt;
            VkBufferImageCopy c;
            shot_w = PX_W(r->w, r->h); shot_h = r->h * (uint32_t)s_scale;
            if (shot_w > r->pw) shot_w = r->pw;
            if (shot_h > r->ph) shot_h = r->ph;
            if (frame_alloc((VkDeviceSize)shot_w * shot_h * 4, 16, &shot)) {
                barrier_all();
                memset(&c, 0, sizeof c);
                c.bufferOffset = shot.off;
                c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount = 1;
                c.imageExtent.width = shot_w; c.imageExtent.height = shot_h; c.imageExtent.depth = 1;
                vkCmdCopyImageToBuffer(cb(), r->im.img, VK_IMAGE_LAYOUT_GENERAL, shot.buf, 1, &c);
            } else shot_path[0] = 0;
        } else shot_path[0] = 0;
    }
    vkEndCommandBuffer(f->cb);
    s_recording = 0;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &f->cb;
    if (presenting) {
        si.waitSemaphoreCount = 1; si.pWaitSemaphores = &f->acquire; si.pWaitDstStageMask = &wait;
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &s_swap_done[img];
    }
    if (vkQueueSubmit(s_queue, 1, &si, f->fence) != VK_SUCCESS) {
        LOG("queue submit failed (device lost?)\n");
        s_failed = 1; s_ready = 0;
        return;
    }
    if (presenting) {
        VkPresentInfoKHR pi;
        VkResult pr;
        memset(&pi, 0, sizeof pi);
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &s_swap_done[img];
        pi.swapchainCount = 1; pi.pSwapchains = &s_swap; pi.pImageIndices = &img;
        pr = vkQueuePresentKHR(s_queue, &pi);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) s_swap_stale = 1;
    }
    if (shot_path[0]) {
        /* 32-bit BGRA BMP, top-down; the copy is done when the fence is. */
        uint32_t hdr[13];
        FILE *fo;
        vkWaitForFences(s_dev, 1, &f->fence, VK_TRUE, UINT64_MAX);
        if ((fo = fopen(shot_path, "wb")) != NULL) {
            memset(hdr, 0, sizeof hdr);
            hdr[0] = 54u + shot_w * shot_h * 4u; hdr[2] = 54; hdr[3] = 40;
            hdr[4] = shot_w; hdr[5] = (uint32_t)-(int32_t)shot_h; hdr[6] = 1u | (32u << 16);
            fwrite("BM", 1, 2, fo);
            fwrite(hdr, 4, 13, fo);
            fwrite(shot.ptr, 4, (size_t)shot_w * shot_h, fo);
            fclose(fo);
            LOG("screenshot %s (%ux%u)\n", shot_path, shot_w, shot_h);
        }
    }
    s_fi = (s_fi + 1) % FRAMES;
    frame_begin();
    s_frame++;
    s_frame_tick = now_ms() / 16u;      /* re-hash textures about 60 times a second (gpu_d3d11.c) */
    {
        int wide = gz_opt_wide();
        if (wide != s_wide) {
            s_wide = wide;
            targets_reset();
            LOG("aspect %s\n", wide ? "16:9" : "4:3");
        }
    }
    if (s_frame == 600 || s_frame == 3600 || (s_frame % 18000) == 0) save_pipeline_cache();
}

static const Nv2aBackend s_backend = { be_clear, be_draw, be_flip, be_draw_program };

/* Can this PC run the back end? Loads Vulkan and looks for a 1.3 device,
 * without a window. main.c asks before choosing who owns the window. */
int gz_gpu_vk_available(void)
{
    static int known = -1;
    VkApplicationInfo app;
    VkInstanceCreateInfo ci;
    VkInstance inst = VK_NULL_HANDLE;
    PFN_vkDestroyInstance destroy;
    PFN_vkEnumeratePhysicalDevices enumerate;
    VkPhysicalDevice pds[16];
    uint32_t n = 16, i, q;
    if (known >= 0) return known;
    known = 0;
    if (!vk_load_library()) return 0;
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO; app.apiVersion = VK_API_VERSION_1_3;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ci.pApplicationInfo = &app;
    if (vkCreateInstance(&ci, NULL, &inst) != VK_SUCCESS) return 0;
    destroy = (PFN_vkDestroyInstance)vkGetInstanceProcAddr(inst, "vkDestroyInstance");
    enumerate = (PFN_vkEnumeratePhysicalDevices)vkGetInstanceProcAddr(inst, "vkEnumeratePhysicalDevices");
    vkGetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceProperties");
    vkGetPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2)vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceFeatures2");
    vkGetPhysicalDeviceQueueFamilyProperties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
        vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (enumerate && enumerate(inst, &n, pds) >= 0)
        for (i = 0; i < n && !known; i++) known = device_usable(pds[i], &q, 0);
    if (destroy) destroy(inst, NULL);
    return known;
}

int gz_gpu_vk_register(void)
{
    if (!getenv("RECOMP_PB_EXEC")) {
        LOG("RECOMP_PB_EXEC is not set, so nothing would call the back end; not registering\n");
        return 0;
    }
    /* Program draws are culled by the rasterizer (be_draw_program), which
     * needs strips wound alike: set before the executor first reads it; an
     * explicit RECOMP_STRIP_ALT_WINDING stands (gpu_d3d11.c does the same). */
    if (env_int("GZ_PROG_CULL", 1, 0, 1) && env_int("GZ_GPU_VSH", 1, 0, 1) && !getenv("RECOMP_STRIP_ALT_WINDING")) {
#ifdef _WIN32
        _putenv("RECOMP_STRIP_ALT_WINDING=1");
#else
        setenv("RECOMP_STRIP_ALT_WINDING", "1", 1);
#endif
    }
    nv2a_backend_register(&s_backend);
    LOG("back end registered (device is created on the first draw)\n");
    return 1;
}

#else
/* Built without the Vulkan headers. */
int gz_gpu_vk_available(void) { return 0; }
int gz_gpu_vk_register(void) { return 0; }
#endif /* GZ_HAVE_VULKAN */
