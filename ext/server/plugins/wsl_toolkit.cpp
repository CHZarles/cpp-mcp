/**
 * @file wsl_toolkit.cpp
 * @brief WSL Toolkit Plugin - File and directory operations for WSL environments
 *
 * This plugin provides tools for creating directories and managing WSL filesystem
 * operations with security validation and path sanitization.
 */

#include "tool_api.h"
#include <string>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <regex>
#include <vector>
#include <sstream>
#include <iostream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace fs = std::filesystem;

// ============================================================================
// Security Validation
// ============================================================================

class PathValidator {
public:
    static constexpr const char* WORKSPACE_DIR = ".wsl_workspace";

    /**
     * @struct ValidationResult
     * @brief Result of path validation
     */
    struct ValidationResult {
        bool valid;
        std::string error;
        fs::path resolved_path;
    };

    /**
     * @brief Validate and resolve a path for directory creation
     * @param path Input path (relative or absolute)
     * @param home_dir User's home directory
     * @return ValidationResult with resolved path or error
     */
    static ValidationResult validate(const std::string& path, const char* home_dir = nullptr) {
        if (home_dir == nullptr) {
            home_dir = std::getenv("HOME");
        }
        if (home_dir == nullptr) {
            home_dir = "/home";
        }

        // Check for dangerous characters
        static const std::regex DANGEROUS_CHARS(R"([;&|$`\"'<>*?!\\]|^\s|\s$)");
        if (std::regex_search(path, DANGEROUS_CHARS)) {
            return {false, "Path contains dangerous characters", {}};
        }

        // Check for path traversal attempts
        if (path.find("..") != std::string::npos) {
            return {false, "Path traversal not allowed", {}};
        }

        // Check for absolute symlink attempts (starting with ~)
        if (!path.empty() && path[0] == '~') {
            return {false, "Tilde expansion not allowed, use absolute path from /home/", {}};
        }

        fs::path input_path(path);

        if (path.empty()) {
            // No path specified - return workspace root
            fs::path workspace = fs::path(home_dir) / WORKSPACE_DIR;
            return {true, "", fs::absolute(workspace)};
        }

        if (input_path.is_absolute()) {
            // Absolute path - verify it's under /home/
            std::string path_str = path;
            if (path_str.rfind("/home/", 0) != 0) {
                return {false, "Absolute paths must be under /home/", {}};
            }
            return {true, "", fs::absolute(input_path)};
        } else {
            // Relative path - prepend workspace
            fs::path workspace = fs::path(home_dir) / WORKSPACE_DIR;
            fs::path resolved = workspace / input_path;

            // Normalize and verify no path leaves workspace
            fs::path normalized = resolved.lexically_normal();
            fs::path workspace_normalized = workspace.lexically_normal();

            // Check if normalized path starts with workspace
            std::string normalized_str = normalized.string();
            std::string workspace_str = workspace_normalized.string();

            if (normalized_str.compare(0, workspace_str.length(), workspace_str) != 0) {
                return {false, "Path resolves outside workspace", {}};
            }

            return {true, "", fs::absolute(resolved)};
        }
    }
};

// ============================================================================
// WSL Distribution Management
// ============================================================================

class WSLDistroManager {
public:
    /**
     * @brief Get list of available WSL distributions
     * @return Vector of distro names (empty on error)
     */
    static std::vector<std::string> get_available_distros() {
        std::vector<std::string> distros;

        FILE* pipe = popen("wsl --list --quiet 2>/dev/null", "r");
        if (pipe == nullptr) {
            return distros;
        }

        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            std::string line(buffer);
            // Trim whitespace
            size_t start = line.find_first_not_of(" \t\r\n");
            size_t end = line.find_last_not_of(" \t\r\n");
            if (start != std::string::npos) {
                line = line.substr(start, end - start + 1);
            } else {
                continue;
            }

            // Skip empty lines and headers
            if (!line.empty() && line != "NAME" && line != "*") {
                // Remove asterisk prefix (default distro marker)
                if (line[0] == '*') {
                    line = line.substr(1);
                    line.erase(0, line.find_first_not_of(" \t"));
                }
                if (!line.empty()) {
                    distros.push_back(line);
                }
            }
        }
        pclose(pipe);

        return distros;
    }

    /**
     * @brief Get the default WSL distribution
     * @return Default distro name, or empty string if none
     */
    static std::string get_default_distro() {
        auto distros = get_available_distros();
        return distros.empty() ? "" : distros[0];
    }

    /**
     * @brief Execute command in a WSL distribution
     * @param distro Target distro name
     * @param command Command to execute
     * @return Exit code (0 = success)
     */
    static int execute_in_distro(const std::string& distro, const std::string& command) {
        std::string full_cmd = "wsl -d " + distro + " -- " + command + " 2>/dev/null";
        return system(full_cmd.c_str());
    }
};

// ============================================================================
// Directory Operations
// ============================================================================

class DirectoryCreator {
public:
    /**
     * @brief Create directory locally (in current WSL instance)
     * @param path Resolved absolute path
     * @return Success message or error
     */
    static std::string create_local(const fs::path& path) {
        if (fs::exists(path)) {
            if (fs::is_directory(path)) {
                return "Directory already exists: " + path.string();
            } else {
                return "Error: Path exists but is not a directory: " + path.string();
            }
        }

        std::error_code ec;
        if (fs::create_directories(path, ec)) {
            return "Created directory: " + path.string();
        } else {
            return "Error creating directory: " + ec.message();
        }
    }

