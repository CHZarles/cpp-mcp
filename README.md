# cpp-mcp

[![Version](https://img.shields.io/badge/version-2025.03.26-blue.svg)](CMakeLists.txt)
[![C++](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](CMakeLists.txt)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

`cpp-mcp` is a C++17 framework for building clients and servers for the
[Model Context Protocol](https://modelcontextprotocol.io/). It provides a
small static library, example programs, tests, and an optional extension server
that can load tool plugins.

The core library focuses on MCP over JSON-RPC 2.0 with Streamable HTTP and
stdio transports. It includes server-side helpers for exposing tools,
resources, resource templates, prompts, session notifications, and client-side
helpers for discovering and invoking MCP capabilities.

## Features

- **C++17 MCP library**: builds a static `mcp` target with public headers in
  [`include/`](include/).
- **Streamable HTTP server**: exposes a configurable MCP endpoint, defaulting
  to `POST /mcp`, `GET /mcp`, and `DELETE /mcp`.
- **Streamable HTTP client**: initializes sessions, sends requests, calls
  tools, reads resources, and can listen for server-initiated notifications over
  SSE.
- **stdio client**: starts and communicates with local MCP servers over
  standard input/output.
- **Tool builder API**: declares tools with JSON-schema-style parameters and
  registers C++ handlers.
- **Resource support**: exposes text, binary, file-backed resources, and
  resource templates.
- **Prompt metadata support**: registers prompt definitions and handlers on the
  server.
- **Optional TLS**: enables HTTPS client/server support through OpenSSL.
- **Examples and tests**: includes ready-to-build server, client, stdio, and
  agent examples plus GoogleTest-based tests.
- **Optional extension server**: builds `mcp-ext-server`, a Linux-oriented
  plugin host for dynamically loaded tool libraries.

## Repository Layout

```text
.
|-- CMakeLists.txt                 # Root CMake project and build options
|-- include/                       # Public cpp-mcp headers
|-- src/                           # Core library implementation
|-- common/                        # Vendored single-header dependencies
|-- examples/                      # Example server, clients, and agent demo
|-- test/                          # GoogleTest test target and test sources
|-- ext/server/                    # Optional plugin-based MCP server
|-- docs/mcp-code-walkthrough/     # Code walkthrough and reading notes
`-- LICENSE                        # MIT license
```

## Requirements

- CMake 3.10 or newer for the core project
- A C++17 compiler
- Threads support
- OpenSSL 3.0 or newer, only when building with `-DMCP_SSL=ON`
- GoogleTest submodule, only when building tests
- `nlohmann_json`, only for the optional extension server; CMake fetches
  `v3.11.2` if it is not already installed

The extension server uses `dlopen`/`dlsym` and `.so` plugins, so it currently
targets Linux/Unix-like systems.

## Build

Clone the repository and configure a release build:

```bash
git clone https://github.com/hkr04/cpp-mcp.git
cd cpp-mcp

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

This builds the `mcp` static library and the example executables under
`build/examples/`.

### Build Options

| Option | Default | Description |
| --- | --- | --- |
| `MCP_SSL` | `OFF` | Enable OpenSSL-backed HTTPS support. |
| `MCP_BUILD_TESTS` | `OFF` | Build the GoogleTest test target. |
| `MCP_BUILD_EXT` | `OFF` | Build the optional extension server and plugins. |
| `MCP_MAX_SESSIONS` | `10` | Maximum concurrent server sessions; `0` means unlimited. |
| `MCP_SESSION_TIMEOUT` | `30` | Inactive session timeout in seconds; `0` disables timeout cleanup. |

Enable tests:

```bash
git submodule update --init --recursive
cmake -B build -DMCP_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

Enable TLS support:

```bash
cmake -B build -DMCP_SSL=ON
cmake --build build --config Release
```

Enable the extension server:

```bash
cmake -B build -DMCP_BUILD_EXT=ON
cmake --build build --config Release
```

## Run the Examples

Start the Streamable HTTP server example:

```bash
./build/examples/server_example
```

The example listens on `0.0.0.0:8888` and registers four tools:

- `get_time`
- `echo`
- `calculator`
- `hello`

In another terminal, run the Streamable HTTP client example:

```bash
./build/examples/streamable_http_client_example
```

Run the stdio client against a local MCP server command:

```bash
./build/examples/stdio_client_example "npx -y @modelcontextprotocol/server-everything"
```

Run the agent example against an OpenAI-compatible chat completions endpoint:

```bash
./build/examples/agent_example \
  --base-url <base_url> \
  --endpoint /v1/chat/completions \
  --api-key <api_key> \
  --model <model_name>
```

When the agent example connects to an HTTPS endpoint, build with
`-DMCP_SSL=ON`.

## Quick Start: Server

The server API lets you register MCP tools with a schema and a handler. Tool
handlers receive the call parameters and the MCP session ID, then return MCP
content as JSON.

```cpp
#include "mcp_server.h"
#include "mcp_tool.h"

int main() {
    mcp::server::configuration config;
    config.host = "localhost";
    config.port = 8888;

    mcp::server server(config);
    server.set_server_info("DemoServer", "1.0.0");
    server.set_capabilities({
        {"tools", mcp::json::object()}
    });

    auto hello = mcp::tool_builder("hello")
        .with_description("Return a greeting")
        .with_string_param("name", "Name to greet", false)
        .build();

    server.register_tool(hello, [](const mcp::json& params,
                                   const std::string& session_id) {
        (void)session_id;
        const std::string name = params.value("name", "World");
        return mcp::json::array({
            {
                {"type", "text"},
                {"text", "Hello, " + name + "!"}
            }
        });
    });

    return server.start(true) ? 0 : 1;
}
```

See [`examples/server_example.cpp`](examples/server_example.cpp) for a fuller
server with time, echo, calculator, and greeting tools.

## Quick Start: Streamable HTTP Client

```cpp
#include "mcp_streamable_http_client.h"

#include <iostream>

int main() {
    mcp::streamable_http_client client("http://localhost:8888", "/mcp");
    client.set_timeout(10);

    if (!client.initialize("DemoClient", mcp::MCP_VERSION)) {
        return 1;
    }

    auto tools = client.get_tools();
    for (const auto& tool : tools) {
        std::cout << tool.name << ": " << tool.description << "\n";
    }

    mcp::json result = client.call_tool("hello", {
        {"name", "cpp-mcp"}
    });

    std::cout << result.dump(2) << "\n";
}
```

See
[`examples/streamable_http_client_example.cpp`](examples/streamable_http_client_example.cpp)
for session initialization, ping, tool discovery, tool calls, and optional SSE
notification handling.

## Quick Start: stdio Client

Use `mcp::stdio_client` when the target MCP server communicates over
stdin/stdout:

```cpp
#include "mcp_stdio_client.h"

int main() {
    mcp::stdio_client client(
        "npx -y @modelcontextprotocol/server-everything");

    if (!client.initialize("DemoStdioClient", "1.0.0")) {
        return 1;
    }

    auto capabilities = client.get_server_capabilities();
    auto tools = client.get_tools();

    (void)capabilities;
    (void)tools;
    return client.ping() ? 0 : 1;
}
```

See [`examples/stdio_client_example.cpp`](examples/stdio_client_example.cpp)
for environment variables, resource listing, resource reads, and ping behavior.

## Consuming the Library with CMake

This repository currently exposes a CMake target named `mcp` from the root
project. The simplest in-tree integration is `add_subdirectory`:

```cmake
cmake_minimum_required(VERSION 3.10)
project(my_mcp_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(path/to/cpp-mcp)

add_executable(my_mcp_app main.cpp)
target_link_libraries(my_mcp_app PRIVATE mcp)
```

If you vendor only the library sources instead of using the `mcp` CMake target,
include both [`include/`](include/) and [`common/`](common/) because the project
uses vendored `httplib.h`, `json.hpp`, and `base64.hpp`. You must also provide
the same compile definitions that the CMake target sets, for example
`MCP_MAX_SESSIONS=10` and `MCP_SESSION_TIMEOUT=30`.

## Extension Server

The optional extension server is a standalone MCP Streamable HTTP server that
loads tool plugins from shared libraries.

Build it from the repository root:

```bash
cmake -B build -DMCP_BUILD_EXT=ON
cmake --build build --config Release
./build/ext/server/mcp-ext-server
```

Root builds place plugin libraries in:

```text
build/plugins/
```

The current extension server includes:

- `libcalculator.so`: calculator tool plugin
- `libwsl_tools.so`: WSL workspace and cleanup helper tools
- WSL scan report resource templates

Read [`ext/server/README.md`](ext/server/README.md) for the plugin ABI,
discovery rules, return format, and examples.

## Documentation

- [`docs/mcp-code-walkthrough/00-reading-map.md`](docs/mcp-code-walkthrough/00-reading-map.md):
  recommended code reading order.
- [`docs/mcp-code-walkthrough/06-server-transport-and-session-lifecycle.md`](docs/mcp-code-walkthrough/06-server-transport-and-session-lifecycle.md):
  server transport and session lifecycle walkthrough.
- [`docs/mcp-code-walkthrough/07-clients-sse-stdio-streamable-http.md`](docs/mcp-code-walkthrough/07-clients-sse-stdio-streamable-http.md):
  client transport walkthrough.
- [`docs/mcp-code-walkthrough/08-examples-tests-and-extension-server.md`](docs/mcp-code-walkthrough/08-examples-tests-and-extension-server.md):
  examples, tests, and extension server walkthrough.
- [`docs/mcp-code-walkthrough/09-known-issues-and-reading-notes.md`](docs/mcp-code-walkthrough/09-known-issues-and-reading-notes.md):
  implementation boundaries and reading notes.
- [Model Context Protocol documentation](https://modelcontextprotocol.io/):
  protocol-level documentation.

## Testing

After enabling `MCP_BUILD_TESTS`, run:

```bash
ctest --test-dir build --output-on-failure
```

The main test target is `mcp_tests`. It covers JSON-RPC message formatting,
server/client Streamable HTTP behavior, event dispatching, plugin helpers, WSL
tool handlers, and WSL resource templates.

## Getting Help

- Start with the runnable examples in [`examples/`](examples/).
- Use the code walkthrough in [`docs/mcp-code-walkthrough/`](docs/mcp-code-walkthrough/)
  to map protocol concepts to repository files.
- For extension server and plugin questions, read
  [`ext/server/README.md`](ext/server/README.md).
- For protocol questions, consult the
  [Model Context Protocol documentation](https://modelcontextprotocol.io/).
- For project-specific issues, use the repository issue tracker for the clone
  you are working from.

## Contributing

Contributions are welcome through issues and pull requests.

Before opening a pull request:

1. Build the project with CMake.
2. Run the test suite with `MCP_BUILD_TESTS=ON`.
3. Keep changes focused and include tests for behavior changes.
4. Update examples or docs when public usage changes.
5. Follow the existing C++17 style and avoid unrelated formatting churn.

There is no separate root `CONTRIBUTING.md` in this repository at the moment.
Until one is added, use this section as the contribution guide.

## Maintainers

This project is maintained by the `cpp-mcp` authors. The license file identifies
the copyright holder as "The cpp-mcp authors"; see [`LICENSE`](LICENSE).

If you maintain a fork, keep this README, build options, and examples aligned
with the behavior in your branch so downstream users can reproduce your setup.

## License

`cpp-mcp` is licensed under the MIT License. See [`LICENSE`](LICENSE) for the
full license text.
