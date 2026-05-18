#ifndef MCP_EXT_WSL_COMMON_H
#define MCP_EXT_WSL_COMMON_H

#include <cstdlib>
#include <filesystem>
#include <regex>
#include <string>

namespace mcp_ext::wsl {

namespace detail {

inline std::filesystem::path home_directory(const char* home_dir = nullptr) {
    if (home_dir == nullptr) {
        home_dir = std::getenv("HOME");
    }
    if (home_dir == nullptr) {
        home_dir = "/home";
    }
    return std::filesystem::absolute(home_dir).lexically_normal();
}

inline bool contains_parent_reference(const std::string& path) {
    for (const auto& part : std::filesystem::path(path)) {
        if (part == "..") {
            return true;
        }
    }
    return false;
}

inline bool is_within_directory(
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

inline bool contains_dangerous_characters(const std::string& path) {
    static const std::regex dangerous("[;&|$`\"'<>!*?]");
    return std::regex_search(path, dangerous);
}

} // namespace detail

inline std::filesystem::path home_directory(const char* home_dir = nullptr) {
    return detail::home_directory(home_dir);
}

inline std::filesystem::path workspace_directory(const char* home_dir = nullptr) {
    return (home_directory(home_dir) / ".wsl_workspace").lexically_normal();
}

inline std::filesystem::path reports_directory(const char* home_dir = nullptr) {
    return (workspace_directory(home_dir) / ".reports").lexically_normal();
}

inline std::filesystem::path trash_files_directory(const char* home_dir = nullptr) {
    return (home_directory(home_dir) / ".local/share/Trash/files").lexically_normal();
}

inline std::filesystem::path trash_info_directory(const char* home_dir = nullptr) {
    return (home_directory(home_dir) / ".local/share/Trash/info").lexically_normal();
}

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

        if (detail::contains_dangerous_characters(path)) {
            return {false, "Path contains dangerous characters", {}};
        }
        if (detail::contains_parent_reference(path)) {
            return {false, "Path traversal not allowed", {}};
        }
        if (!path.empty() && path[0] == '~') {
            return {false, "Tilde expansion not allowed", {}};
        }

        fs::path input_path(path);
        fs::path workspace = workspace_directory(home_dir);
        if (path.empty()) {
            return {true, "", workspace};
        }

        if (input_path.is_absolute()) {
            fs::path normalized_input = fs::absolute(input_path).lexically_normal();
            if (!detail::is_within_directory(normalized_input, workspace)) {
                return {false, "Absolute path must be under " + workspace.string(), {}};
            }
            return {true, "", normalized_input};
        }

        fs::path resolved = (workspace / input_path).lexically_normal();
        if (!detail::is_within_directory(resolved, workspace)) {
            return {false, "Path resolves outside workspace", {}};
        }
        return {true, "", resolved};
    }
};

class HomePathValidator {
public:
    struct ValidationResult {
        bool valid;
        std::string error;
        std::filesystem::path resolved_path;
    };

    static ValidationResult validate(const std::string& path, const char* home_dir = nullptr) {
        namespace fs = std::filesystem;

        if (path.empty()) {
            return {false, "Path is required", {}};
        }
        if (detail::contains_dangerous_characters(path)) {
            return {false, "Path contains dangerous characters", {}};
        }
        if (detail::contains_parent_reference(path)) {
            return {false, "Path traversal not allowed", {}};
        }
        if (!path.empty() && path[0] == '~') {
            return {false, "Tilde expansion not allowed", {}};
        }

        fs::path input_path(path);
        if (!input_path.is_absolute()) {
            return {false, "Absolute path is required", {}};
        }

        fs::path home = home_directory(home_dir);
        fs::path normalized_input = fs::absolute(input_path).lexically_normal();
        if (!detail::is_within_directory(normalized_input, home)) {
            return {false, "Path must be under " + home.string(), {}};
        }
        if (detail::is_within_directory(normalized_input, trash_files_directory(home_dir)) ||
            detail::is_within_directory(normalized_input, trash_info_directory(home_dir))) {
            return {false, "Trash paths are not allowed", {}};
        }

        return {true, "", normalized_input};
    }
};

} // namespace mcp_ext::wsl

#endif // MCP_EXT_WSL_COMMON_H
