#define _GNU_SOURCE
#include "../gamenative_openxr_unix.h"

#if defined(__ANDROID__)
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <android/hardware_buffer.h>
#endif
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define GN_FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
     ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define DRM_FORMAT_ABGR8888 GN_FOURCC('A', 'B', '2', '4')
#define DRM_FORMAT_ARGB8888 GN_FOURCC('A', 'R', '2', '4')

typedef int32_t (*unixlib_entry_t)(void *);

enum gn_transport_kind {
    GN_TRANSPORT_UNKNOWN = 0,
    GN_TRANSPORT_AHARDWAREBUFFER = 1,
    GN_TRANSPORT_DMABUF = 2,
    GN_TRANSPORT_RELAY = 3
};

struct gn_transport_image {
    VkImage image;
    VkDeviceMemory memory;
    VkCommandBuffer command_buffer;
    /* Device and pool that own the objects above; the game may recreate its device. */
    VkDevice owner;
    VkCommandPool command_pool;
    void *hardware_buffer;
    uint8_t relay;
    uint8_t registered;
    uint8_t initialized;
    uint8_t steady_recorded;
    /* Source region the recorded copy covers: the eye's sub-image plus a border. */
    VkOffset2D copy_offset;
    VkExtent2D copy_extent;
};

struct wine_client_object {
    uint64_t loader_magic;
    uint64_t unix_handle;
};

struct gn_image {
    VkImage image;
    VkDeviceMemory memory;
    int dma_buf_fd;
    uint32_t plane_count;
    uint32_t strides[4];
    uint32_t offsets[4];
    uint64_t modifier;
    uint8_t registered_eye_mask;
    uint32_t registered_array_index[2];
    uint8_t transport_kind[2];
    struct gn_transport_image transport[2];
    VkImage relay_source_image;
    VkDeviceMemory relay_source_memory;
    uint8_t submitted;
};

struct gn_swapchain {
    uint8_t allocated;
    uint32_t width;
    uint32_t height;
    VkFormat format;
    uint32_t array_size;
    uint32_t sample_count;
    uint32_t image_count;
    struct gn_image images[GN_UNIX_MAX_IMAGES];
};

static void submit_worker_flush(void);
static uint8_t image_copy_busy[32u][4u];
static pthread_mutex_t submit_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t submit_space_cond = PTHREAD_COND_INITIALIZER;

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t socket_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct gn_swapchain swapchains[GN_UNIX_MAX_SWAPCHAINS];
static VkPhysicalDevice physical_device;
static VkDevice device;
static VkQueue queue;
static uint32_t queue_family_index;
static VkCommandPool command_pool;
static VkDevice command_pool_device;
static void *vulkan_so;
static int transport_fd = -1;
static uint8_t transport_frame_announced[2];
static uint64_t transport_frame_id;

/* Per-frame stage timing (skills/gamenative-stage-concurrency-analysis, contract v1). The Wine
 * process has no profiler ring: the second eye's FRAME line carries these CLOCK_MONOTONIC
 * timestamps to the app, which republishes them as counters. 0 means not measured. */
enum gn_timing_field {
    GN_T_FRAME_SYNC_BEGIN, GN_T_FRAME_SYNC_END,
    GN_T_WAIT_L_BEGIN, GN_T_WAIT_L_END, GN_T_WAIT_R_BEGIN, GN_T_WAIT_R_END,
    GN_T_DRAIN_BEGIN, GN_T_DRAIN_LOCK, GN_T_DRAIN_END,
    GN_T_SUBMIT_END,
    GN_T_FENCE_BEGIN, GN_T_FENCE_END,
    GN_T_SEND_BEGIN,
    GN_T_DELAY_BEGIN, /* contract v1.1: start of the frame-start delay before FRAME_SYNC */
    GN_T_COUNT
};

struct gn_frame_timing {
    int64_t submit_begin;
    int64_t snap; /* FRAME_SYNC serial this frame rendered against, -1 if unknown */
    int64_t t[GN_T_COUNT];
};

static pthread_mutex_t timing_mutex = PTHREAD_MUTEX_INITIALIZER;
static int64_t frame_sync_timing[3]; /* delay begin, FRAME_SYNC begin, FRAME_SYNC end */
static int64_t frame_sync_serial = -1;
static int frame_sync_unclaimed;

/* Frame-start delay. When a GPU-bound game blocks between Submit and xrEndFrame until the
 * previous frame's GPU work retires, the next FRAME_SYNC is held back by part of that excess
 * wait: the pose is sampled later while the GPU still has queued work. The app enables it
 * through the FRAME_SYNC reply (" jit=<target us>", 0 = off). All fields use timing_mutex. */
static int64_t delay_target_ns;
static int64_t delay_ns;
static uint64_t delay_updated_frame;
static uint64_t last_submit_frame;
static int64_t last_submit_wait_end, last_submit_drain_begin;
static uint64_t fence_end_frame[8];
static int64_t fence_end_time[8];
static int64_t swapchain_wait_timing[GN_UNIX_MAX_SWAPCHAINS][2];
static uint8_t swapchain_wait_unclaimed[GN_UNIX_MAX_SWAPCHAINS];

static int64_t monotonic_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}

static PFN_vkGetDeviceProcAddr p_vkGetDeviceProcAddr;
static PFN_vkGetPhysicalDeviceMemoryProperties p_vkGetPhysicalDeviceMemoryProperties;
static PFN_vkGetPhysicalDeviceFormatProperties2 p_vkGetPhysicalDeviceFormatProperties2;
static PFN_vkCreateImage p_vkCreateImage;
static PFN_vkDestroyImage p_vkDestroyImage;
static PFN_vkGetImageMemoryRequirements p_vkGetImageMemoryRequirements;
static PFN_vkAllocateMemory p_vkAllocateMemory;
static PFN_vkFreeMemory p_vkFreeMemory;
static PFN_vkBindImageMemory p_vkBindImageMemory;
static PFN_vkGetImageSubresourceLayout p_vkGetImageSubresourceLayout;
static PFN_vkGetImageDrmFormatModifierPropertiesEXT p_vkGetImageDrmFormatModifierPropertiesEXT;
static PFN_vkGetMemoryFdKHR p_vkGetMemoryFdKHR;
static PFN_vkCreateFence p_vkCreateFence;
static PFN_vkDestroyFence p_vkDestroyFence;
static PFN_vkWaitForFences p_vkWaitForFences;
static PFN_vkGetFenceFdKHR p_vkGetFenceFdKHR;
static PFN_vkQueueSubmit p_vkQueueSubmit;
static PFN_vkQueueWaitIdle p_vkQueueWaitIdle;
static PFN_vkCreateCommandPool p_vkCreateCommandPool;
static PFN_vkDestroyCommandPool p_vkDestroyCommandPool;
static PFN_vkAllocateCommandBuffers p_vkAllocateCommandBuffers;
static PFN_vkFreeCommandBuffers p_vkFreeCommandBuffers;
static PFN_vkResetCommandBuffer p_vkResetCommandBuffer;
static PFN_vkBeginCommandBuffer p_vkBeginCommandBuffer;
static PFN_vkEndCommandBuffer p_vkEndCommandBuffer;
static PFN_vkCmdPipelineBarrier p_vkCmdPipelineBarrier;
static PFN_vkCmdCopyImage p_vkCmdCopyImage;
#if defined(__ANDROID__)
static PFN_vkGetAndroidHardwareBufferPropertiesANDROID
    p_vkGetAndroidHardwareBufferPropertiesANDROID;
#endif

static void log_line(const char *line)
{
    const char *path = getenv("GAMENATIVE_XR_UNIX_LOG");
    if (!path || !*path) path = "/tmp/gamenative-xr-unix.log";
    FILE *f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%s\n", line);
    fclose(f);
}

static void log_vulkan_context_state(void)
{
    char line[384];
    snprintf(line, sizeof(line),
             "Vulkan context phys=%p device=%p queue=%p gpa=%p createImage=%p "
             "getMemoryFd=%p createFence=%p getFenceFd=%p queueSubmit=%p "
             "commandPool=%p ahb=%p",
             (void *)physical_device, (void *)device, (void *)queue,
             (void *)p_vkGetDeviceProcAddr, (void *)p_vkCreateImage,
             (void *)p_vkGetMemoryFdKHR, (void *)p_vkCreateFence,
             (void *)p_vkGetFenceFdKHR, (void *)p_vkQueueSubmit,
             (void *)command_pool,
#if defined(__ANDROID__)
             (void *)p_vkGetAndroidHardwareBufferPropertiesANDROID
#else
             NULL
#endif
    );
    log_line(line);
}

static uintptr_t mapping_lookup_address(uintptr_t address)
{
#if UINTPTR_MAX > 0xffffffffu



    return address & (uintptr_t)0x00ffffffffffffffULL;
#else
    return address;
#endif
}

static uintptr_t readable_mapping_end(uintptr_t address)
{
    address = mapping_lookup_address(address);
    FILE *maps = fopen("/proc/self/maps", "r");
    char line[256];
    if (!maps) return address;
    while (fgets(line, sizeof(line), maps)) {
        unsigned long long start, end;
        char permissions[5] = {0};
        if (sscanf(line, "%llx-%llx %4s", &start, &end, permissions) == 3 &&
            address >= (uintptr_t)start && address < (uintptr_t)end &&
            permissions[0] == 'r') {
            fclose(maps);
            return (uintptr_t)end;
        }
    }
    fclose(maps);
    return address;
}

static int readable_mapping_contains(uintptr_t address, size_t size)
{
    uintptr_t lookup_address;
    uintptr_t end;
    if (!address || !size) return 0;
    lookup_address = mapping_lookup_address(address);
    end = readable_mapping_end(lookup_address);
    return end > lookup_address && size <= end - lookup_address;
}

static uint64_t unwrap_dispatchable(uint64_t client_handle)
{
    char trace[224];
    const struct wine_client_object *client =
        (const struct wine_client_object *)(uintptr_t)client_handle;
    if (!readable_mapping_contains((uintptr_t)client, sizeof(*client))) {
        snprintf(trace, sizeof(trace),
                 "unwrap client=0x%llx is not a readable Wine object",
                 (unsigned long long)client_handle);
        log_line(trace);
        return 0;
    }
    if (!client->unix_handle) {
        snprintf(trace, sizeof(trace), "unwrap client=0x%llx has no unix object",
                 (unsigned long long)client_handle);
        log_line(trace);
        return 0;
    }
    const uint64_t *object =
        (const uint64_t *)(uintptr_t)client->unix_handle;
    if (!readable_mapping_contains((uintptr_t)object, 2 * sizeof(*object))) {
        snprintf(trace, sizeof(trace),
                 "unwrap client=0x%llx unix object=0x%llx is not readable",
                 (unsigned long long)client_handle,
                 (unsigned long long)client->unix_handle);
        log_line(trace);
        return 0;
    }
    if (object[1] == client_handle) {
        snprintf(trace, sizeof(trace),
                 "unwrap client=0x%llx object=0x%llx Wine10+ host=0x%llx",
                 (unsigned long long)client_handle,
                 (unsigned long long)client->unix_handle,
                 (unsigned long long)object[0]);
        log_line(trace);
        return object[0];
    }




    uintptr_t object_address = mapping_lookup_address((uintptr_t)object);
    uintptr_t end = readable_mapping_end(object_address);
    size_t words = end > object_address ?
        (end - object_address) / sizeof(*object) : 0;
    if (words > 8192) words = 8192;
    for (size_t i = 0; i + 1 < words; ++i) {
        if (object[i] == client_handle) {
            snprintf(trace, sizeof(trace),
                     "unwrap client=0x%llx object=0x%llx Wine9 index=%zu host=0x%llx",
                     (unsigned long long)client_handle,
                     (unsigned long long)client->unix_handle, i,
                     (unsigned long long)object[i + 1]);
            log_line(trace);
            return object[i + 1];
        }
    }



    const size_t wine_92_device_host_index = (4088 / sizeof(*object)) + 1;
    if (words > wine_92_device_host_index &&
        object[wine_92_device_host_index - 2] &&
        object[wine_92_device_host_index]) {
        snprintf(trace, sizeof(trace),
                 "unwrap client=0x%llx object=0x%llx Wine9.2 device host=0x%llx",
                 (unsigned long long)client_handle,
                 (unsigned long long)client->unix_handle,
                 (unsigned long long)object[wine_92_device_host_index]);
        log_line(trace);
        return object[wine_92_device_host_index];
    }
    snprintf(trace, sizeof(trace),
             "unwrap client=0x%llx object=0x%llx no match words=%zu "
             "w510=0x%llx w511=0x%llx w512=0x%llx w513=0x%llx",
             (unsigned long long)client_handle,
             (unsigned long long)client->unix_handle, words,
             (unsigned long long)(words > 510 ? object[510] : 0),
             (unsigned long long)(words > 511 ? object[511] : 0),
             (unsigned long long)(words > 512 ? object[512] : 0),
             (unsigned long long)(words > 513 ? object[513] : 0));
    log_line(trace);
    return words ? object[0] : 0;
}

static int write_all(int fd, const void *data, size_t len)
{
    const char *p = data;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        p += n;
        len -= (size_t)n;
    }
    return 1;
}

static int read_line(int fd, char *line, size_t capacity)
{
    size_t used = 0;
    if (!capacity) return 0;
    while (used + 1 < capacity) {
        char ch;
        ssize_t n = recv(fd, &ch, 1, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        if (ch == '\n') {
            line[used] = 0;
            return 1;
        }
        line[used++] = ch;
    }
    line[capacity - 1] = 0;
    return 0;
}

static int send_fd(int socket_fd, int fd)
{
    char payload = 'F';
    struct iovec iov = {&payload, 1};
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
    ssize_t n;
    do n = sendmsg(socket_fd, &msg, MSG_NOSIGNAL); while (n < 0 && errno == EINTR);
    return n == 1;
}

static int recv_fd(int socket_fd)
{
    char payload;
    struct iovec iov = {&payload, 1};
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    ssize_t n;
    do n = recvmsg(socket_fd, &msg, 0); while (n < 0 && errno == EINTR);
    if (n <= 0) return -1;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
            cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
            int fd;
            memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
            return fd;
        }
    }
    return -1;
}

