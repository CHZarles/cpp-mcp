/**
 * @file wsl_recommend_cleanup.cpp
 * @brief WSL cleanup recommendation tool.
 */

#include "plugin_helpers.h"
#include "wsl_common.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

bool is_safe_id(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    });
}

fs::path report_path_for(const std::string& scan_id) {
    return mcp_ext::wsl::reports_directory() / (scan_id + "_report.json");
}

fs::path recommendations_path_for(const std::string& scan_id) {
    return mcp_ext::wsl::reports_directory() / (scan_id + "_recommendations.json");
}

bool category_allowed(const std::string& category, const std::string& aggressiveness) {
    static const std::unordered_set<std::string> safe = {
        "logs",
        "cache",
        "python_cache"
    };
    static const std::unordered_set<std::string> moderate = {
        "logs",
        "cache",
        "python_cache",
        "build",
        "rust_target"
    };
    static const std::unordered_set<std::string> aggressive = {
        "logs",
        "cache",
        "python_cache",
        "build",
        "rust_target",
        "node_modules"
    };

    if (aggressiveness == "safe") return safe.count(category) > 0;
    if (aggressiveness == "moderate") return moderate.count(category) > 0;
    if (aggressiveness == "aggressive") return aggressive.count(category) > 0;
    throw std::runtime_error("Invalid aggressiveness: " + aggressiveness);
}

std::string reason_for(const std::string& category, const std::string& aggressiveness) {
    if (category == "logs") return "Log files are usually safe cleanup candidates.";
    if (category == "cache") return "Cache files can usually be regenerated.";
    if (category == "python_cache") return "Python bytecode caches can be regenerated.";
    if (category == "build") return "Build artifacts can usually be rebuilt from source.";
    if (category == "rust_target") return "Rust target artifacts can usually be rebuilt.";
    if (category == "node_modules") {
        return aggressiveness == "aggressive"
            ? "node_modules can be reinstalled, but review before deleting."
            : "node_modules skipped unless aggressive cleanup is requested.";
    }
    return "Matched cleanup category: " + category;
}

} // namespace

extern "C" char* wsl_recommend_cleanup_handler(const json& req) {
    try {
        std::string distro = req.value("distro", "");
        if (!distro.empty()) {
            return mcp_ext::plugin::make_error_result("Cross-distro recommendation not implemented");
        }

        if (!req.contains("scan_id")) {
            return mcp_ext::plugin::make_error_result("scan_id is required");
        }
        const std::string scan_id = req.at("scan_id").get<std::string>();
        if (!is_safe_id(scan_id)) {
            return mcp_ext::plugin::make_error_result("Invalid scan_id");
        }

        const std::string aggressiveness = req.value("aggressiveness", "safe");
        const fs::path report_path = report_path_for(scan_id);
        std::ifstream in(report_path);
        if (!in.is_open()) {
            return mcp_ext::plugin::make_error_result("Scan report not found: " + report_path.string());
        }

        json report = json::parse(in);
        json suggestions = json::array();
        std::uintmax_t estimated_free = 0;

        for (const auto& file : report.value("top_files", json::array())) {
            const std::string category = file.value("category", "other");
            if (!category_allowed(category, aggressiveness)) {
                continue;
            }

            const std::uintmax_t size = file.value("size_bytes", 0ULL);
            estimated_free += size;
            suggestions.push_back({
                {"paths", json::array({file.value("path", "")})},
                {"size_bytes", size},
                {"category", category},
                {"reason", reason_for(category, aggressiveness)}
            });
        }

        json recommendations = {
            {"scan_id", scan_id},
            {"method", "rule_engine"},
            {"aggressiveness", aggressiveness},
            {"total_suggestions", suggestions.size()},
            {"estimated_free_bytes", estimated_free},
            {"suggestions", suggestions}
        };

        std::error_code ec;
        fs::create_directories(mcp_ext::wsl::reports_directory(), ec);
        if (ec) {
            return mcp_ext::plugin::make_error_result(
                "Cannot create report directory: " + ec.message());
        }

        fs::path output_path = recommendations_path_for(scan_id);
        std::ofstream out(output_path);
        if (!out.is_open()) {
            return mcp_ext::plugin::make_error_result(
                "Cannot write recommendations: " + output_path.string());
        }
        out << recommendations.dump(2);
        out.close();

        json result = {
            {"resource_uri", "wsl://scan/" + scan_id + "/recommendations"},
            {"recommendations_path", output_path.string()},
            {"method", "rule_engine"},
            {"total_suggestions", suggestions.size()},
            {"estimated_free_bytes", estimated_free}
        };
        return mcp_ext::plugin::make_json_result(result);
    } catch (const std::exception& e) {
        return mcp_ext::plugin::make_error_result(e.what());
    }
}
