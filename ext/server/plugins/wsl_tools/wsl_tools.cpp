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
extern "C" char* wsl_scan_files_handler(const json& req);
extern "C" char* wsl_recommend_cleanup_handler(const json& req);
extern "C" char* wsl_safe_delete_handler(const json& req);

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

static const char* INPUT_SCHEMA_SCAN_FILES = R"SCHEMA({
    "type": "object",
    "properties": {
        "distro": {
            "type": "string",
            "description": "WSL distribution name. Only the current distro is supported."
        },
        "start_date": {
            "type": "string",
            "description": "Start date in YYYY-MM-DD format."
        },
        "end_date": {
            "type": "string",
            "description": "End date in YYYY-MM-DD format. Defaults to today."
        },
        "min_size_mb": {
            "type": "number",
            "description": "Minimum file size in MB. Defaults to 0."
        },
        "include_patterns": {
            "type": "array",
            "items": {"type": "string"},
            "description": "Optional glob patterns matched against paths relative to $HOME."
        },
        "exclude_patterns": {
            "type": "array",
            "items": {"type": "string"},
            "description": "Optional glob patterns excluded from the scan."
        }
    },
    "required": ["start_date"]
})SCHEMA";

static const char* INPUT_SCHEMA_RECOMMEND_CLEANUP = R"SCHEMA({
    "type": "object",
    "properties": {
        "distro": {
            "type": "string",
            "description": "WSL distribution name. Only the current distro is supported."
        },
        "scan_id": {
            "type": "string",
            "description": "scan_id returned by wsl_scan_files."
        },
        "aggressiveness": {
            "type": "string",
            "enum": ["safe", "moderate", "aggressive"],
            "default": "safe",
            "description": "Cleanup aggressiveness."
        }
    },
    "required": ["scan_id"]
})SCHEMA";

static const char* INPUT_SCHEMA_SAFE_DELETE = R"SCHEMA({
    "type": "object",
    "properties": {
        "distro": {
            "type": "string",
            "description": "WSL distribution name. Only the current distro is supported."
        },
        "paths": {
            "type": "array",
            "items": {"type": "string"},
            "description": "Absolute paths under $HOME to move to trash."
        },
        "require_confirmation": {
            "type": "boolean",
            "default": true,
            "description": "When true, return a confirmation payload unless confirmed is true."
        },
        "confirmed": {
            "type": "boolean",
            "default": false,
            "description": "Set to true after user confirmation to perform the move."
        }
    },
    "required": ["paths"]
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
    },
    {
        "wsl_scan_files",
        "Scan files under the current WSL home directory and save a JSON report.",
        INPUT_SCHEMA_SCAN_FILES
    },
    {
        "wsl_recommend_cleanup",
        "Generate cleanup recommendations from a wsl_scan_files report.",
        INPUT_SCHEMA_RECOMMEND_CLEANUP
    },
    {
        "wsl_safe_delete",
        "Move confirmed files or directories under $HOME to the WSL trash.",
        INPUT_SCHEMA_SAFE_DELETE
    }
};

static const ToolHandler handlers[] = {
    wsl_create_directory_handler,
    wsl_list_distros_handler,
    wsl_scan_files_handler,
    wsl_recommend_cleanup_handler,
    wsl_safe_delete_handler
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