static void close_transport(void)
{
    if (transport_fd >= 0) close(transport_fd);
    transport_fd = -1;
    // Android may recreate its EGL output after backgrounding. The new receiver
    // has no buffer registrations even though guest swapchains still exist.
    for (uint32_t slot=0; slot<GN_UNIX_MAX_SWAPCHAINS; ++slot) {
        for (uint32_t image=0; image<swapchains[slot].image_count; ++image) {
            struct gn_image *entry=&swapchains[slot].images[image];
            entry->registered_eye_mask=0;
            for (uint32_t eye=0; eye<2; ++eye) entry->transport[eye].registered=0;
        }
    }
}

static int ensure_transport(void)
{
    if (transport_fd >= 0) return 1;
    const char *path = getenv("GAMENATIVE_XR_SOCKET");
    if (!path || !*path) path = "@gamenative-xr";

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    socklen_t length;
    if (path[0] == '@') {
        size_t size = strlen(path + 1);
        if (size > sizeof(address.sun_path) - 2) size = sizeof(address.sun_path) - 2;
        address.sun_path[0] = 0;
        memcpy(address.sun_path + 1, path + 1, size);
        length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + size);
    } else {
        strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);
        length = sizeof(address);
    }
    if (connect(fd, (struct sockaddr *)&address, length) < 0) {
        close(fd);
        return 0;
    }
    if (!write_all(fd, "HELLO producer=wine-unixlib version=2\n", 38)) {
        close(fd);
        return 0;
    }
    char response[64];
    if (!read_line(fd, response, sizeof(response)) || strncmp(response, "OK", 2)) {
        close(fd);
        return 0;
    }
    transport_fd = fd;
    return 1;
}

static int transact_line(const char *line, char *response, size_t response_size)
{
    for (int attempt=0; attempt<2; ++attempt) {
        if (ensure_transport() && write_all(transport_fd, line, strlen(line)) &&
            read_line(transport_fd, response, response_size)) return 1;
        close_transport();
    }
    return 0;
}

static void load_vulkan_functions(void)
{
    if (!vulkan_so) vulkan_so = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!vulkan_so) return;
    p_vkGetDeviceProcAddr = dlsym(vulkan_so, "vkGetDeviceProcAddr");
    p_vkGetPhysicalDeviceMemoryProperties =
        dlsym(vulkan_so, "vkGetPhysicalDeviceMemoryProperties");
    p_vkGetPhysicalDeviceFormatProperties2 =
        dlsym(vulkan_so, "vkGetPhysicalDeviceFormatProperties2");
    if (!p_vkGetDeviceProcAddr || !device) return;
#define LOAD_DEVICE(name) p_##name = (PFN_##name)p_vkGetDeviceProcAddr(device, #name)
    LOAD_DEVICE(vkCreateImage);
    LOAD_DEVICE(vkDestroyImage);
    LOAD_DEVICE(vkGetImageMemoryRequirements);
    LOAD_DEVICE(vkAllocateMemory);
    LOAD_DEVICE(vkFreeMemory);
    LOAD_DEVICE(vkBindImageMemory);
    LOAD_DEVICE(vkGetImageSubresourceLayout);
    LOAD_DEVICE(vkGetImageDrmFormatModifierPropertiesEXT);
    LOAD_DEVICE(vkGetMemoryFdKHR);
    LOAD_DEVICE(vkCreateFence);
    LOAD_DEVICE(vkDestroyFence);
    LOAD_DEVICE(vkWaitForFences);
    LOAD_DEVICE(vkGetFenceFdKHR);
    LOAD_DEVICE(vkQueueSubmit);
    LOAD_DEVICE(vkQueueWaitIdle);
    LOAD_DEVICE(vkCreateCommandPool);
    LOAD_DEVICE(vkDestroyCommandPool);
    LOAD_DEVICE(vkAllocateCommandBuffers);
    LOAD_DEVICE(vkFreeCommandBuffers);
    LOAD_DEVICE(vkResetCommandBuffer);
    LOAD_DEVICE(vkBeginCommandBuffer);
    LOAD_DEVICE(vkEndCommandBuffer);
    LOAD_DEVICE(vkCmdPipelineBarrier);
    LOAD_DEVICE(vkCmdCopyImage);
#if defined(__ANDROID__)
    LOAD_DEVICE(vkGetAndroidHardwareBufferPropertiesANDROID);
#endif
#undef LOAD_DEVICE



#define LOAD_HOST_TRAMPOLINE(name) \
    if (!p_##name) p_##name = (PFN_##name)dlsym(vulkan_so, #name)
    LOAD_HOST_TRAMPOLINE(vkGetImageDrmFormatModifierPropertiesEXT);
    LOAD_HOST_TRAMPOLINE(vkGetMemoryFdKHR);
    LOAD_HOST_TRAMPOLINE(vkGetFenceFdKHR);
#if defined(__ANDROID__)
    LOAD_HOST_TRAMPOLINE(vkGetAndroidHardwareBufferPropertiesANDROID);
#endif
#undef LOAD_HOST_TRAMPOLINE
}

static int find_memory_type(uint32_t bits, VkMemoryPropertyFlags desired)
{
    VkPhysicalDeviceMemoryProperties props;
    p_vkGetPhysicalDeviceMemoryProperties(physical_device, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & desired) == desired) return (int)i;
    }
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if (bits & (1u << i)) return (int)i;
    return -1;
}

static uint32_t format_to_fourcc(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        return DRM_FORMAT_ABGR8888;
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return DRM_FORMAT_ARGB8888;
    default:
        return 0;
    }
}

static void log_vk_result(const char *operation, VkResult result)
{
    char line[160];
    snprintf(line, sizeof(line), "%s failed with VkResult %d", operation, result);
    log_line(line);
}

#if defined(__ANDROID__)
static int ahb_transport_probe(void)
{
    static int cached;
    if (cached) return cached > 0;
    AHardwareBuffer_Desc descriptor = {
        .width = 16,
        .height = 16,
        .layers = 1,
        .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT
    };
    AHardwareBuffer *buffer = NULL;
    if (p_vkGetAndroidHardwareBufferPropertiesANDROID && command_pool &&
        p_vkCmdCopyImage && AHardwareBuffer_allocate(&descriptor, &buffer) == 0 && buffer) {
        AHardwareBuffer_release(buffer);
        cached = 1;
        log_line("AHardwareBuffer transport available; using optimal-tiling render targets");
    } else {
        cached = -1;
        log_line("AHardwareBuffer transport unavailable; render targets stay linear/exportable");
    }
    return cached > 0;
}
#endif

static int create_image(struct gn_swapchain *swapchain, struct gn_image *out,
                        uint32_t array_size, uint32_t mip_count, uint32_t sample_count,
                        int optimal)
{
    if (optimal) {
        VkImageCreateInfo optimal_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = swapchain->format,
            .extent = {swapchain->width, swapchain->height, 1},
            .mipLevels = mip_count ? mip_count : 1,
            .arrayLayers = array_size ? array_size : 1,
            .samples = sample_count == 2 ? VK_SAMPLE_COUNT_2_BIT :
                       sample_count == 4 ? VK_SAMPLE_COUNT_4_BIT :
                       sample_count == 8 ? VK_SAMPLE_COUNT_8_BIT : VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
        };
        VkResult optimal_result = p_vkCreateImage(device, &optimal_info, NULL, &out->image);
        if (optimal_result != VK_SUCCESS) {
            log_vk_result("vkCreateImage(optimal)", optimal_result);
            goto optimal_fail;
        }
        VkMemoryRequirements optimal_requirements;
        p_vkGetImageMemoryRequirements(device, out->image, &optimal_requirements);
        int optimal_type = find_memory_type(
            optimal_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (optimal_type < 0)
            optimal_type = find_memory_type(optimal_requirements.memoryTypeBits, 0);
        if (optimal_type < 0) {
            log_line("No compatible memory type for optimal image");
            goto optimal_fail;
        }
        VkMemoryDedicatedAllocateInfo optimal_dedicated = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .image = out->image
        };
        VkMemoryAllocateInfo optimal_allocate = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &optimal_dedicated,
            .allocationSize = optimal_requirements.size,
            .memoryTypeIndex = (uint32_t)optimal_type
        };
        optimal_result = p_vkAllocateMemory(device, &optimal_allocate, NULL, &out->memory);
        if (optimal_result != VK_SUCCESS) {
            log_vk_result("vkAllocateMemory(optimal)", optimal_result);
            goto optimal_fail;
        }
        optimal_result = p_vkBindImageMemory(device, out->image, out->memory, 0);
        if (optimal_result != VK_SUCCESS) {
            log_vk_result("vkBindImageMemory(optimal)", optimal_result);
            goto optimal_fail;
        }
        out->dma_buf_fd = -1;
        out->plane_count = 0;
        out->modifier = 0;
        return 1;
    optimal_fail:
        if (out->memory && p_vkFreeMemory) p_vkFreeMemory(device, out->memory, NULL);
        out->memory = VK_NULL_HANDLE;
        if (out->image && p_vkDestroyImage) p_vkDestroyImage(device, out->image, NULL);
        out->image = VK_NULL_HANDLE;
        return 0;
    }

    VkExternalMemoryImageCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
    };
    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = swapchain->format,
        .extent = {swapchain->width, swapchain->height, 1},
        .mipLevels = mip_count ? mip_count : 1,
        .arrayLayers = array_size ? array_size : 1,
        .samples = sample_count == 2 ? VK_SAMPLE_COUNT_2_BIT :
                   sample_count == 4 ? VK_SAMPLE_COUNT_4_BIT :
                   sample_count == 8 ? VK_SAMPLE_COUNT_8_BIT : VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };

    VkDrmFormatModifierPropertiesListEXT modifier_list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT
    };
    VkFormatProperties2 format_props = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &modifier_list
    };
    VkDrmFormatModifierPropertiesEXT *modifiers = NULL;
    VkImageDrmFormatModifierListCreateInfoEXT modifier_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT
    };
    uint64_t chosen_modifier = 0;
    uint32_t chosen_plane_count = 1;
    int has_chosen_modifier = 0;
    if (p_vkGetPhysicalDeviceFormatProperties2) {
        p_vkGetPhysicalDeviceFormatProperties2(physical_device, swapchain->format, &format_props);
        if (modifier_list.drmFormatModifierCount) {
            modifiers = calloc(modifier_list.drmFormatModifierCount, sizeof(*modifiers));
            modifier_list.pDrmFormatModifierProperties = modifiers;
            p_vkGetPhysicalDeviceFormatProperties2(physical_device, swapchain->format, &format_props);
            for (uint32_t i = 0; i < modifier_list.drmFormatModifierCount; ++i) {
                VkFormatFeatureFlags features = modifiers[i].drmFormatModifierTilingFeatures;
                if ((features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) &&
                    (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
                    modifiers[i].drmFormatModifierPlaneCount >= 1 &&
                    modifiers[i].drmFormatModifierPlaneCount <= 4) {



                    if (!has_chosen_modifier || modifiers[i].drmFormatModifier == 0) {
                        chosen_modifier = modifiers[i].drmFormatModifier;
                        chosen_plane_count =
                            modifiers[i].drmFormatModifierPlaneCount;
                        has_chosen_modifier = 1;
                    }
                    if (modifiers[i].drmFormatModifier == 0) break;
                }
            }
        }
    }
    if (has_chosen_modifier) {
        modifier_info.drmFormatModifierCount = 1;
        modifier_info.pDrmFormatModifiers = &chosen_modifier;
        external.pNext = &modifier_info;
        info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    }

    VkResult result = p_vkCreateImage(device, &info, NULL, &out->image);
    free(modifiers);
    if (result != VK_SUCCESS && has_chosen_modifier) {
        chosen_modifier = 0;
        chosen_plane_count = 1;
        has_chosen_modifier = 0;
        external.pNext = NULL;
        info.tiling = VK_IMAGE_TILING_LINEAR;
        result = p_vkCreateImage(device, &info, NULL, &out->image);
    }
    if (result != VK_SUCCESS) {
        log_vk_result("vkCreateImage(dma-buf)", result);
        goto fail;
    }

    VkMemoryRequirements requirements;
    p_vkGetImageMemoryRequirements(device, out->image, &requirements);



    int memory_type = find_memory_type(
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memory_type < 0) {
        log_line("No compatible memory type for dma-buf image");
        goto fail;
    }

    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = out->image
    };
    VkExportMemoryAllocateInfo export_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
    };
    VkMemoryAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &export_info,
        .allocationSize = requirements.size,
        .memoryTypeIndex = (uint32_t)memory_type
    };
    result = p_vkAllocateMemory(device, &allocate_info, NULL, &out->memory);
    if (result != VK_SUCCESS) {
        log_vk_result("vkAllocateMemory(dma-buf)", result);
        goto fail;
    }
    result = p_vkBindImageMemory(device, out->image, out->memory, 0);
    if (result != VK_SUCCESS) {
        log_vk_result("vkBindImageMemory(dma-buf)", result);
        goto fail;
    }

    VkMemoryGetFdInfoKHR fd_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = out->memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
    };
    if (!p_vkGetMemoryFdKHR) {
        log_line("vkGetMemoryFdKHR is unavailable");
        goto fail;
    }
    result = p_vkGetMemoryFdKHR(device, &fd_info, &out->dma_buf_fd);
    if (result != VK_SUCCESS) {
        log_vk_result("vkGetMemoryFdKHR(dma-buf)", result);
        goto fail;
    }

    out->plane_count = chosen_plane_count;
    for (uint32_t plane = 0; plane < out->plane_count; ++plane) {
        VkImageSubresource subresource = {
            .aspectMask = out->plane_count == 1
                ? VK_IMAGE_ASPECT_COLOR_BIT
                : (VkImageAspectFlags)
                    (VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << plane),
            .mipLevel = 0,
            .arrayLayer = 0
        };
        VkSubresourceLayout layout;
        p_vkGetImageSubresourceLayout(
            device, out->image, &subresource, &layout);
        out->strides[plane] = (uint32_t)layout.rowPitch;
        out->offsets[plane] = (uint32_t)layout.offset;
    }
    out->modifier = has_chosen_modifier ? chosen_modifier : 0;
    if (has_chosen_modifier && p_vkGetImageDrmFormatModifierPropertiesEXT) {
        VkImageDrmFormatModifierPropertiesEXT props = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT
        };
        if (p_vkGetImageDrmFormatModifierPropertiesEXT(device, out->image, &props) == VK_SUCCESS)
            out->modifier = props.drmFormatModifier;
    }
    {
        char trace[320];
        snprintf(trace, sizeof(trace),
                 "dma-buf image format=%d size=%ux%u layers=%u samples=%u tiling=%s "
                 "modifier=0x%llx planes=%u stride0=%u offset0=%u",
                 (int)swapchain->format, swapchain->width, swapchain->height,
                 array_size ? array_size : 1,
                 sample_count ? sample_count : 1,
                 has_chosen_modifier ? "modifier" : "linear",
                 (unsigned long long)out->modifier, out->plane_count,
                 out->strides[0], out->offsets[0]);
        log_line(trace);
    }
    return 1;

