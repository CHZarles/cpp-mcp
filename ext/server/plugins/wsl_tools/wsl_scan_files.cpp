/**
 * @file wsl_scan_files.cpp
 * @brief WSL file scan tool.
 */

#include "plugin_helpers.h"
#include "wsl_common.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr std::size_t MAX_TOP_FILES = 200;

struct FileEntry {
    fs::path path;
    std::uintmax_t size = 0;
    std::time_t modified = 0;
    std::string category;
};

std::time_t parse_date_start(const std::string& date) {
    std::tm tm{};
    std::istringstream ss(date);
    ss >> std::get_time(&tm, "%Y-%m-%d");
    if (ss.fail()) {
        throw std::runtime_error("Invalid date format, expected YYYY-MM-DD: " + date);
    }
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

std::time_t parse_date_end(const std::string& date) {
    return parse_date_start(date) + (24 * 60 * 60) - 1;
}

std::string today_string() {
    std::time_t now = std::time(nullptr);
    std::tm tm = *std::localtime(&now);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d");
    return out.str();
}

std::time_t to_time_t(fs::file_time_type file_time) {
    auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        file_time - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::system_clock::to_time_t(system_time);
}

std::string format_time(std::time_t value) {
    std::tm tm = *std::localtime(&value);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return out.str();
}

std::string escape_regex(char c) {
    static const std::string specials = R"(\.^$|()[]{}+)";
    if (specials.find(c) != std::string::npos) {
        return std::string("\\") + c;
    }
    return std::string(1, c);
}

std::regex glob_to_regex(const std::string& pattern) {
    std::string regex = "^";
    for (char c : pattern) {
        if (c == '*') {
            regex += ".*";
        } else if (c == '?') {
            regex += ".";
        } else {
            regex += escape_regex(c);
        }
    }
    regex += "$";
    return std::regex(regex);
}

std::vector<std::regex> compile_patterns(const json& req, const char* key) {
    std::vector<std::regex> patterns;
    if (!req.contains(key)) {
        return patterns;
    }
    for (const auto& item : req.at(key)) {
        patterns.push_back(glob_to_regex(item.get<std::string>()));
    }
    return patterns;
}

bool matches_any(const std::string& path, const std::vector<std::regex>& patterns) {
    return std::any_of(patterns.begin(), patterns.end(), [&](const std::regex& pattern) {
        return std::regex_match(path, pattern);
    });
}

std::string categorize(const fs::path& path) {
    std::string text = path.string();
    if (text.find("node_modules") != std::string::npos) return "node_modules";
    if (text.find("/.cache/") != std::string::npos) return "cache";
    if (text.find("__pycache__") != std::string::npos) return "python_cache";
    if (text.find("/build/") != std::string::npos) return "build";
    if (text.find("/target/") != std::string::npos) return "rust_target";
    if (path.extension() == ".log") return "logs";
    std::string ext = path.extension().string();
    return ext.empty() ? "other" : ext.substr(1);
}

std::string make_scan_id() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return "scan_" + std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

bool should_skip_dir(const fs::path& path) {
    std::string text = path.string();
    return text.find("/.local/share/Trash/") != std::string::npos ||
           text.find("/.wsl_workspace/.reports/") != std::string::npos;
}

} // namespace

