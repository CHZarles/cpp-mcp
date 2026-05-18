/**
 * @file wsl_tools.cpp
 * @brief WSL Tools Plugin - Entry point
 *
 * This file assembles multiple WSL tool implementations into a single plugin.
 */

#include "tool_api.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Declare handlers from individual tool files
extern "C" char* wsl_create_directory_handler(const json& req);
extern "C" char* wsl_list_distros_handler(const json& req);

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

// ============================================================================
// Plugin entry point
// ============================================================================
static char* handleRequest(int tool_index, const char* request_json) {
    try {
        json req = json::parse(request_json);

        switch (tool_index) {
            case 0: return wsl_create_directory_handler(req);
            case 1: return wsl_list_distros_handler(req);
            default: {
                json content_item;
                content_item["type"] = "text";
                content_item["text"] = "Unknown tool index: " + std::to_string(tool_index);

                json response;
                response["content"] = json::array({content_item});
                response["isError"] = true;
                return strdup(response.dump().c_str());
            }
        }
    } catch (const std::exception& e) {
        json content_item;
        content_item["type"] = "text";
        content_item["text"] = std::string("Error: ") + e.what();

        json response;
        response["content"] = json::array({content_item});
        response["isError"] = true;
        return strdup(response.dump().c_str());
    }
}

static ToolPluginAPI plugin_api = {
    tools,
    2,
    handleRequest
};

extern "C" {
    TOOL_PLUGIN_API ToolPluginAPI* CreateToolPlugin() {
        return &plugin_api;
    }

    TOOL_PLUGIN_API void DestroyToolPlugin(ToolPluginAPI*) {
    }
}