fail:
    if (out->dma_buf_fd >= 0) close(out->dma_buf_fd);
    out->dma_buf_fd = -1;
    if (out->memory && p_vkFreeMemory) p_vkFreeMemory(device, out->memory, NULL);
    out->memory = VK_NULL_HANDLE;
    if (out->image && p_vkDestroyImage) p_vkDestroyImage(device, out->image, NULL);
    out->image = VK_NULL_HANDLE;
    return 0;
}

#if defined(__ANDROID__)
/* Relay: a second Vulkan device (turnip, or the system driver) used when the game's
 * host device was created without the Android hardware-buffer extension. The game's
 * linear dma-buf render image is imported here and GPU-copied into an AHardwareBuffer
 * so the compositor gets the zero-copy path instead of a per-frame CPU upload. */
static void *relay_lib;
static VkInstance relay_instance;
static VkPhysicalDevice relay_phys;
static VkDevice relay_device;
static VkQueue relay_queue;
static uint32_t relay_queue_family;
static VkCommandPool relay_pool;
static int relay_state; /* 0 untried, 1 ready, -1 unavailable */
static int relay_can_export_fence;

static PFN_vkGetDeviceProcAddr r_vkGetDeviceProcAddr;
static PFN_vkCreateImage r_vkCreateImage;
static PFN_vkDestroyImage r_vkDestroyImage;
static PFN_vkGetImageMemoryRequirements r_vkGetImageMemoryRequirements;
static PFN_vkAllocateMemory r_vkAllocateMemory;
static PFN_vkFreeMemory r_vkFreeMemory;
static PFN_vkBindImageMemory r_vkBindImageMemory;
static PFN_vkCreateCommandPool r_vkCreateCommandPool;
static PFN_vkAllocateCommandBuffers r_vkAllocateCommandBuffers;
static PFN_vkFreeCommandBuffers r_vkFreeCommandBuffers;
static PFN_vkResetCommandBuffer r_vkResetCommandBuffer;
static PFN_vkBeginCommandBuffer r_vkBeginCommandBuffer;
static PFN_vkEndCommandBuffer r_vkEndCommandBuffer;
static PFN_vkCmdPipelineBarrier r_vkCmdPipelineBarrier;
static PFN_vkCmdCopyImage r_vkCmdCopyImage;
static PFN_vkQueueSubmit r_vkQueueSubmit;
static PFN_vkQueueWaitIdle r_vkQueueWaitIdle;
static PFN_vkCreateFence r_vkCreateFence;
static PFN_vkDestroyFence r_vkDestroyFence;
static PFN_vkGetFenceFdKHR r_vkGetFenceFdKHR;
static PFN_vkGetAndroidHardwareBufferPropertiesANDROID r_vkGetAndroidHardwareBufferPropertiesANDROID;
static PFN_vkGetMemoryFdPropertiesKHR r_vkGetMemoryFdPropertiesKHR;
static PFN_vkGetPhysicalDeviceMemoryProperties r_vkGetPhysicalDeviceMemoryProperties;

static int relay_find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags wanted)
{
    VkPhysicalDeviceMemoryProperties properties;
    r_vkGetPhysicalDeviceMemoryProperties(relay_phys, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags & wanted) == wanted)
            return (int)i;
    }
    return -1;
}

/* Minimal Android HAL vulkan module interface: adrenotools-packaged drivers export only
 * HMI (HAL_MODULE_INFO_SYM); the loader entry points come from opening device "vk0". */
struct gn_hw_module;
struct gn_hw_device;
struct gn_hw_methods {
    int (*open)(const struct gn_hw_module *, const char *, struct gn_hw_device **);
};
struct gn_hw_module {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char *id;
    const char *name;
    const char *author;
    struct gn_hw_methods *methods;
    void *dso;
    uint64_t reserved[24];
};
struct gn_hw_device {
    uint32_t tag;
    uint32_t version;
    struct gn_hw_module *module;
    uint64_t reserved[12];
    int (*close)(struct gn_hw_device *);
};
struct gn_hwvulkan_device {
    struct gn_hw_device common;
    PFN_vkEnumerateInstanceExtensionProperties enumerate_instance_extensions;
    PFN_vkCreateInstance create_instance;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
};

static PFN_vkCreateInstance relay_hal_create_instance;

static PFN_vkGetInstanceProcAddr relay_open_hal(void *lib, const char *path)
{
    struct gn_hw_module *module = (struct gn_hw_module *)dlsym(lib, "HMI");
    struct gn_hw_device *device_handle = NULL;
    if (!module || !module->methods || !module->methods->open) return NULL;
    if (module->methods->open(module, "vk0", &device_handle) != 0 || !device_handle) {
        char trace[640];
        snprintf(trace, sizeof(trace), "relay: HAL open failed for %s", path);
        log_line(trace);
        return NULL;
    }
    {
        struct gn_hwvulkan_device *vulkan_device =
            (struct gn_hwvulkan_device *)device_handle;
        if (!vulkan_device->get_instance_proc_addr) return NULL;
        relay_hal_create_instance = vulkan_device->create_instance;
        char trace[640];
        snprintf(trace, sizeof(trace), "relay: opened %s via HAL module", path);
        log_line(trace);
        return vulkan_device->get_instance_proc_addr;
    }
}

static int relay_init(void)
{
    if (relay_state) return relay_state > 0;
    relay_state = -1;

    /* The Wine rootfs shadows the bare sonames with a Linux-platform Mesa that has no
     * Android AHB support, so the useful candidates need absolute paths: the system
     * loader, and the adrenotools turnip the game itself renders with. */
    static char adrenotools_path[512];
    const char *candidates[5];
    int candidate_count = 0;
    candidates[candidate_count++] = "/system/lib64/libvulkan.so";
    {
        const char *dir = getenv("ADRENOTOOLS_DRIVER_PATH");
        const char *name = getenv("ADRENOTOOLS_DRIVER_NAME");
        if (dir && name && strlen(dir) + strlen(name) + 2 < sizeof(adrenotools_path)) {
            snprintf(adrenotools_path, sizeof(adrenotools_path), "%s%s", dir, name);
            candidates[candidate_count++] = adrenotools_path;
        } else {
            log_line("relay: adrenotools driver env not set in this process");
        }
    }
    candidates[candidate_count++] = "libvulkan_freedreno.so";
    candidates[candidate_count] = NULL;
    for (int i = 0; i < candidate_count; ++i) {
        char trace[640];
        snprintf(trace, sizeof(trace), "relay: candidate %d: %s", i, candidates[i]);
        log_line(trace);
    }
    PFN_vkGetInstanceProcAddr gipa = NULL;
    PFN_vkCreateInstance create_instance = NULL;
    int candidate = 0;
next_candidate:
    if (relay_instance) {
        PFN_vkDestroyInstance destroy_instance = gipa ?
            (PFN_vkDestroyInstance)gipa(relay_instance, "vkDestroyInstance") : NULL;
        if (destroy_instance) destroy_instance(relay_instance, NULL);
        relay_instance = VK_NULL_HANDLE;
    }
    if (relay_lib) { dlclose(relay_lib); relay_lib = NULL; }
    gipa = NULL;
    for (; candidates[candidate] && !gipa; ++candidate) {
        relay_lib = dlopen(candidates[candidate], RTLD_NOW | RTLD_LOCAL);
        if (!relay_lib) {
            char trace[640];
            snprintf(trace, sizeof(trace), "relay: dlopen(%s) failed: %s",
                     candidates[candidate], dlerror());
            log_line(trace);
            continue;
        }
        relay_hal_create_instance = NULL;
        gipa = (PFN_vkGetInstanceProcAddr)dlsym(relay_lib, "vk_icdGetInstanceProcAddr");
        if (!gipa) gipa = (PFN_vkGetInstanceProcAddr)dlsym(relay_lib, "vkGetInstanceProcAddr");
        if (!gipa) gipa = relay_open_hal(relay_lib, candidates[candidate]);
        if (!gipa) {
            char trace[640];
            snprintf(trace, sizeof(trace), "relay: %s has no usable entry point",
                     candidates[candidate]);
            log_line(trace);
            dlclose(relay_lib);
            relay_lib = NULL;
            continue;
        }
        {
            char trace[128];
            snprintf(trace, sizeof(trace), "relay: trying %s", candidates[candidate]);
            log_line(trace);
        }
    }
    if (!gipa) { log_line("relay: no usable Vulkan driver"); return 0; }

    create_instance = relay_hal_create_instance;
    if (!create_instance) create_instance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!create_instance) goto next_candidate;
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "gamenative-xr-relay",
        .apiVersion = VK_API_VERSION_1_1
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app
    };
    if (create_instance(&instance_info, NULL, &relay_instance) != VK_SUCCESS) {
        log_line("relay: vkCreateInstance failed");
        goto next_candidate;
    }

#define RELAY_ILOAD(name) PFN_##name i_##name = (PFN_##name)gipa(relay_instance, #name)
    RELAY_ILOAD(vkEnumeratePhysicalDevices);
    RELAY_ILOAD(vkGetPhysicalDeviceQueueFamilyProperties);
    RELAY_ILOAD(vkEnumerateDeviceExtensionProperties);
    RELAY_ILOAD(vkCreateDevice);
    RELAY_ILOAD(vkGetDeviceProcAddr);
    RELAY_ILOAD(vkGetPhysicalDeviceMemoryProperties);
#undef RELAY_ILOAD
    if (!i_vkEnumeratePhysicalDevices || !i_vkGetPhysicalDeviceQueueFamilyProperties ||
        !i_vkEnumerateDeviceExtensionProperties || !i_vkCreateDevice ||
        !i_vkGetDeviceProcAddr || !i_vkGetPhysicalDeviceMemoryProperties)
        goto next_candidate;
    r_vkGetPhysicalDeviceMemoryProperties = i_vkGetPhysicalDeviceMemoryProperties;

    uint32_t count = 1;
    if (i_vkEnumeratePhysicalDevices(relay_instance, &count, &relay_phys) < 0 || !count) {
        log_line("relay: no physical device");
        goto next_candidate;
    }

    VkQueueFamilyProperties families[8];
    uint32_t family_count = 8;
    i_vkGetPhysicalDeviceQueueFamilyProperties(relay_phys, &family_count, families);
    relay_queue_family = 0;
    for (uint32_t i = 0; i < family_count; ++i) {
        if (families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT)) {
            relay_queue_family = i;
            break;
        }
    }

    static const char *wanted[] = {
        "VK_ANDROID_external_memory_android_hardware_buffer",
        "VK_EXT_external_memory_dma_buf",
        "VK_KHR_external_memory_fd",
        "VK_EXT_queue_family_foreign",
        "VK_KHR_sampler_ycbcr_conversion",
        "VK_KHR_external_fence_fd",
        "VK_KHR_dedicated_allocation",
        "VK_KHR_get_memory_requirements2",
        "VK_KHR_bind_memory2",
        "VK_KHR_maintenance1",
        "VK_KHR_external_memory"
    };
    VkExtensionProperties supported[256];
    uint32_t supported_count = 256;
    if (i_vkEnumerateDeviceExtensionProperties(relay_phys, NULL, &supported_count, supported) < 0)
        goto next_candidate;
    const char *enabled[16];
    uint32_t enabled_count = 0;
    int have_ahb = 0, have_dmabuf = 0, have_fd = 0, have_foreign = 0;
    relay_can_export_fence = 0;
    for (uint32_t w = 0; w < sizeof(wanted) / sizeof(wanted[0]); ++w) {
        for (uint32_t i = 0; i < supported_count; ++i) {
            if (strcmp(supported[i].extensionName, wanted[w]) == 0) {
                enabled[enabled_count++] = wanted[w];
                if (w == 0) have_ahb = 1;
                if (w == 1) have_dmabuf = 1;
                if (w == 2) have_fd = 1;
                if (w == 3) have_foreign = 1;
                if (w == 5) relay_can_export_fence = 1;
                break;
            }
        }
    }
    if (!have_ahb || !have_dmabuf || !have_fd || !have_foreign) {
        char trace[128];
        snprintf(trace, sizeof(trace), "relay: missing extensions ahb=%d dmabuf=%d fd=%d foreign=%d",
                 have_ahb, have_dmabuf, have_fd, have_foreign);
        log_line(trace);
        goto next_candidate;
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = relay_queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = enabled_count,
        .ppEnabledExtensionNames = enabled
    };
    if (i_vkCreateDevice(relay_phys, &device_info, NULL, &relay_device) != VK_SUCCESS) {
        log_line("relay: vkCreateDevice failed");
        goto next_candidate;
    }
    r_vkGetDeviceProcAddr = i_vkGetDeviceProcAddr;

