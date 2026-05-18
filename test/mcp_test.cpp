/**
 * @file mcp_test.cpp
 * @brief Tests for MCP message formatting and Streamable HTTP behavior.
 */

#include <gtest/gtest.h>

#include "mcp_message.h"
#include "mcp_server.h"
#include "mcp_streamable_http_client.h"
#include "mcp_tool.h"
#include "../ext/server/plugins/plugin_helpers.h"
#include "../ext/server/plugins/wsl_tools/wsl_common.h"
#include "../ext/server/src/wsl_resources.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <thread>

using namespace mcp;
using json = nlohmann::ordered_json;

class MessageFormatTest : public ::testing::Test {};

TEST_F(MessageFormatTest, RequestMessageFormat) {
    request req = request::create("test_method", {{"key", "value"}});

    json req_json = req.to_json();

    EXPECT_EQ(req_json["jsonrpc"], "2.0");
    EXPECT_TRUE(req_json.contains("id"));
    EXPECT_EQ(req_json["method"], "test_method");
    EXPECT_EQ(req_json["params"]["key"], "value");
}

TEST_F(MessageFormatTest, ResponseMessageFormat) {
    response res = response::create_success("test_id", {{"key", "value"}});

    json res_json = res.to_json();

    EXPECT_EQ(res_json["jsonrpc"], "2.0");
    EXPECT_EQ(res_json["id"], "test_id");
    EXPECT_EQ(res_json["result"]["key"], "value");
    EXPECT_FALSE(res_json.contains("error"));
}

TEST_F(MessageFormatTest, ErrorResponseMessageFormat) {
    response res = response::create_error(
        "test_id",
        error_code::invalid_params,
        "Invalid parameters",
        {{"details", "Missing required field"}});

    json res_json = res.to_json();

    EXPECT_EQ(res_json["jsonrpc"], "2.0");
    EXPECT_EQ(res_json["id"], "test_id");
    EXPECT_FALSE(res_json.contains("result"));
    EXPECT_EQ(res_json["error"]["code"], static_cast<int>(error_code::invalid_params));
    EXPECT_EQ(res_json["error"]["message"], "Invalid parameters");
    EXPECT_EQ(res_json["error"]["data"]["details"], "Missing required field");
}

TEST_F(MessageFormatTest, NotificationMessageFormat) {
    request notification = request::create_notification("test_notification", {{"key", "value"}});

    json notification_json = notification.to_json();

    EXPECT_EQ(notification_json["jsonrpc"], "2.0");
    EXPECT_FALSE(notification_json.contains("id"));
    EXPECT_EQ(notification_json["method"], "notifications/test_notification");
    EXPECT_EQ(notification_json["params"]["key"], "value");
    EXPECT_TRUE(notification.is_notification());
}

TEST(EventDispatcherTest, QueuesMultipleEventsWithoutOverwriting) {
    event_dispatcher dispatcher;
    ASSERT_TRUE(dispatcher.send_event("event: message\r\ndata: first\r\n\r\n"));
    ASSERT_TRUE(dispatcher.send_event("event: message\r\ndata: second\r\n\r\n"));

    std::string first;
    httplib::DataSink first_sink;
    first_sink.write = [&first](const char* data, size_t len) {
        first.append(data, len);
        return true;
    };

    ASSERT_TRUE(dispatcher.wait_event(&first_sink, std::chrono::milliseconds(1)));
    EXPECT_EQ(first, "event: message\r\ndata: first\r\n\r\n");

    std::string second;
    httplib::DataSink second_sink;
    second_sink.write = [&second](const char* data, size_t len) {
        second.append(data, len);
        return true;
    };

    ASSERT_TRUE(dispatcher.wait_event(&second_sink, std::chrono::milliseconds(1)));
    EXPECT_EQ(second, "event: message\r\ndata: second\r\n\r\n");
}

TEST(EventDispatcherTest, ReportsTimeoutSeparatelyFromClosed) {
    event_dispatcher dispatcher;
    std::string output;
    httplib::DataSink sink;
    sink.write = [&output](const char* data, size_t len) {
        output.append(data, len);
        return true;
    };

    EXPECT_EQ(
        dispatcher.wait_event_result(&sink, std::chrono::milliseconds(1)),
        event_dispatcher::wait_result::timeout);
    EXPECT_TRUE(output.empty());

    dispatcher.close();
    EXPECT_EQ(
        dispatcher.wait_event_result(&sink, std::chrono::milliseconds(1)),
        event_dispatcher::wait_result::closed);
}

TEST(PluginHelpersTest, CreatesTextAndErrorToolResults) {
    char* ok_result = mcp_ext::plugin::make_text_result("hello");
    ASSERT_NE(ok_result, nullptr);
    json ok = json::parse(ok_result);
    free(ok_result);

    ASSERT_TRUE(ok.contains("content"));
    EXPECT_EQ(ok["content"][0]["type"], "text");
    EXPECT_EQ(ok["content"][0]["text"], "hello");
    EXPECT_FALSE(ok["isError"]);

    char* error_result = mcp_ext::plugin::make_error_result("bad input");
    ASSERT_NE(error_result, nullptr);
    json error = json::parse(error_result);
    free(error_result);

    ASSERT_TRUE(error.contains("content"));
    EXPECT_EQ(error["content"][0]["type"], "text");
    EXPECT_EQ(error["content"][0]["text"], "Error: bad input");
    EXPECT_TRUE(error["isError"]);
}

