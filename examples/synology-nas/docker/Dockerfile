# syntax=docker/dockerfile:1.7
# mcp-synology: cpp-mcp mcp-ext-server + libsynology_tools.so + synology-api-backend in one image.

# ---------- Stage 1: builder ----------
FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        git \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
# 整个 cpp-mcp 源码作为 build context 根, 拷进来
COPY . /src

# 仅编 mcp-ext-server + synology_tools, 关闭 calculator 和 wsl_tools
RUN cmake -B build -S . \
        -DCMAKE_BUILD_TYPE=Release \
        -DMCP_BUILD_EXT=ON \
        -DMCP_BUILD_SYNOLOGY_EXAMPLE=ON \
        -DMCP_BUILD_CALCULATOR_PLUGIN=OFF \
        -DMCP_BUILD_WSL_PLUGIN=OFF \
    && cmake --build build --target mcp-ext-server synology_tools -j"$(nproc)"

# ---------- Stage 2: runtime ----------
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        tini \
        ca-certificates \
        curl \
        wget \
    && rm -rf /var/lib/apt/lists/*

# uv for Python backend
COPY --from=ghcr.io/astral-sh/uv:latest /uv /usr/local/bin/uv

# C++ 产物
COPY --from=builder /src/build/ext/server/mcp-ext-server /app/bin/mcp-ext-server
COPY --from=builder /src/build/plugins/libsynology_tools.so /app/plugins/libsynology_tools.so

# Python 后端 (整个 uv project)
COPY examples/synology-nas/backend /app/backend

# entrypoint
COPY examples/synology-nas/docker/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

# 在 build 时把 Python 依赖 + 项目本身同步进镜像, 避免每次启动 uv sync
WORKDIR /app/backend
RUN uv sync --frozen

WORKDIR /app

# Defaults (docker run -e 会覆盖)
ENV SYNOLOGY_BACKEND_URL=http://127.0.0.1:9000 \
    BACKEND_PORT=9000 \
    MCP_LISTEN_HOST=0.0.0.0 \
    MCP_LISTEN_PORT=8888 \
    PYTHONUNBUFFERED=1

EXPOSE 8888

HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 \
    CMD curl -fsS -X POST http://127.0.0.1:8888/mcp \
        -H "Content-Type: application/json" \
        -H "Accept: application/json, text/event-stream" \
        -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"healthcheck","version":"1"}}}' \
        || exit 1

ENTRYPOINT ["/usr/bin/tini", "--", "/entrypoint.sh"]
