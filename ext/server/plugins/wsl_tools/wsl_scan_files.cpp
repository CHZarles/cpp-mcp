/**
 * @file wsl_scan_files.cpp
 * @brief WSL file scan tool.
 */

#include "plugin_helpers.h"
#include "wsl_common.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr std::size_t MAX_TOP_FILES = 200;
constexpr std::size_t DEFAULT_MAX_FILES = 50000;
constexpr std::size_t MAX_ALLOWED_FILES = 200000;
constexpr int DEFAULT_MAX_SECONDS = 30;
constexpr int MAX_ALLOWED_SECONDS = 300;
constexpr int DEFAULT_MAX_DEPTH = 8;
constexpr int MAX_ALLOWED_DEPTH = 64;

struct FileEntry {
    fs::path path;
    std::uintmax_t size = 0;
    std::time_t modified = 0;
    std::string category;
};

struct ScanOptions {
    std::string scan_id;
    std::string start_date;
    std::string end_date;
    std::time_t start_time = 0;
    std::time_t end_time = 0;
    std::uintmax_t min_size_bytes = 0;
    std::vector<std::regex> include_patterns;
    std::vector<std::regex> exclude_patterns;
    std::vector<fs::path> targets;
    std::size_t max_files = DEFAULT_MAX_FILES;
    int max_seconds = DEFAULT_MAX_SECONDS;
    int max_depth = DEFAULT_MAX_DEPTH;
    fs::path home;
    fs::path reports_dir;
    fs::path status_path;
    fs::path report_path;
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
    std::tm tm{};
    localtime_r(&now, &tm);
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
    std::tm tm{};
    localtime_r(&value, &tm);
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
    static std::atomic<unsigned long long> sequence{0};
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return "scan_" + std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count()) +
        "_" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

bool should_skip_dir(const fs::path& path) {
    std::string text = path.string();
    for (const auto& part : path) {
        const std::string name = part.string();
        if (name == ".cursor-server" ||
            name == "node_modules" ||
            name == ".git" ||
            name == ".reports" ||
            name == "miniconda3" ||
            name == ".conda" ||
            name == ".cache" ||
            name == ".docker" ||
            name == "dbstorage" ||
            name == "pgdata" ||
            name == "weaviate" ||
            name == "vector_index") {
            return true;
        }
    }

    return text.find("/.local/share/Trash/") != std::string::npos ||
           text.find("/.wsl_workspace/.reports/") != std::string::npos ||
           text.find("/docker/volumes/") != std::string::npos ||
           text.find("/.dify-upstream/docker/volumes/") != std::string::npos;
}

bool matches_patterns_for_path(
    const fs::path& path,
    const fs::path& root,
    const fs::path& home,
    const std::vector<std::regex>& patterns) {

    if (patterns.empty()) {
        return false;
    }

    std::error_code ec;
    const std::string home_relative = fs::relative(path, home, ec).generic_string();
    ec.clear();
    const std::string root_relative = fs::relative(path, root, ec).generic_string();
    ec.clear();
    return matches_any(home_relative, patterns) || matches_any(root_relative, patterns);
}

void write_json_file(const fs::path& path, const json& value) {
    fs::create_directories(path.parent_path());
    const fs::path tmp_path = path.string() + ".tmp";
    {
        std::ofstream out(tmp_path);
        if (!out.is_open()) {
            throw std::runtime_error("Cannot write file: " + tmp_path.string());
        }
        out << value.dump(2);
    }

    std::error_code ec;
    fs::rename(tmp_path, path, ec);
    if (ec) {
        fs::remove(path, ec);
        ec.clear();
        fs::rename(tmp_path, path, ec);
    }
    if (ec) {
        throw std::runtime_error("Cannot replace file: " + path.string() + ": " + ec.message());
    }
}

