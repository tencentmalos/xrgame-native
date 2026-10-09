#include "xr_vulkan_trace.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <dlfcn.h>
#include <sys/system_properties.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#define TLOG(...) __android_log_print(ANDROID_LOG_INFO, "xrvktrace", __VA_ARGS__)

namespace xrimmersive::vulkan {

namespace {

PFN_vkGetInstanceProcAddr gRealInstanceProcAddr = nullptr;
PFN_vkGetDeviceProcAddr gRealDeviceProcAddr = nullptr;
PFN_vkCreateImage gRealCreateImage = nullptr;
PFN_vkDestroyImage gRealDestroyImage = nullptr;
PFN_vkGetImageMemoryRequirements gRealImageRequirements = nullptr;
PFN_vkGetImageMemoryRequirements2 gRealImageRequirements2 = nullptr;
PFN_vkAllocateMemory gRealAllocateMemory = nullptr;
PFN_vkFreeMemory gRealFreeMemory = nullptr;
PFN_vkBindImageMemory gRealBindImageMemory = nullptr;
PFN_vkBindImageMemory2 gRealBindImageMemory2 = nullptr;
PFN_vkGetAndroidHardwareBufferPropertiesANDROID gRealAhbProperties = nullptr;
PFN_vkGetMemoryAndroidHardwareBufferANDROID gRealMemoryAhb = nullptr;

std::mutex gMutex;
std::unordered_map<uint64_t, VkDeviceSize> gMemorySize;
std::unordered_map<uint64_t, VkDeviceSize> gImageRequired;
std::unordered_set<std::string> gResolved;

template <typename T>
uint64_t Handle(T handle) {
    return reinterpret_cast<uint64_t>(handle);
}

std::string Caller(void *address) {
    Dl_info info{};
    if (dladdr(address, &info) == 0 || info.dli_fname == nullptr) return "?";
    const char *slash = std::strrchr(info.dli_fname, '/');
    return slash != nullptr ? slash + 1 : info.dli_fname;
}

std::string DescribeChain(const void *next) {
    std::string out;
    char item[160];
    for (auto *base = static_cast<const VkBaseInStructure *>(next); base != nullptr; base = base->pNext) {
        switch (base->sType) {
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO:
            std::snprintf(item, sizeof(item), " extImage(types=0x%x)",
                          reinterpret_cast<const VkExternalMemoryImageCreateInfo *>(base)->handleTypes);
            break;
        case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: {
            auto *list = reinterpret_cast<const VkImageFormatListCreateInfo *>(base);
            std::string formats;
            for (uint32_t i = 0; i < list->viewFormatCount; ++i) formats += " " + std::to_string(list->pViewFormats[i]);
            std::snprintf(item, sizeof(item), " formatList(%s)", formats.c_str());
            break;
        }
        case VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID: {
            AHardwareBuffer_Desc desc{};
            auto *import = reinterpret_cast<const VkImportAndroidHardwareBufferInfoANDROID *>(base);
            if (import->buffer != nullptr) AHardwareBuffer_describe(import->buffer, &desc);
            std::snprintf(item, sizeof(item), " importAhb(%p %ux%u fmt=%u stride=%u usage=0x%" PRIx64 ")",
                          static_cast<void *>(import->buffer), desc.width, desc.height, desc.format, desc.stride,
                          desc.usage);
            break;
        }
        case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO: {
            auto *dedicated = reinterpret_cast<const VkMemoryDedicatedAllocateInfo *>(base);
            std::snprintf(item, sizeof(item), " dedicated(img=0x%" PRIx64 " buf=0x%" PRIx64 ")",
                          Handle(dedicated->image), Handle(dedicated->buffer));
            break;
        }
        case VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO:
            std::snprintf(item, sizeof(item), " export(types=0x%x)",
                          reinterpret_cast<const VkExportMemoryAllocateInfo *>(base)->handleTypes);
            break;
        case VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR: {
            auto *fd = reinterpret_cast<const VkImportMemoryFdInfoKHR *>(base);
            std::snprintf(item, sizeof(item), " importFd(type=0x%x fd=%d)", fd->handleType, fd->fd);
            break;
        }
        default:
            std::snprintf(item, sizeof(item), " s%d", base->sType);
            break;
        }
        out += item;
    }
    return out;
}

VKAPI_ATTR VkResult VKAPI_CALL TraceCreateImage(VkDevice device, const VkImageCreateInfo *info,
                                                const VkAllocationCallbacks *allocator, VkImage *image) {
    const VkResult result = gRealCreateImage(device, info, allocator, image);
    TLOG("%s createImage img=0x%" PRIx64 " r=%d %ux%ux%u fmt=%d tiling=%d usage=0x%x flags=0x%x samples=%d "
         "mips=%u layers=%u sharing=%d%s",
         Caller(__builtin_return_address(0)).c_str(), result == VK_SUCCESS ? Handle(*image) : 0, result,
         info->extent.width, info->extent.height, info->extent.depth, info->format, info->tiling, info->usage,
         info->flags, info->samples, info->mipLevels, info->arrayLayers, info->sharingMode,
         DescribeChain(info->pNext).c_str());
    return result;
}

VKAPI_ATTR void VKAPI_CALL TraceDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *allocator) {
    TLOG("%s destroyImage img=0x%" PRIx64, Caller(__builtin_return_address(0)).c_str(), Handle(image));
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gImageRequired.erase(Handle(image));
    }
    gRealDestroyImage(device, image, allocator);
}