TEST(WslPathValidatorTest, AcceptsPathsInsideWorkspace) {
    const auto result = mcp_ext::wsl::PathValidator::validate(
        "projects/demo",
        "/home/tester");

    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.resolved_path, "/home/tester/.wsl_workspace/projects/demo");
}

TEST(WslPathValidatorTest, AcceptsAbsolutePathsInsideWorkspace) {
    const auto result = mcp_ext::wsl::PathValidator::validate(
        "/home/tester/.wsl_workspace/projects/demo",
        "/home/tester");

    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.resolved_path, "/home/tester/.wsl_workspace/projects/demo");
}

TEST(WslPathValidatorTest, RejectsAbsolutePathsOutsideWorkspace) {
    const auto result = mcp_ext::wsl::PathValidator::validate(
        "/home/tester/Documents/demo",
        "/home/tester");

    EXPECT_FALSE(result.valid);
}

TEST(WslPathValidatorTest, RejectsWorkspacePrefixByPass) {
    const auto result = mcp_ext::wsl::PathValidator::validate(
        "/home/tester/.wsl_workspace_evil/demo",
        "/home/tester");

    EXPECT_FALSE(result.valid);
}

TEST(WslHomePathValidatorTest, AcceptsPathsInsideHome) {
    const auto result = mcp_ext::wsl::HomePathValidator::validate(
        "/home/tester/projects/demo",
        "/home/tester");

    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.resolved_path, "/home/tester/projects/demo");
}

TEST(WslHomePathValidatorTest, RejectsHomePrefixByPass) {
    const auto result = mcp_ext::wsl::HomePathValidator::validate(
        "/home/tester2/projects/demo",
        "/home/tester");

    EXPECT_FALSE(result.valid);
}

class WslResourceTemplateTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::filesystem::create_directories(mcp_ext::wsl::reports_directory());
        report_path_ = mcp_ext::wsl::reports_directory() / (scan_id_ + "_report.json");
        recommendations_path_ = mcp_ext::wsl::reports_directory() / (scan_id_ + "_recommendations.json");

        std::ofstream report(report_path_);
        report << R"({"scan_id":"codex_resource_test","kind":"report"})";
        report.close();

        std::ofstream recommendations(recommendations_path_);
        recommendations << R"({"scan_id":"codex_resource_test","kind":"recommendations"})";
        recommendations.close();
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove(report_path_, ec);
        std::filesystem::remove(recommendations_path_, ec);
    }

    const std::string scan_id_ = "codex_resource_test";
    std::filesystem::path report_path_;
    std::filesystem::path recommendations_path_;
};

TEST_F(WslResourceTemplateTest, RegistersAndReadsScanResources) {
    server::configuration conf;
    conf.host = "localhost";
    conf.port = 19092;
    conf.session_timeout = 0;

    server test_server(conf);
    test_server.set_server_info("ResourceTestServer", "1.0.0");
    test_server.set_capabilities({{"resources", json::object()}});
    mcp_ext::register_wsl_resources(test_server);

    ASSERT_TRUE(test_server.start(false));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    {
        streamable_http_client client("http://localhost:19092", "/mcp");
        client.set_timeout(5);
        ASSERT_TRUE(client.initialize("ResourceTestClient", MCP_VERSION));

        json templates = client.list_resource_templates();
        ASSERT_TRUE(templates.contains("resourceTemplates"));
        bool found_report_template = false;
        bool found_recommendations_template = false;
        for (const auto& tmpl : templates["resourceTemplates"]) {
            found_report_template = found_report_template ||
                tmpl.value("uriTemplate", "") == "wsl://scan/{scan_id}/report";
            found_recommendations_template = found_recommendations_template ||
                tmpl.value("uriTemplate", "") == "wsl://scan/{scan_id}/recommendations";
        }
        EXPECT_TRUE(found_report_template);
        EXPECT_TRUE(found_recommendations_template);

        json report = client.read_resource("wsl://scan/" + scan_id_ + "/report");
        ASSERT_TRUE(report.contains("contents"));
        ASSERT_EQ(report["contents"].size(), 1U);
        EXPECT_EQ(report["contents"][0]["mimeType"], "application/json");
        EXPECT_NE(report["contents"][0]["text"].get<std::string>().find("\"kind\":\"report\""), std::string::npos);

        json recommendations = client.read_resource("wsl://scan/" + scan_id_ + "/recommendations");
        ASSERT_TRUE(recommendations.contains("contents"));
        ASSERT_EQ(recommendations["contents"].size(), 1U);
        EXPECT_EQ(recommendations["contents"][0]["mimeType"], "application/json");
        EXPECT_NE(
            recommendations["contents"][0]["text"].get<std::string>().find("\"kind\":\"recommendations\""),
            std::string::npos);
    }

    test_server.stop();
}

