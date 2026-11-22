/* SPDX-License-Identifier: ISC
 * Copyright 2026 nx-hbmenu Switch 2 port contributors
 *
 * Adapter for upstream nx_graphics.h: draws render.c's quad list into
 * OunceVK's native VI2 FIFO swapchain at 1920x1080.
 *
 * One frame is in flight. graphicsFrameBegin waits for the previous frame,
 * so the vertex buffer and upload arena are reused, and released textures
 * destroyed, without tracking more than one submission.
 */
#define VK_USE_PLATFORM_VI_NN 1
#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "graphics_vulkan.h"
#include "perf.h"
#include "render.h"
#include "render_backend.h"
#include "../nx_main/nx_graphics.h"

/* SPIR-V generated from switch2/shaders by switch2/Makefile. */
#include "hbmenu_quad_vert.h"
#include "hbmenu_quad_frag.h"

enum {
    MAX_SWAPCHAIN_IMAGES = 16,
    MAX_TEXTURES = 1024,
    UPLOAD_ARENA_BYTES = 16u << 20,
    UPLOAD_ALIGNMENT = 16,
    INITIAL_QUAD_CAPACITY = 16384,
    STAGE_INSTANCE = 1001,
    STAGE_PHYSICAL = 1002,
    STAGE_DEVICE = 1003,
    STAGE_SURFACE = 1004,
    STAGE_SWAPCHAIN = 1005,
    STAGE_BUFFERS = 1006,
    STAGE_COMMANDS = 1007,
    STAGE_PRESENT = 1008,
    STAGE_PIPELINE = 1009,
};

/* upstream main.c clears the framebuffer to bytes of 237. */
static const float kClearGrey = 237.0f / 255.0f;

struct HostBuffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *mapped;
    VkDeviceSize size;
};

struct Texture {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet set;
    uint32_t texel_bytes;
    bool used;
    bool initialized;      /* in SHADER_READ_ONLY_OPTIMAL layout */
    bool uploading;        /* in this frame's upload list */
    bool released;
    uint64_t release_frame;
};

struct Upload {
    RenderTexture texture;
    VkBuffer buffer;
    VkDeviceSize offset;
    uint32_t x, y, width, height;
};

/* Consecutive quads that sample the same texture. */
struct DrawGroup {
    RenderTexture texture;
    uint32_t first_quad;
    uint32_t quad_count;
};

struct Vertex {
    float x, y, u, v;
    uint32_t color;
};

struct Graphics {
    const struct HbmenuGraphicsConfig *config;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkPhysicalDeviceMemoryProperties memory;
    uint32_t queue_family;
    VkDevice device;
    VkQueue queue;

    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    bool recreate;          /* the window's size changed: recreate at the next frame */
    uint32_t scaled_height; /* the swapchain height the layout scale is for */
    uint32_t image_count;
    VkImage images[MAX_SWAPCHAIN_IMAGES];
    VkImageView views[MAX_SWAPCHAIN_IMAGES];
    VkFramebuffer framebuffers[MAX_SWAPCHAIN_IMAGES];
    VkRenderPass render_pass;

    VkDescriptorSetLayout set_layout;
    VkDescriptorPool descriptor_pool;
    VkSampler sampler;         /* nearest */
    VkSampler linear_sampler;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;

    VkCommandPool command_pool;
    VkCommandBuffer commands[MAX_SWAPCHAIN_IMAGES];
    VkSemaphore acquired;
    VkSemaphore ready[MAX_SWAPCHAIN_IMAGES];
    VkFence frame_fence;

    struct HostBuffer vertices;
    struct HostBuffer indices;
    uint32_t quad_capacity;
    struct DrawGroup *groups;
    uint32_t group_count, group_capacity;
    uint32_t drawn_quads;

    struct HostBuffer arena;
    VkDeviceSize arena_used;
    struct HostBuffer *overflow;  /* uploads too large for the arena */
    uint32_t overflow_count, overflow_capacity;
    struct Upload *uploads;
    uint32_t upload_count, upload_capacity;

    Mutex texture_lock;
    struct Texture textures[MAX_TEXTURES];

    /* Frames submitted so far; the frame being built has this index. */
    uint64_t frame_index;
};

static struct Graphics graphics;
static struct HbmenuGraphicsConfig bound_config;
static int graphics_status = STAGE_INSTANCE;

static bool growArray(void **items, uint32_t *capacity, uint32_t needed, size_t item_size) {
    if (needed <= *capacity)
        return true;
    uint32_t grown = *capacity ? *capacity * 2 : 64;
    while (grown < needed)
        grown *= 2;
    void *resized = realloc(*items, grown * item_size);
    if (!resized)
        return false;
    *items = resized;
    *capacity = grown;
    return true;
}

/* --- Memory --------------------------------------------------------------- */

static bool findMemoryType(uint32_t allowed, VkMemoryPropertyFlags wanted, uint32_t *out) {
    for (uint32_t i = 0; i < graphics.memory.memoryTypeCount; i++) {
        if ((allowed & (1u << i)) &&
            (graphics.memory.memoryTypes[i].propertyFlags & wanted) == wanted) {
            *out = i;
            return true;
        }
    }
    return false;
}

