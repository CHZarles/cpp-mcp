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
server_core + session_registry
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

`mcp::server` remains the business-facing SDK object. It owns the public API and
composes the internal components, but it should not directly implement HTTP
request handling. Its internal state should be moved behind `server_core` and
`session_registry` instead of being spread across HTTP handlers.

`server_core` owns MCP server state:

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

`session_registry` owns session lifecycle state:

- session IDs
- initialized flags
- client capabilities
- resource subscriptions
- per-session event dispatchers
- last activity timestamps

The ownership rule is explicit:

- `streamable_http_transport` owns no session maps.
- `streamable_http_transport` owns no tool, prompt, resource, or pending request
  maps.
- `streamable_http_transport` translates HTTP requests into protocol operations
  and delegates stateful work to `server_core` and `session_registry`.
- `server_core` and `session_registry` are the only components allowed to mutate
  MCP state.

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
HTTP. It owns HTTP/protocol mapping decisions but no sockets and no MCP state
maps.

Responsibilities:

- CORS `OPTIONS` response.
- `POST /mcp` JSON parsing and JSON-RPC message classification.
- initialize detection.
- `Mcp-Session-Id` validation.
- delegating session creation, lookup, and deletion to `session_registry`.
- delegating JSON-RPC request execution and client response routing to
  `server_core`.
- returning `202` for notifications and client responses with no inline result.
- returning inline JSON or batch JSON responses.
- opening an SSE stream for `GET /mcp`.
- handling `DELETE /mcp` by delegating session close.

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

Because handler execution, session lookup, and SSE stream setup can require
serialized state access or handler-pool offload, the transport entry point is
asynchronous:

```cpp
class streamable_http_transport {
public:
    boost::asio::awaitable<http::handler_result> handle(http::request req);
};
```

This avoids a false synchronous boundary. A synchronous `handler_result
handle(request)` interface would force the implementation either to block IO
threads or to smuggle asynchronous state through ad hoc futures.

### HTTP Runtime Interface

Create a small runtime interface used by `mcp::server` or by the transport
bootstrap code.

```cpp
class server_runtime {
public:
    using handler =
        std::function<boost::asio::awaitable<handler_result>(request)>;

    virtual ~server_runtime() = default;
    virtual void route(std::string method, std::string path, handler h) = 0;
    virtual bool set_mount_point(
        const std::string& mount_point,
        const std::string& directory,
        headers response_headers) = 0;
    virtual bool listen(runtime_options options, bool blocking) = 0;
    virtual void stop() = 0;
    virtual bool is_running() const = 0;
};
```

The interface should stay intentionally small. It is not a web framework.

`runtime_options` carries runtime-level settings:

```cpp
struct runtime_options {
    std::string host;
    int port = 8080;
    std::size_t io_threads = 1;
    std::size_t max_connections = 1024;
    std::size_t max_request_body_bytes = 8 * 1024 * 1024;
    std::size_t max_header_bytes = 64 * 1024;
    std::optional<tls_options> tls;
};

struct tls_options {
    std::string certificate_path;
    std::string private_key_path;
};
```

TLS is represented in runtime configuration, not hidden behind unrelated
server state. The Beast implementation owns the `ssl::context` lifetime and
selects a plain TCP or TLS connection coroutine at accept time.

### Beast Runtime

`beast_server_runtime` implements the runtime interface.

Expected internal model:

- one `boost::asio::io_context`
- one TCP acceptor
- N worker threads
- one connection object per TCP socket
- each connection owns its Beast flat buffer, parser, serializer, and socket
- each parser enforces `max_request_body_bytes`
- connection admission enforces `max_connections`
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

The Beast socket details are internal to the runtime. Transport handlers are
coroutine entry points returning `boost::asio::awaitable<http::handler_result>`,
but they deal only in project-owned HTTP types and MCP state facades.

Memory and admission control:

- active connections are counted in the runtime
- when `max_connections` is reached, the runtime rejects new connections with a
  minimal `503 Service Unavailable` response when possible, then closes the
  socket
- request body parsing uses Beast parser body limits
- header parsing uses the configured header limit when Beast exposes it; if not,
  the runtime rejects overly large headers through the nearest supported parser
  error path
- flat buffers are connection-scoped and released when the connection closes

Threading:

- Use `configuration.io_threads` as the default IO worker count.
- If it is 0, fall back to `std::thread::hardware_concurrency()`, then 1.
- Keep the existing `configuration.threadpool_size` for business handler
  execution.