void NoteRequirements(void *caller, VkImage image, const VkMemoryRequirements &requirements) {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gImageRequired[Handle(image)] = requirements.size;
    }
    TLOG("%s imageRequirements img=0x%" PRIx64 " size=0x%" PRIx64 " align=0x%" PRIx64 " types=0x%x",
         Caller(caller).c_str(), Handle(image), requirements.size, requirements.alignment,
         requirements.memoryTypeBits);
}

VKAPI_ATTR void VKAPI_CALL TraceImageRequirements(VkDevice device, VkImage image, VkMemoryRequirements *requirements) {
    gRealImageRequirements(device, image, requirements);
    NoteRequirements(__builtin_return_address(0), image, *requirements);
}

VKAPI_ATTR void VKAPI_CALL TraceImageRequirements2(VkDevice device, const VkImageMemoryRequirementsInfo2 *info,
                                                   VkMemoryRequirements2 *requirements) {
    gRealImageRequirements2(device, info, requirements);
    NoteRequirements(__builtin_return_address(0), info->image, requirements->memoryRequirements);
}

VKAPI_ATTR VkResult VKAPI_CALL TraceAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *info,
                                                   const VkAllocationCallbacks *allocator, VkDeviceMemory *memory) {
    const VkResult result = gRealAllocateMemory(device, info, allocator, memory);
    if (result == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(gMutex);
        gMemorySize[Handle(*memory)] = info->allocationSize;
    }
    TLOG("%s allocateMemory mem=0x%" PRIx64 " r=%d size=0x%" PRIx64 " type=%u%s",
         Caller(__builtin_return_address(0)).c_str(), result == VK_SUCCESS ? Handle(*memory) : 0, result,
         info->allocationSize, info->memoryTypeIndex, DescribeChain(info->pNext).c_str());
    return result;
}

VKAPI_ATTR void VKAPI_CALL TraceFreeMemory(VkDevice device, VkDeviceMemory memory,
                                           const VkAllocationCallbacks *allocator) {
    VkDeviceSize size = 0;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        auto it = gMemorySize.find(Handle(memory));
        if (it != gMemorySize.end()) {
            size = it->second;
            gMemorySize.erase(it);
        }
    }
    if (memory != VK_NULL_HANDLE) {
        TLOG("%s freeMemory mem=0x%" PRIx64 " size=0x%" PRIx64, Caller(__builtin_return_address(0)).c_str(),
             Handle(memory), size);
    }
    gRealFreeMemory(device, memory, allocator);
}

void NoteBind(void *caller, VkImage image, VkDeviceMemory memory, VkDeviceSize offset, VkResult result) {
    VkDeviceSize memorySize = 0;
    VkDeviceSize required = 0;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        auto m = gMemorySize.find(Handle(memory));
        if (m != gMemorySize.end()) memorySize = m->second;
        auto r = gImageRequired.find(Handle(image));
        if (r != gImageRequired.end()) required = r->second;
    }
    const bool short_ = required != 0 && memorySize != 0 && offset + required > memorySize;
    __android_log_print(short_ ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO, "xrvktrace",
                        "%s bindImage img=0x%" PRIx64 " mem=0x%" PRIx64 " offset=0x%" PRIx64 " memSize=0x%" PRIx64
                        " required=0x%" PRIx64 " r=%d%s",
                        Caller(caller).c_str(), Handle(image), Handle(memory), offset, memorySize, required, result,
                        short_ ? " MEMORY SMALLER THAN IMAGE" : "");
}

