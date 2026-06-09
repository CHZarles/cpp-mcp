# Synology NAS MCP Example

This example shows how to expose Synology DSM FileStation and Download Station
operations as MCP tools through the optional `mcp-ext-server` plugin host.

The example has two processes:

```text
MCP client
  -> mcp-ext-server
    -> libsynology_tools.so
      -> synology-api-backend
        -> Synology DSM
```

`libsynology_tools.so` is a C++ adapter plugin. It fetches tool schemas from the
backend's `GET /tools` endpoint and forwards MCP tool calls to `POST
/tools/call`.

`backend/` is a Python HTTP backend built with Starlette and
`N4S4/synology-api`. It is intentionally not an MCP transport; it is the runtime
backend for this cpp-mcp usage example.

## Requirements

- CMake 3.14 or newer for the extension server.
- A C++17 compiler.
- Linux or another platform with `dlopen` and `.so` plugin support.
- Python 3.11 or newer.
- `uv` for running the Python backend.
- Synology DSM credentials with access to the DSM APIs used by the tools.

## Configure the Backend

Create a private environment file:

```bash
cp examples/synology-nas/backend/.env.example examples/synology-nas/backend/.env
```

Edit `examples/synology-nas/backend/.env`:

```env
SYNOLOGY_HOST=nas.local
SYNOLOGY_PORT=5000
SYNOLOGY_USERNAME=your-user
SYNOLOGY_PASSWORD=your-password
SYNOLOGY_SECURE=false
SYNOLOGY_CERT_VERIFY=false
BACKEND_TOKEN=replace-with-a-long-random-token
BACKEND_HOST=127.0.0.1
BACKEND_PORT=9000
```

`BACKEND_TOKEN` is the token accepted by the Python backend. The C++ plugin must
send the same value through `SYNOLOGY_BACKEND_TOKEN`.

## Run the Backend

From the repository root:

```bash
examples/synology-nas/scripts/run-backend.sh
```

Or run it directly:

```bash
uv run --project examples/synology-nas/backend synology-api-backend
```

Verify the backend:

```bash
curl http://127.0.0.1:9000/health
curl -H "Authorization: Bearer $BACKEND_TOKEN" http://127.0.0.1:9000/tools
```

## Build the MCP Extension Server

The Synology plugin is an opt-in example target:

```bash
cmake -B build \
  -DMCP_BUILD_EXT=ON \
  -DMCP_BUILD_SYNOLOGY_EXAMPLE=ON

cmake --build build --target mcp-ext-server synology_tools
```

`synology_tools` is not built by default when `MCP_BUILD_EXT=ON`; this keeps the
standard extension server build independent of Synology credentials and the
Python backend.

## Run the MCP Server

Set the adapter environment and start `mcp-ext-server`:

```bash
export SYNOLOGY_BACKEND_URL=http://127.0.0.1:9000
export SYNOLOGY_BACKEND_TOKEN="$BACKEND_TOKEN"
export SYNOLOGY_BACKEND_TIMEOUT=120

./build/ext/server/mcp-ext-server
```

Or use the helper script:

```bash
export SYNOLOGY_BACKEND_TOKEN="$BACKEND_TOKEN"
examples/synology-nas/scripts/run-ext-server.sh
```

The server loads `libsynology_tools.so` from `build/plugins/`, reads the tool
schemas from the backend, and exposes them as MCP tools.

## Smoke Test

With the backend running:

```bash
BACKEND_TOKEN="$BACKEND_TOKEN" examples/synology-nas/scripts/smoke-test.sh
```

The smoke test checks `/health` and `/tools`. It does not call Synology DSM tool
operations.

## Tests

Run the Python backend tests:

```bash
uv run --project examples/synology-nas/backend --extra dev pytest \
  examples/synology-nas/backend/tests
```

Run the C++ project tests:

```bash
cmake -B build -DMCP_BUILD_TESTS=ON -DMCP_BUILD_EXT=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Run the optional Synology adapter build:

```bash
cmake -B build -DMCP_BUILD_EXT=ON -DMCP_BUILD_SYNOLOGY_EXAMPLE=ON
cmake --build build --target synology_tools
```