#define RELAY_DLOAD(name) \
    r_##name = (PFN_##name)r_vkGetDeviceProcAddr(relay_device, #name); \
    if (!r_##name) { log_line("relay: missing " #name); return 0; }
    RELAY_DLOAD(vkCreateImage);
    RELAY_DLOAD(vkDestroyImage);
    RELAY_DLOAD(vkGetImageMemoryRequirements);
    RELAY_DLOAD(vkAllocateMemory);
    RELAY_DLOAD(vkFreeMemory);
    RELAY_DLOAD(vkBindImageMemory);
    RELAY_DLOAD(vkCreateCommandPool);
    RELAY_DLOAD(vkAllocateCommandBuffers);
    RELAY_DLOAD(vkFreeCommandBuffers);
    RELAY_DLOAD(vkResetCommandBuffer);
    RELAY_DLOAD(vkBeginCommandBuffer);
    RELAY_DLOAD(vkEndCommandBuffer);
    RELAY_DLOAD(vkCmdPipelineBarrier);
    RELAY_DLOAD(vkCmdCopyImage);
    RELAY_DLOAD(vkQueueSubmit);
    RELAY_DLOAD(vkQueueWaitIdle);
    RELAY_DLOAD(vkCreateFence);
    RELAY_DLOAD(vkDestroyFence);
    RELAY_DLOAD(vkGetAndroidHardwareBufferPropertiesANDROID);
    RELAY_DLOAD(vkGetMemoryFdPropertiesKHR);
#undef RELAY_DLOAD
    r_vkGetFenceFdKHR = (PFN_vkGetFenceFdKHR)r_vkGetDeviceProcAddr(relay_device, "vkGetFenceFdKHR");
    if (!r_vkGetFenceFdKHR) relay_can_export_fence = 0;

    PFN_vkGetDeviceQueue get_queue =
        (PFN_vkGetDeviceQueue)r_vkGetDeviceProcAddr(relay_device, "vkGetDeviceQueue");
    if (!get_queue) return 0;
    get_queue(relay_device, relay_queue_family, 0, &relay_queue);

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = relay_queue_family
    };
    if (r_vkCreateCommandPool(relay_device, &pool_info, NULL, &relay_pool) != VK_SUCCESS) {
        log_line("relay: command pool failed");
        return 0;
    }
    relay_state = 1;
    log_line("relay: ready — GPU dma-buf to AHardwareBuffer copies enabled");
    return 1;
}

static int relay_create_source(struct gn_swapchain *swapchain, struct gn_image *image)
{
    if (image->relay_source_image) return 1;
    if (image->dma_buf_fd < 0) return 0;
    int fd = dup(image->dma_buf_fd);
    if (fd < 0) return 0;

    VkMemoryFdPropertiesKHR fd_properties = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR
    };
    if (r_vkGetMemoryFdPropertiesKHR(
            relay_device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
            fd, &fd_properties) != VK_SUCCESS) {
        log_line("relay: vkGetMemoryFdPropertiesKHR failed");
        close(fd);
        return 0;
    }

    VkExternalMemoryImageCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
    };
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = swapchain->format,
        .extent = {swapchain->width, swapchain->height, 1},
        .mipLevels = 1,
        .arrayLayers = swapchain->array_size ? swapchain->array_size : 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    if (r_vkCreateImage(relay_device, &image_info, NULL, &image->relay_source_image) != VK_SUCCESS) {
        log_line("relay: source image create failed");
        close(fd);
        return 0;
    }
    VkMemoryRequirements requirements;
    r_vkGetImageMemoryRequirements(relay_device, image->relay_source_image, &requirements);
    int memory_type = relay_find_memory_type(
        requirements.memoryTypeBits & fd_properties.memoryTypeBits, 0);
    if (memory_type < 0) {
        log_line("relay: no memory type for source");
        goto fail;
    }
    VkImportMemoryFdInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = fd
    };
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &import_info,
        .image = image->relay_source_image
    };
    VkMemoryAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated,
        .allocationSize = requirements.size,
        .memoryTypeIndex = (uint32_t)memory_type
    };
    if (r_vkAllocateMemory(relay_device, &allocate_info, NULL, &image->relay_source_memory) != VK_SUCCESS) {
        log_line("relay: source import failed");
        goto fail;
    }
    fd = -1; /* consumed by the import */
    if (r_vkBindImageMemory(relay_device, image->relay_source_image,
                            image->relay_source_memory, 0) != VK_SUCCESS) {
        log_line("relay: source bind failed");
        goto fail;
    }
    return 1;
fail:
    if (fd >= 0) close(fd);
    if (image->relay_source_memory) {
        r_vkFreeMemory(relay_device, image->relay_source_memory, NULL);
        image->relay_source_memory = VK_NULL_HANDLE;
    }
    if (image->relay_source_image) {
        r_vkDestroyImage(relay_device, image->relay_source_image, NULL);
        image->relay_source_image = VK_NULL_HANDLE;
    }
    return 0;
}

static int relay_create_transport(
    const struct gn_swapchain *swapchain, struct gn_transport_image *transport)
{
    AHardwareBuffer_Desc descriptor = {
        .width = swapchain->width,
        .height = swapchain->height,
        .layers = 1,
        .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT
    };
    AHardwareBuffer *buffer = NULL;
    if (AHardwareBuffer_allocate(&descriptor, &buffer) != 0 || !buffer) {
        log_line("relay: AHardwareBuffer allocation failed");
        return 0;
    }
    transport->hardware_buffer = buffer;
    transport->relay = 1;

    VkAndroidHardwareBufferPropertiesANDROID properties = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID
    };
    if (r_vkGetAndroidHardwareBufferPropertiesANDROID(relay_device, buffer, &properties) != VK_SUCCESS) {
        log_line("relay: AHB properties failed");
        return 0;
    }
    VkExternalMemoryImageCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID
    };
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {swapchain->width, swapchain->height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    if (r_vkCreateImage(relay_device, &image_info, NULL, &transport->image) != VK_SUCCESS) {
        log_line("relay: transport image failed");
        return 0;
    }
    VkMemoryRequirements requirements;
    r_vkGetImageMemoryRequirements(relay_device, transport->image, &requirements);
    int memory_type = relay_find_memory_type(
        requirements.memoryTypeBits & properties.memoryTypeBits, 0);
    if (memory_type < 0) {
        log_line("relay: no memory type for transport");
        return 0;
    }
    VkImportAndroidHardwareBufferInfoANDROID import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
        .buffer = buffer
    };
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &import_info,
        .image = transport->image
    };
    VkMemoryAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated,
        .allocationSize = properties.allocationSize,
        .memoryTypeIndex = (uint32_t)memory_type
    };
    if (r_vkAllocateMemory(relay_device, &allocate_info, NULL, &transport->memory) != VK_SUCCESS ||
        r_vkBindImageMemory(relay_device, transport->image, transport->memory, 0) != VK_SUCCESS) {
        log_line("relay: transport memory failed");
        return 0;
    }
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = relay_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    if (r_vkAllocateCommandBuffers(relay_device, &command_info, &transport->command_buffer) != VK_SUCCESS) {
        log_line("relay: command buffer failed");
        return 0;
    }
    return 1;
}

/* The Android side samples only the eye's sub-image, so copy that rectangle instead of the
 * whole image: with a side-by-side atlas each eye's transport otherwise received both eyes.
 * A one-texel border, clamped to the image, keeps bilinear sampling at the edge unchanged. */
static void eye_copy_region(const struct gn_swapchain *swapchain,
                            const struct gn_unix_submit_view_args *view,
                            VkOffset2D *offset, VkExtent2D *extent)
{
    int64_t x0 = 0, y0 = 0, x1 = swapchain->width, y1 = swapchain->height;
    if (view->rect_width && view->rect_height) {
        x0 = (int64_t)view->rect_x - 1;
        y0 = (int64_t)view->rect_y - 1;
        x1 = (int64_t)view->rect_x + view->rect_width + 1;
        y1 = (int64_t)view->rect_y + view->rect_height + 1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > swapchain->width) x1 = swapchain->width;
        if (y1 > swapchain->height) y1 = swapchain->height;
        if (x1 <= x0 || y1 <= y0) {
            x0 = 0;
            y0 = 0;
            x1 = swapchain->width;
            y1 = swapchain->height;
        }
    }
    offset->x = (int32_t)x0;
    offset->y = (int32_t)y0;
    extent->width = (uint32_t)(x1 - x0);
    extent->height = (uint32_t)(y1 - y0);
}

static int same_copy_region(const struct gn_transport_image *transport,
                            VkOffset2D offset, VkExtent2D extent)
{
    return transport->copy_offset.x == offset.x && transport->copy_offset.y == offset.y &&
           transport->copy_extent.width == extent.width &&
           transport->copy_extent.height == extent.height;
}

static int relay_record_copy(
    const struct gn_swapchain *swapchain, struct gn_image *image,
    struct gn_transport_image *transport, const struct gn_unix_submit_view_args *view)
{
    const uint32_t array_index = view->array_index;
    VkOffset2D offset;
    VkExtent2D extent;
    eye_copy_region(swapchain, view, &offset, &extent);
    if (transport->steady_recorded && same_copy_region(transport, offset, extent)) return 1;
    if (r_vkResetCommandBuffer(transport->command_buffer, 0) != VK_SUCCESS) return 0;
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
    };
    if (r_vkBeginCommandBuffer(transport->command_buffer, &begin) != VK_SUCCESS) return 0;

    VkImageMemoryBarrier before[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
            .dstQueueFamilyIndex = relay_queue_family,
            .image = image->relay_source_image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, array_index, 1}
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = transport->initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = transport->initialized ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = transport->initialized ? relay_queue_family : VK_QUEUE_FAMILY_IGNORED,
            .image = transport->image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
        }
    };
    r_vkCmdPipelineBarrier(
        transport->command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, before);

    VkImageCopy copy = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, array_index, 1},
        .srcOffset = {offset.x, offset.y, 0},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffset = {offset.x, offset.y, 0},
        .extent = {extent.width, extent.height, 1}
    };
    r_vkCmdCopyImage(
        transport->command_buffer, image->relay_source_image,
        VK_IMAGE_LAYOUT_GENERAL, transport->image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    VkImageMemoryBarrier after = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = relay_queue_family,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
        .image = transport->image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
    };
    r_vkCmdPipelineBarrier(
        transport->command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &after);
    VkImageMemoryBarrier source_release = before[0];
    source_release.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    source_release.dstAccessMask = 0;
    source_release.srcQueueFamilyIndex = relay_queue_family;
    source_release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    r_vkCmdPipelineBarrier(transport->command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &source_release);
    if (r_vkEndCommandBuffer(transport->command_buffer) != VK_SUCCESS) return 0;
    transport->copy_offset = offset;
    transport->copy_extent = extent;
    transport->steady_recorded = transport->initialized;
    return 1;
}

static int relay_submit(const VkCommandBuffer *commands, uint32_t command_count, int *submit_ok)
{
    *submit_ok = 0;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = command_count,
        .pCommandBuffers = commands
    };
    if (!relay_can_export_fence) {
        if (r_vkQueueSubmit(relay_queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) return -1;
        if (r_vkQueueWaitIdle(relay_queue) != VK_SUCCESS) return -1;
        *submit_ok = 1;
        return -1;
    }
    VkExportFenceCreateInfo export_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT
    };
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = &export_info
    };
    VkFence fence = VK_NULL_HANDLE;
    if (r_vkCreateFence(relay_device, &fence_info, NULL, &fence) != VK_SUCCESS) {
        if (r_vkQueueSubmit(relay_queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) return -1;
        if (r_vkQueueWaitIdle(relay_queue) != VK_SUCCESS) return -1;
        *submit_ok = 1;
        return -1;
    }
    if (r_vkQueueSubmit(relay_queue, 1, &submit, fence) != VK_SUCCESS) {
        r_vkDestroyFence(relay_device, fence, NULL);
        return -1;
    }
    int fd = -1;
    VkFenceGetFdInfoKHR fd_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR,
        .fence = fence,
        .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT
    };
    if (r_vkGetFenceFdKHR(relay_device, &fd_info, &fd) != VK_SUCCESS) fd = -1;
    r_vkDestroyFence(relay_device, fence, NULL);
    *submit_ok = 1;
    return fd;
}

static int relay_register(uint32_t slot, uint32_t image_index, uint32_t eye)
{
    struct gn_swapchain *swapchain = &swapchains[slot];
    struct gn_image *image = &swapchain->images[image_index];
    struct gn_transport_image *transport = &image->transport[eye];
    if (swapchain->sample_count != 1) return 0;
    if (!relay_init()) return 0;
    if (!relay_create_source(swapchain, image)) return 0;
    if (!transport->hardware_buffer && !relay_create_transport(swapchain, transport)) return 0;
    if (transport->registered) return 1;

    const uint32_t transport_index = slot * GN_UNIX_MAX_IMAGES + image_index;
    const int swap_red_blue =
        swapchain->format == VK_FORMAT_B8G8R8A8_UNORM ||
        swapchain->format == VK_FORMAT_B8G8R8A8_SRGB;
    char line[192], response[64] = {0};
    snprintf(line, sizeof(line),
             "BUFFER eye=%u index=%u w=%u h=%u swizzle=%u\n",
             eye, transport_index, swapchain->width, swapchain->height,
             swap_red_blue ? 1u : 0u);
    if (!transact_line(line, response, sizeof(response)) ||
        strncmp(response, "OK", 2) ||
        AHardwareBuffer_sendHandleToUnixSocket(
            (AHardwareBuffer *)transport->hardware_buffer, transport_fd) != 0 ||
        !read_line(transport_fd, response, sizeof(response)) ||
        strncmp(response, "OK", 2)) {
        log_line("relay: registration failed");
        close_transport();
        return 0;
    }
    transport->registered = 1;
    {
        char trace[192];
        snprintf(trace, sizeof(trace),
                 "relay registered eye=%u index=%u size=%ux%u swizzle=%u",
                 eye, transport_index, swapchain->width, swapchain->height,
                 swap_red_blue ? 1u : 0u);
        log_line(trace);
    }
    return 1;
}
#endif