json make_status(
    const ScanOptions& options,
    const std::string& state,
    bool complete,
    const std::string& truncated_reason,
    std::size_t scanned_files,
    std::size_t matched_files,
    std::uintmax_t total_size,
    const std::string& error = "") {

    json status = {
        {"scan_id", options.scan_id},
        {"state", state},
        {"complete", complete},
        {"truncated_reason", truncated_reason.empty() ? json(nullptr) : json(truncated_reason)},
        {"scanned_files", scanned_files},
        {"matched_files", matched_files},
        {"total_size_bytes", total_size},
        {"status_uri", "wsl://scan/" + options.scan_id + "/status"},
        {"report_uri", "wsl://scan/" + options.scan_id + "/report"}
    };
    if (!error.empty()) {
        status["error"] = error;
    }
    return status;
}

json make_report(
    const ScanOptions& options,
    const std::vector<FileEntry>& files,
    const std::unordered_map<std::string, std::uintmax_t>& categories,
    std::size_t scanned_files,
    std::size_t matched_files,
    std::uintmax_t total_size,
    bool complete,
    const std::string& truncated_reason) {

    std::vector<FileEntry> sorted = files;
    std::sort(sorted.begin(), sorted.end(), [](const FileEntry& lhs, const FileEntry& rhs) {
        return lhs.size > rhs.size;
    });

    json top_files = json::array();
    for (std::size_t i = 0; i < sorted.size() && i < MAX_TOP_FILES; i++) {
        const auto& file = sorted[i];
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

    json roots = json::array();
    for (const auto& target : options.targets) {
        roots.push_back(target.string());
    }

    return {
        {"scan_id", options.scan_id},
        {"environment", "current"},
        {"state", "completed"},
        {"complete", complete},
        {"truncated_reason", truncated_reason.empty() ? json(nullptr) : json(truncated_reason)},
        {"start_date", options.start_date},
        {"end_date", options.end_date},
        {"roots", roots},
        {"scanned_files", scanned_files},
        {"matched_files", matched_files},
        {"total_files", matched_files},
        {"total_size_bytes", total_size},
        {"top_files", top_files},
        {"categories", category_json}
    };
}

std::size_t read_size_limit(const json& req, const char* key, std::size_t default_value, std::size_t max_value) {
    const std::size_t value = req.value(key, default_value);
    if (value == 0) {
        throw std::runtime_error(std::string(key) + " must be greater than 0");
    }
    return std::min(value, max_value);
}

int read_int_limit(const json& req, const char* key, int default_value, int max_value) {
    const int value = req.value(key, default_value);
    if (value <= 0) {
        throw std::runtime_error(std::string(key) + " must be greater than 0");
    }
    return std::min(value, max_value);
}

fs::path validate_scan_target(const std::string& value) {
    fs::path input(value);
    if (input.is_absolute()) {
        auto validation = mcp_ext::wsl::HomePathValidator::validate(value);
        if (!validation.valid) {
            throw std::runtime_error(validation.error);
        }
        return validation.resolved_path;
    }

    auto validation = mcp_ext::wsl::PathValidator::validate(value);
    if (!validation.valid) {
        throw std::runtime_error(validation.error);
    }
    return validation.resolved_path;
}

std::vector<fs::path> read_targets(const json& req) {
    std::vector<fs::path> targets;
    auto add_target = [&](const std::string& value, bool require_directory) {
        fs::path target = validate_scan_target(value);
        std::error_code ec;
        if (!fs::exists(target, ec)) {
            throw std::runtime_error("Scan target does not exist: " + target.string());
        }
        if (require_directory && !fs::is_directory(target, ec)) {
            throw std::runtime_error("Scan root must be a directory: " + target.string());
        }
        targets.push_back(target);
    };

    if (req.contains("roots")) {
        for (const auto& root : req.at("roots")) {
            add_target(root.get<std::string>(), true);
        }
    }
    if (req.contains("paths")) {
        for (const auto& path : req.at("paths")) {
            add_target(path.get<std::string>(), false);
        }
    }
    if (targets.empty()) {
        targets.push_back(mcp_ext::wsl::workspace_directory());
    }
    return targets;
}

ScanOptions parse_options(const json& req) {
    if (!req.contains("start_date")) {
        throw std::runtime_error("start_date is required");
    }

    ScanOptions options;
    options.scan_id = make_scan_id();
    options.start_date = req.at("start_date").get<std::string>();
    options.end_date = req.value("end_date", today_string());
    options.start_time = parse_date_start(options.start_date);
    options.end_time = parse_date_end(options.end_date);
    if (options.end_time < options.start_time) {
        throw std::runtime_error("end_date must be on or after start_date");
    }

    const double min_size_mb = req.value("min_size_mb", 0.0);
    options.min_size_bytes = static_cast<std::uintmax_t>(
        std::max(0.0, min_size_mb) * 1024.0 * 1024.0);
    options.include_patterns = compile_patterns(req, "include_patterns");
    options.exclude_patterns = compile_patterns(req, "exclude_patterns");
    options.targets = read_targets(req);
    options.max_files = read_size_limit(req, "max_files", DEFAULT_MAX_FILES, MAX_ALLOWED_FILES);
    options.max_seconds = read_int_limit(req, "max_seconds", DEFAULT_MAX_SECONDS, MAX_ALLOWED_SECONDS);
    options.max_depth = read_int_limit(req, "max_depth", DEFAULT_MAX_DEPTH, MAX_ALLOWED_DEPTH);
    options.home = mcp_ext::wsl::home_directory();
    options.reports_dir = mcp_ext::wsl::reports_directory();
    options.status_path = options.reports_dir / (options.scan_id + "_status.json");
    options.report_path = options.reports_dir / (options.scan_id + "_report.json");
    return options;
}

void inspect_file(
    const fs::directory_entry& entry,
    const fs::path& root,
    const ScanOptions& options,
    std::vector<FileEntry>& files,
    std::unordered_map<std::string, std::uintmax_t>& categories,
    std::size_t& scanned_files,
    std::size_t& matched_files,
    std::uintmax_t& total_size,
    std::error_code& ec) {

    const fs::path path = entry.path();
    scanned_files++;

    if (!options.include_patterns.empty() &&
        !matches_patterns_for_path(path, root, options.home, options.include_patterns)) {
        return;
    }
    if (matches_patterns_for_path(path, root, options.home, options.exclude_patterns)) {
        return;
    }

    std::uintmax_t size = entry.file_size(ec);
    if (ec || size < options.min_size_bytes) {
        ec.clear();
        return;
    }

    std::time_t modified = to_time_t(entry.last_write_time(ec));
    if (ec || modified < options.start_time || modified > options.end_time) {
        ec.clear();
        return;
    }

    std::string category = categorize(path);
    files.push_back({path, size, modified, category});
    total_size += size;
    matched_files++;
    categories[category] += size;
}

void run_scan(ScanOptions options) {
    std::vector<FileEntry> files;
    std::unordered_map<std::string, std::uintmax_t> categories;
    std::uintmax_t total_size = 0;
    std::size_t scanned_files = 0;
    std::size_t matched_files = 0;
    bool complete = true;
    std::string truncated_reason;
    const auto started = std::chrono::steady_clock::now();

    try {
        auto is_timed_out = [&]() {
            return std::chrono::steady_clock::now() - started >=
                std::chrono::seconds(options.max_seconds);
        };
        auto mark_truncated = [&](const std::string& reason) {
            if (truncated_reason.empty()) {
                truncated_reason = reason;
                complete = false;
            }
        };

        for (const auto& target : options.targets) {
            std::error_code ec;
            if (!fs::exists(target, ec)) {
                continue;
            }

            if (fs::is_regular_file(target, ec)) {
                inspect_file(
                    fs::directory_entry(target),
                    target.parent_path(),
                    options,
                    files,
                    categories,
                    scanned_files,
                    matched_files,
                    total_size,
                    ec);
                if (scanned_files >= options.max_files) {
                    mark_truncated("max_files");
                    break;
                }
                if (is_timed_out()) {
                    mark_truncated("max_seconds");
                    break;
                }
                continue;
            }

            if (!fs::is_directory(target, ec)) {
                continue;
            }

            fs::recursive_directory_iterator it(
                target,
                fs::directory_options::skip_permission_denied,
                ec);
            fs::recursive_directory_iterator end;
            while (!ec && it != end) {
                const fs::directory_entry entry = *it;
                const fs::path path = entry.path();

                if (is_timed_out()) {
                    mark_truncated("max_seconds");
                    break;
                }

                if (entry.is_directory(ec)) {
                    if (should_skip_dir(path)) {
                        it.disable_recursion_pending();
                    } else if (it.depth() >= options.max_depth) {
                        mark_truncated("max_depth");
                        it.disable_recursion_pending();
                    }
                    ec.clear();
                    it.increment(ec);
                    continue;
                }

                if (entry.is_regular_file(ec)) {
                    inspect_file(
                        entry,
                        target,
                        options,
                        files,
                        categories,
                        scanned_files,
                        matched_files,
                        total_size,
                        ec);
                    if (scanned_files >= options.max_files) {
                        mark_truncated("max_files");
                        break;
                    }
                }
                ec.clear();
                it.disable_recursion_pending();
                it.increment(ec);
            }

            if (!ec && truncated_reason.empty()) {
                continue;
            }
            ec.clear();
            if (!truncated_reason.empty()) {
                break;
            }
        }

        write_json_file(
            options.report_path,
            make_report(
                options,
                files,
                categories,
                scanned_files,
                matched_files,
                total_size,
                complete,
                truncated_reason));
        write_json_file(
            options.status_path,
            make_status(
                options,
                "completed",
                complete,
                truncated_reason,
                scanned_files,
                matched_files,
                total_size));
    } catch (const std::exception& e) {
        try {
            write_json_file(
                options.status_path,
                make_status(
                    options,
                    "failed",
                    false,
                    "",
                    scanned_files,
                    matched_files,
                    total_size,
                    e.what()));
        } catch (...) {
        }
    }
}

} // namespace

