/**
 * @file mcp_test.cpp
 * @brief Tests for MCP message formatting and Streamable HTTP behavior.
 */

#include <gtest/gtest.h>

#include "mcp_message.h"
#include "mcp_server.h"
#include "mcp_streamable_http_client.h"
#include "mcp_tool.h"

#include <chrono>
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