static void destroy_transport_image(struct gn_transport_image *transport)
{
#if defined(__ANDROID__)
    if (transport->relay) {
        if (transport->command_buffer && relay_pool)
            r_vkFreeCommandBuffers(relay_device, relay_pool, 1, &transport->command_buffer);
        if (transport->image) r_vkDestroyImage(relay_device, transport->image, NULL);
        if (transport->memory) r_vkFreeMemory(relay_device, transport->memory, NULL);
        if (transport->hardware_buffer)
            AHardwareBuffer_release((AHardwareBuffer *)transport->hardware_buffer);
        memset(transport, 0, sizeof(*transport));
        return;
    }
#endif
    VkDevice owner = transport->owner ? transport->owner : device;
    if (transport->command_buffer && transport->command_pool && p_vkFreeCommandBuffers)
        p_vkFreeCommandBuffers(owner, transport->command_pool, 1, &transport->command_buffer);
    if (transport->image && p_vkDestroyImage)
        p_vkDestroyImage(owner, transport->image, NULL);
    if (transport->memory && p_vkFreeMemory)
        p_vkFreeMemory(owner, transport->memory, NULL);
#if defined(__ANDROID__)
    if (transport->hardware_buffer)
        AHardwareBuffer_release((AHardwareBuffer *)transport->hardware_buffer);
#endif
    memset(transport, 0, sizeof(*transport));
}

#if defined(__ANDROID__)
/* One line per step: a fault inside the driver leaves the last completed step in unix.log. */
static void ahb_step(const char *step, const struct gn_swapchain *swapchain,
                     uint64_t size, uint32_t bits)
{
    char line[160];
    snprintf(line, sizeof(line), "AHB transport %s %ux%u size=%llu bits=0x%x", step,
             swapchain->width, swapchain->height, (unsigned long long)size, bits);
    log_line(line);
}

static int create_ahardwarebuffer_transport(
    const struct gn_swapchain *swapchain, struct gn_transport_image *transport)
{
    if (!p_vkGetAndroidHardwareBufferPropertiesANDROID || !command_pool ||
        !p_vkAllocateCommandBuffers || !p_vkResetCommandBuffer ||
        !p_vkBeginCommandBuffer || !p_vkEndCommandBuffer ||
        !p_vkCmdPipelineBarrier || !p_vkCmdCopyImage || !p_vkQueueSubmit)
        return 0;

    AHardwareBuffer_Desc descriptor = {
        .width = swapchain->width,
        .height = swapchain->height,
        .layers = 1,
        .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT
    };
    AHardwareBuffer *buffer = NULL;
    if (AHardwareBuffer_allocate(&descriptor, &buffer) != 0 || !buffer) {
        log_line("AHardwareBuffer allocation unavailable; using dma-buf fallback");
        return 0;
    }
    transport->hardware_buffer = buffer;
    transport->owner = device;
    transport->command_pool = command_pool;
    ahb_step("allocated", swapchain, 0, 0);

    VkAndroidHardwareBufferFormatPropertiesANDROID format_properties = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID
    };
    VkAndroidHardwareBufferPropertiesANDROID properties = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
        .pNext = &format_properties
    };
    VkResult result = p_vkGetAndroidHardwareBufferPropertiesANDROID(
        device, buffer, &properties);
    if (result != VK_SUCCESS) {
        log_vk_result("vkGetAndroidHardwareBufferPropertiesANDROID", result);
        goto fail;
    }
    ahb_step("properties", swapchain, properties.allocationSize, properties.memoryTypeBits);
    if (format_properties.format != VK_FORMAT_R8G8B8A8_UNORM) {
        char trace[160];
        snprintf(trace, sizeof(trace),
                 "AHardwareBuffer returned unsupported Vulkan format=%d",
                 (int)format_properties.format);
        log_line(trace);
        goto fail;
    }

    VkExternalMemoryImageCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID
    };
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {swapchain->width, swapchain->height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    result = p_vkCreateImage(device, &image_info, NULL, &transport->image);
    if (result != VK_SUCCESS) {
        log_vk_result("vkCreateImage(AHardwareBuffer)", result);
        goto fail;
    }

    VkMemoryRequirements requirements;
    p_vkGetImageMemoryRequirements(device, transport->image, &requirements);
    ahb_step("image", swapchain, requirements.size, requirements.memoryTypeBits);
    const uint32_t compatible_types =
        requirements.memoryTypeBits & properties.memoryTypeBits;
    int memory_type = find_memory_type(compatible_types, 0);
    if (memory_type < 0) {
        log_line("No compatible memory type for AHardwareBuffer image");
        goto fail;
    }
    VkImportAndroidHardwareBufferInfoANDROID import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
        .buffer = buffer
    };
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &import_info,
        .image = transport->image
    };
    VkMemoryAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated,
        .allocationSize = properties.allocationSize,
        .memoryTypeIndex = (uint32_t)memory_type
    };
    ahb_step("import", swapchain, properties.allocationSize, (uint32_t)memory_type);
    result = p_vkAllocateMemory(
        device, &allocate_info, NULL, &transport->memory);
    if (result != VK_SUCCESS) {
        log_vk_result("vkAllocateMemory(AHardwareBuffer)", result);
        goto fail;
    }
    ahb_step("bind", swapchain, 0, 0);
    result = p_vkBindImageMemory(
        device, transport->image, transport->memory, 0);
    if (result != VK_SUCCESS) {
        log_vk_result("vkBindImageMemory(AHardwareBuffer)", result);
        goto fail;
    }

    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    result = p_vkAllocateCommandBuffers(
        device, &command_info, &transport->command_buffer);
    if (result != VK_SUCCESS) {
        log_vk_result("vkAllocateCommandBuffers(AHardwareBuffer)", result);
        goto fail;
    }
    log_line("AHardwareBuffer Vulkan transport image created");
    return 1;

fail:
    destroy_transport_image(transport);
    return 0;
}

static int record_ahardwarebuffer_copy(
    const struct gn_swapchain *swapchain, const struct gn_image *source,
    struct gn_transport_image *transport, const struct gn_unix_submit_view_args *view)
{
    const uint32_t array_index = view->array_index;
    VkOffset2D offset;
    VkExtent2D extent;
    eye_copy_region(swapchain, view, &offset, &extent);
    if (transport->initialized && transport->steady_recorded &&
        same_copy_region(transport, offset, extent)) return 1;

    VkResult result = p_vkResetCommandBuffer(transport->command_buffer, 0);
    if (result != VK_SUCCESS) {
        log_vk_result("vkResetCommandBuffer(AHardwareBuffer)", result);
        return 0;
    }
    /* Recorded once and resubmitted every frame once steady, so not ONE_TIME_SUBMIT:
     * Turnip only restores its submit-time patch points for reusable command buffers. */
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
    };
    result = p_vkBeginCommandBuffer(transport->command_buffer, &begin);
    if (result != VK_SUCCESS) {
        log_vk_result("vkBeginCommandBuffer(AHardwareBuffer)", result);
        return 0;
    }

    VkImageMemoryBarrier before[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = source->image,
            .subresourceRange = {
                VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, array_index, 1
            }
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = transport->initialized ? VK_ACCESS_MEMORY_READ_BIT : 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = transport->initialized ?
                VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = transport->initialized ?
                VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = transport->initialized ?
                queue_family_index : VK_QUEUE_FAMILY_IGNORED,
            .image = transport->image,
            .subresourceRange = {
                VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1
            }
        }
    };
    p_vkCmdPipelineBarrier(
        transport->command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, before);

    VkImageCopy copy = {
        .srcSubresource = {
            VK_IMAGE_ASPECT_COLOR_BIT, 0, array_index, 1
        },
        .srcOffset = {offset.x, offset.y, 0},
        .dstSubresource = {
            VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1
        },
        .dstOffset = {offset.x, offset.y, 0},
        .extent = {extent.width, extent.height, 1}
    };
    p_vkCmdCopyImage(
        transport->command_buffer, source->image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, transport->image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    VkImageMemoryBarrier after[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT |
                             VK_ACCESS_MEMORY_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = source->image,
            .subresourceRange = {
                VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, array_index, 1
            }
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = queue_family_index,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
            .image = transport->image,
            .subresourceRange = {
                VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1
            }
        }
    };
    p_vkCmdPipelineBarrier(
        transport->command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 2, after);
    result = p_vkEndCommandBuffer(transport->command_buffer);
    if (result != VK_SUCCESS) {
        log_vk_result("vkEndCommandBuffer(AHardwareBuffer)", result);
        return 0;
    }
    transport->copy_offset = offset;
    transport->copy_extent = extent;
    if (transport->initialized) transport->steady_recorded = 1;
    return 1;
}
#endif


static void destroy_image(struct gn_image *image)
{
    for (uint32_t eye = 0; eye < 2; ++eye)
        destroy_transport_image(&image->transport[eye]);
#if defined(__ANDROID__)
    if (image->relay_source_image)
        r_vkDestroyImage(relay_device, image->relay_source_image, NULL);
    if (image->relay_source_memory)
        r_vkFreeMemory(relay_device, image->relay_source_memory, NULL);
#endif
    if (image->dma_buf_fd >= 0) close(image->dma_buf_fd);
    if (image->image && p_vkDestroyImage) p_vkDestroyImage(device, image->image, NULL);
    if (image->memory && p_vkFreeMemory) p_vkFreeMemory(device, image->memory, NULL);
    memset(image, 0, sizeof(*image));
    image->dma_buf_fd = -1;
}

static int register_ahardwarebuffer(
    uint32_t slot, uint32_t image_index, uint32_t eye)
{
#if defined(__ANDROID__)
    struct gn_swapchain *swapchain = &swapchains[slot];
    struct gn_image *image = &swapchain->images[image_index];
    struct gn_transport_image *transport = &image->transport[eye];
    if (swapchain->sample_count != 1) return 0;
    if (!transport->hardware_buffer &&
        !create_ahardwarebuffer_transport(swapchain, transport)) return 0;
    if (transport->registered) return 1;

    const uint32_t transport_index = slot * GN_UNIX_MAX_IMAGES + image_index;
    const int swap_red_blue =
        swapchain->format == VK_FORMAT_B8G8R8A8_UNORM ||
        swapchain->format == VK_FORMAT_B8G8R8A8_SRGB;
    char line[192], response[64] = {0};
    snprintf(line, sizeof(line),
             "BUFFER eye=%u index=%u w=%u h=%u swizzle=%u\n",
             eye, transport_index, swapchain->width, swapchain->height,
             swap_red_blue ? 1u : 0u);
    if (!transact_line(line, response, sizeof(response)) ||
        strncmp(response, "OK", 2) ||
        AHardwareBuffer_sendHandleToUnixSocket(
            (AHardwareBuffer *)transport->hardware_buffer,
            transport_fd) != 0 ||
        !read_line(transport_fd, response, sizeof(response)) ||
        strncmp(response, "OK", 2)) {
        log_line("AHardwareBuffer registration failed; using dma-buf fallback");
        close_transport();
        return 0;
    }
    transport->registered = 1;
    {
        char trace[192];
        snprintf(trace, sizeof(trace),
                 "AHardwareBuffer registered eye=%u index=%u size=%ux%u swizzle=%u",
                 eye, transport_index, swapchain->width, swapchain->height,
                 swap_red_blue ? 1u : 0u);
        log_line(trace);
    }
    return 1;
#else
    (void)slot;
    (void)image_index;
    (void)eye;
    return 0;
#endif
}

static int register_image(uint32_t slot, uint32_t image_index, uint32_t eye,
                          uint32_t array_index)
{
    struct gn_swapchain *swapchain = &swapchains[slot];
    struct gn_image *image = &swapchain->images[image_index];
    const uint8_t bit = (uint8_t)(1u << eye);
    if (array_index >= swapchain->array_size) return 0;
    if ((image->registered_eye_mask & bit) &&
        image->registered_array_index[eye] == array_index) return 1;

    if (image->transport_kind[eye] == GN_TRANSPORT_UNKNOWN) {
        if (register_ahardwarebuffer(slot, image_index, eye)) {
            image->transport_kind[eye] = GN_TRANSPORT_AHARDWAREBUFFER;
            image->registered_eye_mask |= bit;
            image->registered_array_index[eye] = array_index;
            return 1;
        }
        destroy_transport_image(&image->transport[eye]);
#if defined(__ANDROID__)
        if (relay_register(slot, image_index, eye)) {
            image->transport_kind[eye] = GN_TRANSPORT_RELAY;
            image->registered_eye_mask |= bit;
            image->registered_array_index[eye] = array_index;
            return 1;
        }
        destroy_transport_image(&image->transport[eye]);
#endif
        image->transport_kind[eye] = GN_TRANSPORT_DMABUF;
    }
    if (image->transport_kind[eye] == GN_TRANSPORT_RELAY) {
        if (!image->transport[eye].registered && !relay_register(slot, image_index, eye)) return 0;
        image->registered_eye_mask |= bit;
        if (image->registered_array_index[eye] != array_index) {
            image->transport[eye].steady_recorded = 0;
        }
        image->registered_array_index[eye] = array_index;
        return 1;
    }
    if (image->transport_kind[eye] == GN_TRANSPORT_AHARDWAREBUFFER) {
        if (image->registered_array_index[eye] != array_index) {
            image->transport[eye].steady_recorded = 0;
        }
        image->registered_array_index[eye] = array_index;
        return 1;
    }

    if (image->dma_buf_fd < 0) {
        log_line("optimal render image has no dma-buf; AHardwareBuffer transport is required");
        return 0;
    }

    VkSubresourceLayout layouts[4];
    for (uint32_t plane = 0; plane < image->plane_count; ++plane) {
        VkImageSubresource subresource = {
            .aspectMask = image->plane_count == 1
                ? VK_IMAGE_ASPECT_COLOR_BIT
                : (VkImageAspectFlags)
                    (VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << plane),
            .mipLevel = 0,
            .arrayLayer = array_index
        };
        p_vkGetImageSubresourceLayout(
            device, image->image, &subresource, &layouts[plane]);
    }

    char line[512], response[64] = {0};
    const uint32_t fourcc = format_to_fourcc(swapchain->format);
    if (!fourcc) return 0;
    const uint32_t transport_index = slot * GN_UNIX_MAX_IMAGES + image_index;
    int length = snprintf(
        line, sizeof(line),
        "DMABUF eye=%u index=%u w=%u h=%u planes=%u fourcc=0x%08x "
        "modifier=0x%llx",
        eye, transport_index, swapchain->width, swapchain->height,
        image->plane_count, fourcc, (unsigned long long)image->modifier);
    for (uint32_t plane = 0;
         plane < image->plane_count && length > 0 &&
         (size_t)length < sizeof(line); ++plane) {
        length += snprintf(
            line + length, sizeof(line) - (size_t)length,
            " stride%u=%u offset%u=%u",
            plane, (uint32_t)layouts[plane].rowPitch,
            plane, (uint32_t)layouts[plane].offset);
    }
    if (length <= 0 || (size_t)length + 2 > sizeof(line)) return 0;
    line[length++] = '\n';
    line[length] = 0;
    int sent_planes = 1;
    if (!transact_line(line, response, sizeof(response)) ||
        strncmp(response, "OK", 2)) {
        char trace[192];
        snprintf(trace, sizeof(trace),
                 "dma-buf registration rejected eye=%u index=%u response=%s",
                 eye, transport_index, response[0] ? response : "<none>");
        log_line(trace);
        sent_planes = 0;
    }
    for (uint32_t plane = 0;
         sent_planes && plane < image->plane_count; ++plane) {
        if (!send_fd(transport_fd, image->dma_buf_fd)) sent_planes = 0;
    }
    if (!sent_planes ||
        !read_line(transport_fd, response, sizeof(response)) || strncmp(response, "OK", 2)) {
        close_transport();
        return 0;
    }
    image->registered_eye_mask |= bit;
    image->registered_array_index[eye] = array_index;
    {
        char trace[320];
        snprintf(trace, sizeof(trace),
                 "dma-buf registered eye=%u index=%u array=%u size=%ux%u "
                 "fourcc=0x%08x modifier=0x%llx planes=%u stride0=%u offset0=%u",
                 eye, transport_index, array_index, swapchain->width,
                 swapchain->height, fourcc,
                 (unsigned long long)image->modifier, image->plane_count,
                 (uint32_t)layouts[0].rowPitch, (uint32_t)layouts[0].offset);
        log_line(trace);
    }
    return 1;
}

