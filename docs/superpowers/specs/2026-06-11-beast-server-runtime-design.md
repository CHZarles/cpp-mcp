# Beast Server Runtime Design

Date: 2026-06-11

## Purpose

Replace the server-side use of `cpp-httplib` with a C++20
Boost.Asio/Boost.Beast HTTP runtime and make the MCP Streamable HTTP server
look like a real SDK component rather than a demo server wrapped around a
convenience library.

The important design goal is not merely changing dependencies. The current
server exposes `httplib` types in `mcp_server.h`, binds SSE delivery to
`httplib::DataSink`, and mixes HTTP response mutation with MCP session and
JSON-RPC protocol rules. That makes the server harder to test, harder to
optimize, and harder to explain as an SDK architecture.

The final version should separate three concerns:

- MCP business API and server state.
- Streamable HTTP transport semantics.
- HTTP socket/runtime implementation.

Confidence: high. This boundary matches the MCP problem domain and removes the
actual architectural weakness instead of only renaming the HTTP dependency.

## Non-Goals

- Do not rewrite `mcp_streamable_http_client` in this phase.
- Do not change tool, prompt, resource, or method handler APIs to coroutine or
  future-based APIs.
- Do not make SDK users write coroutine handlers to register normal MCP tools.
- Do not add WebSocket, HTTP/2, middleware, routing frameworks, or a general web
  framework.
- Do not replace every remaining use of `httplib` in examples, tests, or
  plugins unless that use leaks into the server SDK boundary.
- Do not add benchmark numbers as correctness assertions.

## Recommended Approach

Use C++20, Boost.Asio, and Boost.Beast for the default server runtime.

Reasons:

- The project has no compatibility burden and can move from C++17 to C++20.
- The local environment has Boost.Beast headers available.
- Beast gives direct control over accept, read, write, body limits, long-lived
  SSE responses, and shutdown semantics.
- Beast keeps the SDK lightweight compared with adopting a full web framework
  such as Drogon.
- C++20 coroutines let the runtime be written as clear asynchronous state
  machines rather than callback chains.
- The runtime can be explained cleanly in interviews: acceptor, coroutine per
  connection, parser, serializer, cancellation, SSE writer, and protocol
  transport layer.

## Language Standard

Raise the project language standard from C++17 to C++20.

Use C++20 where it improves the architecture:

- Boost.Asio `awaitable`, `co_spawn`, and `co_await` for the Beast HTTP runtime.
- `std::jthread` and `std::stop_token` for lifecycle-managed background
  threads where they simplify shutdown.
- `std::string_view` for writer interfaces and non-owning byte/text views.
- `std::span` only where contiguous buffers are passed without ownership.

Do not use C++20 as decoration:

- Do not rewrite protocol code into ranges-heavy pipelines.
- Do not expose concepts-heavy public SDK APIs.
- Do not make public tool, prompt, resource, or method handlers coroutine-only.

The public SDK should remain simple for business users. C++20 is primarily an
implementation tool for the HTTP runtime and shutdown model.

## Target Architecture

```text
mcp::server public API
        |
        v
streamable_http_transport
        |
        v
mcp::http::server_runtime
        |
        v
mcp::http::beast_server_runtime
```

### `mcp::server`

`mcp::server` remains the business-facing SDK object. It owns MCP server state:

- server name, version, capabilities, and instructions
- method handlers
- notification handlers
- tools
- prompts
- resources and resource templates
- session initialization state
- client capabilities
- resource subscriptions
- pending client requests used by sampling
- session cleanup handlers

The public usage pattern remains:

```cpp
mcp::server::configuration conf;
conf.host = "localhost";
conf.port = 8888;

mcp::server server(conf);
server.register_tool(tool, handler);
server.start(true);
```

The server public header must no longer include `httplib.h` and must no longer
expose `httplib` types.

### Streamable HTTP Transport

Introduce an internal transport layer that understands MCP over Streamable
HTTP. It owns protocol decisions but no sockets.

Responsibilities:

- CORS `OPTIONS` response.
- `POST /mcp` JSON parsing and JSON-RPC dispatch.
- initialize detection and session creation.
- `Mcp-Session-Id` validation.
- routing JSON-RPC responses from the client to pending server requests.
- returning `202` for notifications and client responses with no inline result.
- returning inline JSON or batch JSON responses.
- opening an SSE stream for `GET /mcp`.
- handling `DELETE /mcp` session close.