static VkResult allocateMemory(VkMemoryRequirements requirements, VkMemoryPropertyFlags wanted,
                               VkMemoryPropertyFlags fallback, VkDeviceMemory *out) {
    uint32_t type;
    if (!findMemoryType(requirements.memoryTypeBits, wanted, &type) &&
        !findMemoryType(requirements.memoryTypeBits, fallback, &type))
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    const VkMemoryAllocateInfo info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    return vkAllocateMemory(graphics.device, &info, NULL, out);
}

static void destroyHostBuffer(struct HostBuffer *buffer) {
    if (buffer->mapped)
        vkUnmapMemory(graphics.device, buffer->memory);
    if (buffer->buffer)
        vkDestroyBuffer(graphics.device, buffer->buffer, NULL);
    if (buffer->memory)
        vkFreeMemory(graphics.device, buffer->memory, NULL);
    memset(buffer, 0, sizeof(*buffer));
}

static VkResult createHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                 struct HostBuffer *out) {
    memset(out, 0, sizeof(*out));
    const VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult result = vkCreateBuffer(graphics.device, &info, NULL, &out->buffer);
    if (result != VK_SUCCESS)
        return result;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(graphics.device, out->buffer, &requirements);
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    result = allocateMemory(requirements, host, host, &out->memory);
    if (result == VK_SUCCESS)
        result = vkBindBufferMemory(graphics.device, out->buffer, out->memory, 0);
    if (result == VK_SUCCESS)
        result = vkMapMemory(graphics.device, out->memory, 0, size, 0, &out->mapped);
    if (result != VK_SUCCESS) {
        destroyHostBuffer(out);
        return result;
    }
    out->size = size;
    return VK_SUCCESS;
}

/* Vertex and index buffers for `capacity` quads. Called only while no
 * submitted frame uses them. */
static VkResult createQuadBuffers(uint32_t capacity) {
    destroyHostBuffer(&graphics.vertices);
    destroyHostBuffer(&graphics.indices);
    graphics.quad_capacity = 0;
    VkResult result = createHostBuffer((VkDeviceSize)capacity * 4 * sizeof(struct Vertex),
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &graphics.vertices);
    if (result != VK_SUCCESS)
        return result;
    result = createHostBuffer((VkDeviceSize)capacity * 6 * sizeof(uint32_t),
                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &graphics.indices);
    if (result != VK_SUCCESS)
        return result;
    uint32_t *indices = graphics.indices.mapped;
    for (uint32_t quad = 0; quad < capacity; quad++) {
        const uint32_t vertex = quad * 4;
        indices[0] = vertex;
        indices[1] = vertex + 1;
        indices[2] = vertex + 2;
        indices[3] = vertex + 2;
        indices[4] = vertex + 3;
        indices[5] = vertex;
        indices += 6;
    }
    graphics.quad_capacity = capacity;
    return VK_SUCCESS;
}

/* --- Textures ------------------------------------------------------------- */

static void destroyTexture(struct Texture *texture) {
    if (texture->set)
        vkFreeDescriptorSets(graphics.device, graphics.descriptor_pool, 1, &texture->set);
    if (texture->view)
        vkDestroyImageView(graphics.device, texture->view, NULL);
    if (texture->image)
        vkDestroyImage(graphics.device, texture->image, NULL);
    if (texture->memory)
        vkFreeMemory(graphics.device, texture->memory, NULL);
    memset(texture, 0, sizeof(*texture));
}

static VkResult initTexture(struct Texture *texture, uint32_t width, uint32_t height,
                            enum RenderTextureFormat format, enum RenderTextureFilter filter) {
    const bool alpha = format == RenderTextureFormat_Alpha8;
    const VkFormat vk_format = alpha ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    texture->texel_bytes = alpha ? 1 : 4;

    const VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = vk_format,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult result = vkCreateImage(graphics.device, &image_info, NULL, &texture->image);
    if (result != VK_SUCCESS)
        return result;
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(graphics.device, texture->image, &requirements);
    result = allocateMemory(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
                            &texture->memory);
    if (result == VK_SUCCESS)
        result = vkBindImageMemory(graphics.device, texture->image, texture->memory, 0);
    if (result != VK_SUCCESS)
        return result;

    const VkComponentMapping identity = {
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
    };
    const VkComponentMapping coverage = {
        VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE,
        VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R,
    };
    const VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = vk_format,
        .components = alpha ? coverage : identity,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    result = vkCreateImageView(graphics.device, &view_info, NULL, &texture->view);
    if (result != VK_SUCCESS)
        return result;

    const VkDescriptorSetAllocateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = graphics.descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &graphics.set_layout,
    };
    result = vkAllocateDescriptorSets(graphics.device, &set_info, &texture->set);
    if (result != VK_SUCCESS)
        return result;
    const VkDescriptorImageInfo descriptor = {
        .sampler = filter == RenderTextureFilter_Linear ? graphics.linear_sampler
                                                        : graphics.sampler,
        .imageView = texture->view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    const VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = texture->set,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &descriptor,
    };
    vkUpdateDescriptorSets(graphics.device, 1, &write, 0, NULL);
    return VK_SUCCESS;
}

