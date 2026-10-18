// SPDX-License-Identifier: GPL-3.0-or-later
// Presents through the official client's GPU path without Steam: an X11 window on the surface
// server ($DISPLAY, Xephyr) and a Vulkan swapchain that the glibc Turnip WSI routes to
// XRGAME_X11_HOST_DISPLAY. Prints each step; exit status is the first failing step.
// Build: aarch64-linux-gnu-gcc -O2 -I<Vulkan-Headers>/include vk-x11-host-probe.c -lxcb -ldl
#define VK_USE_PLATFORM_XCB_KHR
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include <xcb/xcb.h>

#define CHECK(step, expr) do { VkResult r_ = (expr); printf("%-28s %d\n", step, r_); fflush(stdout); \
    if (r_ < 0) return __LINE__; } while (0)

static PFN_vkGetInstanceProcAddr gipa;
#define LOAD(instance, name) PFN_##name name = (PFN_##name)gipa(instance, #name)

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int main(int argc, char **argv) {
    int frames = argc > 1 ? atoi(argv[1]) : 120;
    void *loader = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!loader) { printf("dlopen libvulkan.so.1: %s\n", dlerror()); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(loader, "vkGetInstanceProcAddr");

    xcb_connection_t *conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) { printf("xcb_connect failed\n"); return 2; }
    xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    xcb_window_t window = xcb_generate_id(conn);
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, window, screen->root, 40, 30, 640, 360, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, NULL);
    xcb_map_window(conn, window);
    xcb_flush(conn);

    LOAD(NULL, vkCreateInstance);
    const char *instance_ext[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XCB_SURFACE_EXTENSION_NAME };
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                 .enabledExtensionCount = 2, .ppEnabledExtensionNames = instance_ext };
    VkInstance instance;
    CHECK("vkCreateInstance", vkCreateInstance(&ici, NULL, &instance));
    LOAD(instance, vkEnumeratePhysicalDevices);
    LOAD(instance, vkGetPhysicalDeviceProperties);
    LOAD(instance, vkCreateXcbSurfaceKHR);
    LOAD(instance, vkGetPhysicalDeviceXcbPresentationSupportKHR);
    LOAD(instance, vkGetPhysicalDeviceSurfaceSupportKHR);
    LOAD(instance, vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOAD(instance, vkCreateDevice);
    LOAD(instance, vkGetDeviceProcAddr);
    uint32_t count = 1;
    VkPhysicalDevice gpu;
    CHECK("vkEnumeratePhysicalDevices", vkEnumeratePhysicalDevices(instance, &count, &gpu));
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);
    printf("device %s\n", props.deviceName);
    printf("%-28s %u\n", "xcb presentation support",
           vkGetPhysicalDeviceXcbPresentationSupportKHR(gpu, 0, conn, screen->root_visual));
    VkXcbSurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR, .connection = conn, .window = window };
    VkSurfaceKHR surface;
    CHECK("vkCreateXcbSurfaceKHR", vkCreateXcbSurfaceKHR(instance, &sci, NULL, &surface));
    VkBool32 supported = 0;
    CHECK("SurfaceSupport", vkGetPhysicalDeviceSurfaceSupportKHR(gpu, 0, surface, &supported));
    printf("%-28s %u\n", "surface supported", supported);
    VkSurfaceCapabilitiesKHR caps;
    CHECK("SurfaceCapabilities", vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps));
    printf("extent %ux%u minImages %u\n", caps.currentExtent.width, caps.currentExtent.height, caps.minImageCount);

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &priority };
    const char *device_ext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = 1, .ppEnabledExtensionNames = device_ext };
    VkDevice device;
    CHECK("vkCreateDevice", vkCreateDevice(gpu, &dci, NULL, &device));
#define DLOAD(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(device, #name)
    DLOAD(vkGetDeviceQueue); DLOAD(vkCreateSwapchainKHR); DLOAD(vkGetSwapchainImagesKHR);
    DLOAD(vkAcquireNextImageKHR); DLOAD(vkQueuePresentKHR); DLOAD(vkCreateCommandPool);
    DLOAD(vkAllocateCommandBuffers); DLOAD(vkBeginCommandBuffer); DLOAD(vkEndCommandBuffer);
    DLOAD(vkCmdPipelineBarrier); DLOAD(vkCmdClearColorImage); DLOAD(vkQueueSubmit);
    DLOAD(vkCreateSemaphore); DLOAD(vkCreateFence); DLOAD(vkWaitForFences); DLOAD(vkResetFences);
    DLOAD(vkDeviceWaitIdle); DLOAD(vkDestroySwapchainKHR);
    VkQueue queue;
    vkGetDeviceQueue(device, 0, 0, &queue);

    VkSwapchainCreateInfoKHR swci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surface,
        .minImageCount = caps.minImageCount, .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, .imageExtent = caps.currentExtent,
        .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE };
    VkSwapchainKHR swapchain;
    CHECK("vkCreateSwapchainKHR", vkCreateSwapchainKHR(device, &swci, NULL, &swapchain));
    uint32_t image_count = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &image_count, NULL);
    VkImage images[8];
    if (image_count > 8) image_count = 8;
    vkGetSwapchainImagesKHR(device, swapchain, &image_count, images);
    printf("swapchain images %u\n", image_count);

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    VkCommandPool pool;
    vkCreateCommandPool(device, &pci, NULL, &pool);
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &cai, &cmd);
    VkSemaphoreCreateInfo semi = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore acquired, rendered;
    vkCreateSemaphore(device, &semi, NULL, &acquired);
    vkCreateSemaphore(device, &semi, NULL, &rendered);
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    VkFence fence;
    vkCreateFence(device, &fci, NULL, &fence);

    double start = now();
    for (int frame = 0; frame < frames; frame++) {
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(device, 1, &fence);
        uint32_t index;
        VkResult acquire = vkAcquireNextImageKHR(device, swapchain, 2000000000ull, acquired, VK_NULL_HANDLE, &index);
        if (acquire < 0 || frame == 0) printf("acquire[%d] %d index %u\n", frame, acquire, index);
        if (acquire < 0) return 90;
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        vkBeginCommandBuffer(cmd, &begin);
        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageMemoryBarrier to_dst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = images[index], .subresourceRange = range };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_dst);
        float t = (float)frame / (frames > 1 ? frames - 1 : 1);
        VkClearColorValue color = { .float32 = { t, 0.2f, 1.0f - t, 1.0f } };
        vkCmdClearColorImage(cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
        VkImageMemoryBarrier to_present = to_dst;
        to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_present.dstAccessMask = 0;
        to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &to_present);
        vkEndCommandBuffer(cmd);
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &acquired,
            .pWaitDstStageMask = &wait_stage, .commandBufferCount = 1, .pCommandBuffers = &cmd,
            .signalSemaphoreCount = 1, .pSignalSemaphores = &rendered };
        vkQueueSubmit(queue, 1, &submit, fence);
        VkPresentInfoKHR present = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &rendered,
                                     .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &index };
        VkResult presented = vkQueuePresentKHR(queue, &present);
        if (presented < 0 || frame == 0) printf("present[%d] %d\n", frame, presented);
        if (presented < 0) return 91;
    }
    vkDeviceWaitIdle(device);
    double elapsed = now() - start;
    printf("frames %d in %.2fs = %.1f fps\n", frames, elapsed, frames / elapsed);
    vkDestroySwapchainKHR(device, swapchain, NULL);
    xcb_disconnect(conn);
    return 0;
}