static int32_t unix_init(void *opaque)
{
    struct gn_unix_init_args *args = opaque;
    args->result = args->abi_version == GN_UNIX_ABI_VERSION ?
        GN_UNIX_SUCCESS : GN_UNIX_ERROR_ARGUMENT;
    log_line("unixlib initialized");
    return 0;
}

static int32_t unix_set_vulkan_context(void *opaque)
{
    struct gn_unix_vulkan_context_args *args = opaque;
    char trace[256];
    pthread_mutex_lock(&state_mutex);
    args->diagnostic_flags = 0;
    snprintf(trace, sizeof(trace),
             "Vulkan context request source=%s phys=0x%llx device=0x%llx queue=0x%llx",
             args->handles_are_host ? "host" : "wine-client",
             (unsigned long long)args->client_physical_device,
             (unsigned long long)args->client_device,
             (unsigned long long)args->client_queue);
    log_line(trace);
    if (args->handles_are_host) {
        physical_device = (VkPhysicalDevice)(uintptr_t)args->client_physical_device;
        device = (VkDevice)(uintptr_t)args->client_device;
        queue = (VkQueue)(uintptr_t)args->client_queue;
    } else {
        physical_device = (VkPhysicalDevice)(uintptr_t)unwrap_dispatchable(args->client_physical_device);
        device = (VkDevice)(uintptr_t)unwrap_dispatchable(args->client_device);
        queue = (VkQueue)(uintptr_t)unwrap_dispatchable(args->client_queue);
    }
    queue_family_index = args->queue_family_index;
    if (readable_mapping_contains((uintptr_t)physical_device, sizeof(void *)))
        args->diagnostic_flags |= GN_UNIX_VK_DIAG_PHYSICAL_DEVICE;
    if (readable_mapping_contains((uintptr_t)device, sizeof(void *)))
        args->diagnostic_flags |= GN_UNIX_VK_DIAG_DEVICE;
    if (readable_mapping_contains((uintptr_t)queue, sizeof(void *)))
        args->diagnostic_flags |= GN_UNIX_VK_DIAG_QUEUE;
    if ((args->diagnostic_flags &
         (GN_UNIX_VK_DIAG_PHYSICAL_DEVICE | GN_UNIX_VK_DIAG_DEVICE |
          GN_UNIX_VK_DIAG_QUEUE)) !=
        (GN_UNIX_VK_DIAG_PHYSICAL_DEVICE | GN_UNIX_VK_DIAG_DEVICE |
         GN_UNIX_VK_DIAG_QUEUE)) {
        log_line("Vulkan context rejected: resolved host handles are not readable");
        physical_device = VK_NULL_HANDLE;
        device = VK_NULL_HANDLE;
        queue = VK_NULL_HANDLE;
        args->result = GN_UNIX_ERROR_UNAVAILABLE;
        pthread_mutex_unlock(&state_mutex);
        return 0;
    }
    load_vulkan_functions();
#if defined(__ANDROID__)
    if (command_pool && command_pool_device != device) {
        /* The game recreated its device (for example for a new OpenXR session). The
         * old device may already be destroyed, so its pool is neither used nor
         * destroyed here; transports keep their own owner for cleanup. */
        log_line("Vulkan device changed; creating a new AHardwareBuffer command pool");
        command_pool = VK_NULL_HANDLE;
        command_pool_device = VK_NULL_HANDLE;
    }
    if (p_vkGetAndroidHardwareBufferPropertiesANDROID &&
        p_vkCreateCommandPool && !command_pool) {
        VkCommandPoolCreateInfo pool_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = queue_family_index
        };
        VkResult pool_result = p_vkCreateCommandPool(
            device, &pool_info, NULL, &command_pool);
        if (pool_result != VK_SUCCESS) {
            command_pool = VK_NULL_HANDLE;
            log_vk_result("vkCreateCommandPool(AHardwareBuffer)", pool_result);
        } else {
            command_pool_device = device;
        }
    }
#endif
    if (vulkan_so) args->diagnostic_flags |= GN_UNIX_VK_DIAG_VULKAN_LIBRARY;
    if (p_vkGetDeviceProcAddr)
        args->diagnostic_flags |= GN_UNIX_VK_DIAG_GET_DEVICE_PROC_ADDR;
    if (p_vkCreateImage) args->diagnostic_flags |= GN_UNIX_VK_DIAG_CREATE_IMAGE;
    if (p_vkGetMemoryFdKHR)
        args->diagnostic_flags |= GN_UNIX_VK_DIAG_GET_MEMORY_FD;
    log_vulkan_context_state();
    args->result = physical_device && device && queue && p_vkCreateImage &&
                   p_vkGetMemoryFdKHR ? GN_UNIX_SUCCESS : GN_UNIX_ERROR_UNAVAILABLE;
    pthread_mutex_unlock(&state_mutex);
    log_line(args->result ? "Vulkan context unavailable" : "Vulkan context ready");
    return 0;
}

static int32_t unix_create_swapchain(void *opaque)
{
    struct gn_unix_create_swapchain_args *args = opaque;
    if (args->slot >= GN_UNIX_MAX_SWAPCHAINS || !args->width || !args->height ||
        !device || !format_to_fourcc((VkFormat)args->format)) {
        args->result = GN_UNIX_ERROR_ARGUMENT;
        return 0;
    }
    pthread_mutex_lock(&state_mutex);
    struct gn_swapchain *swapchain = &swapchains[args->slot];
    if (swapchain->allocated) {
        args->result = GN_UNIX_ERROR_ARGUMENT;
        pthread_mutex_unlock(&state_mutex);
        return 0;
    }
    memset(swapchain, 0, sizeof(*swapchain));
    swapchain->width = args->width;
    swapchain->height = args->height;
    swapchain->format = (VkFormat)args->format;
    swapchain->array_size = args->array_size ? args->array_size : 1;
    swapchain->sample_count = args->sample_count ? args->sample_count : 1;


    swapchain->image_count = 4;
    int optimal = 0;
#if defined(__ANDROID__)
    optimal = swapchain->sample_count == 1 && ahb_transport_probe();
#endif
    for (uint32_t i = 0; i < swapchain->image_count; ++i) {
        swapchain->images[i].dma_buf_fd = -1;
        if (!create_image(swapchain, &swapchain->images[i], args->array_size,
                          args->mip_count, args->sample_count, optimal)) {
            for (uint32_t j = 0; j <= i; ++j) destroy_image(&swapchain->images[j]);
            memset(swapchain, 0, sizeof(*swapchain));
            args->result = GN_UNIX_ERROR_VULKAN;
            pthread_mutex_unlock(&state_mutex);
            return 0;
        }
        args->images[i] = (gn_u64)(uintptr_t)swapchain->images[i].image;
    }
    swapchain->allocated = 1;
    args->image_count = swapchain->image_count;
    args->result = GN_UNIX_SUCCESS;
    pthread_mutex_unlock(&state_mutex);
    log_line("swapchain created");
    return 0;
}

static int32_t unix_destroy_swapchain(void *opaque)
{
    struct gn_unix_destroy_swapchain_args *args = opaque;
    if (args->slot >= GN_UNIX_MAX_SWAPCHAINS) {
        args->result = GN_UNIX_ERROR_ARGUMENT;
        return 0;
    }
    submit_worker_flush();
    pthread_mutex_lock(&state_mutex);
    struct gn_swapchain *swapchain = &swapchains[args->slot];
    for (uint32_t i = 0; i < swapchain->image_count; ++i)
        destroy_image(&swapchain->images[i]);
    memset(swapchain, 0, sizeof(*swapchain));
    args->result = GN_UNIX_SUCCESS;
    pthread_mutex_unlock(&state_mutex);
    return 0;
}

/* The app-side release wait: one ACQUIRE round trip per registered eye. */
static int32_t acquire_image_from_app(const struct gn_unix_acquire_image_args *args)
{
    pthread_mutex_lock(&socket_mutex);
    char line[512], response[64] = {0};
    struct gn_image *image = &swapchains[args->slot].images[args->image_index];
    int timeout_ms;
    if (args->timeout_ns < 0 || args->timeout_ns > 2147483647000000LL)
        timeout_ms = -1;
    else
        timeout_ms = (int)((args->timeout_ns + 999999) / 1000000);
    for (uint32_t eye = 0; eye < 2; ++eye) {
        if (!(image->registered_eye_mask & (1u << eye))) continue;
        const uint32_t transport_index =
            args->slot * GN_UNIX_MAX_IMAGES + args->image_index;
        snprintf(line, sizeof(line), "ACQUIRE eye=%u index=%u timeout=%d\n",
                 eye, transport_index, timeout_ms);
        if (!transact_line(line, response, sizeof(response))) {
            pthread_mutex_unlock(&socket_mutex);
            return GN_UNIX_ERROR_TRANSPORT;
        }
        if (!strcmp(response, "ERR timeout")) {
            pthread_mutex_unlock(&socket_mutex);
            return GN_UNIX_ERROR_TIMEOUT;
        }
        if (strncmp(response, "OK", 2)) {
            pthread_mutex_unlock(&socket_mutex);
            return GN_UNIX_ERROR_TRANSPORT;
        }
        if (strstr(response, "fence=1")) {
            int fd = recv_fd(transport_fd);
            if (fd < 0) {
                close_transport();
                pthread_mutex_unlock(&socket_mutex);
                return GN_UNIX_ERROR_TRANSPORT;
            }
            struct pollfd pfd = {fd, POLLIN, 0};
            int waited;
            do waited = poll(&pfd, 1, timeout_ms); while (waited < 0 && errno == EINTR);
            close(fd);
            if (waited <= 0) {
                pthread_mutex_unlock(&socket_mutex);
                return waited == 0 ? GN_UNIX_ERROR_TIMEOUT : GN_UNIX_ERROR_TRANSPORT;
            }
        }
    }
    image->submitted = 0;
    pthread_mutex_unlock(&socket_mutex);
    return GN_UNIX_SUCCESS;
}

static int32_t unix_acquire_image(void *opaque)
{
    struct gn_unix_acquire_image_args *args = opaque;
    if (args->slot >= GN_UNIX_MAX_SWAPCHAINS ||
        args->image_index >= GN_UNIX_MAX_IMAGES) {
        args->result = GN_UNIX_ERROR_ARGUMENT;
        return 0;
    }
    const int64_t wait_begin = monotonic_ns();
    pthread_mutex_lock(&submit_mutex);
    while (image_copy_busy[args->slot][args->image_index])
        pthread_cond_wait(&submit_space_cond, &submit_mutex);
    pthread_mutex_unlock(&submit_mutex);
    args->result = acquire_image_from_app(args);
    pthread_mutex_lock(&timing_mutex);
    swapchain_wait_timing[args->slot][0] = wait_begin;
    swapchain_wait_timing[args->slot][1] = monotonic_ns();
    swapchain_wait_unclaimed[args->slot] = 1;
    pthread_mutex_unlock(&timing_mutex);
    return 0;
}

/* " snap=<serial> tb=<submit begin> t=<GN_T_COUNT values>": each value is relative to tb,
 * "_" when not measured. Returns the number of characters needed, like snprintf. */
static int format_frame_timing(char *out, size_t size, const struct gn_frame_timing *timing)
{
    int length = snprintf(out, size, " snap=%lld tb=%lld t=",
                          (long long)timing->snap, (long long)timing->submit_begin);
    for (int i = 0; i < GN_T_COUNT && length >= 0; ++i) {
        const size_t used = (size_t)length < size ? (size_t)length : size;
        const int64_t value = timing->t[i];
        const int added = value ?
            snprintf(out + used, size - used, "%s%lld", i ? "," : "",
                     (long long)(value - timing->submit_begin)) :
            snprintf(out + used, size - used, "%s_", i ? "," : "");
        length = added < 0 ? -1 : length + added;
    }
    return length;
}