/* The live texture for a handle, or NULL. */
static struct Texture *textureFor(RenderTexture id) {
    if (id < 0 || id >= MAX_TEXTURES || !graphics.textures[id].used)
        return NULL;
    return &graphics.textures[id];
}

RenderTexture renderBackendCreateTexture(uint32_t width, uint32_t height,
                                         enum RenderTextureFormat format,
                                         enum RenderTextureFilter filter) {
    if (graphics_status || !graphics.descriptor_pool)
        return RenderTexture_None;
    mutexLock(&graphics.texture_lock);
    RenderTexture id = RenderTexture_None;
    for (RenderTexture i = 0; i < MAX_TEXTURES; i++) {
        if (!graphics.textures[i].used) {
            id = i;
            break;
        }
    }
    if (id != RenderTexture_None) {
        struct Texture *texture = &graphics.textures[id];
        const VkResult result = initTexture(texture, width, height, format, filter);
        if (result == VK_SUCCESS) {
            texture->used = true;
        } else {
            destroyTexture(texture);
            id = RenderTexture_None;
        }
    }
    mutexUnlock(&graphics.texture_lock);
    return id;
}

void renderBackendReleaseTexture(RenderTexture id) {
    mutexLock(&graphics.texture_lock);
    struct Texture *texture = textureFor(id);
    if (texture && !texture->released) {
        texture->released = true;
        texture->release_frame = graphics.frame_index;
    }
    mutexUnlock(&graphics.texture_lock);
}

/* Destroys textures released before the frames that have completed. */
static void destroyReleasedTextures(bool all) {
    mutexLock(&graphics.texture_lock);
    for (int i = 0; i < MAX_TEXTURES; i++) {
        struct Texture *texture = &graphics.textures[i];
        if (texture->used &&
            (all || (texture->released && texture->release_frame < graphics.frame_index)))
            destroyTexture(texture);
    }
    mutexUnlock(&graphics.texture_lock);
}

/* --- Uploads -------------------------------------------------------------- */

static void *reserveUpload(VkDeviceSize bytes, VkBuffer *buffer, VkDeviceSize *offset) {
    const VkDeviceSize start = (graphics.arena_used + UPLOAD_ALIGNMENT - 1) &
                               ~(VkDeviceSize)(UPLOAD_ALIGNMENT - 1);
    if (start + bytes <= graphics.arena.size) {
        graphics.arena_used = start + bytes;
        *buffer = graphics.arena.buffer;
        *offset = start;
        return (uint8_t *)graphics.arena.mapped + start;
    }
    if (!growArray((void **)&graphics.overflow, &graphics.overflow_capacity,
                   graphics.overflow_count + 1, sizeof(*graphics.overflow)))
        return NULL;
    struct HostBuffer *dedicated = &graphics.overflow[graphics.overflow_count];
    if (createHostBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, dedicated) != VK_SUCCESS)
        return NULL;
    graphics.overflow_count++;
    *buffer = dedicated->buffer;
    *offset = 0;
    return dedicated->mapped;
}

void *renderBackendUpload(RenderTexture id, uint32_t x, uint32_t y,
                          uint32_t width, uint32_t height) {
    const struct Texture *texture = textureFor(id);
    if (graphics_status || !texture || !width || !height)
        return NULL;
    if (!growArray((void **)&graphics.uploads, &graphics.upload_capacity,
                   graphics.upload_count + 1, sizeof(*graphics.uploads)))
        return NULL;
    struct Upload *upload = &graphics.uploads[graphics.upload_count];
    const VkDeviceSize bytes = (VkDeviceSize)width * height * texture->texel_bytes;
    void *memory = reserveUpload(bytes, &upload->buffer, &upload->offset);
    if (!memory)
        return NULL;
    upload->texture = id;
    upload->x = x;
    upload->y = y;
    upload->width = width;
    upload->height = height;
    graphics.upload_count++;
    return memory;
}

static void textureBarrier(VkCommandBuffer command, VkImage image,
                           VkImageLayout from, VkImageLayout to,
                           VkAccessFlags src_access, VkAccessFlags dst_access,
                           VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    const VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(command, src_stage, dst_stage, 0, 0, NULL, 0, NULL,
                                   1, &barrier);
}

