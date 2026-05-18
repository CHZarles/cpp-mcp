#ifndef PLUGIN_LOADER_H
#define PLUGIN_LOADER_H

#include <string>
#include <vector>
#include <memory>

#include "../plugins/tool_api.h"

namespace mcp_ext {

class PluginLoader {
public:
    PluginLoader();
    ~PluginLoader();

    // Load all plugins from directory
    bool loadPlugins(const std::string& directory);

    // Unload all plugins
    void unloadPlugins();

    // Get loaded plugins
    std::vector<ToolPluginAPI*> getPlugins() const;

private:
    struct PluginEntry {
        void* handle;
        ToolPluginAPI* api;
        std::string path;

        ~PluginEntry();
    };

    std::vector<std::unique_ptr<PluginEntry>> plugins_;

    bool isPluginFile(const std::string& filename) const;
};

} // namespace mcp_ext

#endif // PLUGIN_LOADER_H