static int send_frame(const struct gn_unix_submit_view_args *view, int fence_fd,
                      uint64_t frame_id, const struct gn_frame_timing *timing)
{
    char line[1024], response[64] = {0};
    const uint32_t transport_index =
        view->slot * GN_UNIX_MAX_IMAGES + view->image_index;
    int length = snprintf(line, sizeof(line),
             "FRAME frame=%llu eye=%u index=%u fence=%u x=%d y=%d w=%u h=%u flip=%u "
             "projection=1 qx=%lld qy=%lld qz=%lld qw=%lld "
             "px=%lld py=%lld pz=%lld fl=%lld fr=%lld fu=%lld fd=%lld",
             (unsigned long long)frame_id, view->eye, transport_index,
             fence_fd >= 0 ? 1u : 0u,
             view->rect_x, view->rect_y, view->rect_width, view->rect_height,
             view->flip_y ? 1u : 0u,
             (long long)view->orientation_micro[0],
             (long long)view->orientation_micro[1],
             (long long)view->orientation_micro[2],
             (long long)view->orientation_micro[3],
             (long long)view->position_micro[0],
             (long long)view->position_micro[1],
             (long long)view->position_micro[2],
             (long long)view->fov_micro[0],
             (long long)view->fov_micro[1],
             (long long)view->fov_micro[2],
             (long long)view->fov_micro[3]);
    if (timing && length >= 0 && (size_t)length < sizeof(line)) {
        const int added = format_frame_timing(line + length, sizeof(line) - (size_t)length, timing);
        length = added < 0 ? -1 : length + added;
    }
    /* The app reads at most 1023 characters per line. */
    if (length < 0 || (size_t)length + 1 >= sizeof(line)) {
        log_line("FRAME line too long");
        return 0;
    }
    line[length] = '\n';
    line[length + 1] = 0;
    int ok = 0;
    if (fence_fd >= 0) {
        ok = transact_line(line, response, sizeof(response)) &&
             !strncmp(response, "OK", 2) &&
             send_fd(transport_fd, fence_fd) &&
             read_line(transport_fd, response, sizeof(response)) &&
             !strncmp(response, "OK", 2);
    } else {
        ok = transact_line(line, response, sizeof(response)) &&
             !strncmp(response, "OK", 2);
    }
    if (!ok) {
        char trace[192];
        snprintf(trace, sizeof(trace),
                 "frame transport failed eye=%u index=%u response=%s",
                 view->eye, transport_index, response[0] ? response : "<none>");
        log_line(trace);
        close_transport();
    } else if (!transport_frame_announced[view->eye]) {
        char trace[160];
        snprintf(trace, sizeof(trace),
                 "first transported frame eye=%u index=%u fence=%u",
                 view->eye, transport_index, fence_fd >= 0 ? 1u : 0u);
        log_line(trace);
        transport_frame_announced[view->eye] = 1;
    }
    swapchains[view->slot].images[view->image_index].submitted = ok ? 1 : 0;
    return ok;
}


/* Asynchronous frame shipper: xrEndFrame used to drain the game's GPU queue synchronously
 * before transporting the frame, serializing the game's CPU and GPU completely. Instead,
 * xrEndFrame submits the frame's game-queue work (the AHardwareBuffer copies, or an empty
 * marker) with a fence and returns; this worker waits for that fence, runs the transport
 * (relay copy + socket send), and the game overlaps its next frame's CPU work with the GPU.
 *
 * Only xrEndFrame touches the game's VkQueue. The PE runtime holds DXVK's (or vkd3d's)
 * submission lock around that call, so our submits never race the game's own submit
 * thread; the worker only waits on fences. */
struct gn_pending_submit {
    struct gn_unix_submit_view_args views[2];
    uint32_t view_count;
    VkFence fence;
    uint64_t frame_id;
    struct gn_frame_timing timing;
    int used;
};

static struct gn_pending_submit submit_ring[4];
static uint32_t submit_ring_head, submit_ring_tail;
static pthread_cond_t submit_cond = PTHREAD_COND_INITIALIZER;
static pthread_t submit_thread;
static int submit_thread_running;
static int submit_worker_busy;

static int ship_views(const struct gn_unix_submit_view_args *views, uint32_t view_count,
                      uint64_t frame_id, struct gn_frame_timing *timing);

static void *submit_worker(void *unused)
{
    (void)unused;
    for (;;) {
        struct gn_pending_submit item;
        pthread_mutex_lock(&submit_mutex);
        while (submit_thread_running && !submit_ring[submit_ring_tail].used)
            pthread_cond_wait(&submit_cond, &submit_mutex);
        if (!submit_thread_running) {
            pthread_mutex_unlock(&submit_mutex);
            return NULL;
        }
        item = submit_ring[submit_ring_tail];
        submit_ring[submit_ring_tail].used = 0;
        submit_ring_tail = (submit_ring_tail + 1) % 4;
        submit_worker_busy = 1;
        pthread_cond_signal(&submit_space_cond);
        pthread_mutex_unlock(&submit_mutex);

        item.timing.t[GN_T_FENCE_BEGIN] = monotonic_ns();
        if (item.fence != VK_NULL_HANDLE) {
            p_vkWaitForFences(device, 1, &item.fence, VK_TRUE, UINT64_MAX);
            p_vkDestroyFence(device, item.fence, NULL);
        }
        item.timing.t[GN_T_FENCE_END] = monotonic_ns();
        pthread_mutex_lock(&timing_mutex);
        fence_end_frame[item.frame_id % 8] = item.frame_id;
        fence_end_time[item.frame_id % 8] = item.timing.t[GN_T_FENCE_END];
        pthread_mutex_unlock(&timing_mutex);
        if (!ship_views(item.views, item.view_count, item.frame_id, &item.timing)) {
            static int failure_logged;
            if (!failure_logged) {
                failure_logged = 1;
                log_line("async transport failed; later frames may recover");
            }
        }
        /* The relay copy reads the source image on its own queue; the game must not
         * re-render into that image until the copy has actually executed. */
        if (relay_state > 0 && r_vkQueueWaitIdle) r_vkQueueWaitIdle(relay_queue);
        pthread_mutex_lock(&submit_mutex);
        for (uint32_t i = 0; i < item.view_count; ++i) {
            if (item.views[i].slot < GN_UNIX_MAX_SWAPCHAINS &&
                item.views[i].image_index < GN_UNIX_MAX_IMAGES &&
                image_copy_busy[item.views[i].slot][item.views[i].image_index])
                --image_copy_busy[item.views[i].slot][item.views[i].image_index];
        }
        submit_worker_busy = 0;
        pthread_cond_broadcast(&submit_space_cond);
        pthread_mutex_unlock(&submit_mutex);
    }
}

static void submit_worker_flush(void)
{
    pthread_mutex_lock(&submit_mutex);
    while (submit_thread_running &&
           (submit_ring[submit_ring_tail].used || submit_worker_busy))
        pthread_cond_wait(&submit_space_cond, &submit_mutex);
    pthread_mutex_unlock(&submit_mutex);
}

static int submit_game_queue_work(const struct gn_unix_submit_view_args *views,
                                  uint32_t view_count, VkFence *out_fence);
static int submit_views_sync(const struct gn_unix_submit_view_args *views, uint32_t view_count,
                             struct gn_frame_timing *timing);

static int submit_views_async(const struct gn_unix_submit_view_args *views, uint32_t view_count,
                              struct gn_frame_timing *timing)
{
    if (!p_vkWaitForFences || !p_vkCreateFence || !p_vkDestroyFence || !p_vkQueueSubmit)
        return submit_views_sync(views, view_count, timing);

    pthread_mutex_lock(&submit_mutex);
    if (!submit_thread_running) {
        submit_thread_running = 1;
        if (pthread_create(&submit_thread, NULL, submit_worker, NULL) != 0) {
            submit_thread_running = 0;
            pthread_mutex_unlock(&submit_mutex);
            return submit_views_sync(views, view_count, timing);
        }
        log_line("async frame shipper started");
    }
    pthread_mutex_unlock(&submit_mutex);

    VkFence fence = VK_NULL_HANDLE;
    if (!submit_game_queue_work(views, view_count, &fence)) return 0;

    pthread_mutex_lock(&submit_mutex);
    while (submit_ring[submit_ring_head].used)
        pthread_cond_wait(&submit_space_cond, &submit_mutex);
    struct gn_pending_submit *slot = &submit_ring[submit_ring_head];
    for (uint32_t i = 0; i < view_count; ++i) {
        slot->views[i] = views[i];
        if (views[i].slot < GN_UNIX_MAX_SWAPCHAINS &&
            views[i].image_index < GN_UNIX_MAX_IMAGES)
            ++image_copy_busy[views[i].slot][views[i].image_index];
    }
    slot->view_count = view_count;
    slot->fence = fence;
    slot->frame_id = __atomic_add_fetch(&transport_frame_id, 1, __ATOMIC_RELAXED);
    const uint64_t frame_id = slot->frame_id;
    /* Submit end includes any wait for ring space: that is what the game thread pays. */
    timing->t[GN_T_SUBMIT_END] = monotonic_ns();
    slot->timing = *timing;
    slot->used = 1;
    submit_ring_head = (submit_ring_head + 1) % 4;
    pthread_cond_signal(&submit_cond);
    pthread_mutex_unlock(&submit_mutex);

    pthread_mutex_lock(&timing_mutex);
    last_submit_frame = frame_id;
    last_submit_wait_end = timing->t[GN_T_WAIT_R_END] ? timing->t[GN_T_WAIT_R_END] : timing->t[GN_T_WAIT_L_END];
    last_submit_drain_begin = timing->t[GN_T_DRAIN_BEGIN];
    pthread_mutex_unlock(&timing_mutex);
    return 1;
}

/* Delay to apply before the next FRAME_SYNC. Updated once per submitted frame:
 * - the game entered xrEndFrame within [-1, +1.5] ms of the previous frame's GPU completion, so
 *   it was blocked on that GPU work: grow by half of the wait in excess of the target;
 * - otherwise the game was not waiting for the GPU (CPU-bound, or the delay overshot): shrink.
 * The delay never exceeds 40 ms. */
static int64_t frame_start_delay(void)
{
    pthread_mutex_lock(&timing_mutex);
    if (delay_target_ns <= 0) {
        delay_ns = 0;
    } else if (last_submit_frame && last_submit_frame != delay_updated_frame) {
        delay_updated_frame = last_submit_frame;
        const uint64_t previous = last_submit_frame - 1;
        const int64_t previous_fence =
            fence_end_frame[previous % 8] == previous ? fence_end_time[previous % 8] : 0;
        const int64_t coupling = last_submit_drain_begin - previous_fence;
        const int blocked = previous_fence && last_submit_drain_begin && last_submit_wait_end &&
            coupling >= -1000000 && coupling <= 1500000;
        const int64_t wait = last_submit_drain_begin - last_submit_wait_end;
        if (blocked) {
            if (wait > delay_target_ns) delay_ns += (wait - delay_target_ns) / 2;
        } else {
            delay_ns -= delay_ns / 4;
        }
        if (delay_ns > 40000000) delay_ns = 40000000;
        if (delay_ns < 0) delay_ns = 0;
    }
    const int64_t result = delay_ns;
    pthread_mutex_unlock(&timing_mutex);
    return result;
}

static int views_valid(const struct gn_unix_submit_view_args *views, uint32_t view_count)
{
    if (!view_count || view_count > 2) return 0;
    for (uint32_t i = 0; i < view_count; ++i) {
        const struct gn_unix_submit_view_args *view = &views[i];
        if (view->slot >= GN_UNIX_MAX_SWAPCHAINS || view->eye >= 2 ||
            view->image_index >= swapchains[view->slot].image_count)
            return 0;
    }
    return 1;
}

/* Phase 1, on the xrEndFrame thread while the runtime holds the game's submission lock:
 * register the images, record the AHardwareBuffer copies and submit them, or an empty
 * marker when nothing needs a copy, on the game's queue. *out_fence signals once that work
 * and everything the game submitted before it have executed. */
static int submit_game_queue_work(const struct gn_unix_submit_view_args *views,
                                  uint32_t view_count, VkFence *out_fence)
{
    *out_fence = VK_NULL_HANDLE;
    if (!views_valid(views, view_count) || !p_vkQueueSubmit) return 0;

    pthread_mutex_lock(&socket_mutex);
    VkCommandBuffer commands[2];
    struct gn_transport_image *recorded[2];
    uint32_t command_count = 0;
    for (uint32_t i = 0; i < view_count; ++i) {
        const struct gn_unix_submit_view_args *view = &views[i];
        if (!register_image(view->slot, view->image_index, view->eye,
                            view->array_index)) {
            pthread_mutex_unlock(&socket_mutex);
            return 0;
        }
#if defined(__ANDROID__)
        struct gn_image *image =
            &swapchains[view->slot].images[view->image_index];
        if (image->transport_kind[view->eye] == GN_TRANSPORT_AHARDWAREBUFFER) {
            struct gn_transport_image *transport = &image->transport[view->eye];
            if (!record_ahardwarebuffer_copy(
                    &swapchains[view->slot], image, transport, view)) {
                const uint8_t bit = (uint8_t)(1u << view->eye);
                log_line("AHardwareBuffer GPU copy failed; switching image to dma-buf");
                destroy_transport_image(transport);
                image->transport_kind[view->eye] = GN_TRANSPORT_DMABUF;
                image->registered_eye_mask &= (uint8_t)~bit;
                if (!register_image(view->slot, view->image_index, view->eye,
                                    view->array_index)) {
                    pthread_mutex_unlock(&socket_mutex);
                    return 0;
                }
            } else {
                commands[command_count] = transport->command_buffer;
                recorded[command_count] = transport;
                ++command_count;
            }
        }
#endif
    }

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (!p_vkCreateFence || !p_vkDestroyFence ||
        p_vkCreateFence(device, &fence_info, NULL, &fence) != VK_SUCCESS)
        fence = VK_NULL_HANDLE;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = command_count,
        .pCommandBuffers = command_count ? commands : NULL
    };
    VkResult result = p_vkQueueSubmit(queue, 1, &submit, fence);
    if (result != VK_SUCCESS) {
        log_vk_result("vkQueueSubmit(stereo transport)", result);
        if (fence != VK_NULL_HANDLE) p_vkDestroyFence(device, fence, NULL);
        pthread_mutex_unlock(&socket_mutex);
        return 0;
    }
    /* Without a fence, wait here: the queue is only ours while the game's lock is held. */
    if (fence == VK_NULL_HANDLE &&
        (!p_vkQueueWaitIdle || p_vkQueueWaitIdle(queue) != VK_SUCCESS)) {
        pthread_mutex_unlock(&socket_mutex);
        return 0;
    }
    for (uint32_t i = 0; i < command_count; ++i) recorded[i]->initialized = 1;
    pthread_mutex_unlock(&socket_mutex);
    *out_fence = fence;
    return 1;
}