The transport should return explicit HTTP result objects instead of mutating
runtime-specific response classes.

```cpp
namespace mcp::http {

using headers = std::map<std::string, std::string>;

struct request {
    std::string method;
    std::string target;
    headers headers;
    std::string body;
    std::string remote_address;
    uint16_t remote_port = 0;
};

struct response {
    int status = 200;
    headers headers;
    std::string body;
    std::string content_type;
};

struct stream_response {
    int status = 200;
    headers headers;
    std::shared_ptr<sse_stream_source> stream;
};

using handler_result = std::variant<response, stream_response>;

} // namespace mcp::http
```

The exact names can change during implementation if the local code reads better,
but the boundary must remain: transport code deals in project-owned HTTP types,
not Beast or httplib types.

### HTTP Runtime Interface

Create a small runtime interface used by `mcp::server` or by the transport
bootstrap code.

```cpp
class server_runtime {
public:
    using handler = std::function<handler_result(const request&)>;

    virtual ~server_runtime() = default;
    virtual void route(std::string method, std::string path, handler h) = 0;
    virtual bool set_mount_point(
        const std::string& mount_point,
        const std::string& directory,
        headers response_headers) = 0;
    virtual bool listen(const std::string& host, int port, bool blocking) = 0;
    virtual void stop() = 0;
    virtual bool is_running() const = 0;
};
```

The interface should stay intentionally small. It is not a web framework.

### Beast Runtime

`beast_server_runtime` implements the runtime interface.

Expected internal model:

- one `boost::asio::io_context`
- one TCP acceptor
- N worker threads
- one connection object per TCP socket
- each connection owns its Beast flat buffer, parser, serializer, and socket
- request handling is expressed as one C++20 coroutine per connection
- route dispatch normalizes Beast requests into project-owned `http::request`
- normal responses are serialized with Beast
- SSE responses are written through an async coroutine stream writer

The runtime shape should be:

```text
io_context
  -> co_spawn(listener.run())
  -> async_accept
  -> co_spawn(http_connection.run())
  -> co_await async_read(request)
  -> transport handles MCP semantics
  -> co_await async_write(response) or co_await sse_stream.run()
```

Representative internal API:

```cpp
boost::asio::awaitable<void> listener::run();
boost::asio::awaitable<void> http_connection::run();
boost::asio::awaitable<void> http_connection::write_response(http::response);
boost::asio::awaitable<void> http_connection::write_stream(http::stream_response);
```

The coroutine boundary is internal to the runtime. Transport handlers can remain
ordinary functions returning `http::handler_result`.

Threading:

- Use `configuration.threadpool_size` as the default IO worker count.
- If it is 0, fall back to `std::thread::hardware_concurrency()`, then 1.
- Keep the existing MCP handler thread pool for business handler execution in
  this phase.
- Prefer `std::jthread` for IO workers and maintenance loops when it gives a
  cleaner stop path than manual thread flags.

## SSE and Event Dispatch

The current `event_dispatcher` writes directly to `httplib::DataSink`. Replace
that with a project-owned writer abstraction.

```cpp
class event_writer {
public:
    virtual ~event_writer() = default;
    virtual bool write(std::string_view bytes) = 0;
};
```

`event_dispatcher` should expose:

```cpp
wait_result wait_event_result(
    event_writer& writer,
    std::chrono::milliseconds timeout = std::chrono::seconds(10));

bool wait_event(
    event_writer& writer,
    std::chrono::milliseconds timeout = std::chrono::seconds(10));
```

Behavior:

- queued events are written in FIFO order
- timeout is distinct from closed
- writer failure returns `write_failed`
- `close()` wakes waiters
- activity timestamp updates when messages or keepalives are sent

For `GET /mcp`, Beast opens an SSE response:

- `Content-Type: text/event-stream`
- `Cache-Control: no-cache`
- `Connection: keep-alive`
- queued MCP messages are written as SSE frames
- keepalive comment `: keepalive\r\n\r\n` is sent after the configured wait
  interval when no event is available
- client disconnect closes only that SSE stream, not necessarily the MCP
  session

The session should be removed only by `DELETE /mcp`, session timeout, explicit
server cleanup, or another protocol-level close path. This allows clients to
reconnect their SSE stream without losing the MCP session.

