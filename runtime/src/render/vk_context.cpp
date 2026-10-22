// Vulkan bootstrap: loader, instance, device, allocator.

#define VMA_IMPLEMENTATION
#include "render/vk_context.h"

#include <cstring>
#include <format>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <dlfcn.h>
#endif

// VkPhysicalDevicePortabilitySubsetFeaturesKHR lives in the beta header, which vulkan.h only pulls in
// under VK_ENABLE_BETA_EXTENSIONS; defining that for volk would drag in the beta function tables. Its
// sType enumerator is behind the same guard, so the value is spelled out below.
#include <vulkan/vulkan_beta.h>

#include "core/log.h"

namespace dal {
namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr std::uint32_t kRequiredApiVersion = VK_API_VERSION_1_3;
constexpr VkStructureType kPortabilitySubsetFeaturesType = static_cast<VkStructureType>(1000163000);

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) == 0) return VK_FALSE;
    static_cast<std::atomic<std::uint32_t>*>(user)->fetch_add(1, std::memory_order_relaxed);
    logError("dal: render: validation: {}",
             data != nullptr && data->pMessage != nullptr ? data->pMessage : "(no message)");
    return VK_FALSE;
}

// Loads the Vulkan loader. volk's own search can fail on macOS (it does when DYLD_FALLBACK_LIBRARY_PATH is
// set), so the install locations are tried by path before giving up.
bool initLoader(std::string& error) {
    if (volkInitialize() == VK_SUCCESS) return true;
#if defined(__APPLE__)
    std::vector<std::string> paths = {"/usr/local/lib/libvulkan.1.dylib"};
#if defined(DAL_VULKAN_SDK_LIB_DIR)
    paths.push_back(std::string(DAL_VULKAN_SDK_LIB_DIR) + "/libvulkan.1.dylib");
#endif
    for (const std::string& path : paths) {
        void* library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (library == nullptr) continue;
        auto get_instance_proc_addr =
                reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
        if (get_instance_proc_addr == nullptr) {
            dlclose(library);
            continue;
        }
        volkInitializeCustom(get_instance_proc_addr);
        return true;
    }
    error = "the Vulkan loader was not found (install the LunarG Vulkan SDK with its system-global files; see "
            "docs/building.md)";
#else
    error = "the Vulkan loader was not found";
#endif
    return false;
}

bool hasName(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const VkExtensionProperties& ext : list) {
        if (std::strcmp(ext.extensionName, name) == 0) return true;
    }
    return false;
}

std::vector<VkExtensionProperties> instanceExtensions() {
    std::uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> list(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, list.data());
    list.resize(count);
    return list;
}

std::vector<VkExtensionProperties> deviceExtensions(VkPhysicalDevice device) {
    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> list(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, list.data());
    list.resize(count);
    return list;
}

bool layerInstalled(const char* name) {
    std::uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    layers.resize(count);
    for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, name) == 0) return true;
    }
    return false;
}

struct Candidate {
    VkPhysicalDevice device = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    VkDriverId driver_id = static_cast<VkDriverId>(0);
    std::string driver_name;
    std::string driver_info;
    std::uint32_t graphics_family = 0;
    bool portability_subset = false;                        // the device advertises VK_KHR_portability_subset
    VkPhysicalDevicePortabilitySubsetFeaturesKHR subset{};  // what that extension reports as supported
    bool triangle_fans = true;
};

