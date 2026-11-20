/* volk.h's stand-in: see there. */
#include "volk.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
#define MDR_VK_DEFINE(name) PFN_##name name;
MDR_VK_GLOBAL_FUNCTIONS(MDR_VK_DEFINE)
MDR_VK_INSTANCE_FUNCTIONS(MDR_VK_DEFINE)
MDR_VK_DEVICE_FUNCTIONS(MDR_VK_DEFINE)
#undef MDR_VK_DEFINE

void volkInitializeCustom(PFN_vkGetInstanceProcAddr handler)
{
    vkGetInstanceProcAddr = handler;
#define MDR_VK_LOAD(name) name = (PFN_##name)vkGetInstanceProcAddr(NULL, #name);
    MDR_VK_GLOBAL_FUNCTIONS(MDR_VK_LOAD)
#undef MDR_VK_LOAD
}

VkResult volkInitialize(void)
{
    PFN_vkGetInstanceProcAddr gipa = NULL;

    if (SDL_WasInit(SDL_INIT_VIDEO)) {
        if (SDL_Vulkan_LoadLibrary(NULL))
            gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    } else {
        /* Without SDL's video (a test tool): the loader by its usual name. */
#if defined(_WIN32)
        SDL_SharedObject *lib = SDL_LoadObject("vulkan-1.dll");
#elif defined(__APPLE__)
        SDL_SharedObject *lib = SDL_LoadObject("libvulkan.1.dylib");
#else
        SDL_SharedObject *lib = SDL_LoadObject("libvulkan.so.1");
#endif
        if (lib)
            gipa = (PFN_vkGetInstanceProcAddr)SDL_LoadFunction(lib, "vkGetInstanceProcAddr");
    }
    if (!gipa)
        return VK_ERROR_INITIALIZATION_FAILED;
    volkInitializeCustom(gipa);
    return vkCreateInstance ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
}

void volkLoadInstance(VkInstance instance)
{
#define MDR_VK_LOAD(name) name = (PFN_##name)vkGetInstanceProcAddr(instance, #name);
    MDR_VK_INSTANCE_FUNCTIONS(MDR_VK_LOAD)
    MDR_VK_DEVICE_FUNCTIONS(MDR_VK_LOAD)
#undef MDR_VK_LOAD
}

void volkLoadDevice(VkDevice device)
{
#define MDR_VK_LOAD(name) { PFN_##name f = (PFN_##name)vkGetDeviceProcAddr(device, #name); if (f) name = f; }
    MDR_VK_DEVICE_FUNCTIONS(MDR_VK_LOAD)
#undef MDR_VK_LOAD
}
