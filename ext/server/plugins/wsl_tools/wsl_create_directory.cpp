/**
 * @file wsl_create_directory.cpp
 * @brief WSL Create Directory Tool
 */

#include "tool_api.h"
#include "plugin_helpers.h"
#include "wsl_common.h"
#include <string>
#include <filesystem>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

// Tool implementation
static char* handleCreateDirectory(const json& req) {
    std::string path = req.value("path", "");
    std::string distro = req.value("distro", "");

    auto validation = mcp_ext::wsl::PathValidator::validate(path);
    if (!validation.valid) {
        return mcp_ext::plugin::make_error_result("Validation error: " + validation.error);
    }

    if (distro.empty()) {
        std::error_code ec;
        if (fs::create_directories(validation.resolved_path, ec)) {
            return mcp_ext::plugin::make_text_result(
                "Created directory: " + validation.resolved_path.string());
        }
        if (ec) {
            return mcp_ext::plugin::make_error_result(ec.message());
        }

        return mcp_ext::plugin::make_text_result(
            "Directory already exists: " + validation.resolved_path.string());
    }

    return mcp_ext::plugin::make_error_result("Cross-distro not implemented");
}

// Export for use in main plugin file
extern "C" char* wsl_create_directory_handler(const json& req) {
    return handleCreateDirectory(req);
}