First phase event delivery remains queue-based. The important change is removing
`httplib::DataSink` from the queue and writer boundary. An Asio-native async
event queue can be a later optimization after the Beast runtime replacement is
correct and covered by tests.

## Public API Changes

Keep the core business API stable:

- `server::configuration`
- `server::start(bool blocking)`
- `server::stop()`
- `server::is_running()`
- `server::set_server_info`
- `server::set_capabilities`
- `server::set_instructions`
- `server::register_method`
- `server::register_notification`
- `server::register_resource`
- `server::register_resource_template`
- `server::register_prompt`
- `server::register_tool`
- `server::register_session_cleanup`
- `server::set_auth_handler`
- `server::send_request`
- `server::request_sampling`
- `server::broadcast_notification`
- `server::notify_resource_updated`
- `server::notify_prompts_list_changed`
- `server::get_active_sessions`

Change `set_mount_point` to remove `httplib::Headers`:

```cpp
using headers = std::map<std::string, std::string>;

bool set_mount_point(
    const std::string& mount_point,
    const std::string& dir,
    headers response_headers = {});
```

Add body limit configuration:

```cpp
std::size_t max_request_body_bytes = 8 * 1024 * 1024;
```

This is a real SDK concern: MCP tool calls can have meaningful JSON payloads,
but the server must not accept unbounded request bodies.

## HTTP Behavior to Preserve

### `OPTIONS /mcp`

- Return `204`.
- Include CORS headers:
  - `Access-Control-Allow-Origin: *`
  - `Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS`
  - `Access-Control-Allow-Headers: Content-Type, Accept, Mcp-Session-Id`
  - `Access-Control-Expose-Headers: Mcp-Session-Id`

### `POST /mcp`

- Invalid JSON returns `400` with a JSON-RPC parse error body.
- Initialize request without existing session creates a new session.
- Initialize response includes `Mcp-Session-Id`.
- Initialize with an existing live session returns `400`.
- Non-initialize request without `Mcp-Session-Id` returns `400`.
- Non-initialize request with unknown session returns `404`.
- Notification-only request returns `202`.
- Client JSON-RPC response routed to a pending server request returns `202` if
  there is no other inline result.
- Single JSON-RPC request returns one JSON response object.
- Batch JSON-RPC requests return a JSON response array.
- If the client accepts `text/event-stream` and the transport chooses to stream
  multiple responses, return SSE frames.

### `GET /mcp`

- Missing `Mcp-Session-Id` returns `400`.
- Unknown session returns `404`.
- Uninitialized session returns `400`.
- Initialized session returns an SSE stream.

### `DELETE /mcp`

- Missing `Mcp-Session-Id` returns `400`.
- Unknown session returns `404`.
- Existing session is closed and returns `200`.

## Error Handling and Shutdown

### Runtime Errors

- Bad request parse at HTTP level returns `400`.
- Body exceeding `max_request_body_bytes` returns `413`.
- Unknown route returns `404`.
- Known route with an unsupported method returns `405`.
- Unexpected C++ exceptions are logged and return `500` with a small JSON error
  body.
- Protocol-level JSON-RPC errors should remain JSON-RPC errors where the
  protocol already expects a JSON-RPC response.

### Server Stop

`stop()` must:

- mark the server as no longer running
- stop accepting new sockets
- close active SSE writers
- close dispatchers during session cleanup
- stop the `io_context`
- join worker threads
- stop and join the maintenance thread

The shutdown path must be idempotent.

## CMake and Dependencies

Update CMake to use Boost for the server runtime.

Expected shape:

```cmake
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED COMPONENTS system)
target_link_libraries(mcp PUBLIC Boost::system Threads::Threads)
```

If the local Boost package exposes different imported targets, adapt to the
installed CMake package while keeping the dependency explicit.

If `MCP_SSL=ON`, keep OpenSSL support and implement the Beast TLS path with
`boost::asio::ssl::context`. TLS behavior must match the current
certificate/private-key configuration. Mutual TLS and advanced certificate
reload behavior are out of scope.

`common/httplib.h` remains in the repository for the streamable HTTP client,
tests, examples, and plugins during this phase. The hard requirement is that
server SDK headers and server runtime implementation no longer depend on it.

## Documentation Updates

Update README server dependency language:

