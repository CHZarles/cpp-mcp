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
extern "C" char* wsl_get_scan_status_handler(const json& req);
extern "C" char* wsl_get_scan_report_handler(const json& req);
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
        "roots": {
            "type": "array",
            "items": {"type": "string"},
            "description": "Optional absolute or workspace-relative directories to scan. Defaults to ~/.wsl_workspace, not the entire $HOME."
        },
        "paths": {
            "type": "array",
            "items": {"type": "string"},
            "description": "Optional absolute or workspace-relative file or directory paths to scan. Use this to scan specific files or directories outside the default ~/.wsl_workspace scope."
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
        },
        "max_files": {
            "type": "integer",
            "description": "Maximum number of files to inspect before truncating the scan."
        },
        "max_seconds": {
            "type": "integer",
            "description": "Maximum runtime in seconds before truncating the scan."
        },
        "max_depth": {
            "type": "integer",
            "description": "Maximum directory depth to recurse into before truncating the scan."
        }
    },
    "required": ["start_date"]
})SCHEMA";

static const char* INPUT_SCHEMA_RECOMMEND_CLEANUP = R"SCHEMA({
    "type": "object",
    "properties": {
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

static const char* INPUT_SCHEMA_SCAN_READ = R"SCHEMA({
    "type": "object",
    "properties": {
        "scan_id": {
            "type": "string",
            "description": "scan_id returned by wsl_scan_files."
        }
    },
    "required": ["scan_id"]
})SCHEMA";

static const char* INPUT_SCHEMA_SAFE_DELETE = R"SCHEMA({
    "type": "object",
    "properties": {
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
        "Start an asynchronous WSL file scan job. The tool returns immediately with scan_id, status_uri, and report_uri; read status_uri until completed or failed, then read report_uri. Defaults to ~/.wsl_workspace, not the entire $HOME; pass roots or paths to scan other $HOME locations.",
        INPUT_SCHEMA_SCAN_FILES
    },
    {
        "wsl_get_scan_status",
        "Read scan status by scan_id. This is a read-only tool wrapper for clients that cannot call MCP resources/read on wsl://scan/{scan_id}/status.",
        INPUT_SCHEMA_SCAN_READ
    },
    {
        "wsl_get_scan_report",
        "Read scan report by scan_id. This is a read-only tool wrapper for clients that cannot call MCP resources/read on wsl://scan/{scan_id}/report.",
        INPUT_SCHEMA_SCAN_READ
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
    wsl_get_scan_status_handler,
    wsl_get_scan_report_handler,
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