VKAPI_ATTR VkResult VKAPI_CALL TraceBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory,
                                                    VkDeviceSize offset) {
    const VkResult result = gRealBindImageMemory(device, image, memory, offset);
    NoteBind(__builtin_return_address(0), image, memory, offset, result);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL TraceBindImageMemory2(VkDevice device, uint32_t count,
                                                     const VkBindImageMemoryInfo *infos) {
    const VkResult result = gRealBindImageMemory2(device, count, infos);
    for (uint32_t i = 0; i < count; ++i) {
        NoteBind(__builtin_return_address(0), infos[i].image, infos[i].memory, infos[i].memoryOffset, result);
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL TraceAhbProperties(VkDevice device, const struct AHardwareBuffer *buffer,
                                                  VkAndroidHardwareBufferPropertiesANDROID *properties) {
    const VkResult result = gRealAhbProperties(device, buffer, properties);
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(buffer, &desc);
    VkFormat format = VK_FORMAT_UNDEFINED;
    for (auto *base = static_cast<const VkBaseInStructure *>(properties->pNext); base != nullptr; base = base->pNext) {
        if (base->sType == VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID) {
            format = reinterpret_cast<const VkAndroidHardwareBufferFormatPropertiesANDROID *>(base)->format;
        }
    }
    TLOG("%s ahbProperties ahb=%p %ux%u fmt=%u stride=%u usage=0x%" PRIx64 " r=%d allocationSize=0x%" PRIx64
         " types=0x%x vkFormat=%d",
         Caller(__builtin_return_address(0)).c_str(), static_cast<const void *>(buffer), desc.width, desc.height,
         desc.format, desc.stride, desc.usage, result, properties->allocationSize, properties->memoryTypeBits, format);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL TraceMemoryAhb(VkDevice device, const VkMemoryGetAndroidHardwareBufferInfoANDROID *info,
                                              struct AHardwareBuffer **buffer) {
    const VkResult result = gRealMemoryAhb(device, info, buffer);
    AHardwareBuffer_Desc desc{};
    if (result == VK_SUCCESS && *buffer != nullptr) AHardwareBuffer_describe(*buffer, &desc);
    TLOG("%s exportAhb mem=0x%" PRIx64 " r=%d ahb=%p %ux%u fmt=%u stride=%u usage=0x%" PRIx64,
         Caller(__builtin_return_address(0)).c_str(), Handle(info->memory), result,
         result == VK_SUCCESS ? static_cast<void *>(*buffer) : nullptr, desc.width, desc.height, desc.format,
         desc.stride, desc.usage);
    return result;
}

struct Hook {
    const char *name;
    PFN_vkVoidFunction wrapper;
    PFN_vkVoidFunction *real;
};

#define XRVK_HOOK(name, wrapper, real) \
    {name, reinterpret_cast<PFN_vkVoidFunction>(wrapper), reinterpret_cast<PFN_vkVoidFunction *>(&real)}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL TraceDeviceProcAddr(VkDevice device, const char *name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL TraceInstanceProcAddr(VkInstance instance, const char *name);

const Hook kHooks[] = {
    XRVK_HOOK("vkGetInstanceProcAddr", TraceInstanceProcAddr, gRealInstanceProcAddr),
    XRVK_HOOK("vkGetDeviceProcAddr", TraceDeviceProcAddr, gRealDeviceProcAddr),
    XRVK_HOOK("vkCreateImage", TraceCreateImage, gRealCreateImage),
    XRVK_HOOK("vkDestroyImage", TraceDestroyImage, gRealDestroyImage),
    XRVK_HOOK("vkGetImageMemoryRequirements", TraceImageRequirements, gRealImageRequirements),
    XRVK_HOOK("vkGetImageMemoryRequirements2", TraceImageRequirements2, gRealImageRequirements2),
    XRVK_HOOK("vkGetImageMemoryRequirements2KHR", TraceImageRequirements2, gRealImageRequirements2),
    XRVK_HOOK("vkAllocateMemory", TraceAllocateMemory, gRealAllocateMemory),
    XRVK_HOOK("vkFreeMemory", TraceFreeMemory, gRealFreeMemory),
    XRVK_HOOK("vkBindImageMemory", TraceBindImageMemory, gRealBindImageMemory),
    XRVK_HOOK("vkBindImageMemory2", TraceBindImageMemory2, gRealBindImageMemory2),
    XRVK_HOOK("vkBindImageMemory2KHR", TraceBindImageMemory2, gRealBindImageMemory2),
    XRVK_HOOK("vkGetAndroidHardwareBufferPropertiesANDROID", TraceAhbProperties, gRealAhbProperties),
    XRVK_HOOK("vkGetMemoryAndroidHardwareBufferANDROID", TraceMemoryAhb, gRealMemoryAhb),
};

#undef XRVK_HOOK

void NoteLookup(void *caller, const char *kind, const char *name, PFN_vkVoidFunction real) {
    if (name == nullptr) return;
    const std::string library = Caller(caller);
    const std::string key = library + ":" + name;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (!gResolved.insert(key).second) return;
    }
    TLOG("%s %s %s -> %p", library.c_str(), kind, name, reinterpret_cast<void *>(real));
}

PFN_vkVoidFunction Intercept(const char *name, PFN_vkVoidFunction real) {
    if (real == nullptr || name == nullptr) return real;
    for (const Hook &hook : kHooks) {
        if (std::strcmp(hook.name, name) != 0) continue;
        std::lock_guard<std::mutex> lock(gMutex);
        if (*hook.real == nullptr) *hook.real = real;
        return hook.wrapper;
    }
    return real;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL TraceDeviceProcAddr(VkDevice device, const char *name) {
    const PFN_vkVoidFunction real = gRealDeviceProcAddr(device, name);
    NoteLookup(__builtin_return_address(0), "device", name, real);
    return Intercept(name, real);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL TraceInstanceProcAddr(VkInstance instance, const char *name) {
    const PFN_vkVoidFunction real = gRealInstanceProcAddr(instance, name);
    NoteLookup(__builtin_return_address(0), "instance", name, real);
    return Intercept(name, real);
}

VKAPI_ATTR VkBool32 VKAPI_CALL AddressCallback(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                               VkDebugUtilsMessageTypeFlagsEXT types,
                                               const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
    if (data == nullptr || !(types & VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT)) return VK_FALSE;
    for (auto *base = static_cast<const VkBaseInStructure *>(data->pNext); base != nullptr; base = base->pNext) {
        if (base->sType != VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT) continue;
        auto *binding = reinterpret_cast<const VkDeviceAddressBindingCallbackDataEXT *>(base);
        const VkDebugUtilsObjectNameInfoEXT *object = data->objectCount > 0 ? &data->pObjects[0] : nullptr;
        __android_log_print(ANDROID_LOG_INFO, "xraddr", "%s type=%d obj=0x%" PRIx64 " va=0x%" PRIx64
                            "-0x%" PRIx64 " size=0x%" PRIx64 " flags=0x%x%s%s",
                            binding->bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT ? "bind" : "unbind",
                            object != nullptr ? object->objectType : 0,
                            object != nullptr ? object->objectHandle : 0, binding->baseAddress,
                            binding->baseAddress + binding->size, binding->size, binding->flags,
                            object != nullptr && object->pObjectName != nullptr ? " " : "",
                            object != nullptr && object->pObjectName != nullptr ? object->pObjectName : "");
    }
    return VK_FALSE;
}

}  // namespace

bool TraceEnabled() {
    char value[PROP_VALUE_MAX] = {};
    return __system_property_get("debug.xrgame.xr.vktrace", value) > 0 && std::strcmp(value, "1") == 0;
}

VkDebugUtilsMessengerEXT CreateAddressReport(VkInstance instance, PFN_vkGetInstanceProcAddr getInstanceProcAddr) {
    auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        getInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    if (create == nullptr) {
        TLOG("address report unavailable: no vkCreateDebugUtilsMessengerEXT");
        return VK_NULL_HANDLE;
    }
    VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT;
    info.pfnUserCallback = AddressCallback;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    const VkResult result = create(instance, &info, nullptr, &messenger);
    TLOG("address report messenger r=%d", result);
    return result == VK_SUCCESS ? messenger : VK_NULL_HANDLE;
}

void DestroyAddressReport(VkInstance instance, PFN_vkGetInstanceProcAddr getInstanceProcAddr,
                          VkDebugUtilsMessengerEXT messenger) {
    if (messenger == VK_NULL_HANDLE || instance == VK_NULL_HANDLE) return;
    auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        getInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (destroy != nullptr) destroy(instance, messenger, nullptr);
}

PFN_vkGetInstanceProcAddr TraceWrapInstanceProcAddr(PFN_vkGetInstanceProcAddr real) {
    if (real == nullptr || !TraceEnabled()) return real;
    gRealInstanceProcAddr = real;
    TLOG("Vulkan call trace enabled for the app-process device");
    return TraceInstanceProcAddr;
}

}  // namespace xrimmersive::vulkan