/* Copies this frame's uploads into their textures before the render pass. */
static void recordUploads(VkCommandBuffer command) {
    for (uint32_t i = 0; i < graphics.upload_count; i++) {
        const struct Upload *upload = &graphics.uploads[i];
        struct Texture *texture = &graphics.textures[upload->texture];
        if (!texture->used)
            continue;
        if (!texture->uploading) {
            textureBarrier(command, texture->image,
                           texture->initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                : VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT);
            texture->uploading = true;
        }
        const VkBufferImageCopy region = {
            .bufferOffset = upload->offset,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { (int32_t)upload->x, (int32_t)upload->y, 0 },
            .imageExtent = { upload->width, upload->height, 1 },
        };
        vkCmdCopyBufferToImage(command, upload->buffer, texture->image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    for (uint32_t i = 0; i < graphics.upload_count; i++) {
        struct Texture *texture = &graphics.textures[graphics.uploads[i].texture];
        if (!texture->uploading)
            continue;
        textureBarrier(command, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        texture->uploading = false;
        texture->initialized = true;
    }
}

/* Called after the previous frame completed. */
static void resetUploads(void) {
    for (uint32_t i = 0; i < graphics.overflow_count; i++)
        destroyHostBuffer(&graphics.overflow[i]);
    graphics.overflow_count = 0;
    graphics.upload_count = 0;
    graphics.arena_used = 0;
}

/* --- Setup ---------------------------------------------------------------- */

static int createInstance(void) {
    const char *extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME, VK_NN_VI_SURFACE_EXTENSION_NAME,
    };
    const VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "nx-hbmenu",
        .apiVersion = VK_API_VERSION_1_0,
    };
    const VkInstanceCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = extensions,
    };
    const VkResult result = vkCreateInstance(&info, NULL, &graphics.instance);
    if (result != VK_SUCCESS)
        return STAGE_INSTANCE;
    return 0;
}

static int createDevice(void) {
    uint32_t count = 1;
    VkResult result = vkEnumeratePhysicalDevices(graphics.instance, &count, &graphics.physical);
    if (result != VK_SUCCESS || count != 1 || !graphics.physical)
        return STAGE_PHYSICAL;
    vkGetPhysicalDeviceMemoryProperties(graphics.physical, &graphics.memory);
    VkQueueFamilyProperties families[8];
    count = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(graphics.physical, &count, families);
    graphics.queue_family = count;
    for (uint32_t i = 0; i < count && i < 8; i++) {
        if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            graphics.queue_family = i;
            break;
        }
    }
    if (graphics.queue_family == count)
        return STAGE_PHYSICAL;

    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = graphics.queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const char *extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    const VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = extensions,
    };
    result = vkCreateDevice(graphics.physical, &device_info, NULL, &graphics.device);
    if (result != VK_SUCCESS || !graphics.device)
        return STAGE_DEVICE;
    vkGetDeviceQueue(graphics.device, graphics.queue_family, 0, &graphics.queue);
    return graphics.queue ? 0 : STAGE_DEVICE;
}

static int createSurface(void) {
    const VkViSurfaceCreateInfoNN surface_info = {
        .sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN,
        .window = graphics.config->window,
    };
    VkResult result = vkCreateViSurfaceNN(graphics.instance, &surface_info, NULL,
                                            &graphics.surface);
    if (result != VK_SUCCESS || !graphics.surface)
        return STAGE_SURFACE;

    /* Upstream blends in stored byte values, so render without sRGB
     * conversion. */
    VkSurfaceFormatKHR formats[16];
    uint32_t count = 16;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(graphics.physical, graphics.surface,
                                                    &count, formats);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE)
        return STAGE_SURFACE;
    graphics.format = VK_FORMAT_UNDEFINED;
    for (uint32_t i = 0; i < count; i++) {
        if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            graphics.format = formats[i].format;
    }
    return graphics.format == VK_FORMAT_UNDEFINED ? STAGE_SURFACE : 0;
}

/* A swapchain at the window's current size, replacing `old` (destroyed
 * here) if given. The window follows the console's default resolution:
 * 1920x1080 handheld, the TV output setting docked (ouncevk_platform.c). */
static VkResult createSwapchain(VkSwapchainKHR old) {
    VkSurfaceCapabilitiesKHR capabilities = {0};
    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        graphics.physical, graphics.surface, &capabilities);
    if (result != VK_SUCCESS)
        return result;
    if (capabilities.minImageCount == 0 || capabilities.minImageCount > MAX_SWAPCHAIN_IMAGES ||
        !capabilities.currentExtent.width || capabilities.currentExtent.width == UINT32_MAX ||
        !(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
        return VK_ERROR_INITIALIZATION_FAILED;

    const VkSwapchainCreateInfoKHR swapchain_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = graphics.surface,
        .minImageCount = capabilities.minImageCount,
        .imageFormat = graphics.format,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = capabilities.currentExtent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
        .oldSwapchain = old,
    };
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    result = vkCreateSwapchainKHR(graphics.device, &swapchain_info, NULL, &swapchain);
    if (old)
        vkDestroySwapchainKHR(graphics.device, old, NULL);
    graphics.swapchain = swapchain;
    if (result != VK_SUCCESS)
        return result;
    uint32_t count = MAX_SWAPCHAIN_IMAGES;
    result = vkGetSwapchainImagesKHR(graphics.device, graphics.swapchain, &count,
                                       graphics.images);
    if (result != VK_SUCCESS || count < capabilities.minImageCount)
        return result != VK_SUCCESS ? result : VK_ERROR_INITIALIZATION_FAILED;
    graphics.image_count = count;
    graphics.extent = capabilities.currentExtent;
    return VK_SUCCESS;
}

