#include "plugin_loader.h"
#include <filesystem>
#include <dlfcn.h>
#include <iostream>

namespace mcp_ext {

PluginLoader::PluginLoader() {}

PluginLoader::~PluginLoader() {
    unloadPlugins();
}

bool PluginLoader::loadPlugins(const std::string& directory) {
    namespace fs = std::filesystem;

    if (!fs::exists(directory)) {
        std::cerr << "Plugin directory not found: " << directory << std::endl;
        return false;
    }

    for (const auto& dir_entry : fs::directory_iterator(directory)) {
        if (!dir_entry.is_regular_file()) continue;

        std::string path = dir_entry.path().string();
        if (!isPluginFile(dir_entry.path().filename().string())) continue;

        // Load dynamic library
        void* handle = dlopen(path.c_str(), RTLD_LAZY);
        if (!handle) {
            std::cerr << "Failed to load plugin: " << path << " - " << dlerror() << std::endl;
            continue;
        }

        // Get factory functions
        auto createFunc = reinterpret_cast<ToolPluginAPI* (*)()>(
            dlsym(handle, "CreateToolPlugin"));
        auto destroyFunc = reinterpret_cast<void (*)(ToolPluginAPI*)>(
            dlsym(handle, "DestroyToolPlugin"));

        if (!createFunc || !destroyFunc) {
            std::cerr << "Invalid plugin: " << path << " - missing required functions" << std::endl;
            dlclose(handle);
            continue;
        }

        // Create plugin instance
        ToolPluginAPI* api = createFunc();
        if (!api || !api->tools || api->tool_count <= 0) {
            std::cerr << "Invalid plugin: " << path << " - no tools defined" << std::endl;
            dlclose(handle);
            continue;
        }

        // Store plugin entry
        auto plugin_entry = std::make_unique<PluginEntry>();
        plugin_entry->handle = handle;
        plugin_entry->api = api;
        plugin_entry->path = path;
        plugins_.push_back(std::move(plugin_entry));

        // Log each tool in the plugin
        for (int i = 0; i < api->tool_count; i++) {
            std::cout << "Loaded plugin tool: " << api->tools[i].name << std::endl;
        }
    }

    return true;
}

void PluginLoader::unloadPlugins() {
    plugins_.clear();
}

std::vector<ToolPluginAPI*> PluginLoader::getPlugins() const {
    std::vector<ToolPluginAPI*> result;
    result.reserve(plugins_.size());
    for (const auto& entry : plugins_) {
        result.push_back(entry->api);
    }
    return result;
}

bool PluginLoader::isPluginFile(const std::string& filename) const {
    return filename.find("lib") == 0 && filename.find(".so") != std::string::npos;
}

PluginLoader::PluginEntry::~PluginEntry() {
    if (api) {
        auto destroyFunc = reinterpret_cast<void (*)(ToolPluginAPI*)>(
            dlsym(handle, "DestroyToolPlugin"));
        if (destroyFunc) {
            destroyFunc(api);
        }
    }
    if (handle) {
        dlclose(handle);
    }
}

} // namespace mcp_ext