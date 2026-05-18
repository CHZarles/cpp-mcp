#ifndef MCP_EXT_WSL_COMMON_H
#define MCP_EXT_WSL_COMMON_H

#include <cstdlib>
#include <filesystem>
#include <regex>
#include <string>

namespace mcp_ext::wsl {

class PathValidator {
public:
    static constexpr const char* WORKSPACE_DIR = ".wsl_workspace";

    struct ValidationResult {
        bool valid;
        std::string error;
        std::filesystem::path resolved_path;
    };

    static ValidationResult validate(const std::string& path, const char* home_dir = nullptr) {
        namespace fs = std::filesystem;

        if (home_dir == nullptr) {
            home_dir = std::getenv("HOME");
        }
        if (home_dir == nullptr) {
            home_dir = "/home";
        }

        static const std::regex dangerous("[;&|$`\"'<>!*?]");
        if (std::regex_search(path, dangerous)) {
            return {false, "Path contains dangerous characters", {}};
        }
        if (contains_parent_reference(path)) {
            return {false, "Path traversal not allowed", {}};
        }
        if (!path.empty() && path[0] == '~') {
            return {false, "Tilde expansion not allowed", {}};
        }

        fs::path input_path(path);
        fs::path home_path = fs::absolute(home_dir);
        fs::path workspace = (home_path / WORKSPACE_DIR).lexically_normal();
        if (path.empty()) {
            return {true, "", workspace};
        }

        if (input_path.is_absolute()) {
            fs::path normalized_input = fs::absolute(input_path).lexically_normal();
            if (!is_within_directory(normalized_input, workspace)) {
                return {false, "Absolute path must be under " + workspace.string(), {}};
            }
            return {true, "", normalized_input};
        }

        fs::path resolved = (workspace / input_path).lexically_normal();
        if (!is_within_directory(resolved, workspace)) {
            return {false, "Path resolves outside workspace", {}};
        }
        return {true, "", resolved};
    }

private:
    static bool contains_parent_reference(const std::string& path) {
        for (const auto& part : std::filesystem::path(path)) {
            if (part == "..") {
                return true;
            }
        }
        return false;
    }

    static bool is_within_directory(
        const std::filesystem::path& child,
        const std::filesystem::path& parent) {

        std::string child_str = child.lexically_normal().string();
        std::string parent_str = parent.lexically_normal().string();
        if (child_str == parent_str) {
            return true;
        }
        if (parent_str.back() != std::filesystem::path::preferred_separator) {
            parent_str += std::filesystem::path::preferred_separator;
        }
        return child_str.rfind(parent_str, 0) == 0;
    }
};

} // namespace mcp_ext::wsl

#endif // MCP_EXT_WSL_COMMON_H