- Prefer `std::jthread` for IO workers and maintenance loops when it gives a
  cleaner stop path than manual thread flags.

## Concurrency and State Synchronization

The Beast runtime can run multiple IO threads, so the design must define where
shared MCP state is accessed. The rule is:

- socket IO runs on the runtime `io_context`
- each HTTP connection coroutine is serialized by its own strand or equivalent
  coroutine sequencing
- all mutable MCP server/session state is serialized through one
  `server_state_strand`
- long-running business handlers run on the existing handler pool, not on
  `server_state_strand` and not on IO threads

Mutable MCP state includes:

- session registry maps
- initialized-session flags
- client capabilities
- resource subscriptions
- pending sampling/client request promises
- tool, prompt, method, notification, and resource registries when mutated

Request handling uses this pattern:

```text
HTTP connection coroutine
  -> co_await transport.handle(request)
      -> co_await server_state_strand operation for session/protocol state
      -> submit business handler to handler pool when needed
      -> co_await handler completion without blocking IO threads
      -> co_await server_state_strand operation for response-side state updates
  -> co_await Beast write
```

The `server_state_strand` sections must stay small. They validate and mutate
state, snapshot the registered handler needed for a request, and publish final
state changes. Tool/resource/prompt handlers must not execute on the strand.

Public `mcp::server` APIs use the same state facade:

- setup APIs called before `start()` can mutate state directly through the
  facade before IO workers exist
- APIs callable while running, such as `broadcast_notification`,
  `notify_resource_updated`, `request_sampling`, and `get_active_sessions`,
  must enter through the serialized state facade

The first implementation may default `io_threads` to 1 to reduce migration risk,
but it must still use the strand-based state facade. That prevents accidental
data races when `io_threads` is raised later.

## SSE and Event Dispatch

The current `event_dispatcher` writes directly to `httplib::DataSink`. Replace
that with an asynchronous stream-source abstraction. A synchronous
`event_writer::write()` is not acceptable for the Beast runtime because it would
either block an IO thread or force hidden buffering inside the writer.

```cpp
class sse_stream_source {
public:
    virtual ~sse_stream_source() = default;
    virtual boost::asio::awaitable<std::optional<std::string>>
        async_next_frame(std::chrono::milliseconds keepalive_after) = 0;
    virtual void close() = 0;
};
```

`event_dispatcher` becomes a bounded, asynchronous producer/consumer queue:

```cpp
class event_dispatcher : public sse_stream_source {
public:
    enum class enqueue_result {
        queued,
        closed,
        overflow
    };

    enqueue_result enqueue(std::string frame);

    boost::asio::awaitable<std::optional<std::string>>
        async_next_frame(std::chrono::milliseconds keepalive_after) override;

    void close() override;
};
```

Behavior:

- queued events are written in FIFO order
- `async_next_frame()` returns an event frame when one is available
- `async_next_frame()` returns a keepalive frame after `keepalive_after`
- `async_next_frame()` returns `std::nullopt` when the dispatcher is closed
- `close()` resumes waiters
- activity timestamp updates when messages or keepalives are sent
- the queue has a bounded capacity
- overflow is explicit and never silently drops frames

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

Backpressure rules:

- Add `configuration.max_sse_queue_depth`, default `1024`.
- Add `configuration.max_sse_frame_bytes`, default `1 * 1024 * 1024`.
- `enqueue()` returns `overflow` if the queue is full.
- On overflow, close that session with a logged backpressure reason instead of
  blocking an IO thread or silently dropping protocol messages.
- A slow client can therefore lose its session, but it cannot force unbounded
  memory growth.

Keepalive timers:

- each SSE stream owns a `boost::asio::steady_timer`
- the timer is scoped to the stream coroutine
- timer cancellation is part of stream close
- keepalive scheduling must not block or sleep an IO thread

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

This is an intentional breaking source change for callers that pass
`httplib::Headers`. Migration is mechanical:

```cpp
// before
httplib::Headers headers{{"Cache-Control", "no-store"}};
server.set_mount_point("/files", "./files", headers);

// after
mcp::http::headers headers{{"Cache-Control", "no-store"}};
server.set_mount_point("/files", "./files", headers);
```

Add runtime and memory-limit configuration:

```cpp
std::size_t max_request_body_bytes = 8 * 1024 * 1024;
std::size_t max_header_bytes = 64 * 1024;
std::size_t max_connections = 1024;
std::size_t max_sse_queue_depth = 1024;
std::size_t max_sse_frame_bytes = 1 * 1024 * 1024;
std::size_t io_threads = 1;
unsigned int threadpool_size = std::thread::hardware_concurrency();
```

These are real SDK concerns: MCP tool calls can have meaningful JSON payloads,
but the server must not accept unbounded request bodies, unlimited connections,
or unlimited per-session SSE backlog. `io_threads` controls Beast socket IO;
`threadpool_size` continues to control MCP business handler execution.

## HTTP Behavior to Preserve

### `OPTIONS /mcp`

- Return `204`.
- Always bypass MCP session validation.
- Include CORS headers:
  - `Access-Control-Allow-Origin: *`
  - `Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS`
  - `Access-Control-Allow-Headers: Content-Type, Accept, Mcp-Session-Id`
  - `Access-Control-Expose-Headers: Mcp-Session-Id`

### `POST /mcp`

- Invalid JSON returns `400` with a JSON-RPC parse error body.
- Valid JSON that is not a valid JSON-RPC request, response, notification, or
  batch returns `400` with a JSON-RPC invalid request error body.
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
- Header block exceeding `max_header_bytes` returns `431` if Beast exposes that
  distinction cleanly, otherwise `400`.
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

TLS implementation requirements:

- `server::configuration` converts certificate/private-key fields into
  `runtime_options::tls`.
- `beast_server_runtime` owns the `boost::asio::ssl::context`.
- TLS connections perform async handshake before HTTP read.
- Plain TCP and TLS connection coroutines share request handling after the
  stream is established.
- SSL-specific socket details do not leak into `streamable_http_transport` or
  `server_core`.

Compiler support baseline:

- Linux GCC 11.5 is the local baseline for this repository.
- CI should cover at least one GCC and one Clang C++20 build before claiming
  portable C++20 support.
- MSVC support remains intended because the project already has Windows build
  branches, but it should be treated as unverified until a Windows C++20 CI job
  is added.

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
- valid JSON with invalid JSON-RPC shape returns `400`
- initialize returns `200` and `Mcp-Session-Id`
- initialize on existing live session returns `400`
- missing session returns `400`
- unknown session returns `404`
- notification-only post returns `202`
- batch requests return an array
- delete session returns `200`
- after delete, subsequent request returns `404`

These tests should use project-owned `http::request` and `http::response`
objects, not Beast or httplib types. The transport tests should execute through
the async transport entry point so the tested interface matches the Beast
runtime contract.

### SSE Dispatcher Unit Tests

Replace `httplib::DataSink` tests with async queue/source tests.

Cases:

- queues multiple events without overwriting
- returns keepalive after the configured interval
- reports closed as `std::nullopt`
- close wakes a waiting coroutine
- queue overflow returns `overflow`
- oversized SSE frame returns overflow/failure without queue growth

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
- oversized headers return `431` or `400`, matching the implemented runtime
  behavior
- excessive SSE backlog closes the affected session
- slow or disconnected SSE clients do not block other requests

The existing `streamable_http_client` can still be used for integration tests.
Raw HTTP/SSE tests may continue to use `httplib::Client` as one compatibility
client if it keeps the test short, because the requirement is to remove
`httplib` from the server SDK boundary.

Add a small raw TCP or Beast/Asio negative-test client for cases `httplib`
abstracts away:

- malformed request line
- partial request followed by disconnect
- request body larger than configured limit
- unsupported method on a known route
- unknown route

## Performance Validation

Add a repeatable smoke benchmark target and a CI warning path. The benchmark is
not a correctness gate, but it must produce numbers that can be compared across
runs.

Recommended workload:

- start one Beast-backed MCP server
- create N concurrent clients
- initialize each client
- keep one SSE stream open per client
- issue concurrent echo tool calls
- report completed requests, failed requests, p50 latency, and p95 latency

Benchmark requirements:

- provide a local executable or script, for example `mcp_server_benchmark`
- write JSON output with workload parameters and measured results
- keep a checked-in baseline format, not a hard-coded universal truth
- CI may run the benchmark in warning mode
- warning mode reports a regression if throughput drops by more than 30% or p95
  latency rises by more than 30% against the selected baseline
- warning mode must not fail correctness CI until the benchmark environment is
  stable enough to make it a gate

