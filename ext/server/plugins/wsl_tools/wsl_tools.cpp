/**
 * @file wsl_tools.cpp
 * @brief WSL Tools Plugin - Entry point
 *
 * This file assembles multiple WSL tool implementations into a single plugin.
 */

#include "tool_api.h"
#include "plugin_helpers.h"
#include <nlohmann/json.hpp>
#include <iterator>
#include <string>

using json = nlohmann::json;

// Declare handlers from individual tool files
extern "C" char* wsl_create_directory_handler(const json& req);
extern "C" char* wsl_list_distros_handler(const json& req);

namespace {

using ToolHandler = char* (*)(const json&);

// ============================================================================
// Tool definitions
// ============================================================================
static const char* INPUT_SCHEMA_CREATE_DIR = R"SCHEMA({
    "type": "object",
    "properties": {
        "path": {
            "type": "string",
            "description": "Directory name or path relative to ~/.wsl_workspace"
        },
        "distro": {
            "type": "string",
            "description": "WSL distribution name (optional)"
        }
    },
    "required": []
})SCHEMA";

static const char* INPUT_SCHEMA_LIST_DISTROS = R"SCHEMA({
    "type": "object",
    "properties": {},
    "required": []
})SCHEMA";

static ToolPlugin tools[] = {
    {
        "wsl_create_directory",
        "Create a directory in WSL filesystem.",
        INPUT_SCHEMA_CREATE_DIR
    },
    {
        "wsl_list_distros",
        "List available WSL distributions.",
        INPUT_SCHEMA_LIST_DISTROS
    }
};

static const ToolHandler handlers[] = {
    wsl_create_directory_handler,
    wsl_list_distros_handler
};

static_assert(std::size(handlers) == std::size(tools), "handler table must match tool definitions");

// ============================================================================
// Plugin entry point
// ============================================================================
static char* handleRequest(int tool_index, const char* request_json) {
    try {
        if (tool_index < 0 || tool_index >= static_cast<int>(std::size(handlers))) {
            return mcp_ext::plugin::make_error_result(
                "Unknown tool index: " + std::to_string(tool_index));
        }
        if (!request_json) {
            return mcp_ext::plugin::make_error_result("Request JSON is null");
        }

        json req = json::parse(request_json);
        return handlers[tool_index](req);
    } catch (const std::exception& e) {
        return mcp_ext::plugin::make_error_result(e.what());
    }
}

static ToolPluginAPI plugin_api = {
    tools,
    static_cast<int>(std::size(tools)),
    handleRequest
};

} // namespace

extern "C" {
    TOOL_PLUGIN_API ToolPluginAPI* CreateToolPlugin() {
        return &plugin_api;
    }

    TOOL_PLUGIN_API void DestroyToolPlugin(ToolPluginAPI*) {
    }
}