TEST(CoreResourceTest, ResourceTemplateRegistrationAlsoEnablesResourceList) {
    server::configuration conf;
    conf.host = "localhost";
    conf.port = 19093;
    conf.session_timeout = 0;

    server test_server(conf);
    test_server.set_server_info("ResourceCoreTestServer", "1.0.0");
    test_server.set_capabilities({{"resources", {{"subscribe", true}}}});
    test_server.register_resource_template(
        "test://items/{id}",
        "test_item",
        "application/json",
        "Test item template",
        [](const std::string& uri, const std::map<std::string, std::string>& uri_params, const std::string&) {
            return json{
                {"uri", uri},
                {"mimeType", "application/json"},
                {"text", json{{"id", uri_params.at("id")}}.dump()}
            };
        });

    ASSERT_TRUE(test_server.start(false));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    {
        streamable_http_client client("http://localhost:19093", "/mcp");
        client.set_timeout(5);
        ASSERT_TRUE(client.initialize("ResourceCoreTestClient", MCP_VERSION));

        json resources = client.list_resources();
        ASSERT_TRUE(resources.contains("resources"));
        EXPECT_TRUE(resources["resources"].is_array());
        EXPECT_TRUE(resources["resources"].empty());

        json templates = client.list_resource_templates();
        ASSERT_TRUE(templates.contains("resourceTemplates"));
        ASSERT_EQ(templates["resourceTemplates"].size(), 1U);
        EXPECT_EQ(templates["resourceTemplates"][0]["uriTemplate"], "test://items/{id}");

        EXPECT_EQ(client.subscribe_to_resource("test://items/abc"), json::object());
        EXPECT_EQ(client.unsubscribe_from_resource("test://items/abc"), json::object());
    }

    test_server.stop();
}

TEST(CoreResourceTest, StaticResourceSupportsLatestMetadataAndContentMeta) {
    server::configuration conf;
    conf.host = "localhost";
    conf.port = 19097;
    conf.session_timeout = 0;

    server test_server(conf);
    test_server.set_server_info("LatestResourceServer", "1.0.0");
    test_server.set_capabilities({{"resources", {{"subscribe", true}}}});

    auto resource = std::make_shared<text_resource>(
        "test://docs/readme",
        "readme",
        "text/markdown",
        "Project readme");
    resource->set_title("Project README");
    resource->set_icons(json::array({
        {
            {"src", "https://example.com/readme.png"},
            {"mimeType", "image/png"},
            {"sizes", json::array({"48x48"})}
        }
    }));
    resource->set_annotations({
        {"audience", json::array({"user"})},
        {"priority", 0.8}
    });
    resource->set_size(12);
    resource->set_meta({{"source", "unit-test"}});
    resource->set_content_meta({{"etag", "abc123"}});
    resource->set_text("hello latest");

    test_server.register_resource("test://docs/readme", resource);

    ASSERT_TRUE(test_server.start(false));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    {
        streamable_http_client client("http://localhost:19097", "/mcp");
        client.set_timeout(5);
        ASSERT_TRUE(client.initialize("LatestResourceClient", MCP_VERSION));

        json listed = client.list_resources();
        ASSERT_TRUE(listed.contains("resources"));
        ASSERT_EQ(listed["resources"].size(), 1U);
        const json& metadata = listed["resources"][0];
        EXPECT_EQ(metadata["uri"], "test://docs/readme");
        EXPECT_EQ(metadata["name"], "readme");
        EXPECT_EQ(metadata["title"], "Project README");
        EXPECT_EQ(metadata["description"], "Project readme");
        EXPECT_EQ(metadata["mimeType"], "text/markdown");
        EXPECT_EQ(metadata["icons"][0]["src"], "https://example.com/readme.png");
        EXPECT_EQ(metadata["annotations"]["audience"][0], "user");
        EXPECT_EQ(metadata["annotations"]["priority"], 0.8);
        EXPECT_EQ(metadata["size"], 12);
        EXPECT_EQ(metadata["_meta"]["source"], "unit-test");

        json read = client.read_resource("test://docs/readme");
        ASSERT_TRUE(read.contains("contents"));
        ASSERT_EQ(read["contents"].size(), 1U);
        EXPECT_EQ(read["contents"][0]["text"], "hello latest");
        EXPECT_EQ(read["contents"][0]["_meta"]["etag"], "abc123");
    }

    test_server.stop();
}

