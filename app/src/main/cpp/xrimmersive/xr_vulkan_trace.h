#pragma once

// Debug tracing of the Vulkan calls the OpenXR runtime and the composite make on the app-process
// device (debug.xrgame.xr.vktrace=1): image creation, memory requirements, allocation, binding
// and release, each tagged with the calling library. GPU page faults in the composite can then be
// matched to the allocation and the code that owns it.

#include <vulkan/vulkan.h>

namespace xrimmersive::vulkan {

bool TraceEnabled();

// Returns a tracing entry point when the property is set, otherwise `real` unchanged.
PFN_vkGetInstanceProcAddr TraceWrapInstanceProcAddr(PFN_vkGetInstanceProcAddr real);

// GPU address of every image, buffer and driver BO bound on the device
// (VK_EXT_device_address_binding_report, delivered through VK_EXT_debug_utils), so a KGSL fault
// address can be matched to its object. The instance must enable VK_EXT_debug_utils and the device
// the report extension and feature.
VkDebugUtilsMessengerEXT CreateAddressReport(VkInstance instance, PFN_vkGetInstanceProcAddr getInstanceProcAddr);
void DestroyAddressReport(VkInstance instance, PFN_vkGetInstanceProcAddr getInstanceProcAddr,
                          VkDebugUtilsMessengerEXT messenger);

}  // namespace xrimmersive::vulkan