static int createRenderPass(void) {
    const VkAttachmentDescription attachment = {
        .format = graphics.format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    const VkAttachmentReference color = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    const VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
    };
    /* The acquire semaphore is waited at colour output; the image layout
     * transition must not start before it. */
    const VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    const VkRenderPassCreateInfo pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };
    return vkCreateRenderPass(graphics.device, &pass_info, NULL,
                                        &graphics.render_pass) == VK_SUCCESS ?
           0 : STAGE_SWAPCHAIN;
}

static void destroyFramebuffers(void) {
    for (uint32_t i = 0; i < MAX_SWAPCHAIN_IMAGES; i++) {
        if (graphics.framebuffers[i])
            vkDestroyFramebuffer(graphics.device, graphics.framebuffers[i], NULL);
        if (graphics.views[i])
            vkDestroyImageView(graphics.device, graphics.views[i], NULL);
        graphics.framebuffers[i] = VK_NULL_HANDLE;
        graphics.views[i] = VK_NULL_HANDLE;
    }
}

static VkResult createFramebuffers(void) {
    for (uint32_t i = 0; i < graphics.image_count; i++) {
        const VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = graphics.images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = graphics.format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        VkResult result = vkCreateImageView(graphics.device, &view_info, NULL,
                                              &graphics.views[i]);
        if (result != VK_SUCCESS)
            return result;
        const VkFramebufferCreateInfo framebuffer_info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = graphics.render_pass,
            .attachmentCount = 1,
            .pAttachments = &graphics.views[i],
            .width = graphics.extent.width,
            .height = graphics.extent.height,
            .layers = 1,
        };
        result = vkCreateFramebuffer(graphics.device, &framebuffer_info, NULL,
                                       &graphics.framebuffers[i]);
        if (result != VK_SUCCESS)
            return result;
    }
    return VK_SUCCESS;
}

static uint32_t greatestCommonDivisor(uint32_t a, uint32_t b) {
    while (b) {
        const uint32_t rest = a % b;
        a = b;
        b = rest;
    }
    return a;
}

/* The menu's 1280x720 layout scales to the swapchain: its height over 720
 * physical pixels per logical pixel (3/2 at 1080p, 3/1 at 2160p). */
static void updateLayoutScale(void) {
    const uint32_t divisor = greatestCommonDivisor(graphics.extent.height, HBMENU_LOGICAL_HEIGHT);
    renderSetScale(graphics.extent.height / divisor, HBMENU_LOGICAL_HEIGHT / divisor);
    graphics.scaled_height = graphics.extent.height;
}

/* After OunceVK reports the window's size changed (VK_SUBOPTIMAL_KHR or
 * VK_ERROR_OUT_OF_DATE_KHR, on docking or undocking). One frame is in
 * flight, so waiting for the device frees every image. */
static VkResult recreateSwapchain(void) {
    vkDeviceWaitIdle(graphics.device);
    destroyFramebuffers();
    VkResult result = createSwapchain(graphics.swapchain);
    if (result == VK_SUCCESS)
        result = createFramebuffers();
    graphics.recreate = false;
    return result;
}

static int createRenderTargets(void) {
    VkResult result = createSwapchain(VK_NULL_HANDLE);
    if (result != VK_SUCCESS)
        return STAGE_SWAPCHAIN;
    if (createRenderPass())
        return STAGE_SWAPCHAIN;
    return createFramebuffers() == VK_SUCCESS ? 0 : STAGE_SWAPCHAIN;
}

static VkResult createShader(const uint32_t *code, size_t bytes, VkShaderModule *out) {
    const VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = bytes,
        .pCode = code,
    };
    return vkCreateShaderModule(graphics.device, &info, NULL, out);
}

static VkResult createGraphicsPipeline(VkShaderModule vertex, VkShaderModule fragment) {
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertex,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragment,
            .pName = "main",
        },
    };
    const VkVertexInputBindingDescription binding = {
        .binding = 0,
        .stride = sizeof(struct Vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    const VkVertexInputAttributeDescription attributes[3] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,
          .offset = offsetof(struct Vertex, x) },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,
          .offset = offsetof(struct Vertex, u) },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R8G8B8A8_UNORM,
          .offset = offsetof(struct Vertex, color) },
    };
    const VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 3,
        .pVertexAttributeDescriptions = attributes,
    };
    const VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    /* The swapchain's size changes with the window; set per frame. */
    const VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    const VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
    };
    const VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states,
    };
    const VkPipelineRasterizationStateCreateInfo rasterization = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    /* Upstream's BlendColor: dst = src * a + dst * (1 - a). */
    const VkPipelineColorBlendAttachmentState blend = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .alphaBlendOp = VK_BLEND_OP_MAX,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo blend_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend,
    };
    const VkGraphicsPipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pColorBlendState = &blend_state,
        .pDynamicState = &dynamic_state,
        .layout = graphics.pipeline_layout,
        .renderPass = graphics.render_pass,
    };
    return vkCreateGraphicsPipelines(graphics.device, VK_NULL_HANDLE, 1, &info,
                                               NULL, &graphics.pipeline);
}