extern "C" char* wsl_scan_files_handler(const json& req) {
    try {
        std::string distro = req.value("distro", "");
        if (!distro.empty()) {
            return mcp_ext::plugin::make_error_result("Cross-distro scan not implemented");
        }
        if (!req.contains("start_date")) {
            return mcp_ext::plugin::make_error_result("start_date is required");
        }

        const std::string start_date = req.at("start_date").get<std::string>();
        const std::string end_date = req.value("end_date", today_string());
        const std::time_t start_time = parse_date_start(start_date);
        const std::time_t end_time = parse_date_end(end_date);
        if (end_time < start_time) {
            return mcp_ext::plugin::make_error_result("end_date must be on or after start_date");
        }

        const double min_size_mb = req.value("min_size_mb", 0.0);
        const std::uintmax_t min_size_bytes = static_cast<std::uintmax_t>(
            std::max(0.0, min_size_mb) * 1024.0 * 1024.0);
        const auto include_patterns = compile_patterns(req, "include_patterns");
        const auto exclude_patterns = compile_patterns(req, "exclude_patterns");

        const fs::path home = mcp_ext::wsl::home_directory();
        std::vector<FileEntry> files;
        std::unordered_map<std::string, std::uintmax_t> categories;
        std::uintmax_t total_size = 0;
        std::size_t total_files = 0;

        std::error_code ec;
        fs::recursive_directory_iterator it(
            home,
            fs::directory_options::skip_permission_denied,
            ec);
        fs::recursive_directory_iterator end;
        while (!ec && it != end) {
            const fs::directory_entry entry = *it;
            const fs::path path = entry.path();
            if (entry.is_directory(ec) && should_skip_dir(path)) {
                it.disable_recursion_pending();
                it.increment(ec);
                continue;
            }
            if (!entry.is_regular_file(ec)) {
                it.increment(ec);
                continue;
            }

            std::string rel = fs::relative(path, home, ec).string();
            if (!include_patterns.empty() && !matches_any(rel, include_patterns)) {
                it.increment(ec);
                continue;
            }
            if (matches_any(rel, exclude_patterns)) {
                it.increment(ec);
                continue;
            }

            std::uintmax_t size = entry.file_size(ec);
            if (ec || size < min_size_bytes) {
                ec.clear();
                it.increment(ec);
                continue;
            }

            std::time_t modified = to_time_t(entry.last_write_time(ec));
            if (ec || modified < start_time || modified > end_time) {
                ec.clear();
                it.increment(ec);
                continue;
            }

            std::string category = categorize(path);
            files.push_back({path, size, modified, category});
            total_size += size;
            total_files++;
            categories[category] += size;
            it.increment(ec);
        }

        std::sort(files.begin(), files.end(), [](const FileEntry& lhs, const FileEntry& rhs) {
            return lhs.size > rhs.size;
        });

        json top_files = json::array();
        for (std::size_t i = 0; i < files.size() && i < MAX_TOP_FILES; i++) {
            const auto& file = files[i];
            top_files.push_back({
                {"path", file.path.string()},
                {"size_bytes", file.size},
                {"modified", format_time(file.modified)},
                {"extension", file.path.extension().string()},
                {"category", file.category}
            });
        }

        json category_json = json::object();
        for (const auto& [category, size] : categories) {
            category_json[category] = size;
        }

        std::string scan_id = make_scan_id();
        json report = {
            {"scan_id", scan_id},
            {"distro", distro.empty() ? "current" : distro},
            {"start_date", start_date},
            {"end_date", end_date},
            {"total_files", total_files},
            {"total_size_bytes", total_size},
            {"top_files", top_files},
            {"categories", category_json}
        };

        fs::path reports_dir = mcp_ext::wsl::reports_directory();
        fs::create_directories(reports_dir, ec);
        if (ec) {
            return mcp_ext::plugin::make_error_result("Cannot create report directory: " + ec.message());
        }
        fs::path report_path = reports_dir / (scan_id + "_report.json");
        std::ofstream out(report_path);
        if (!out.is_open()) {
            return mcp_ext::plugin::make_error_result("Cannot write report: " + report_path.string());
        }
        out << report.dump(2);
        out.close();

        json result = {
            {"scan_id", scan_id},
            {"report_path", report_path.string()},
            {"resource_uri", "wsl://scan/" + scan_id + "/report"},
            {"summary", "Scanned " + std::to_string(total_files) + " files, total size " +
                std::to_string(total_size) + " bytes"}
        };
        return mcp_ext::plugin::make_json_result(result);
    } catch (const std::exception& e) {
        return mcp_ext::plugin::make_error_result(e.what());
    }
}