// Returns an empty string when `device` is usable, else the reason it is not.
std::string checkDevice(VkPhysicalDevice device, const VkContextOptions& options, Candidate& out) {
    VkPhysicalDeviceDriverProperties driver{};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceProperties2 properties2{};
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties2.pNext = &driver;
    vkGetPhysicalDeviceProperties(device, &out.properties);
    out.device = device;
    if (out.properties.apiVersion >= VK_API_VERSION_1_2) {
        vkGetPhysicalDeviceProperties2(device, &properties2);
        out.driver_id = driver.driverID;
        out.driver_name = driver.driverName;
        out.driver_info = driver.driverInfo;
    }
    if (out.properties.apiVersion < kRequiredApiVersion) return "Vulkan 1.3 is not supported";

    std::uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, families.data());
    bool found = false;
    for (std::uint32_t i = 0; i < family_count; i++) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && families[i].queueCount > 0) {
            out.graphics_family = i;
            found = true;
            break;
        }
    }
    if (!found) return "no graphics queue";

    const std::vector<VkExtensionProperties> extensions = deviceExtensions(device);
    if (!hasName(extensions, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) {
        return "VK_KHR_push_descriptor is missing";
    }
    if (options.window && !hasName(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        return "VK_KHR_swapchain is missing";
    }

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    out.subset.sType = kPortabilitySubsetFeaturesType;
    out.portability_subset = hasName(extensions, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
    features13.pNext = out.portability_subset ? &out.subset : nullptr;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &features13;
    vkGetPhysicalDeviceFeatures2(device, &features);
    // MoltenVK reports triangleFans but drops every triangle after the first when the fan topology is set
    // with vkCmdSetPrimitiveTopology, which is how the renderer draws, so fans are expanded there too.
    out.triangle_fans =
            (!out.portability_subset || out.subset.triangleFans == VK_TRUE) && out.driver_id != VK_DRIVER_ID_MOLTENVK;
    // Extended dynamic state is core functionality in 1.3 and has no separate feature bit there.
    if (features13.dynamicRendering == VK_FALSE) return "dynamicRendering is not supported";
    if (features13.synchronization2 == VK_FALSE) return "synchronization2 is not supported";
    // glslang compiles `discard` for Vulkan 1.3 as a demote to a helper invocation.
    if (features13.shaderDemoteToHelperInvocation == VK_FALSE) return "shaderDemoteToHelperInvocation is not supported";
    return {};
}

bool formatUsableAsDepthStencil(VkPhysicalDevice device, VkFormat format) {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(device, format, &props);
    return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
}

}  // namespace

VkContext::~VkContext() {
    if (allocator_ != nullptr) vmaDestroyAllocator(allocator_);
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        vkDestroyDevice(device_, nullptr);
    }
    if (messenger_ != VK_NULL_HANDLE) vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
}

bool vkValidationLayerAvailable() {
    std::string error;
    if (!initLoader(error)) return false;
    return layerInstalled(kValidationLayer);
}