TEST(CoreResourceTest, FileResourcePreservesContentMeta) {
    const auto file_path = std::filesystem::temp_directory_path() /
        ("cpp_mcp_file_resource_meta_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
    {
        std::ofstream file(file_path);
        file << "file resource text";
    }

    server::configuration conf;
    conf.host = "localhost";
    conf.port = 19100;
    conf.session_timeout = 0;

    server test_server(conf);
    test_server.set_server_info("LatestFileResourceServer", "1.0.0");
    test_server.set_capabilities({{"resources", json::object()}});

    auto resource = std::make_shared<file_resource>(file_path.string(), "text/plain", "File resource");
    resource->set_content_meta({{"etag", "file-abc123"}});
    const std::string resource_uri = resource->get_uri();
    test_server.register_resource(resource_uri, resource);

    ASSERT_TRUE(test_server.start(false));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    {
        streamable_http_client client("http://localhost:19100", "/mcp");
        client.set_timeout(5);
        ASSERT_TRUE(client.initialize("LatestFileResourceClient", MCP_VERSION));

        json read = client.read_resource(resource_uri);
        ASSERT_TRUE(read.contains("contents"));
        ASSERT_EQ(read["contents"].size(), 1U);
        EXPECT_EQ(read["contents"][0]["text"], "file resource text");
        EXPECT_EQ(read["contents"][0]["_meta"]["etag"], "file-abc123");
    }

    test_server.stop();
    std::filesystem::remove(file_path);
}

TEST(CoreResourceTest, ResourceTemplateSupportsLatestMetadata) {
    server::configuration conf;
    conf.host = "localhost";
    conf.port = 19098;
    conf.session_timeout = 0;

    server test_server(conf);
    test_server.set_server_info("LatestResourceTemplateServer", "1.0.0");
    test_server.set_capabilities({{"resources", json::object()}});
    test_server.register_resource_template(
        "test://reports/{id}",
        "report",
        "application/json",
        "Report template",
        [](const std::string& uri, const std::map<std::string, std::string>& uri_params, const std::string&) {
            return json{
                {"uri", uri},
                {"mimeType", "application/json"},
                {"text", json{{"id", uri_params.at("id")}}.dump()}
            };
        },
        {
            {"title", "Report by ID"},
            {"icons", json::array({{{"src", "https://example.com/report.png"}, {"mimeType", "image/png"}}})},
            {"annotations", {{"audience", json::array({"assistant"})}}},
            {"_meta", {{"category", "reports"}}}
        });

    ASSERT_TRUE(test_server.start(false));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    {
        streamable_http_client client("http://localhost:19098", "/mcp");
        client.set_timeout(5);
        ASSERT_TRUE(client.initialize("LatestResourceTemplateClient", MCP_VERSION));

        json templates = client.list_resource_templates();
        ASSERT_TRUE(templates.contains("resourceTemplates"));
        ASSERT_EQ(templates["resourceTemplates"].size(), 1U);
        const json& tmpl = templates["resourceTemplates"][0];
        EXPECT_EQ(tmpl["uriTemplate"], "test://reports/{id}");
        EXPECT_EQ(tmpl["name"], "report");
        EXPECT_EQ(tmpl["title"], "Report by ID");
        EXPECT_EQ(tmpl["icons"][0]["src"], "https://example.com/report.png");
        EXPECT_EQ(tmpl["annotations"]["audience"][0], "assistant");
        EXPECT_EQ(tmpl["_meta"]["category"], "reports");
    }

    test_server.stop();
}

TEST(CoreResourceTest, SupportsUnsubscribeMethod) {
    server::configuration conf;
    conf.host = "localhost";
    conf.port = 19099;
    conf.session_timeout = 0;

    server test_server(conf);
    test_server.set_server_info("ResourceSubscribeServer", "1.0.0");
    test_server.set_capabilities({{"resources", {{"subscribe", true}}}});

    auto resource = std::make_shared<text_resource>(
        "test://docs/subscribed",
        "subscribed",
        "text/plain",
        "Subscribable resource");
    resource->set_text("subscribable");
    test_server.register_resource("test://docs/subscribed", resource);

    ASSERT_TRUE(test_server.start(false));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    {
        streamable_http_client client("http://localhost:19099", "/mcp");
        client.set_timeout(5);
        ASSERT_TRUE(client.initialize("ResourceSubscribeClient", MCP_VERSION));

        json subscribe_result = client.subscribe_to_resource("test://docs/subscribed");
        EXPECT_EQ(subscribe_result, json::object());

        json unsubscribe_result = client.unsubscribe_from_resource("test://docs/subscribed");
        EXPECT_EQ(unsubscribe_result, json::object());
    }

    test_server.stop();
}

TEST(CoreResourceTest, ResourceUpdatedNotificationsFollowSubscriptions) {
    server::configuration conf;
    conf.host = "localhost";
    conf.port = 19101;
    conf.session_timeout = 0;

    server test_server(conf);
    test_server.set_server_info("ResourceNotificationServer", "1.0.0");
    test_server.set_capabilities({{"resources", {{"subscribe", true}}}});

    auto resource = std::make_shared<text_resource>(
        "test://docs/updates",
        "updates",
        "text/plain",
        "Updated resource");
    resource->set_text("initial");
    test_server.register_resource("test://docs/updates", resource);

    ASSERT_TRUE(test_server.start(false));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    {
        streamable_http_client client("http://localhost:19101", "/mcp");
        client.set_timeout(5);
        ASSERT_TRUE(client.initialize("ResourceNotificationClient", MCP_VERSION));

        std::atomic<int> update_count{0};
        std::promise<json> first_update_promise;
        auto first_update = first_update_promise.get_future();
        client.set_notification_handler([&](const std::string& method, const json& params) {
            if (method == "notifications/resources/updated") {
                if (update_count.fetch_add(1) == 0) {
                    first_update_promise.set_value(params);
                }
            }
        });

        ASSERT_TRUE(client.start_sse_stream());
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        EXPECT_EQ(client.subscribe_to_resource("test://docs/updates"), json::object());
        test_server.notify_resource_updated("test://docs/updates");

        ASSERT_EQ(first_update.wait_for(std::chrono::seconds(5)), std::future_status::ready);
        EXPECT_EQ(first_update.get()["uri"], "test://docs/updates");

        EXPECT_EQ(client.unsubscribe_from_resource("test://docs/updates"), json::object());
        test_server.notify_resource_updated("test://docs/updates");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        EXPECT_EQ(update_count.load(), 1);

        client.stop_sse_stream();
    }

    test_server.stop();
}

TEST(CorePromptTest, ListsAndGetsRegisteredPrompts) {
    server::configuration conf;
    conf.host = "localhost";
    conf.port = 19094;
    conf.session_timeout = 0;

    server test_server(conf);
    test_server.set_server_info("PromptTestServer", "1.0.0");
    test_server.set_capabilities({{"prompts", json::object()}});

    prompt review_prompt;
    review_prompt.name = "review_cleanup";
    review_prompt.description = "Review a cleanup recommendation report";
    review_prompt.arguments.push_back({"scan_id", "Scan identifier", true});

    test_server.register_prompt(
        review_prompt,
        [](const json& arguments, const std::string&) {
            const std::string scan_id = arguments.value("scan_id", "");
            return json{
                {"messages", json::array({
                    {
                        {"role", "user"},
                        {"content", {
                            {"type", "text"},
                            {"text", "Review cleanup recommendations for scan " + scan_id}
                        }}
                    }
                })}
            };
        });

    ASSERT_TRUE(test_server.start(false));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    {
        streamable_http_client client("http://localhost:19094", "/mcp");
        client.set_timeout(5);
        ASSERT_TRUE(client.initialize("PromptTestClient", MCP_VERSION));

        json prompts = client.send_request("prompts/list").result;
        ASSERT_TRUE(prompts.contains("prompts"));
        ASSERT_EQ(prompts["prompts"].size(), 1U);
        EXPECT_EQ(prompts["prompts"][0]["name"], "review_cleanup");
        EXPECT_EQ(prompts["prompts"][0]["description"], "Review a cleanup recommendation report");
        ASSERT_TRUE(prompts["prompts"][0].contains("arguments"));
        EXPECT_EQ(prompts["prompts"][0]["arguments"][0]["name"], "scan_id");
        EXPECT_TRUE(prompts["prompts"][0]["arguments"][0]["required"]);

        json prompt_result = client.send_request(
            "prompts/get",
            {
                {"name", "review_cleanup"},
                {"arguments", {{"scan_id", "scan-123"}}}
            }).result;
        ASSERT_TRUE(prompt_result.contains("description"));
        ASSERT_TRUE(prompt_result.contains("messages"));
        EXPECT_EQ(prompt_result["description"], "Review a cleanup recommendation report");
        EXPECT_EQ(
            prompt_result["messages"][0]["content"]["text"],
            "Review cleanup recommendations for scan scan-123");
    }

    test_server.stop();
}

TEST(CorePromptTest, AdvertisesPromptCapabilityAndListsLatestMetadata) {
    server::configuration conf;
    conf.host = "localhost";
    conf.port = 19102;
    conf.session_timeout = 0;

    server test_server(conf);
    test_server.set_server_info("PromptMetadataServer", "1.0.0");
    test_server.set_capabilities({{"tools", json::object()}});

    prompt metadata_prompt;
    metadata_prompt.name = "cleanup_review";
    metadata_prompt.title = "Cleanup Review";
    metadata_prompt.description = "Review cleanup recommendations";
    metadata_prompt.icons = json::array({
        {
            {"src", "prompt://icons/cleanup.svg"},
            {"mimeType", "image/svg+xml"},
            {"sizes", "any"}
        }
    });
    metadata_prompt.metadata = {{"origin", "test"}};

    prompt_argument scan_id;
    scan_id.name = "scan_id";
    scan_id.title = "Scan ID";
    scan_id.description = "Identifier returned by scan_files";
    scan_id.required = true;
    scan_id.metadata = {{"format", "report-id"}};
    metadata_prompt.arguments.push_back(scan_id);

    test_server.register_prompt(
        metadata_prompt,
        [](const json& arguments, const std::string&) {
            return json{
                {"messages", json::array({
                    {
                        {"role", "user"},
                        {"content", {
                            {"type", "text"},
                            {"text", "Review scan " + arguments["scan_id"].get<std::string>()}
                        }}
                    }
                })}
            };
        });

    ASSERT_TRUE(test_server.start(false));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    {
        streamable_http_client client("http://localhost:19102", "/mcp");
        client.set_timeout(5);
        ASSERT_TRUE(client.initialize("PromptMetadataClient", MCP_VERSION));

        json capabilities = client.get_server_capabilities();
        ASSERT_TRUE(capabilities.contains("tools"));
        ASSERT_TRUE(capabilities.contains("prompts"));

        json prompts = client.send_request("prompts/list", {{"cursor", "ignored"}}).result;
        ASSERT_TRUE(prompts.contains("prompts"));
        ASSERT_EQ(prompts["prompts"].size(), 1U);
        EXPECT_FALSE(prompts.contains("nextCursor"));

        const json& listed = prompts["prompts"][0];
        EXPECT_EQ(listed["name"], "cleanup_review");
        EXPECT_EQ(listed["title"], "Cleanup Review");
        EXPECT_EQ(listed["description"], "Review cleanup recommendations");
        ASSERT_TRUE(listed.contains("icons"));
        EXPECT_EQ(listed["icons"][0]["src"], "prompt://icons/cleanup.svg");
        ASSERT_TRUE(listed.contains("_meta"));
        EXPECT_EQ(listed["_meta"]["origin"], "test");
        ASSERT_TRUE(listed.contains("arguments"));
        EXPECT_EQ(listed["arguments"][0]["title"], "Scan ID");
        EXPECT_EQ(listed["arguments"][0]["_meta"]["format"], "report-id");
    }

    test_server.stop();
}

TEST(CorePromptTest, RejectsInvalidPromptArgumentsAndHandlerResults) {
    server::configuration conf;
    conf.host = "localhost";
    conf.port = 19103;
    conf.session_timeout = 0;

    server test_server(conf);
    test_server.set_server_info("PromptValidationServer", "1.0.0");

    prompt review_prompt;
    review_prompt.name = "review_cleanup";
    review_prompt.arguments.push_back({"scan_id", "Scan identifier", true});

    test_server.register_prompt(
        review_prompt,
        [](const json& arguments, const std::string&) {
            if (arguments.value("scan_id", "") == "bad-result") {
                return json{{"messages", json::object()}};
            }

            return json{
                {"messages", json::array({
                    {
                        {"role", "user"},
                        {"content", {
                            {"type", "text"},
                            {"text", "Review scan " + arguments["scan_id"].get<std::string>()}
                        }}
                    }
                })}
            };
        });

    ASSERT_TRUE(test_server.start(false));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    {
        streamable_http_client client("http://localhost:19103", "/mcp");
        client.set_timeout(5);
        ASSERT_TRUE(client.initialize("PromptValidationClient", MCP_VERSION));

        try {
            client.send_request(
                "prompts/get",
                {
                    {"name", "review_cleanup"},
                    {"arguments", {{"scan_id", 123}}}
                });
            FAIL() << "Expected invalid params for non-string prompt argument";
        } catch (const mcp_exception& e) {
            EXPECT_EQ(e.code(), error_code::invalid_params);
        }

        try {
            client.send_request(
                "prompts/get",
                {
                    {"name", "review_cleanup"},
                    {"arguments", {{"scan_id", "bad-result"}}}
                });
            FAIL() << "Expected internal error for invalid prompt handler result";
        } catch (const mcp_exception& e) {
            EXPECT_EQ(e.code(), error_code::internal_error);
        }
    }

    test_server.stop();
}

TEST(CorePromptTest, NotifiesPromptListChangedWhenAdvertised) {
    server::configuration conf;
    conf.host = "localhost";
    conf.port = 19104;
    conf.session_timeout = 0;

    server test_server(conf);
    test_server.set_server_info("PromptNotificationServer", "1.0.0");
    test_server.set_capabilities({{"prompts", {{"listChanged", true}}}});

    prompt review_prompt;
    review_prompt.name = "review_cleanup";
    test_server.register_prompt(
        review_prompt,
        [](const json&, const std::string&) {
            return json{
                {"messages", json::array({
                    {
                        {"role", "user"},
                        {"content", {
                            {"type", "text"},
                            {"text", "Review cleanup recommendations"}
                        }}
                    }
                })}
            };
        });

    ASSERT_TRUE(test_server.start(false));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    {
        streamable_http_client client("http://localhost:19104", "/mcp");
        client.set_timeout(5);
        ASSERT_TRUE(client.initialize("PromptNotificationClient", MCP_VERSION));

        std::promise<void> notification_received;
        auto notification_future = notification_received.get_future();
        std::atomic<bool> fulfilled{false};
        client.set_notification_handler(
            [&notification_received, &fulfilled](const std::string& method, const json& params) {
                if (method == "notifications/prompts/list_changed" && params.empty() && !fulfilled.exchange(true)) {
                    notification_received.set_value();
                }
            });

        ASSERT_TRUE(client.start_sse_stream());
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        test_server.notify_prompts_list_changed();

        ASSERT_EQ(notification_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
        client.stop_sse_stream();
    }

    test_server.stop();
}

namespace {

bool extract_jsonrpc_sse_message(std::string& buffer, const char* data, size_t length, json& message) {
    buffer.append(data, length);
    size_t crlf_pos = buffer.find("\r\n");
    while (crlf_pos != std::string::npos) {
        buffer.replace(crlf_pos, 2, "\n");
        crlf_pos = buffer.find("\r\n", crlf_pos + 1);
    }

    size_t event_end = buffer.find("\n\n");
    if (event_end == std::string::npos) {
        return false;
    }

    const std::string event = buffer.substr(0, event_end);
    buffer.erase(0, event_end + 2);

    std::istringstream stream(event);
    std::string line;
    std::string data_content;
    while (std::getline(stream, line)) {
        if (line.rfind("data: ", 0) == 0) {
            if (!data_content.empty()) {
                data_content += '\n';
            }
            data_content += line.substr(6);
        }
    }

    if (data_content.empty()) {
        return false;
    }

    message = json::parse(data_content);
    return true;
}

} // namespace

TEST(CoreSamplingTest, SendsCreateMessageRequestAndWaitsForClientResponse) {
    server::configuration conf;
    conf.host = "localhost";
    conf.port = 19095;
    conf.session_timeout = 0;

    server test_server(conf);
    test_server.set_server_info("SamplingTestServer", "1.0.0");
    test_server.set_capabilities({{"tools", json::object()}});

    ASSERT_TRUE(test_server.start(false));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    streamable_http_client client("http://localhost:19095", "/mcp");
    client.set_timeout(5);
    client.set_capabilities({{"sampling", json::object()}});
    ASSERT_TRUE(client.initialize("SamplingTestClient", MCP_VERSION));
    const std::string session_id = client.get_session_id();
    ASSERT_FALSE(session_id.empty());

    std::promise<json> received_request_promise;
    auto received_request = received_request_promise.get_future();

    std::thread sse_thread([&]() {
        httplib::Client raw_client("http://localhost:19095");
        raw_client.set_read_timeout(30, 0);
        httplib::Headers headers{
            {"Mcp-Session-Id", session_id},
            {"MCP-Protocol-Version", MCP_VERSION},
            {"Accept", "text/event-stream"}
        };

        std::string buffer;
        auto res = raw_client.Get(
            "/mcp",
            headers,
            [&](const char* data, size_t length) {
                json message;
                if (extract_jsonrpc_sse_message(buffer, data, length, message) &&
                    message.value("method", "") == "sampling/createMessage") {
                    received_request_promise.set_value(message);
                }
                return true;
            });
        (void)res;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto sampling_result_future = std::async(std::launch::async, [&]() {
        return test_server.request_sampling(
            session_id,
            {
                {"messages", json::array({
                    {
                        {"role", "user"},
                        {"content", {
                            {"type", "text"},
                            {"text", "Summarize this report"}
                        }}
                    }
                })},
                {"maxTokens", 128}
            },
            std::chrono::seconds(5));
    });

    if (received_request.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        test_server.stop();
        if (sse_thread.joinable()) {
            sse_thread.join();
        }
        FAIL() << "Timed out waiting for sampling/createMessage on SSE stream";
    }
    json sampling_request = received_request.get();
    ASSERT_TRUE(sampling_request.contains("id"));
    EXPECT_EQ(sampling_request["method"], "sampling/createMessage");
    EXPECT_EQ(sampling_request["params"]["maxTokens"], 128);

    httplib::Client response_client("http://localhost:19095");
    httplib::Headers response_headers{
        {"Mcp-Session-Id", session_id},
        {"MCP-Protocol-Version", MCP_VERSION},
        {"Content-Type", "application/json"}
    };
    json client_response = {
        {"jsonrpc", "2.0"},
        {"id", sampling_request["id"]},
        {"result", {
            {"role", "assistant"},
            {"content", {
                {"type", "text"},
                {"text", "Summary from client sampling"}
            }},
            {"model", "test-model"},
            {"stopReason", "endTurn"}
        }}
    };

    auto post_result = response_client.Post(
        "/mcp",
        response_headers,
        client_response.dump(),
        "application/json");
    ASSERT_TRUE(post_result);
    EXPECT_EQ(post_result->status, 202);

    ASSERT_EQ(sampling_result_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    json sampling_result = sampling_result_future.get();
    EXPECT_EQ(sampling_result["content"]["text"], "Summary from client sampling");
    EXPECT_EQ(sampling_result["model"], "test-model");

    test_server.stop();
    if (sse_thread.joinable()) {
        sse_thread.join();
    }
}

TEST(CoreSamplingTest, RejectsSamplingWhenClientDidNotAdvertiseCapability) {
    server::configuration conf;
    conf.host = "localhost";
    conf.port = 19096;
    conf.session_timeout = 0;

    server test_server(conf);
    test_server.set_server_info("SamplingCapabilityTestServer", "1.0.0");
    test_server.set_capabilities({{"tools", json::object()}});

    ASSERT_TRUE(test_server.start(false));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    {
        streamable_http_client client("http://localhost:19096", "/mcp");
        client.set_timeout(5);
        client.set_capabilities({{"roots", json::object()}});
        ASSERT_TRUE(client.initialize("NoSamplingClient", MCP_VERSION));

        try {
            test_server.request_sampling(
                client.get_session_id(),
                {
                    {"messages", json::array()},
                    {"maxTokens", 64}
                },
                std::chrono::seconds(1));
            FAIL() << "Expected request_sampling to reject a client without sampling capability";
        } catch (const mcp_exception& e) {
            EXPECT_NE(std::string(e.what()).find("does not advertise sampling"), std::string::npos);
        }
    }

    test_server.stop();
}

namespace {

json echo_handler(const json& params, const std::string&) {
    return {
        {"content", json::array({{{"type", "text"}, {"text", params.value("text", "")}}})},
        {"isError", false}
    };
}

json calculator_handler(const json& params, const std::string&) {
    const std::string operation = params.value("operation", "");
    const double a = params.value("a", 0.0);
    const double b = params.value("b", 0.0);

    if (operation == "add") {
        return {
            {"content", json::array({{{"type", "text"}, {"text", std::to_string(a + b)}}})},
            {"isError", false}
        };
    }
    if (operation == "multiply") {
        return {
            {"content", json::array({{{"type", "text"}, {"text", std::to_string(a * b)}}})},
            {"isError", false}
        };
    }

    throw mcp_exception(error_code::invalid_params, "Unsupported operation: " + operation);
}

} // namespace

class StreamableHttpTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        server::configuration conf;
        conf.host = "localhost";
        conf.port = port_;
        conf.session_timeout = 0;

        server_ = std::make_unique<server>(conf);
        server_->set_server_info("TestServer", "1.0.0");
        server_->set_capabilities({{"tools", json::object()}});

        tool echo_tool = tool_builder("echo")
            .with_description("Echo input")
            .with_string_param("text", "Text to echo")
            .build();

        tool calc_tool = tool_builder("calculator")
            .with_description("Calculator")
            .with_string_param("operation", "Operation")
            .with_number_param("a", "First operand")
            .with_number_param("b", "Second operand")
            .build();

        server_->register_tool(echo_tool, echo_handler);
        server_->register_tool(calc_tool, calculator_handler);

        ASSERT_TRUE(server_->start(false));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ASSERT_TRUE(server_->is_running());
    }

    static void TearDownTestSuite() {
        if (server_) {
            server_->stop();
            server_.reset();
        }
    }

    std::unique_ptr<streamable_http_client> make_client() const {
        auto client = std::make_unique<streamable_http_client>(
            "http://localhost:" + std::to_string(port_),
            "/mcp");
        client->set_timeout(5);
        client->set_capabilities({{"roots", {{"listChanged", true}}}});
        return client;
    }

    static constexpr int port_ = 19091;
    static std::unique_ptr<server> server_;
};

std::unique_ptr<server> StreamableHttpTest::server_;

TEST_F(StreamableHttpTest, InitializeAndPing) {
    auto client = make_client();

    ASSERT_TRUE(client->initialize("TestClient", MCP_VERSION));
    EXPECT_FALSE(client->get_session_id().empty());
    EXPECT_TRUE(client->is_running());
    EXPECT_TRUE(client->ping());

    json capabilities = client->get_server_capabilities();
    EXPECT_TRUE(capabilities.contains("tools"));
}

TEST_F(StreamableHttpTest, StandardPingReturnsEmptyResult) {
    auto client = make_client();

    ASSERT_TRUE(client->initialize("TestClient", MCP_VERSION));
    response ping_response = client->send_request("ping");

    EXPECT_EQ(ping_response.jsonrpc, "2.0");
    EXPECT_EQ(ping_response.result, json::object());
}

TEST_F(StreamableHttpTest, ListAndCallTools) {
    auto client = make_client();

    ASSERT_TRUE(client->initialize("TestClient", MCP_VERSION));

    auto tools = client->get_tools();
    ASSERT_EQ(tools.size(), 2U);

    json echo_result = client->call_tool("echo", {{"text", "hello streamable"}});
    ASSERT_TRUE(echo_result.contains("content"));
    EXPECT_EQ(echo_result["content"][0]["text"], "hello streamable");

    json calc_result = client->call_tool("calculator", {
        {"operation", "add"},
        {"a", 10},
        {"b", 32}
    });
    ASSERT_TRUE(calc_result.contains("content"));
    EXPECT_EQ(calc_result["content"][0]["text"], "42.000000");
}

TEST_F(StreamableHttpTest, StartsAndStopsNotificationStream) {
    auto client = make_client();

    ASSERT_TRUE(client->initialize("TestClient", MCP_VERSION));
    client->set_notification_handler([](const std::string&, const json&) {});

    EXPECT_TRUE(client->start_sse_stream());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(client->is_running());

    client->stop_sse_stream();
}

TEST_F(StreamableHttpTest, ReceivesServerNotificationsOverSseStream) {
    auto client = make_client();

    ASSERT_TRUE(client->initialize("TestClient", MCP_VERSION));

    std::promise<json> notification_promise;
    auto notification_future = notification_promise.get_future();
    client->set_notification_handler([&](const std::string& method, const json& params) {
        if (method == "notifications/test_event") {
            notification_promise.set_value(params);
        }
    });

    ASSERT_TRUE(client->start_sse_stream());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    server_->broadcast_notification(request::create_notification(
        "test_event",
        {{"value", "delivered"}}));

    ASSERT_EQ(notification_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    json notification = notification_future.get();
    EXPECT_EQ(notification["value"], "delivered");

    client->stop_sse_stream();
}
