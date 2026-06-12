# syntax=docker/dockerfile:1.7
# mcp-synology runtime image.
#
# C++ binaries are built on the host first, then copied from:
#   build/ext/server/mcp-ext-server
#   build/plugins/libsynology_tools.so
#
# This Dockerfile intentionally does not install build-essential/cmake/git or
# compile cpp-mcp inside the image.

ARG PYTHON_RUNTIME_IMAGE=python:3.12-slim-trixie

FROM ${PYTHON_RUNTIME_IMAGE} AS python-deps

COPY --from=ghcr.io/astral-sh/uv:latest /uv /usr/local/bin/uv

WORKDIR /app/backend
COPY ext/server/plugins/synology/backend /app/backend

ENV UV_PYTHON=/usr/local/bin/python3 \
    UV_PYTHON_DOWNLOADS=never

RUN uv sync --frozen --no-dev --compile-bytecode \
    && rm -rf /root/.cache/uv

FROM ${PYTHON_RUNTIME_IMAGE}

RUN apt-get update && apt-get install -y --no-install-recommends \
        tini \
        ca-certificates \
        wget \
    && rm -rf /var/lib/apt/lists/*

# Locally prebuilt C++ artifacts.
COPY build/ext/server/mcp-ext-server /app/bin/mcp-ext-server
COPY build/plugins/libsynology_tools.so /app/plugins/libsynology_tools.so

# Python backend with dependencies already installed in /app/backend/.venv.
COPY --from=python-deps /app/backend /app/backend

# entrypoint
COPY ext/server/plugins/synology/docker/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh /app/bin/mcp-ext-server

WORKDIR /app

# Defaults (docker run -e 会覆盖)
ENV SYNOLOGY_BACKEND_URL=http://127.0.0.1:9000 \
    BACKEND_PORT=9000 \
    MCP_LISTEN_HOST=0.0.0.0 \
    MCP_LISTEN_PORT=8888 \
    PYTHONUNBUFFERED=1

EXPOSE 8888

HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 \
    CMD wget -qO- http://127.0.0.1:8888/mcp \
        --header="Content-Type: application/json" \
        --header="Accept: application/json, text/event-stream" \
        --post-data='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"healthcheck","version":"1"}}}' \
        >/dev/null \
        || exit 1

ENTRYPOINT ["/usr/bin/tini", "--", "/entrypoint.sh"]
