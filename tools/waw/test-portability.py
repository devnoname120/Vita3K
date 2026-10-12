from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'vita3k/renderer/src/vulkan/renderer.cpp').read_text()
start = source.index('bool VKState::create(')
start = source.index('        const std::set<std::string> optional_instance_extensions', start)
end = source.index('        // look if we can use the validation layer', start)
selection = source[start:end].replace('vk::enumerateInstanceExtensionProperties()', 'available')
start = source.index('        vk::InstanceCreateInfo instance_info{', end)
end = source.index('        instance_info.setPEnabledLayerNames', start)
creation = source[start:end]
out = ROOT / 'build/waw-checks'
program = r'''
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_METAL_EXT
#define VULKAN_HPP_NO_CONSTRUCTORS
#include <vulkan/vulkan.hpp>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <set>
#include <string>
#include <string_view>
#include <vector>
static PFN_vkGetInstanceProcAddr get_proc;
static unsigned checks;
static vk::ExtensionProperties prop(const char *name) {
    vk::ExtensionProperties p{}; std::strncpy(p.extensionName.data(), name, VK_MAX_EXTENSION_NAME_SIZE-1); return p;
}
static void check(std::vector<const char *> instance_extensions, const std::vector<vk::ExtensionProperties> &available, bool expected, bool live=false) {
''' + selection + r'''
    const void *instance_create_pnext=nullptr;
    const vk::ApplicationInfo app_info{.apiVersion=VK_API_VERSION_1_0};
''' + creation + r'''
    unsigned ports=0;
    for (const auto *name : instance_extensions) ports += std::string_view(name)==VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    assert(ports==unsigned(expected));
    assert(bool(instance_info.flags & vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR)==expected);
    assert(std::count_if(instance_extensions.begin(),instance_extensions.end(),[](const char *s){return std::string_view(s)==VK_KHR_SURFACE_EXTENSION_NAME;})==1);
    assert(std::count_if(instance_extensions.begin(),instance_extensions.end(),[](const char *s){return std::string_view(s)==VK_EXT_METAL_SURFACE_EXTENSION_NAME;})==1);
    ++checks;
    if (live) {
        instance_info.setPEnabledExtensionNames(instance_extensions);
        auto create=reinterpret_cast<PFN_vkCreateInstance>(get_proc(nullptr,"vkCreateInstance"));
        VkInstance instance=nullptr;
        VkResult result=create(reinterpret_cast<const VkInstanceCreateInfo *>(&instance_info),nullptr,&instance);
        std::printf("ACTUAL_MOLTENVK_CREATE %d\n",result); assert(result==VK_SUCCESS && instance);
        auto enumerate=reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get_proc(instance,"vkEnumeratePhysicalDevices"));
        unsigned count=0;assert(enumerate(instance,&count,nullptr)==VK_SUCCESS && count>0);
        std::printf("PHYSICAL_DEVICES %u\n",count);
        reinterpret_cast<PFN_vkDestroyInstance>(get_proc(instance,"vkDestroyInstance"))(instance,nullptr);
    }
}
int main(int argc,char **argv) {
    const char *port=VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    for (bool supported : {false,true}) for (unsigned duplicate : {0u,1u,2u,3u}) {
        std::vector<const char *> enabled={VK_KHR_SURFACE_EXTENSION_NAME,VK_EXT_METAL_SURFACE_EXTENSION_NAME};
        enabled.insert(enabled.end(),duplicate,port);
        std::vector<vk::ExtensionProperties> available={prop(VK_KHR_SURFACE_EXTENSION_NAME),prop(VK_EXT_METAL_SURFACE_EXTENSION_NAME)};
        if(supported) available.push_back(prop(port));
        check(enabled,available,supported);
    }
    if(argc==2) {
        void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(lib);
        get_proc=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib,"vkGetInstanceProcAddr"));assert(get_proc);
        auto enumerate=reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(get_proc(nullptr,"vkEnumerateInstanceExtensionProperties"));
        unsigned count=0;assert(enumerate(nullptr,&count,nullptr)==VK_SUCCESS);
        std::vector<vk::ExtensionProperties> available(count);
        assert(enumerate(nullptr,&count,reinterpret_cast<VkExtensionProperties *>(available.data()))==VK_SUCCESS);
        available.resize(count);
        bool supported=std::any_of(available.begin(),available.end(),[&](const auto &p){return std::string_view(p.extensionName.data())==port;});
        check({VK_KHR_SURFACE_EXTENSION_NAME,VK_EXT_METAL_SURFACE_EXTENSION_NAME,port},available,supported,true);
    }
    std::printf("%u actual source selection/flag cases passed\n",checks);
}
'''
(out / 'portability-test.cpp').write_text(program)
subprocess.run(['clang++','-std=c++23','-O0','-g','-I',str(ROOT/'external/VulkanMemoryAllocator-Hpp/Vulkan-Headers/include'),str(out/'portability-test.cpp'),'-o',str(out/'portability-test')],check=True)
subprocess.run([str(out/'portability-test'),str(Path.home()/'Applications/Vita3K-WAW.app/Contents/Frameworks/libMoltenVK.dylib')],check=True)