Correctness tests still prove protocol behavior and concurrency stability. The
benchmark catches obvious performance regressions without pretending that shared
CI machines produce deterministic latency.

## Implementation Order

1. Raise the project language standard to C++20 and verify the existing build.
2. Introduce project-owned HTTP types, runtime options, and async handler
   contract.
3. Introduce `server_core`, `session_registry`, and the serialized
   `server_state_strand` facade.
4. Replace `event_dispatcher`/`httplib::DataSink` with the bounded async SSE
   stream source and update unit tests.
5. Extract Streamable HTTP behavior from `server::handle_mcp_post/get/delete`
   into an async transport component that consumes project HTTP types and
   delegates stateful work to `server_core` and `session_registry`.
6. Add coroutine-based Beast runtime and route it to the transport.
7. Update `mcp::server` to own/use the Beast runtime and remove `httplib` from
   `mcp_server.h`.
8. Update CMake and README.
9. Run unit and integration tests.
10. Add the smoke benchmark target after protocol acceptance criteria pass. This is
   useful supporting evidence, not part of the correctness gate.

## Acceptance Criteria

- `include/mcp_server.h` does not include `httplib.h`.
- No public server API contains `httplib` types.
- The project builds as C++20.
- `src/mcp_server.cpp` no longer constructs or calls `httplib::Server`.
- The Beast server runtime uses Asio coroutine primitives such as `awaitable`,
  `co_spawn`, and `co_await` for connection handling.
- Transport handlers use an async contract rather than a synchronous
  `handler_result handle(request)` boundary.
- Mutable session/server state is accessed through `server_core`,
  `session_registry`, and the serialized state facade, not directly from HTTP
  connection coroutines.
- Tool/resource/prompt handlers do not run on IO threads or on the
  `server_state_strand`.
- SSE delivery uses a bounded async queue/source with explicit overflow
  behavior.
- Beast runtime handles `OPTIONS`, `POST`, `GET`, and `DELETE` for the MCP
  endpoint.
- Existing server examples still compile with either no source change or only
  mount-point header type changes.
- Existing Streamable HTTP client tests still pass against the Beast-backed
  server.
- SSE notification delivery and sampling over SSE still pass integration tests.
- Oversized request body returns `413`.
- Raw TCP or Beast/Asio negative tests cover malformed request, partial
  disconnect, unknown route, and unsupported method.
- Backpressure tests prove a slow SSE client cannot grow memory without bound.
- `MCP_SSL=ON` builds through the Beast runtime path, or SSL support is
  explicitly disabled with a documented compile-time error for that phase. The
  preferred acceptance target is that `MCP_SSL=ON` builds.
- README accurately describes Boost server dependency and remaining client-side
  `httplib` use.
- Benchmark target emits JSON results and CI warning mode can compare against a
  baseline without failing correctness tests.

## Risks and Mitigations

- Risk: Beast implementation becomes too low-level and spreads socket details
  through protocol code.
  Mitigation: keep Beast confined to `beast_server_runtime`.

- Risk: SSE stream lifecycle accidentally deletes MCP sessions on transient
  disconnect.
  Mitigation: make stream close and session delete separate concepts in tests.

- Risk: shared MCP state races when multiple Beast IO threads handle requests.
  Mitigation: route all mutable server/session state through
  `server_state_strand` and keep business handlers off that strand.

- Risk: blocking MCP handlers stall IO threads.
  Mitigation: keep handler execution on the existing MCP thread pool where
  practical; IO threads should primarily perform socket work and lightweight
  dispatch.

- Risk: slow SSE clients cause unbounded memory growth.
  Mitigation: bounded per-session SSE queue, maximum frame size, overflow-driven
  session close, and tests for slow-client behavior.

- Risk: Beast buffers grow under large or malformed requests.
  Mitigation: enforce request body limit, header limit, maximum connections, and
  negative tests for oversized and partial requests.

- Risk: TLS support takes more time than plain HTTP.
  Mitigation: represent TLS in `runtime_options`, keep `ssl::context` ownership
  inside `beast_server_runtime`, and require at least a build check for
  `MCP_SSL=ON`.

- Risk: C++20 coroutine behavior differs across compilers.
  Mitigation: use Boost.Asio coroutine APIs rather than custom coroutine
  machinery, document the compiler baseline, and add GCC/Clang C++20 CI before
  claiming portable support.

- Risk: dirty worktree contains unrelated changes.
  Mitigation: stage and commit only files created or modified for this design
  and implementation.