std::unique_ptr<VkContext> createVkContext(const VkContextOptions& options, std::string& error) {
    if (!initLoader(error)) return nullptr;
    if (volkGetInstanceVersion() < kRequiredApiVersion) {
        error = "the Vulkan loader is older than 1.3";
        return nullptr;
    }

    std::unique_ptr<VkContext> ctx(new VkContext());

    const std::vector<VkExtensionProperties> available = instanceExtensions();
    std::vector<const char*> instance_exts;
    std::vector<const char*> layers;
    bool want_messenger = false;
    if (options.validation && layerInstalled(kValidationLayer) &&
        hasName(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        layers.push_back(kValidationLayer);
        instance_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        want_messenger = true;
    }
    VkInstanceCreateFlags instance_flags = 0;
#if defined(__APPLE__)
    // MoltenVK is a portability driver: the loader hides it unless the application opts in.
    if (hasName(available, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        instance_exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        instance_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#endif
    if (options.window) {
#if defined(__APPLE__)
        const char* const platform_surface = "VK_EXT_metal_surface";
#else
        const char* const platform_surface = "VK_KHR_win32_surface";
#endif
        // Literal names: the platform headers that define the surface macros are not included here.
        for (const char* name : {VK_KHR_SURFACE_EXTENSION_NAME, platform_surface}) {
            if (!hasName(available, name)) {
                error = std::format("instance extension {} is missing", name);
                return nullptr;
            }
            instance_exts.push_back(name);
        }
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "dal";
    app.apiVersion = kRequiredApiVersion;

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.flags = instance_flags;
    instance_info.pApplicationInfo = &app;
    instance_info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    instance_info.ppEnabledLayerNames = layers.data();
    instance_info.enabledExtensionCount = static_cast<std::uint32_t>(instance_exts.size());
    instance_info.ppEnabledExtensionNames = instance_exts.data();
    VkResult result = vkCreateInstance(&instance_info, nullptr, &ctx->instance_);
    if (result != VK_SUCCESS) {
        ctx->instance_ = VK_NULL_HANDLE;
        error = std::format("vkCreateInstance failed ({})", static_cast<int>(result));
        return nullptr;
    }
    volkLoadInstance(ctx->instance_);

    if (want_messenger) {
        VkDebugUtilsMessengerCreateInfoEXT messenger_info{};
        messenger_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messenger_info.pfnUserCallback = &debugCallback;
        messenger_info.pUserData = &ctx->validation_errors_;
        if (vkCreateDebugUtilsMessengerEXT(ctx->instance_, &messenger_info, nullptr, &ctx->messenger_) != VK_SUCCESS) {
            ctx->messenger_ = VK_NULL_HANDLE;
        }
    }

    std::uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(ctx->instance_, &device_count, nullptr);
    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(ctx->instance_, &device_count, devices.data());
    devices.resize(device_count);
    if (devices.empty()) {
        error = "no Vulkan physical device";
        return nullptr;
    }

    // A discrete GPU beats any other type; among devices of one type KosmicKrisp is preferred (macOS can list
    // both KosmicKrisp and MoltenVK, in no fixed order, and KosmicKrisp is a full driver without the
    // portability subset); otherwise the first usable device wins.
    Candidate chosen;
    int chosen_rank = -1;
    bool have_choice = false;
    std::string rejected;
    for (VkPhysicalDevice device : devices) {
        Candidate candidate;
        const std::string reason = checkDevice(device, options, candidate);
        if (!reason.empty()) {
            if (!rejected.empty()) rejected += "; ";
            rejected += std::format("{}: {}", candidate.properties.deviceName, reason);
            continue;
        }
        const int rank = (candidate.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 0) +
                         (candidate.driver_id == VK_DRIVER_ID_MESA_KOSMICKRISP ? 1 : 0);
        if (rank > chosen_rank) {
            chosen = candidate;
            chosen_rank = rank;
            have_choice = true;
        }
    }
    if (!have_choice) {
        error = "no suitable Vulkan device (" + rejected + ")";
        return nullptr;
    }
    ctx->physical_device_ = chosen.device;
    ctx->properties_ = chosen.properties;
    ctx->graphics_family_ = chosen.graphics_family;
    ctx->driver_id_ = chosen.driver_id;
    ctx->driver_name_ = chosen.driver_name;
    ctx->driver_info_ = chosen.driver_info;
    ctx->triangle_fans_ = chosen.triangle_fans;
    ctx->has_d24s8_ = formatUsableAsDepthStencil(chosen.device, VK_FORMAT_D24_UNORM_S8_UINT);
    if (!ctx->has_d24s8_ && !formatUsableAsDepthStencil(chosen.device, VK_FORMAT_D32_SFLOAT_S8_UINT)) {
        error = "the device supports neither D24S8 nor D32S8 attachments";
        return nullptr;
    }

    std::vector<const char*> device_exts = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    if (options.window) device_exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    // The spec requires a portability device to be created with this extension enabled.
    if (chosen.portability_subset) device_exts.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;
    features13.shaderDemoteToHelperInvocation = VK_TRUE;
    // Enable every subset feature the device offers (image view swizzle and others the renderer relies on).
    if (chosen.portability_subset) features13.pNext = &chosen.subset;

    // Optional: DXT guest textures fall back to the placeholder without it.
    VkPhysicalDeviceFeatures supported_features{};
    vkGetPhysicalDeviceFeatures(chosen.device, &supported_features);
    ctx->has_texture_bc_ = supported_features.textureCompressionBC == VK_TRUE;
    VkPhysicalDeviceFeatures enabled_features{};
    enabled_features.textureCompressionBC = ctx->has_texture_bc_ ? VK_TRUE : VK_FALSE;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = chosen.graphics_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.pNext = &features13;
    device_info.pEnabledFeatures = &enabled_features;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = static_cast<std::uint32_t>(device_exts.size());
    device_info.ppEnabledExtensionNames = device_exts.data();
    result = vkCreateDevice(chosen.device, &device_info, nullptr, &ctx->device_);
    if (result != VK_SUCCESS) {
        ctx->device_ = VK_NULL_HANDLE;
        error = std::format("vkCreateDevice failed ({})", static_cast<int>(result));
        return nullptr;
    }
    volkLoadDevice(ctx->device_);
    vkGetDeviceQueue(ctx->device_, chosen.graphics_family, 0, &ctx->graphics_queue_);

    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo allocator_info{};
    allocator_info.physicalDevice = chosen.device;
    allocator_info.device = ctx->device_;
    allocator_info.instance = ctx->instance_;
    allocator_info.vulkanApiVersion = kRequiredApiVersion;
    allocator_info.pVulkanFunctions = &functions;
    result = vmaCreateAllocator(&allocator_info, &ctx->allocator_);
    if (result != VK_SUCCESS) {
        ctx->allocator_ = nullptr;
        error = std::format("vmaCreateAllocator failed ({})", static_cast<int>(result));
        return nullptr;
    }
    return ctx;
}

}  // namespace dal
