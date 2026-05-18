#include "plugin_registry.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace mcp_ext {

namespace {

mcp::tool_handler make_tool_handler(ToolPluginAPI* plugin, int tool_index) {
    return [plugin, tool_index](
        const mcp::json& params,
        const std::string& /* session_id */) -> mcp::json {

        std::string request = params.dump();
        char* result_str = plugin->HandleRequest(tool_index, request.c_str());
        if (!result_str) {
            throw mcp::mcp_exception(
                mcp::error_code::internal_error,
                "Plugin returned null");
        }

        std::string result(result_str);
        free(result_str);
        return mcp::json::parse(result);
    };
}

void register_tool(mcp::server& server, ToolPluginAPI* plugin, int tool_index) {
    const auto& tool_def = plugin->tools[tool_index];
    mcp::json input_schema = mcp::json::parse(tool_def.inputSchema);

    mcp::tool tool = mcp::tool_builder(tool_def.name)
        .with_description(tool_def.description)
        .with_input_schema(input_schema)
        .build();

    server.register_tool(tool, make_tool_handler(plugin, tool_index));
    std::cout << "Registered tool: " << tool_def.name << std::endl;
}

} // namespace

void register_plugin_tools(mcp::server& server, const PluginLoader& loader) {
    for (auto* plugin : loader.getPlugins()) {
        for (int tool_index = 0; tool_index < plugin->tool_count; tool_index++) {
            register_tool(server, plugin, tool_index);
        }
    }
}

} // namespace mcp_ext