- server runtime uses Boost.Asio/Boost.Beast
- streamable HTTP client still uses vendored `httplib.h` in this phase
- vendoring only the library now requires `include/`, `common/`, and Boost
  headers/libraries as described by CMake

Mention the architectural split briefly in the server section so the final code
is explainable from the docs.

## Tests

### Transport Unit Tests

Add tests that do not start sockets:

- invalid JSON returns `400`
- initialize returns `200` and `Mcp-Session-Id`
- initialize on existing live session returns `400`
- missing session returns `400`
- unknown session returns `404`
- notification-only post returns `202`
- batch requests return an array
- delete session returns `200`
- after delete, subsequent request returns `404`

These tests should use project-owned `http::request` and `http::response`
objects, not Beast or httplib types.

### SSE Dispatcher Unit Tests

Replace `httplib::DataSink` tests with a fake `event_writer`.

Cases:

- queues multiple events without overwriting
- reports timeout separately from closed
- writer failure returns `write_failed`
- close wakes a waiter

### Beast Integration Tests

Keep or adapt existing integration coverage:

- initialize
- ping
- tools/list
- tools/call
- start and stop SSE stream
- receive server notification over SSE
- sampling request over SSE and response over POST
- delete session cleanup
- CORS `OPTIONS`
- oversized body returns `413`

The existing `streamable_http_client` can still be used for integration tests.
Raw HTTP/SSE tests may continue to use `httplib::Client` as a test client if it
keeps the test short, because the requirement is to remove `httplib` from the
server SDK boundary.

## Performance Validation

Add an optional smoke benchmark or manual target rather than a brittle CI gate.

Recommended workload:

- start one Beast-backed MCP server
- create N concurrent clients
- initialize each client
- keep one SSE stream open per client
- issue concurrent echo tool calls
- report completed requests, failed requests, p50 latency, and p95 latency

Do not hard-code performance thresholds in correctness tests. CI machines vary
too much. The correctness gate should prove concurrency stability; the benchmark
tool should provide numbers for local comparison.

## Implementation Order

1. Introduce project-owned HTTP types and event writer abstraction.
2. Move `event_dispatcher` off `httplib::DataSink` and update unit tests.
3. Extract Streamable HTTP behavior from `server::handle_mcp_post/get/delete`
   into a transport component that consumes project HTTP types.
4. Raise the project language standard to C++20.
5. Add coroutine-based Beast runtime and route it to the transport.
6. Update `mcp::server` to own/use the Beast runtime and remove `httplib` from
   `mcp_server.h`.
7. Update CMake and README.
8. Run unit and integration tests.
9. Add a smoke benchmark target only after all acceptance criteria pass. This is
   useful supporting evidence, not part of the correctness gate.

## Acceptance Criteria

- `include/mcp_server.h` does not include `httplib.h`.
- No public server API contains `httplib` types.
- The project builds as C++20.
- `src/mcp_server.cpp` no longer constructs or calls `httplib::Server`.
- The Beast server runtime uses Asio coroutine primitives such as `awaitable`,
  `co_spawn`, and `co_await` for connection handling.
- Beast runtime handles `OPTIONS`, `POST`, `GET`, and `DELETE` for the MCP
  endpoint.
- Existing server examples still compile with either no source change or only
  mount-point header type changes.
- Existing Streamable HTTP client tests still pass against the Beast-backed
  server.
- SSE notification delivery and sampling over SSE still pass integration tests.
- Oversized request body returns `413`.
- README accurately describes Boost server dependency and remaining client-side
  `httplib` use.

## Risks and Mitigations

- Risk: Beast implementation becomes too low-level and spreads socket details
  through protocol code.
  Mitigation: keep Beast confined to `beast_server_runtime`.

- Risk: SSE stream lifecycle accidentally deletes MCP sessions on transient
  disconnect.
  Mitigation: make stream close and session delete separate concepts in tests.

- Risk: blocking MCP handlers stall IO threads.
  Mitigation: keep handler execution on the existing MCP thread pool where
  practical; IO threads should primarily perform socket work and lightweight
  dispatch.

- Risk: TLS support takes more time than plain HTTP.
  Mitigation: preserve the existing `MCP_SSL` option and implement the Beast
  TLS path only behind that flag.

- Risk: dirty worktree contains unrelated changes.
  Mitigation: stage and commit only files created or modified for this design
  and implementation.