    /**
     * @brief Create directory in a specific WSL distribution
     * @param distro Target WSL distro
     * @param path Resolved absolute path
     * @return Success message or error
     */
    static std::string create_in_distro(const std::string& distro, const fs::path& path) {
        std::string cmd = "mkdir -p \"" + path.string() + "\"";
        int result = WSLDistroManager::execute_in_distro(distro, cmd);

        if (result == 0) {
            return "Created directory in " + distro + ": " + path.string();
        } else {
            return "Error: Failed to create directory in WSL distro (exit code " +
                   std::to_string(result) + ")";
        }
    }
};

// ============================================================================
// Plugin Implementation
// ============================================================================

struct WSLToolkitPlugin {
    ToolPluginAPI api;

    /**
     * @brief Handle create_directory action
     */
    static char* handleCreateDirectory(const json& req) {
        std::string path = req.value("path", "");
        std::string distro = req.value("distro", "");

        // Check if already approved via elicitation
        if (req.value("_elicitation_approved", false)) {
            // Execute the actual directory creation
            auto validation = PathValidator::validate(path);
            if (!validation.valid) {
                json error = {
                    {"type", "text"},
                    {"text", "Validation error: " + validation.error}
                };
                return strdup(json(error).dump().c_str());
            }

            std::string result;
            if (distro.empty()) {
                result = DirectoryCreator::create_local(validation.resolved_path);
            } else {
                result = DirectoryCreator::create_in_distro(distro, validation.resolved_path);
            }

            json response = {
                {"type", "text"},
                {"text", result}
            };
            return strdup(json(response).dump().c_str());
        }

        // Need user consent - validate path first
        auto validation = PathValidator::validate(path);
        if (!validation.valid) {
            // Validation errors are returned directly without elicitation
            json error = {
                {"type", "text"},
                {"text", "Validation error: " + validation.error}
            };
            return strdup(json(error).dump().c_str());
        }

        // Return input_required to request user consent
        std::string full_path = validation.resolved_path.string();
        json pending_response = {
            {"_mcp_type", "input_required"},
            {"message", "即将在 " + (distro.empty() ? "当前 WSL" : distro) +
                        " 中创建目录:\n" + full_path + "\n\n是否继续?"},
            {"requestedSchema", {
                {"type", "object"},
                {"properties", {
                    {"confirm", {
                        {"type", "boolean"},
                        {"description", "确认创建此目录"}
                    }}
                }}
            }}
        };
        return strdup(json(pending_response).dump().c_str());
    }

    /**
     * @brief Handle list_distros action
     */
    static char* handleListDistros() {
        auto distros = WSLDistroManager::get_available_distros();
        std::string default_distro = WSLDistroManager::get_default_distro();

        json response = {
            {"type", "text"},
            {"text", "Available WSL distributions: " + json(distros).dump()},
            {"distros", distros},
            {"default", default_distro}
        };
        return strdup(json(response).dump().c_str());
    }

    /**
     * @brief Main request handler
     */
    static char* handleRequest(const char* request) {
        try {
            json req = json::parse(request);
            std::string action = req.value("action", "");

            if (action == "create_directory") {
                return handleCreateDirectory(req);
            } else if (action == "list_distros") {
                return handleListDistros();
            } else {
                json error = {
                    {"type", "text"},
                    {"text", "Unknown action: " + action + ". Supported: create_directory, list_distros"}
                };
                return strdup(json(error).dump().c_str());
            }
        } catch (const json::parse_error& e) {
            json error = {
                {"type", "text"},
                {"text", std::string("JSON parse error: ") + e.what()}
            };
            return strdup(json(error).dump().c_str());
        } catch (const std::exception& e) {
            json error = {
                {"type", "text"},
                {"text", std::string("Error: ") + e.what()}
            };
            return strdup(json(error).dump().c_str());
        }
    }

    WSLToolkitPlugin() {
        api.tool.name = "wsl_toolkit";
        api.tool.description = R"(
WSL Toolkit - File and directory operations for WSL environments.

Actions:
- create_directory: Create a directory (idempotent - returns success if exists)
- list_distros: List available WSL distributions

Path handling:
- Relative paths: created under ~/.wsl_workspace/
- Absolute paths: must be under /home/
- Path traversal (..) is blocked
- Dangerous characters are rejected

Security features:
- Validates all paths against /home/ restriction
- Blocks path traversal attempts
- Rejects dangerous shell characters
- Idempotent operations (safe to retry)
)";
        api.tool.inputSchema = std::string(
            "{\n"
            "  \"type\": \"object\",\n"
            "  \"properties\": {\n"
            "    \"action\": {\n"
            "      \"type\": \"string\",\n"
            "      \"description\": \"Action to perform\",\n"
            "      \"enum\": [\"create_directory\", \"list_distros\"]\n"
            "    },\n"
            "    \"path\": {\n"
            "      \"type\": \"string\",\n"
            "      \"description\": \"Path for the directory to create. Relative paths go to ~/.wsl_workspace/. Absolute paths must be under /home/. Omit to create workspace root.\"\n"
            "    },\n"
            "    \"distro\": {\n"
            "      \"type\": \"string\",\n"
            "      \"description\": \"Optional WSL distro name (empty for local filesystem)\"\n"
            "    }\n"
            "  },\n"
            "  \"required\": [\"action\"]\n"
            "}"
        ).c_str();
        api.HandleRequest = handleRequest;
    }
};

static WSLToolkitPlugin instance;

extern "C" {
    TOOL_PLUGIN_API ToolPluginAPI* CreateToolPlugin() {
        return &instance.api;
    }

    TOOL_PLUGIN_API void DestroyToolPlugin(ToolPluginAPI*) {
        // Static instance, no cleanup needed
    }
}