static int createPipeline(void) {
    const VkDescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    const VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding,
    };
    if (vkCreateDescriptorSetLayout(graphics.device, &layout_info, NULL,
                                      &graphics.set_layout) != VK_SUCCESS)
        return STAGE_PIPELINE;
    const VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = MAX_TEXTURES,
    };
    const VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = MAX_TEXTURES,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    if (vkCreateDescriptorPool(graphics.device, &pool_info, NULL,
                                 &graphics.descriptor_pool) != VK_SUCCESS)
        return STAGE_PIPELINE;
    /* Nearest sampling for textures drawn one texel per pixel; linear for
     * images scaled to the screen (the layout is 1.5x at 1080p, 3x at 4K). */
    VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    if (vkCreateSampler(graphics.device, &sampler_info, NULL, &graphics.sampler) !=
        VK_SUCCESS)
        return STAGE_PIPELINE;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    if (vkCreateSampler(graphics.device, &sampler_info, NULL, &graphics.linear_sampler) !=
        VK_SUCCESS)
        return STAGE_PIPELINE;
    const VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .size = 2 * sizeof(float),
    };
    const VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &graphics.set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    };
    if (vkCreatePipelineLayout(graphics.device, &pipeline_layout_info, NULL,
                                 &graphics.pipeline_layout) != VK_SUCCESS)
        return STAGE_PIPELINE;

    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    VkResult result = createShader(hbmenu_quad_vert, sizeof(hbmenu_quad_vert), &vertex);
    if (result == VK_SUCCESS)
        result = createShader(hbmenu_quad_frag, sizeof(hbmenu_quad_frag), &fragment);
    if (result == VK_SUCCESS)
        result = createGraphicsPipeline(vertex, fragment);
    if (vertex)
        vkDestroyShaderModule(graphics.device, vertex, NULL);
    if (fragment)
        vkDestroyShaderModule(graphics.device, fragment, NULL);
    return result == VK_SUCCESS ? 0 : STAGE_PIPELINE;
}

static int createBuffers(void) {
    VkResult result = createHostBuffer(UPLOAD_ARENA_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                       &graphics.arena);
    if (result == VK_SUCCESS)
        result = createQuadBuffers(INITIAL_QUAD_CAPACITY);
    return result == VK_SUCCESS ? 0 : STAGE_BUFFERS;
}

static int createCommands(void) {
    const VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = graphics.queue_family,
    };
    if (vkCreateCommandPool(graphics.device, &pool_info, NULL, &graphics.command_pool) !=
        VK_SUCCESS)
        return STAGE_COMMANDS;
    const VkCommandBufferAllocateInfo commands_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = graphics.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = MAX_SWAPCHAIN_IMAGES,  /* a recreated swapchain may have more */
    };
    if (vkAllocateCommandBuffers(graphics.device, &commands_info, graphics.commands) !=
        VK_SUCCESS)
        return STAGE_COMMANDS;
    const VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    if (vkCreateSemaphore(graphics.device, &semaphore_info, NULL, &graphics.acquired) !=
        VK_SUCCESS)
        return STAGE_COMMANDS;
    for (uint32_t i = 0; i < MAX_SWAPCHAIN_IMAGES; i++) {
        if (vkCreateSemaphore(graphics.device, &semaphore_info, NULL, &graphics.ready[i]) !=
            VK_SUCCESS)
            return STAGE_COMMANDS;
    }
    const VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    const VkResult result = vkCreateFence(graphics.device, &fence_info, NULL,
                                            &graphics.frame_fence);
    return result == VK_SUCCESS ? 0 : STAGE_COMMANDS;
}

static int openGraphics(void) {
    int stage = createInstance();
    if (!stage)
        stage = createDevice();
    if (!stage)
        stage = createSurface();
    if (!stage)
        stage = createRenderTargets();
    if (!stage)
        stage = createPipeline();
    if (!stage)
        stage = createBuffers();
    if (!stage)
        stage = createCommands();
    return stage;
}

