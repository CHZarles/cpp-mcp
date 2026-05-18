#include <iostream>
#include <filesystem>
#include "mcp_server.h"
#include "plugin_loader.h"

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
    conf.host = "localhost";
    conf.port = 8888;

    mcp::server server(conf);
    server.set_server_info("MCP-Ext-Server", "1.0.0");

    // Register capabilities
    mcp::json capabilities = {
        {"tools", mcp::json::object()}
    };
    server.set_capabilities(capabilities);

    // Register tools from plugins (one tool per .so)
    for (auto* plugin : loader.getPlugins()) {
        // Each .so now provides one tool (tool_count should be 1)
        for (int tool_idx = 0; tool_idx < plugin->tool_count; tool_idx++) {
            const auto& tool_def = plugin->tools[tool_idx];

            // Parse INPUT_SCHEMA from plugin
            mcp::json input_schema = mcp::json::parse(tool_def.inputSchema);

            mcp::tool tool = mcp::tool_builder(tool_def.name)
                .with_description(tool_def.description)
                .with_input_schema(input_schema)
                .build();

            // Capture plugin and tool_index
            int captured_idx = tool_idx;
            mcp::tool_handler handler = [plugin, captured_idx](
                const mcp::json& params,
                const std::string& /* session_id */) -> mcp::json {

                using json = mcp::json;

                // Convert params to string and call plugin handler with tool index
                std::string request = params.dump();
                char* result_str = plugin->HandleRequest(captured_idx, request.c_str());

                if (!result_str) {
                    throw mcp::mcp_exception(mcp::error_code::internal_error,
                        "Plugin returned null");
                }

                std::string result(result_str);
                free(result_str);

                json plugin_result = mcp::json::parse(result);

                return plugin_result;
            };

            server.register_tool(tool, handler);
            std::cout << "Registered tool: " << tool_def.name << std::endl;
        }
    }

    // Start server
    std::cout << "Starting MCP server at " << conf.host << ":" << conf.port << std::endl;
    server.start(true);

    return 0;
}