/* Phase 2, after the phase 1 fence has signaled: run the relay copy on the relay device and
 * send the frame to the Android side. Never touches the game's queue. */
static int ship_views(const struct gn_unix_submit_view_args *views, uint32_t view_count,
                      uint64_t frame_id, struct gn_frame_timing *timing)
{
    if (!views_valid(views, view_count)) return 0;

    pthread_mutex_lock(&socket_mutex);
    int fence_fd = -1;
#if defined(__ANDROID__)
    VkCommandBuffer relay_commands[2];
    struct gn_transport_image *relay_recorded[2];
    uint32_t relay_count = 0;
    for (uint32_t i = 0; i < view_count; ++i) {
        const struct gn_unix_submit_view_args *view = &views[i];
        struct gn_image *image =
            &swapchains[view->slot].images[view->image_index];
        if (image->transport_kind[view->eye] != GN_TRANSPORT_RELAY) continue;
        struct gn_transport_image *transport = &image->transport[view->eye];
        if (!relay_record_copy(&swapchains[view->slot], image, transport, view)) {
            pthread_mutex_unlock(&socket_mutex);
            return 0;
        }
        relay_commands[relay_count] = transport->command_buffer;
        relay_recorded[relay_count] = transport;
        ++relay_count;
    }
    if (relay_count) {
        // The render has finished (phase 1 fence); the relay copy runs on its own device,
        // and its fence becomes the acquire fence.
        int relay_ok = 0;
        fence_fd = relay_submit(relay_commands, relay_count, &relay_ok);
        if (!relay_ok) {
            if (fence_fd >= 0) close(fence_fd);
            pthread_mutex_unlock(&socket_mutex);
            return 0;
        }
        for (uint32_t i = 0; i < relay_count; ++i) relay_recorded[i]->initialized = 1;
    }
#endif

    int ok = 1;
    timing->t[GN_T_SEND_BEGIN] = monotonic_ns();
    for (uint32_t i = 0; i < view_count; ++i) {
        const int view_fence_fd = i == 0 ? fence_fd : -1;
        if (!send_frame(&views[i], view_fence_fd, frame_id,
                        i + 1 == view_count ? timing : NULL)) {
            ok = 0;
            break;
        }
    }
    if (fence_fd >= 0) close(fence_fd);

    pthread_mutex_unlock(&socket_mutex);
    return ok;
}

/* Both phases on the caller's thread, for when the worker is unavailable. */
static int submit_views_sync(const struct gn_unix_submit_view_args *views, uint32_t view_count,
                             struct gn_frame_timing *timing)
{
    VkFence fence = VK_NULL_HANDLE;
    if (!submit_game_queue_work(views, view_count, &fence)) return 0;
    const uint64_t frame_id = __atomic_add_fetch(&transport_frame_id, 1, __ATOMIC_RELAXED);
    timing->t[GN_T_SUBMIT_END] = timing->t[GN_T_FENCE_BEGIN] = monotonic_ns();
    if (fence != VK_NULL_HANDLE) {
        p_vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        p_vkDestroyFence(device, fence, NULL);
    }
    timing->t[GN_T_FENCE_END] = monotonic_ns();
    return ship_views(views, view_count, frame_id, timing);
}

/* Claims the FRAME_SYNC and swapchain waits this frame paid for. A wait belongs to the first
 * submit after it; when both eyes share one swapchain only the left eye gets it. */
static void begin_frame_timing(struct gn_frame_timing *timing,
                               const struct gn_unix_submit_view_args *views, uint32_t view_count,
                               const struct gn_unix_submit_stereo_args *drain)
{
    memset(timing, 0, sizeof(*timing));
    timing->submit_begin = monotonic_ns();
    timing->snap = -1;
    pthread_mutex_lock(&timing_mutex);
    if (frame_sync_unclaimed) {
        timing->t[GN_T_DELAY_BEGIN] = frame_sync_timing[0];
        timing->t[GN_T_FRAME_SYNC_BEGIN] = frame_sync_timing[1];
        timing->t[GN_T_FRAME_SYNC_END] = frame_sync_timing[2];
        timing->snap = frame_sync_serial;
        frame_sync_unclaimed = 0;
    }
    for (uint32_t i = 0; i < view_count && i < 2; ++i) {
        const uint32_t slot = views[i].slot;
        if (slot >= GN_UNIX_MAX_SWAPCHAINS || !swapchain_wait_unclaimed[slot]) continue;
        timing->t[i ? GN_T_WAIT_R_BEGIN : GN_T_WAIT_L_BEGIN] = swapchain_wait_timing[slot][0];
        timing->t[i ? GN_T_WAIT_R_END : GN_T_WAIT_L_END] = swapchain_wait_timing[slot][1];
        swapchain_wait_unclaimed[slot] = 0;
    }
    pthread_mutex_unlock(&timing_mutex);
    if (!drain || drain->qpc_frequency <= 0) return;
    const int64_t qpc[3] = {drain->qpc_drain_begin, drain->qpc_drain_lock, drain->qpc_drain_end};
    int64_t converted[3];
    for (int i = 0; i < 3; ++i) {
        const double age_ns =
            (double)(drain->qpc_call - qpc[i]) * 1e9 / (double)drain->qpc_frequency;
        if (age_ns < 0.0 || age_ns > 10e9) return;
        converted[i] = timing->submit_begin - (int64_t)age_ns;
    }
    for (int i = 0; i < 3; ++i) timing->t[GN_T_DRAIN_BEGIN + i] = converted[i];
}

static int32_t unix_submit_image(void *opaque)
{
    struct gn_unix_submit_image_args *args = opaque;
    const struct gn_unix_submit_view_args *view = (const struct gn_unix_submit_view_args *)args;
    struct gn_frame_timing timing;
    begin_frame_timing(&timing, view, 1, NULL);
    args->result = submit_views_async(view, 1, &timing) ?
        GN_UNIX_SUCCESS : GN_UNIX_ERROR_TRANSPORT;
    return 0;
}


/* Control-plane fast path: the PE runtime's winsock round trip to the Kotlin control
 * server costs ~40ms under Wine+FEX because blocking recv detours through wineserver.
 * The same request over a plain bionic socket from the unix side is sub-millisecond. */
static int control_fast_fd = -1;
static pthread_mutex_t control_fast_mutex = PTHREAD_MUTEX_INITIALIZER;
static char control_fast_buf[1024];
static uint32_t control_fast_len, control_fast_off;

static void control_fast_close(void)
{
    if (control_fast_fd >= 0) close(control_fast_fd);
    control_fast_fd = -1;
    control_fast_len = 0;
    control_fast_off = 0;
}

static int control_fast_read_byte(char *value)
{
    if (control_fast_off >= control_fast_len) {
        ssize_t got = recv(control_fast_fd, control_fast_buf, sizeof(control_fast_buf), 0);
        if (got <= 0) return 0;
        control_fast_len = (uint32_t)got;
        control_fast_off = 0;
    }
    *value = control_fast_buf[control_fast_off++];
    return 1;
}

static int control_fast_read_line(char *out, size_t out_size)
{
    size_t offset = 0;
    char value = 0;
    for (;;) {
        if (!control_fast_read_byte(&value)) return 0;
        if (value == '\n') break;
        if (offset + 1 < out_size) out[offset++] = value;
    }
    out[offset] = 0;
    return 1;
}

static int control_fast_connect(void)
{
    if (control_fast_fd >= 0) return 1;
    const char *port_text = getenv("GAMENATIVE_XR_BRIDGE_PORT");
    int port = port_text ? atoi(port_text) : 38476;
    if (port <= 0) port = 38476;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = htonl(0x7f000001);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return 0;
    }
    control_fast_fd = fd;
    control_fast_len = 0;
    control_fast_off = 0;
    char line[128];
    if (!write_all(fd, "HELLO\n", 6) ||
        !control_fast_read_line(line, sizeof(line)) ||
        strncmp(line, "OK", 2) != 0) {
        control_fast_close();
        return 0;
    }
    log_line("unix control fast path connected");
    return 1;
}

static int32_t unix_control_transact(void *opaque)
{
    struct gn_unix_control_transact_args *args = opaque;
    args->request[sizeof(args->request) - 1] = 0;
    uint32_t lines = args->response_lines ? args->response_lines : 1;
    if (lines > 8) lines = 8;
    args->result = GN_UNIX_ERROR_TRANSPORT;
    const int frame_sync = !strcmp(args->request, "FRAME_SYNC");
    int64_t delay_begin = 0, delay = 0;
    if (frame_sync) {
        delay = frame_start_delay();
        delay_begin = monotonic_ns();
        if (delay > 0) {
            struct timespec remaining = {delay / 1000000000, delay % 1000000000};
            while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {}
        }
    }
    const int64_t transact_begin = frame_sync ? monotonic_ns() : 0;
    pthread_mutex_lock(&control_fast_mutex);
    if (!control_fast_connect()) {
        pthread_mutex_unlock(&control_fast_mutex);
        return 0;
    }
    int sent;
    if (frame_sync) {
        /* The server's pacing estimate excludes the time this side chose to wait. */
        char request[64];
        const int length = snprintf(request, sizeof(request), "FRAME_SYNC delay_us=%lld\n",
                                    (long long)((transact_begin - delay_begin) / 1000));
        sent = length > 0 && (size_t)length < sizeof(request) && write_all(control_fast_fd, request, (size_t)length);
    } else {
        size_t request_length = strlen(args->request);
        args->request[request_length] = '\n';
        sent = write_all(control_fast_fd, args->request, request_length + 1);
        args->request[request_length] = 0;
    }
    if (!sent) {
        control_fast_close();
        pthread_mutex_unlock(&control_fast_mutex);
        return 0;
    }
    size_t offset = 0;
    int truncated = 0;
    for (uint32_t i = 0; i < lines; ++i) {
        char line[1024];
        if (!control_fast_read_line(line, sizeof(line))) {
            control_fast_close();
            pthread_mutex_unlock(&control_fast_mutex);
            return 0;
        }
        size_t line_length = strlen(line);
        if (truncated || offset + line_length + 2 > sizeof(args->response)) {
            truncated = 1;
        } else {
            if (i) args->response[offset++] = '\n';
            memcpy(args->response + offset, line, line_length);
            offset += line_length;
        }
        /* single-line commands whose reply is an error carry no continuation lines */
        if (i == 0 && strncmp(line, "OK", 2) != 0) break;
    }
    args->response[offset] = 0;
    args->result = GN_UNIX_SUCCESS;
    pthread_mutex_unlock(&control_fast_mutex);
    if (frame_sync && !strncmp(args->response, "OK", 2)) {
        const char *serial = strstr(args->response, " serial=");
        const char *target = strstr(args->response, " jit=");
        pthread_mutex_lock(&timing_mutex);
        frame_sync_timing[0] = delay_begin;
        frame_sync_timing[1] = transact_begin;
        frame_sync_timing[2] = monotonic_ns();
        frame_sync_serial = serial ? strtoll(serial + 8, NULL, 10) : -1;
        frame_sync_unclaimed = 1;
        delay_target_ns = target ? strtoll(target + 5, NULL, 10) * 1000 : 0;
        pthread_mutex_unlock(&timing_mutex);
    }
    return 0;
}

static int32_t unix_submit_stereo(void *opaque)
{
    struct gn_unix_submit_stereo_args *args = opaque;
    if (!args->view_count || args->view_count > 2) {
        args->result = GN_UNIX_ERROR_ARGUMENT;
        return 0;
    }
    struct gn_frame_timing timing;
    begin_frame_timing(&timing, args->views, args->view_count, args);
    args->result = submit_views_async(args->views, args->view_count, &timing) ?
        GN_UNIX_SUCCESS : GN_UNIX_ERROR_TRANSPORT;
    return 0;
}

__attribute__((visibility("default")))
const unixlib_entry_t __wine_unix_call_funcs[GN_UNIX_CALL_COUNT] = {
    unix_init,
    unix_set_vulkan_context,
    unix_create_swapchain,
    unix_destroy_swapchain,
    unix_acquire_image,
    unix_submit_image,
    unix_submit_stereo,
    unix_control_transact
};



__attribute__((visibility("default")))
const unixlib_entry_t __wine_unix_call_wow64_funcs[GN_UNIX_CALL_COUNT] = {
    unix_init,
    unix_set_vulkan_context,
    unix_create_swapchain,
    unix_destroy_swapchain,
    unix_acquire_image,
    unix_submit_image,
    unix_submit_stereo,
    unix_control_transact
};

__attribute__((destructor))
static void unix_shutdown(void)
{
    close_transport();
    for (uint32_t slot = 0; slot < GN_UNIX_MAX_SWAPCHAINS; ++slot) {
        if (!swapchains[slot].allocated) continue;
        for (uint32_t image = 0;
             image < swapchains[slot].image_count; ++image)
            destroy_image(&swapchains[slot].images[image]);
    }
    if (command_pool && p_vkDestroyCommandPool)
        p_vkDestroyCommandPool(command_pool_device ? command_pool_device : device, command_pool, NULL);
    command_pool = VK_NULL_HANDLE;
    command_pool_device = VK_NULL_HANDLE;
}