static void closeGraphics(void) {
    if (!graphics.device) {
        if (graphics.instance)
            vkDestroyInstance(graphics.instance, NULL);
        return;
    }
    vkDeviceWaitIdle(graphics.device);
    renderShutdown();
    destroyReleasedTextures(true);
    resetUploads();
    free(graphics.overflow);
    free(graphics.uploads);
    free(graphics.groups);
    destroyHostBuffer(&graphics.arena);
    destroyHostBuffer(&graphics.vertices);
    destroyHostBuffer(&graphics.indices);
    if (graphics.frame_fence)
        vkDestroyFence(graphics.device, graphics.frame_fence, NULL);
    if (graphics.acquired)
        vkDestroySemaphore(graphics.device, graphics.acquired, NULL);
    destroyFramebuffers();
    for (uint32_t i = 0; i < MAX_SWAPCHAIN_IMAGES; i++) {
        if (graphics.ready[i])
            vkDestroySemaphore(graphics.device, graphics.ready[i], NULL);
    }
    if (graphics.command_pool)
        vkDestroyCommandPool(graphics.device, graphics.command_pool, NULL);
    if (graphics.pipeline)
        vkDestroyPipeline(graphics.device, graphics.pipeline, NULL);
    if (graphics.pipeline_layout)
        vkDestroyPipelineLayout(graphics.device, graphics.pipeline_layout, NULL);
    if (graphics.sampler)
        vkDestroySampler(graphics.device, graphics.sampler, NULL);
    if (graphics.linear_sampler)
        vkDestroySampler(graphics.device, graphics.linear_sampler, NULL);
    if (graphics.descriptor_pool)
        vkDestroyDescriptorPool(graphics.device, graphics.descriptor_pool, NULL);
    if (graphics.set_layout)
        vkDestroyDescriptorSetLayout(graphics.device, graphics.set_layout, NULL);
    if (graphics.render_pass)
        vkDestroyRenderPass(graphics.device, graphics.render_pass, NULL);
    if (graphics.swapchain)
        vkDestroySwapchainKHR(graphics.device, graphics.swapchain, NULL);
    if (graphics.surface)
        vkDestroySurfaceKHR(graphics.instance, graphics.surface, NULL);
    vkDestroyDevice(graphics.device, NULL);
    vkDestroyInstance(graphics.instance, NULL);
}

/* --- Frames --------------------------------------------------------------- */

static bool pushGroup(RenderTexture texture, uint32_t quad) {
    if (graphics.group_count) {
        struct DrawGroup *last = &graphics.groups[graphics.group_count - 1];
        if (last->texture == texture) {
            last->quad_count++;
            return true;
        }
    }
    if (!growArray((void **)&graphics.groups, &graphics.group_capacity,
                   graphics.group_count + 1, sizeof(*graphics.groups)))
        return false;
    graphics.groups[graphics.group_count++] = (struct DrawGroup){
        .texture = texture, .first_quad = quad, .quad_count = 1,
    };
    return true;
}

static bool drawable(const struct RenderQuad *quad) {
    return textureFor(quad->texture) && quad->x0 < quad->x1 && quad->y0 < quad->y1;
}

/* Writes the frame's vertices and groups consecutive quads by texture. */
static VkResult writeVertices(const struct RenderQuad *quads, uint32_t count) {
    if (count > graphics.quad_capacity) {
        uint32_t capacity = graphics.quad_capacity;
        while (capacity < count)
            capacity *= 2;
        const VkResult result = createQuadBuffers(capacity);
        if (result != VK_SUCCESS)
            return result;
    }
    struct Vertex *vertices = graphics.vertices.mapped;
    uint32_t written = 0;
    graphics.group_count = 0;
    for (uint32_t i = 0; i < count; i++) {
        const struct RenderQuad *quad = &quads[i];
        if (!drawable(quad))
            continue;
        if (!pushGroup(quad->texture, written))
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        vertices[0] = (struct Vertex){ quad->x0, quad->y0, quad->u0, quad->v0, quad->color };
        vertices[1] = (struct Vertex){ quad->x1, quad->y0, quad->u1, quad->v0, quad->color };
        vertices[2] = (struct Vertex){ quad->x1, quad->y1, quad->u1, quad->v1, quad->color };
        vertices[3] = (struct Vertex){ quad->x0, quad->y1, quad->u0, quad->v1, quad->color };
        vertices += 4;
        written++;
    }
    graphics.drawn_quads = written;
    return VK_SUCCESS;
}

static void recordDraws(VkCommandBuffer command, uint32_t image_index) {
    const VkClearValue clear = {
        .color = { .float32 = { kClearGrey, kClearGrey, kClearGrey, 1.0f } },
    };
    const VkRenderPassBeginInfo pass = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = graphics.render_pass,
        .framebuffer = graphics.framebuffers[image_index],
        .renderArea = { .extent = graphics.extent },
        .clearValueCount = 1,
        .pClearValues = &clear,
    };
    vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics.pipeline);
    const VkViewport viewport = {
        .width = (float)graphics.extent.width,
        .height = (float)graphics.extent.height,
        .maxDepth = 1.0f,
    };
    const VkRect2D scissor = { .extent = graphics.extent };
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    const float scale[2] = { 2.0f / graphics.extent.width, 2.0f / graphics.extent.height };
    vkCmdPushConstants(command, graphics.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
                         0, sizeof(scale), scale);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(command, 0, 1, &graphics.vertices.buffer, &offset);
    vkCmdBindIndexBuffer(command, graphics.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    for (uint32_t i = 0; i < graphics.group_count; i++) {
        const struct DrawGroup *group = &graphics.groups[i];
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  graphics.pipeline_layout, 0, 1,
                                  &graphics.textures[group->texture].set, 0, NULL);
        vkCmdDrawIndexed(command, group->quad_count * 6, 1, group->first_quad * 6, 0, 0);
    }
    vkCmdEndRenderPass(command);
}

