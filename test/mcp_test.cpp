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

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
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