extern "C" char* wsl_scan_files_handler(const json& req) {
    try {
        ScanOptions options = parse_options(req);
        std::error_code ec;
        fs::create_directories(options.reports_dir, ec);
        if (ec) {
            return mcp_ext::plugin::make_error_result("Cannot create report directory: " + ec.message());
        }

        write_json_file(
            options.status_path,
            make_status(options, "running", false, "", 0, 0, 0));
        write_json_file(
            options.report_path,
            json{
                {"scan_id", options.scan_id},
                {"state", "running"},
                {"complete", false},
                {"truncated_reason", nullptr},
                {"top_files", json::array()},
                {"categories", json::object()}
            });

        const std::string scan_id = options.scan_id;
        const std::string report_path = options.report_path.string();
        const std::string status_path = options.status_path.string();
        const std::string status_uri = "wsl://scan/" + scan_id + "/status";
        const std::string report_uri = "wsl://scan/" + scan_id + "/report";
        std::thread(run_scan, std::move(options)).detach();

        json result = {
            {"accepted", true},
            {"async", true},
            {"scan_id", scan_id},
            {"job_state", "running"},
            {"status_path", status_path},
            {"report_path", report_path},
            {"status_uri", status_uri},
            {"report_uri", report_uri},
            {"resource_uri", report_uri},
            {"default_scope", "~/.wsl_workspace"},
            {"next_action", "Scan job started asynchronously; this is not the final scan result. Read " +
                status_uri + " until state is completed or failed. After completion, read " +
                report_uri + ". To scan other directories under $HOME, call wsl_scan_files with roots or paths."},
            {"summary", "Asynchronous scan job accepted. Read status_uri for final state, then report_uri for the report."}
        };
        return mcp_ext::plugin::make_json_result(result);
    } catch (const std::exception& e) {
        return mcp_ext::plugin::make_error_result(e.what());
    }
}