static VkResult submitFrame(VkCommandBuffer command, uint32_t image_index) {
    VkResult result = vkResetFences(graphics.device, 1, &graphics.frame_fence);
    if (result != VK_SUCCESS)
        return result;
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &graphics.acquired,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &command,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &graphics.ready[image_index],
    };
    result = vkQueueSubmit(graphics.queue, 1, &submit, graphics.frame_fence);
    if (result != VK_SUCCESS)
        return result;
    graphics.frame_index++;
    const VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &graphics.ready[image_index],
        .swapchainCount = 1,
        .pSwapchains = &graphics.swapchain,
        .pImageIndices = &image_index,
    };
    result = vkQueuePresentKHR(graphics.queue, &present);
    if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) {
        graphics.recreate = true;
        result = VK_SUCCESS;
    }
    return result;
}

/* Records uploads and draws into the acquired image, then submits and
 * presents it. */
static VkResult presentFrame(void) {
    uint32_t index = 0;
    VkResult result = vkAcquireNextImageKHR(graphics.device, graphics.swapchain, UINT64_MAX,
                                              graphics.acquired, VK_NULL_HANDLE, &index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        /* No image from the old swapchain: recreate now. This frame keeps
         * the layout scale it was built with; the next one is rescaled. */
        result = recreateSwapchain();
        if (result == VK_SUCCESS)
            result = vkAcquireNextImageKHR(graphics.device, graphics.swapchain, UINT64_MAX,
                                             graphics.acquired, VK_NULL_HANDLE, &index);
    }
    if (result == VK_SUBOPTIMAL_KHR) {
        graphics.recreate = true;
        result = VK_SUCCESS;
    }
    if (result != VK_SUCCESS)
        return result;
    VkCommandBuffer command = graphics.commands[index];
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    result = vkResetCommandBuffer(command, 0);
    if (result == VK_SUCCESS)
        result = vkBeginCommandBuffer(command, &begin);
    if (result != VK_SUCCESS)
        return result;
    recordUploads(command);
    recordDraws(command, index);
    result = vkEndCommandBuffer(command);
    if (result != VK_SUCCESS)
        return result;
    return submitFrame(command, index);
}

/* --- nx_graphics.h -------------------------------------------------------- */

int hbmenuGraphicsBind(const struct HbmenuGraphicsConfig *config) {
    if (!config || !config->window || graphics.config)
        return 0;
    bound_config = *config;
    graphics.config = &bound_config;
    graphics_status = 0;
    return 1;
}

int hbmenuGraphicsStatus(void) {
    return graphics_status;
}

/* The size comes from the window (ouncevk_platform.c), not from main.c. */
void graphicsInit(u32 width, u32 height) {
    (void)width;
    (void)height;
    if (!graphics.config) {
        graphics_status = STAGE_INSTANCE;
        return;
    }
    mutexInit(&graphics.texture_lock);
    graphics_status = openGraphics();
    if (graphics_status) {
        closeGraphics();
        const struct HbmenuGraphicsConfig *config = graphics.config;
        memset(&graphics, 0, sizeof(graphics));
        graphics.config = config;
        return;
    }
    renderInit();
    updateLayoutScale();
}

/* Switch 2 has no CPU framebuffer: menuLoop draws through render.h, and
 * the render pass replaces main.c's clear. */
void *graphicsFrameBegin(u32 *out_stride) {
    if (out_stride)
        *out_stride = 0;
    if (!graphics_status) {
        perfFrameStart();
        const VkResult result = vkWaitForFences(graphics.device, 1,
                                                          &graphics.frame_fence, VK_TRUE,
                                                          UINT64_MAX);
        perfMark(PerfPhase_Wait);
        if (result == VK_SUCCESS) {
            resetUploads();
            destroyReleasedTextures(false);
            if (graphics.recreate && recreateSwapchain() != VK_SUCCESS)
                graphics_status = STAGE_SWAPCHAIN;
            else if (graphics.extent.height != graphics.scaled_height)
                updateLayoutScale();
        } else {
            graphics_status = STAGE_PRESENT;
        }
    }
    renderFrameBegin();
    return NULL;
}

void graphicsFrameEnd(void) {
    if (graphics_status) {
        uint32_t ignored;
        renderFrameFinish(&ignored);
        return;
    }
    perfDrawOverlay();
    uint32_t count = 0;
    const struct RenderQuad *quads = renderFrameFinish(&count);
    perfMark(PerfPhase_Draw);
    VkResult result = writeVertices(quads, count);
    perfCountDraws(graphics.drawn_quads, graphics.group_count);
    perfMark(PerfPhase_Copy);
    if (result == VK_SUCCESS)
        result = presentFrame();
    perfMark(PerfPhase_Present);
    perfFrameEnd();
    if (result != VK_SUCCESS) {
        graphics_status = STAGE_PRESENT;
    }
}

void graphicsExit(void) {
    if (graphics.config)
        closeGraphics();
    memset(&graphics, 0, sizeof(graphics));
    memset(&bound_config, 0, sizeof(bound_config));
    graphics_status = STAGE_INSTANCE;
}
