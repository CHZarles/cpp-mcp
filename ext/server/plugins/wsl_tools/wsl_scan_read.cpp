/**
 * @file wsl_scan_read.cpp
 * @brief Read-only WSL scan status/report tools.
 */

#include "plugin_helpers.h"
#include "wsl_common.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

bool is_safe_id(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    });
}

fs::path scan_file_path(const std::string& scan_id, const std::string& suffix) {
    return mcp_ext::wsl::reports_directory() / (scan_id + suffix);
}

json read_scan_json(
    const json& req,
    const std::string& suffix,
    const std::string& missing_label,
    const std::string& uri_kind) {

    if (!req.contains("scan_id")) {
        throw std::runtime_error("scan_id is required");
    }

    const std::string scan_id = req.at("scan_id").get<std::string>();
    if (!is_safe_id(scan_id)) {
        throw std::runtime_error("Invalid scan_id");
    }

    const fs::path path = scan_file_path(scan_id, suffix);
    std::ifstream in(path);
    if (!in.is_open()) {
        throw std::runtime_error(missing_label + " not found: " + path.string());
    }

    json result = json::parse(in);
    result["scan_id"] = result.value("scan_id", scan_id);
    result["status_uri"] = "wsl://scan/" + scan_id + "/status";
    result["report_uri"] = "wsl://scan/" + scan_id + "/report";
    result["resource_uri"] = "wsl://scan/" + scan_id + "/" + uri_kind;
    return result;
}

} // namespace

extern "C" char* wsl_get_scan_status_handler(const json& req) {
    try {
        return mcp_ext::plugin::make_json_result(
            read_scan_json(req, "_status.json", "Scan status", "status"));
    } catch (const std::exception& e) {
        return mcp_ext::plugin::make_error_result(e.what());
    }
}

extern "C" char* wsl_get_scan_report_handler(const json& req) {
    try {
        return mcp_ext::plugin::make_json_result(
            read_scan_json(req, "_report.json", "Scan report", "report"));
    } catch (const std::exception& e) {
        return mcp_ext::plugin::make_error_result(e.what());
    }
}
