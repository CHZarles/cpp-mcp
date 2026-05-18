/**
 * @file wsl_safe_delete.cpp
 * @brief WSL safe delete tool.
 */

#include "plugin_helpers.h"
#include "wsl_common.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct DeleteTarget {
    fs::path path;
    std::uintmax_t size = 0;
};

std::uintmax_t path_size(const fs::path& path) {
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) {
        return fs::file_size(path, ec);
    }
    if (!fs::is_directory(path, ec)) {
        return 0;
    }

    std::uintmax_t total = 0;
    fs::recursive_directory_iterator it(
        path,
        fs::directory_options::skip_permission_denied,
        ec);
    fs::recursive_directory_iterator end;
    while (!ec && it != end) {
        if (it->is_regular_file(ec)) {
            total += it->file_size(ec);
        }
        ec.clear();
        it.increment(ec);
    }
    return total;
}

std::string timestamp() {
    std::time_t now = std::time(nullptr);
    std::tm tm = *std::localtime(&now);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y%m%d%H%M%S");
    return out.str();
}

std::string trash_date() {
    std::time_t now = std::time(nullptr);
    std::tm tm = *std::localtime(&now);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return out.str();
}

fs::path unique_trash_path(const fs::path& source) {
    fs::path trash_dir = mcp_ext::wsl::trash_files_directory();
    std::string stem = source.filename().string();
    if (stem.empty()) {
        stem = "deleted";
    }

    fs::path candidate = trash_dir / (stem + "." + timestamp());
    int suffix = 1;
    std::error_code ec;
    while (fs::exists(candidate, ec)) {
        candidate = trash_dir / (stem + "." + timestamp() + "." + std::to_string(suffix++));
    }
    return candidate;
}

void write_trash_info(const fs::path& original, const fs::path& trash_path) {
    fs::path info_path = mcp_ext::wsl::trash_info_directory() /
        (trash_path.filename().string() + ".trashinfo");

    std::ofstream out(info_path);
    if (!out.is_open()) {
        throw std::runtime_error("Cannot write trash info: " + info_path.string());
    }
    out << "[Trash Info]\n";
    out << "Path=" << original.string() << "\n";
    out << "DeletionDate=" << trash_date() << "\n";
}

std::vector<DeleteTarget> parse_targets(const json& req) {
    if (!req.contains("paths") || !req.at("paths").is_array()) {
        throw std::runtime_error("paths array is required");
    }

    std::vector<DeleteTarget> targets;
    for (const auto& item : req.at("paths")) {
        const std::string path = item.get<std::string>();
        auto validation = mcp_ext::wsl::HomePathValidator::validate(path);
        if (!validation.valid) {
            throw std::runtime_error("Invalid path '" + path + "': " + validation.error);
        }
        std::error_code ec;
        if (!fs::exists(validation.resolved_path, ec)) {
            throw std::runtime_error("Path does not exist: " + validation.resolved_path.string());
        }
        targets.push_back({validation.resolved_path, path_size(validation.resolved_path)});
    }
    if (targets.empty()) {
        throw std::runtime_error("paths array must not be empty");
    }
    return targets;
}

} // namespace

extern "C" char* wsl_safe_delete_handler(const json& req) {
    try {
        std::string distro = req.value("distro", "");
        if (!distro.empty()) {
            return mcp_ext::plugin::make_error_result("Cross-distro delete not implemented");
        }

        std::vector<DeleteTarget> targets = parse_targets(req);
        std::uintmax_t total_size = 0;
        json details = json::array();
        for (const auto& target : targets) {
            total_size += target.size;
            details.push_back({
                {"path", target.path.string()},
                {"size_bytes", target.size}
            });
        }

        bool require_confirmation = req.value("require_confirmation", true);
        bool confirmed = req.value("confirmed", false);
        if (require_confirmation && !confirmed) {
            json result = {
                {"action_required", "confirm"},
                {"message", "Confirm moving " + std::to_string(targets.size()) +
                    " path(s) to trash, estimated size " + std::to_string(total_size) + " bytes."},
                {"details", details}
            };
            return mcp_ext::plugin::make_json_result(result);
        }

        std::error_code ec;
        fs::create_directories(mcp_ext::wsl::trash_files_directory(), ec);
        if (ec) {
            return mcp_ext::plugin::make_error_result("Cannot create trash files directory: " + ec.message());
        }
        fs::create_directories(mcp_ext::wsl::trash_info_directory(), ec);
        if (ec) {
            return mcp_ext::plugin::make_error_result("Cannot create trash info directory: " + ec.message());
        }

        json moved = json::array();
        std::size_t deleted_count = 0;
        std::uintmax_t freed_bytes = 0;
        for (const auto& target : targets) {
            fs::path trash_path = unique_trash_path(target.path);
            fs::rename(target.path, trash_path, ec);
            if (ec) {
                return mcp_ext::plugin::make_error_result(
                    "Cannot move to trash: " + target.path.string() + ": " + ec.message());
            }
            write_trash_info(target.path, trash_path);
            deleted_count++;
            freed_bytes += target.size;
            moved.push_back({
                {"original_path", target.path.string()},
                {"trash_path", trash_path.string()},
                {"size_bytes", target.size}
            });
        }

        json result = {
            {"deleted_count", deleted_count},
            {"freed_bytes", freed_bytes},
            {"moved", moved}
        };
        return mcp_ext::plugin::make_json_result(result);
    } catch (const std::exception& e) {
        return mcp_ext::plugin::make_error_result(e.what());
    }
}
