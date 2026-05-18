/**
 * @file wsl_create_directory.cpp
 * @brief WSL Create Directory Tool
 */

#include "tool_api.h"
#include <string>
#include <cstdlib>
#include <filesystem>
#include <regex>
#include <iostream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

class PathValidator {
public:
    static constexpr const char* WORKSPACE_DIR = ".wsl_workspace";

    struct ValidationResult {
        bool valid;
        std::string error;
        fs::path resolved_path;
    };

    static ValidationResult validate(const std::string& path, const char* home_dir = nullptr) {
        if (home_dir == nullptr) home_dir = std::getenv("HOME");
        if (home_dir == nullptr) home_dir = "/home";

        static const std::regex DANGEROUS("[;&|$`\"'<>!*?]");
        if (std::regex_search(path, DANGEROUS)) {
            return {false, "Path contains dangerous characters", {}};
        }
        if (path.find("..") != std::string::npos) {
            return {false, "Path traversal not allowed", {}};
        }
        if (!path.empty() && path[0] == '~') {
            return {false, "Tilde expansion not allowed", {}};
        }

        fs::path input_path(path);
        if (path.empty()) {
            return {true, "", fs::absolute(fs::path(home_dir) / WORKSPACE_DIR)};
        }
        if (input_path.is_absolute()) {
            // Only allow absolute paths under user's home directory
            std::string home_path = fs::absolute(home_dir).string();
            std::string input_str = fs::absolute(input_path).string();
            if (input_str.rfind(home_path, 0) != 0) {
                return {false, "Absolute path must be under " + home_path, {}};
            }
            return {true, "", fs::absolute(input_path)};
        }

        // Relative paths are always relative to workspace
        fs::path resolved = fs::absolute(fs::path(home_dir) / WORKSPACE_DIR / input_path);
        fs::path normalized = resolved.lexically_normal();
        fs::path workspace = fs::absolute(fs::path(home_dir) / WORKSPACE_DIR).lexically_normal();

        std::string norm_str = normalized.string();
        std::string ws_str = workspace.string();
        if (norm_str.compare(0, ws_str.length(), ws_str) != 0) {
            return {false, "Path resolves outside workspace", {}};
        }
        return {true, "", resolved};
    }
};

static char* handleCreateDirectory(int tool_index, const json& req) {
    std::string path = req.value("path", "");
    std::string distro = req.value("distro", "");

    // Validate path
    auto validation = PathValidator::validate(path);
    if (!validation.valid) {
        json content_item;
        content_item["type"] = "text";
        content_item["text"] = "Validation error: " + validation.error;

        json response;
        response["content"] = json::array({content_item});
        response["isError"] = false;
        return strdup(response.dump().c_str());
    }

    if (distro.empty()) {
        std::error_code ec;
        if (fs::create_directories(validation.resolved_path, ec)) {
            json content_item;
            content_item["type"] = "text";
            content_item["text"] = "Created directory: " + validation.resolved_path.string();

            json response;
            response["content"] = json::array({content_item});
            response["isError"] = false;
            return strdup(response.dump().c_str());
        } else {
            json content_item;
            content_item["type"] = "text";
            content_item["text"] = "Error: " + ec.message();

            json response;
            response["content"] = json::array({content_item});
            response["isError"] = false;
            return strdup(response.dump().c_str());
        }
    }

    json content_item;
    content_item["type"] = "text";
    content_item["text"] = "Cross-distro not implemented";

    json response;
    response["content"] = json::array({content_item});
    response["isError"] = false;
    return strdup(response.dump().c_str());
}

static char* handleRequest(int tool_index, const char* request_json) {
    try {
        json req = json::parse(request_json);
        return handleCreateDirectory(tool_index, req);
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

static const char* INPUT_SCHEMA = R"SCHEMA({
    "type": "object",
    "properties": {
        "path": {
            "type": "string",
            "description": "Directory name or path relative to ~/.wsl_workspace (e.g., 'myproject' creates ~/.wsl_workspace/myproject). For absolute paths, must be under /home/charles/"
        },
        "distro": {
            "type": "string",
            "description": "WSL distribution name (optional, cross-distro not implemented)"
        }
    },
    "required": []
})SCHEMA";

static ToolPlugin create_directory_tool = {
    "wsl_create_directory",
    "Create a directory in WSL filesystem.",
    INPUT_SCHEMA
};

static ToolPluginAPI plugin_api = {
    &create_directory_tool,
    1,
    handleRequest
};

extern "C" {
    TOOL_PLUGIN_API ToolPluginAPI* CreateToolPlugin() {
        return &plugin_api;
    }

    TOOL_PLUGIN_API void DestroyToolPlugin(ToolPluginAPI*) {
    }
}