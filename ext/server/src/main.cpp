#include <iostream>
#include <filesystem>
#include <cstdlib>
#include "mcp_server.h"
#include "plugin_loader.h"
#include "plugin_registry.h"

int main() {
    namespace fs = std::filesystem;

    // Ensure directories exist
    fs::create_directories("./plugins");

    // Determine plugin directory
    std::string plugin_dir = "./plugins";
    #ifdef __linux__
    // On Linux, also check relative to executable location
    char exe_path[1024];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len != -1) {
        exe_path[len] = '\0';
        std::string exe_dir = std::filesystem::path(exe_path).parent_path().string();

        // Check relative to executable: ../plugins (from ext/server/)
        std::string relative_plugin_dir = exe_dir + "/../plugins";
        if (std::filesystem::exists(relative_plugin_dir)) {
            plugin_dir = relative_plugin_dir;
        }
    }
    #endif

    // Initialize plugin loader
    mcp_ext::PluginLoader loader;
    if (!loader.loadPlugins(plugin_dir)) {
        std::cerr << "Failed to load plugins from " << plugin_dir << std::endl;
        return 1;
    }

    std::cout << "Loaded " << loader.getPlugins().size() << " plugin libraries" << std::endl;

    // Create MCP server
    mcp::server::configuration conf;
    if (const char* h = std::getenv("MCP_LISTEN_HOST"); h && *h) {
        conf.host = h;
    } else {
        conf.host = "localhost";
    }
    if (const char* p = std::getenv("MCP_LISTEN_PORT"); p && *p) {
        conf.port = std::atoi(p);
    } else {
        conf.port = 8888;
    }

    mcp::server server(conf);
    server.set_server_info("MCP-Ext-Server", "1.0.0");

    // Register capabilities
    mcp::json capabilities = {
        {"tools", mcp::json::object()}
    };
    server.set_capabilities(capabilities);

    mcp_ext::register_plugin_tools(server, loader);

    // Start server
    std::cout << "Starting MCP server at " << conf.host << ":" << conf.port << std::endl;
    server.start(true);

    return 0;
}
