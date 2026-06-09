# Synology NAS MCP Docker 化实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 `examples/synology-nas/`（mcp-ext-server + libsynology_tools.so + Python 后端 + 配置）打包成单一 OCI 镜像，让用户用一条 `docker run` 起起来后，从 MCP 客户端连 `http://localhost:8888/mcp` 即可看到 17 个 Synology 工具。

**Architecture:** 多阶段 Dockerfile（builder 阶段用 `debian:bookworm-slim` 编译 C++，runtime 阶段塞 `tini` + 二进制 + `.so` + uv 同步好的 Python 后端）。`tini` PID 1 → `entrypoint.sh` 拉起后端（uv 跑 `synology-api-backend`，端口 9000 留容器内）等健康后 `exec` mcp-ext-server（端口 8888，对外暴露）。MCP 客户端用 JSON-RPC over HTTP 接 `:8888/mcp`。

**Tech Stack:** CMake 3.14+ / C++17、starlette + uvicorn + synology-api、uv、tini、debian:bookworm-slim。

**User Verification:** YES — 真实 NAS 凭据下 `tools/call list_shares` 真实返回用户的共享目录，需要用户在自己的环境验证。

---

## 文件结构

### 新增

| 路径 | 职责 |
|---|---|
| `examples/synology-nas/docker/Dockerfile` | 多阶段构建 builder → runtime |
| `examples/synology-nas/docker/entrypoint.sh` | 校验 env、写临时 `.env`、拉起后端、exec ext-server |
| `examples/synology-nas/docker/.dockerignore` | build context 排除项 |
| `examples/synology-nas/docker/.env.example` | 环境变量模板（无真实凭据） |
| `examples/synology-nas/docker/README.md` | 镜像用法 + Claude/Codex 接入 |

### 修改

| 路径 | 改动 |
|---|---|
| `ext/server/CMakeLists.txt` | 加 `MCP_BUILD_CALCULATOR_PLUGIN` 和 `MCP_BUILD_WSL_PLUGIN` 两个 option（默认 ON），OFF 时跳过对应 `add_library` |
| `ext/server/src/main.cpp` | `conf.host` / `conf.port` 改为读 `MCP_LISTEN_HOST` / `MCP_LISTEN_PORT` 环境变量 |
| `examples/synology-nas/README.md` | 顶部加一段指向 `docker/README.md` 的提示 |

### 不动

- `examples/synology-nas/backend/*` 原样进镜像
- `examples/synology-nas/scripts/*` 留给原生编译用户
- `ext/server/plugins/calculator.cpp`、`wsl_tools/*`、`synology_tools.cpp` 源码不动

---

## Task 1: ext-server CMake — 加插件开关 option

**Goal:** 让 `cmake -B build -DMCP_BUILD_CALCULATOR_PLUGIN=OFF -DMCP_BUILD_WSL_PLUGIN=OFF` 时只编出 `libsynology_tools.so`，不编 calculator/wsl_tools。

**Files:**
- Modify: `ext/server/CMakeLists.txt:25-87`

**Acceptance Criteria:**
- [ ] 默认（不传新 option）行为与改动前一致 —— calculator + wsl_tools + synology_tools 三个 .so 全部生成
- [ ] `cmake -B /tmp/test-build -DMCP_BUILD_EXT=ON -DMCP_BUILD_SYNOLOGY_EXAMPLE=ON -DMCP_BUILD_CALCULATOR_PLUGIN=OFF -DMCP_BUILD_WSL_PLUGIN=OFF ...` 后，`/tmp/test-build/plugins/` 下只有 `libsynology_tools.so` 一个文件

**Verify:**
```bash
rm -rf /tmp/test-cmake-off
cmake -B /tmp/test-cmake-off -S /home/charles/cpp-mcp \
  -DMCP_BUILD_EXT=ON -DMCP_BUILD_SYNOLOGY_EXAMPLE=ON \
  -DMCP_BUILD_CALCULATOR_PLUGIN=OFF -DMCP_BUILD_WSL_PLUGIN=OFF 2>&1 | tail -3
cmake --build /tmp/test-cmake-off --target mcp-ext-server synology_tools -j2 2>&1 | tail -5
ls /tmp/test-cmake-off/plugins/
# 期望: 只有 libsynology_tools.so
rm -rf /tmp/test-cmake-off
```

**Steps:**

- [ ] **Step 1: 编辑 `ext/server/CMakeLists.txt`，加两个 option**

在 line 27（`option(MCP_BUILD_SYNOLOGY_EXAMPLE ...)` 之后）插入：

```cmake
option(MCP_BUILD_CALCULATOR_PLUGIN "Build the calculator example plugin" ON)
option(MCP_BUILD_WSL_PLUGIN "Build the WSL tools plugin" ON)
```

- [ ] **Step 2: 包 calculator 块**

把 line 47-52：

```cmake
# Example plugin
add_library(calculator SHARED plugins/calculator.cpp)
target_include_directories(calculator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/plugins)
target_link_libraries(calculator PRIVATE nlohmann_json::nlohmann_json)

set(EXT_PLUGIN_TARGETS calculator)
```

改为：

```cmake
# Example plugin (calculator) — opt-in via MCP_BUILD_CALCULATOR_PLUGIN
set(EXT_PLUGIN_TARGETS "")
if(MCP_BUILD_CALCULATOR_PLUGIN)
    add_library(calculator SHARED plugins/calculator.cpp)
    target_include_directories(calculator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/plugins)
    target_link_libraries(calculator PRIVATE nlohmann_json::nlohmann_json)
    list(APPEND EXT_PLUGIN_TARGETS calculator)
endif()
```

注意：把原来的 `set(EXT_PLUGIN_TARGETS calculator)`（line 52）从无条件赋值改为在 if 块内 `list(APPEND ...)`，并把初始化（line 52 原内容）改为空字符串 `set(EXT_PLUGIN_TARGETS "")` 在 if 之前。

- [ ] **Step 3: 包 wsl_tools 块**

把 line 69-87（`# WSL Tools - combined plugin ...` 块的最后 `list(APPEND EXT_PLUGIN_TARGETS wsl_tools)`）包在 `if(MCP_BUILD_WSL_PLUGIN) ... endif()` 里：

```cmake
# WSL Tools - combined plugin (one .so with multiple tools)
if(MCP_BUILD_WSL_PLUGIN)
    set(WSL_TOOLS_SOURCES
        plugins/wsl_tools/wsl_tools.cpp
        plugins/wsl_tools/wsl_create_directory.cpp
        plugins/wsl_tools/wsl_list_distros.cpp
        plugins/wsl_tools/wsl_scan_files.cpp
        plugins/wsl_tools/wsl_scan_read.cpp
        plugins/wsl_tools/wsl_recommend_cleanup.cpp
        plugins/wsl_tools/wsl_safe_delete.cpp
    )
    add_library(wsl_tools SHARED ${WSL_TOOLS_SOURCES})
    target_include_directories(wsl_tools PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/plugins
        ${CMAKE_CURRENT_SOURCE_DIR}/plugins/wsl_tools  # For relative includes like ../plugins/tool_api.h
    )
    target_link_libraries(wsl_tools PRIVATE nlohmann_json::nlohmann_json)
    list(APPEND EXT_PLUGIN_TARGETS wsl_tools)
endif()
```

- [ ] **Step 4: 验证默认行为未变**

```bash
cd /home/charles/cpp-mcp
rm -rf build
cmake -B build -DMCP_BUILD_EXT=ON -DMCP_BUILD_SYNOLOGY_EXAMPLE=ON 2>&1 | tail -3
cmake --build build --target mcp-ext-server synology_tools -j2 2>&1 | tail -5
ls build/plugins/
# 期望: libcalculator.so  libsynology_tools.so  libwsl_tools.so 三个都在
```

- [ ] **Step 5: 验证 OFF 行为**

```bash
cd /home/charles/cpp-mcp
rm -rf /tmp/test-cmake-off
cmake -B /tmp/test-cmake-off -S . \
  -DMCP_BUILD_EXT=ON -DMCP_BUILD_SYNOLOGY_EXAMPLE=ON \
  -DMCP_BUILD_CALCULATOR_PLUGIN=OFF -DMCP_BUILD_WSL_PLUGIN=OFF 2>&1 | tail -3
cmake --build /tmp/test-cmake-off --target mcp-ext-server synology_tools -j2 2>&1 | tail -5
ls /tmp/test-cmake-off/plugins/
# 期望: 只有 libsynology_tools.so
rm -rf /tmp/test-cmake-off
```

- [ ] **Step 6: 提交**

```bash
cd /home/charles/cpp-mcp
git add ext/server/CMakeLists.txt
git commit -m "feat(ext): gate calculator/wsl plugins behind cmake options"
```

---

## Task 2: ext-server — 读 `MCP_LISTEN_HOST` / `MCP_LISTEN_PORT` 环境变量

**Goal:** mcp-ext-server 启动时从环境变量读监听地址，默认 `127.0.0.1:8888`。Dockerfile 里 override 成 `0.0.0.0:8888`，让 `docker -p` 能把端口暴露出去。

**Files:**
- Modify: `ext/server/src/main.cpp:42-44`

**Acceptance Criteria:**
- [ ] 不传环境变量时，启动日志仍是 `Starting MCP server at localhost:8888`（行为兼容）
- [ ] 传 `MCP_LISTEN_HOST=0.0.0.0 MCP_LISTEN_PORT=9999` 后，日志变成 `Starting MCP server at 0.0.0.0:9999`
- [ ] 用 `ss -ltn` 看到 server 确实绑在 override 后的地址

**Verify:**
```bash
cd /home/charles/cpp-mcp
cmake --build build --target mcp-ext-server 2>&1 | tail -3
MCP_LISTEN_HOST=0.0.0.0 MCP_LISTEN_PORT=9999 ./build/ext/server/mcp-ext-server &
SERVER_PID=$!
sleep 2
ss -ltn | grep ':9999' && echo "OK: bound to 0.0.0.0:9999" || echo "FAIL"
curl -sf -m 2 -X POST http://127.0.0.1:9999/mcp \
  -H "Content-Type: application/json" -H "Accept: application/json, text/event-stream" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"t","version":"1"}}}' \
  | head -c 200
kill $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null
```

**Steps:**

- [ ] **Step 1: 编辑 `ext/server/src/main.cpp`**

在 line 42（`mcp::server::configuration conf;` 之前）插入 `#include <cstdlib>`（如果文件里没用到的话）。文件 line 1-6 已有 `#include <iostream>` 等。直接在文件顶部追加：

```cpp
#include <cstdlib>
```

然后把 line 41-44：

```cpp
    // Create MCP server
    mcp::server::configuration conf;
    conf.host = "localhost";
    conf.port = 8888;
```

改为：

```cpp
    // Create MCP server (host/port overridable via env for containerized deployment)
    mcp::server::configuration conf;
    if (const char* h = std::getenv("MCP_LISTEN_HOST"); h && *h) {
        conf.host = h;
    } else {
        conf.host = "localhost";
    }
    if (const char* p = std::getenv("MCP_LISTEN_PORT"); p && *p) {
        conf.port = std::atoi(p);
    } else {
        conf.port = 8888;
    }
```

- [ ] **Step 2: 重新编译**

```bash
cd /home/charles/cpp-mcp
cmake --build build --target mcp-ext-server 2>&1 | tail -3
```

期望：编译成功，无 warning（开了 `-Wall` 的话 `getenv` 用法 OK）。

- [ ] **Step 3: 验证默认行为未变**

```bash
cd /home/charles/cpp-mcp
./build/ext/server/mcp-ext-server &
SERVER_PID=$!
sleep 2
ss -ltn | grep ':8888' && echo "OK: default port 8888" || echo "FAIL: default port"
grep "Starting MCP server" <(journalctl --no-pager 2>/dev/null) || \
  (kill $SERVER_PID 2>/dev/null; wait $SERVER_PID 2>/dev/null)
# 简单方法: 重启看 log
kill $SERVER_PID 2>/dev/null; wait $SERVER_PID 2>/dev/null

# 第二次启动只是为了拿一行 log
cd /home/charles/cpp-mcp
( ./build/ext/server/mcp-ext-server & echo $! > /tmp/srv.pid ) 2>&1 | head -3
kill $(cat /tmp/srv.pid) 2>/dev/null
```

期望看到 `Starting MCP server at localhost:8888`。

- [ ] **Step 4: 验证 env 覆盖生效**

```bash
cd /home/charles/cpp-mcp
( MCP_LISTEN_HOST=0.0.0.0 MCP_LISTEN_PORT=9999 ./build/ext/server/mcp-ext-server & echo $! > /tmp/srv.pid ) 2>&1 | head -5
sleep 1
ss -ltn | grep ':9999' && echo "OK: bound to override" || echo "FAIL"
kill $(cat /tmp/srv.pid) 2>/dev/null
rm -f /tmp/srv.pid
```

期望看到 `Starting MCP server at 0.0.0.0:9999` + `OK: bound to override`。

- [ ] **Step 5: 提交**

```bash
cd /home/charles/cpp-mcp
git add ext/server/src/main.cpp
git commit -m "feat(ext): honor MCP_LISTEN_HOST/PORT env vars"
```

---

## Task 3: Docker scaffold — `.dockerignore` 和 `.env.example`

**Goal:** 建立 `examples/synology-nas/docker/` 目录的两个最小辅助文件。

**Files:**
- Create: `examples/synology-nas/docker/.dockerignore`
- Create: `examples/synology-nas/docker/.env.example`

**Acceptance Criteria:**
- [ ] 两个文件存在且内容正确
- [ ] `.dockerignore` 排除 build 输出、.venv、.git、docs 等无关内容

**Verify:**
```bash
cd /home/charles/cpp-mcp
ls -la examples/synology-nas/docker/
cat examples/synology-nas/docker/.dockerignore
cat examples/synology-nas/docker/.env.example
```

**Steps:**

- [ ] **Step 1: 创建 `examples/synology-nas/docker/.dockerignore`**

```gitignore
# Build outputs (重建时生成)
build/
build-*/

# VCS / IDE / OS
.git/
.gitignore
.idea/
.vscode/
*.swp
.DS_Store
Thumbs.db

# Python venvs (镜像内用 uv 重建)
.venv/
**/.venv/
**/__pycache__/
*.pyc

# Docs / specs / plans (不进镜像)
docs/
*.md
!examples/synology-nas/docker/README.md
!examples/synology-nas/README.md

# 不打包到镜像的 examples (除 synology-nas/backend)
examples/*/backend/.venv/
examples/*/build/
```

⚠️ 注意最后一行：`.dockerignore` 默认会忽略所有 `*.md`。上面的 `!examples/synology-nas/docker/README.md` 和 `!examples/synology-nas/README.md` 是显式重新包含这两个（虽然它们在 build context 中其实不会被 Dockerfile 引用——但保留规则让用户在 docker build -t 时不会惊讶为啥某些文件没看到）。如果发现 `!` 规则和默认 `*.md` 冲突导致 build 失败，简化：直接删除 `!` 那两行，README 不进 build context 也无所谓（运行时再 COPY 进来即可——但当前 Dockerfile 实际上不 COPY README，所以更可以放心删 `!` 那两行）。

简化版（推荐用这个）：

```gitignore
# Build outputs (重建时生成)
build/
build-*/

# VCS / IDE / OS
.git/
.gitignore
.idea/
.vscode/
*.swp
.DS_Store
Thumbs.db

# Python venvs (镜像内用 uv 重建)
**/.venv/
**/__pycache__/
*.pyc

# Docs / specs / plans (不进镜像)
docs/
*.md

# 不打包到镜像的 examples
examples/*/build/
```

- [ ] **Step 2: 创建 `examples/synology-nas/docker/.env.example`**

```env
# 必填 — DSM 连接信息
SYNOLOGY_HOST=nas.local
SYNOLOGY_USERNAME=your-user
SYNOLOGY_PASSWORD=your-password
SYNOLOGY_PORT=5001
SYNOLOGY_SECURE=true
SYNOLOGY_CERT_VERIFY=false
SYNOLOGY_DSM_VERSION=7

# 必填 — cpp-mcp 插件与 python 后端共享的 bearer token
# (随便设, 但 Sdocker run 和 docker -e 必须保持一致)
BACKEND_TOKEN=replace-with-a-long-random-token

# 可选 — 一般不需要改
# BACKEND_HOST=127.0.0.1
# BACKEND_PORT=9000
# MCP_LISTEN_HOST=0.0.0.0
# MCP_LISTEN_PORT=8888
# SYNOLOGY_BACKEND_TIMEOUT=120
```

- [ ] **Step 3: 提交**

```bash
cd /home/charles/cpp-mcp
git add examples/synology-nas/docker/.dockerignore examples/synology-nas/docker/.env.example
git commit -m "chore(synology-docker): scaffold .dockerignore and .env.example"
```

---

## Task 4: 创建 `entrypoint.sh`

**Goal:** 写容器启动脚本，校验 env、生成后端 .env、起后端、等健康、exec ext-server、信号处理。

**Files:**
- Create: `examples/synology-nas/docker/entrypoint.sh`

**Acceptance Criteria:**
- [ ] 缺任一必填 env → exit 2 + stderr 写明缺哪个
- [ ] 后端没起来（30s 内）/ 中途死掉 → exit 1
- [ ] 后端起来后 `exec` ext-server（PID 不变）
- [ ] `trap` 在 SIGTERM 时 kill 后端子进程
- [ ] `shellcheck` 通过

**Verify:**
```bash
cd /home/charles/cpp-mcp
shellcheck examples/synology-nas/docker/entrypoint.sh && echo "shellcheck OK"
# 缺 env 测试
chmod +x examples/synology-nas/docker/entrypoint.sh
env -i PATH=/usr/bin:/bin BACKEND_TOKEN=t examples/synology-nas/docker/entrypoint.sh 2>&1 | grep -q "missing required env" && echo "OK: 缺 SYNOLOGY_HOST 被抓到" || echo "FAIL"
# 完整流程需要镜像, 留给 Task 8
```

**Steps:**

- [ ] **Step 1: 创建 `examples/synology-nas/docker/entrypoint.sh`**

```sh
#!/bin/sh
# mcp-synology entrypoint
# 1. 校验必填 env
# 2. 写临时 .env 喂 python 后端
# 3. 后台拉起 synology-api-backend, 等 /health
# 4. exec mcp-ext-server (占前台, tini 转发的信号直接到它)

set -eu

# ---------- 1. 校验必填 ----------
for v in SYNOLOGY_HOST SYNOLOGY_USERNAME SYNOLOGY_PASSWORD BACKEND_TOKEN; do
    eval "[ -n \"\${$v:-}\" ]" || {
        echo "entrypoint: missing required env $v" >&2
        exit 2
    }
done

# ---------- 2. 写后端 .env (chmod 600) ----------
BACKEND_PORT="${BACKEND_PORT:-9000}"
cat > /app/backend/.env <<EOF
SYNOLOGY_HOST=${SYNOLOGY_HOST}
SYNOLOGY_PORT=${SYNOLOGY_PORT:-5000}
SYNOLOGY_USERNAME=${SYNOLOGY_USERNAME}
SYNOLOGY_PASSWORD=${SYNOLOGY_PASSWORD}
SYNOLOGY_SECURE=${SYNOLOGY_SECURE:-true}
SYNOLOGY_CERT_VERIFY=${SYNOLOGY_CERT_VERIFY:-false}
SYNOLOGY_DSM_VERSION=${SYNOLOGY_DSM_VERSION:-7}
BACKEND_TOKEN=${BACKEND_TOKEN}
BACKEND_HOST=${BACKEND_HOST:-127.0.0.1}
BACKEND_PORT=${BACKEND_PORT}
EOF
chmod 600 /app/backend/.env

# ---------- 3. 起后端, 等 /health ----------
cd /app/backend
uv run --no-sync synology-api-backend &
BACKEND_PID=$!

cleanup() {
    if kill -0 "$BACKEND_PID" 2>/dev/null; then
        kill "$BACKEND_PID" 2>/dev/null || true
    fi
}
trap 'cleanup; exit 143' TERM INT

HEALTHY=0
for i in $(seq 1 30); do
    if wget -qO- "http://127.0.0.1:${BACKEND_PORT}/health" >/dev/null 2>&1; then
        HEALTHY=1
        break
    fi
    if ! kill -0 "$BACKEND_PID" 2>/dev/null; then
        echo "entrypoint: backend died before becoming healthy" >&2
        exit 1
    fi
    sleep 1
done

if [ "$HEALTHY" -ne 1 ]; then
    echo "entrypoint: backend not healthy after 30s" >&2
    cleanup
    exit 1
fi

# ---------- 4. exec ext-server (占前台) ----------
export SYNOLOGY_BACKEND_URL="http://127.0.0.1:${BACKEND_PORT}"
export SYNOLOGY_BACKEND_TOKEN="${BACKEND_TOKEN}"
export MCP_LISTEN_HOST="${MCP_LISTEN_HOST:-0.0.0.0}"
export MCP_LISTEN_PORT="${MCP_LISTEN_PORT:-8888}"
exec /app/bin/mcp-ext-server
```

- [ ] **Step 2: 加执行权限并 shellcheck**

```bash
cd /home/charles/cpp-mcp
chmod +x examples/synology-nas/docker/entrypoint.sh
command -v shellcheck >/dev/null && shellcheck examples/synology-nas/docker/entrypoint.sh || echo "(skip: shellcheck not installed)"
```

- [ ] **Step 3: 缺 env 行为验证**

```bash
cd /home/charles/cpp-mcp
env -i PATH=/usr/bin:/bin BACKEND_TOKEN=t \
    examples/synology-nas/docker/entrypoint.sh 2>&1 | head -3
# 期望: "entrypoint: missing required env SYNOLOGY_HOST"
```

- [ ] **Step 4: 提交**

```bash
cd /home/charles/cpp-mcp
git add examples/synology-nas/docker/entrypoint.sh
git commit -m "feat(synology-docker): add entrypoint with env validation and signal handling"
```

---

## Task 5: 创建多阶段 `Dockerfile`

**Goal:** 写 Dockerfile，多阶段构建，builder 阶段用 cmake 编 C++，runtime 阶段塞二进制 + Python 后端 + tini + entrypoint + uv 同步好依赖，配置 HEALTHCHECK。

**Files:**
- Create: `examples/synology-nas/docker/Dockerfile`

**Acceptance Criteria:**
- [ ] `docker build -f examples/synology-nas/docker/Dockerfile -t mcp-synology:test .` 在当前 linux/amd64 上成功
- [ ] 镜像大小 < 250 MB
- [ ] `docker inspect mcp-synology:test` 显示 `HEALTHCHECK` 配置
- [ ] `docker history mcp-synology:test --no-trunc` 不含任何明文 SYNOLOGY_PASSWORD 字样

**Verify:**
```bash
cd /home/charles/cpp-mcp
docker build -f examples/synology-nas/docker/Dockerfile -t mcp-synology:test . 2>&1 | tail -10
docker images mcp-synology:test --format "{{.Size}}"
# 期望 < 250 MB (实际 ~180 MB)
docker inspect mcp-synology:test --format '{{json .Config.Healthcheck}}'
# 期望非空 JSON
```

**Steps:**

- [ ] **Step 1: 创建 `examples/synology-nas/docker/Dockerfile`**

```dockerfile
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
```

- [ ] **Step 2: 构建镜像**

```bash
cd /home/charles/cpp-mcp
docker build -f examples/synology-nas/docker/Dockerfile -t mcp-synology:test . 2>&1 | tail -15
```

期望最后两行有 `naming to docker.io/library/mcp-synology:test` 之类的成功提示。

- [ ] **Step 3: 检查大小**

```bash
docker images mcp-synology:test --format "{{.Repository}}:{{.Tag}} {{.Size}}"
```

期望 < 250 MB（实测约 180-200 MB）。

- [ ] **Step 4: 检查 HEALTHCHECK 和 entrypoint**

```bash
docker inspect mcp-synology:test --format 'HEALTHCHECK: {{json .Config.Healthcheck}}'
docker inspect mcp-synology:test --format 'ENTRYPOINT: {{json .Config.Entrypoint}}'
# 期望 HEALTHCHECK 非空, ENTRYPOINT 是 ["/usr/bin/tini", "--", "/entrypoint.sh"]
```

- [ ] **Step 5: 检查镜像里无明文凭据**

```bash
docker history mcp-synology:test --no-trunc 2>&1 | grep -i password && echo "FAIL: 凭据泄漏" || echo "OK: 镜像干净"
```

期望输出 `OK: 镜像干净`。

- [ ] **Step 6: 提交**

```bash
cd /home/charles/cpp-mcp
git add examples/synology-nas/docker/Dockerfile
git commit -m "feat(synology-docker): add multi-stage Dockerfile"
```

---

## Task 6: 创建 `docker/README.md`

**Goal:** 写自包含用法文档，覆盖快速开始、环境变量、Claude Code + Codex 接入、故障排查。

**Files:**
- Create: `examples/synology-nas/docker/README.md`

**Acceptance Criteria:**
- [ ] 文档存在且涵盖：clone + build + run 完整命令、所有 env 变量、Claude Code mcpServers JSON、Codex config.toml、`docker logs` 故障排查
- [ ] 显式说明 build context 必须是 cpp-mcp 项目根
- [ ] 没有真实的凭据值

**Verify:**
```bash
cd /home/charles/cpp-mcp
ls -la examples/synology-nas/docker/README.md
# 简单内容检查
grep -c "mcpServers" examples/synology-nas/docker/README.md  # 期望 >= 1
grep -c "mcp_servers" examples/synology-nas/docker/README.md  # 期望 >= 1
grep -c "BACKEND_TOKEN" examples/synology-nas/docker/README.md  # 期望 >= 1
```

**Steps:**

- [ ] **Step 1: 创建 `examples/synology-nas/docker/README.md`**

````markdown
# mcp-synology Docker

把 `cpp-mcp` 的 mcp-ext-server、libsynology_tools.so 插件、Python 后端打包成单一 OCI 镜像。

跑起来后，从 MCP 客户端连 `http://localhost:8888/mcp` 即可看到 17 个 Synology 工具（FileStation 10 个 + Download Station 7 个）。

## 快速开始

```bash
# 1. clone 源码（必须，因为 Dockerfile 用整个 cpp-mcp 目录作为 build context）
git clone https://github.com/your-org/cpp-mcp.git
cd cpp-mcp

# 2. 编译镜像
docker build -f examples/synology-nas/docker/Dockerfile -t mcp-synology:latest .

# 3. 启动容器
docker run -d --name mcp-synology -p 8888:8888 \
  -e SYNOLOGY_HOST=192.168.1.195 \
  -e SYNOLOGY_PORT=5001 \
  -e SYNOLOGY_USERNAME=czh \
  -e SYNOLOGY_PASSWORD=你的密码 \
  -e SYNOLOGY_SECURE=true \
  -e SYNOLOGY_CERT_VERIFY=false \
  -e BACKEND_TOKEN=$(uuidgen) \
  mcp-synology:latest

# 4. 验证
docker logs mcp-synology
curl -X POST http://localhost:8888/mcp \
  -H "Content-Type: application/json" -H "Accept: application/json, text/event-stream" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"curl","version":"1"}}}'

# 5. 停止
docker stop mcp-synology
```

## 环境变量

### 必填

| 变量 | 说明 |
|---|---|
| `SYNOLOGY_HOST` | DSM 主机 / IP |
| `SYNOLOGY_USERNAME` | DSM 账号 |
| `SYNOLOGY_PASSWORD` | DSM 密码 |
| `BACKEND_TOKEN` | cpp 插件和 python 后端共享的 bearer。随便设，但 `docker run` 时每次给同一个值 |

### 可选

| 变量 | 默认 | 说明 |
|---|---|---|
| `SYNOLOGY_PORT` | `5000` | DSM HTTPS 端口（自签名常见 5001） |
| `SYNOLOGY_SECURE` | `true` | DSM 走 HTTPS |
| `SYNOLOGY_CERT_VERIFY` | `false` | 自签名证书场景关校验 |
| `SYNOLOGY_DSM_VERSION` | `7` | DSM 主版本 |
| `BACKEND_HOST` | `127.0.0.1` | python 后端监听 |
| `BACKEND_PORT` | `9000` | python 后端端口（容器内，**不要**改） |
| `MCP_LISTEN_HOST` | `0.0.0.0` | ext-server 监听 |
| `MCP_LISTEN_PORT` | `8888` | ext-server 暴露端口（`docker -p` 用这个） |
| `SYNOLOGY_BACKEND_TIMEOUT` | `120` | cpp 插件等后端超时秒数 |

## 接入 Claude Code

编辑 `~/.claude/settings.json`（项目级用 `.claude/settings.local.json`）：

```json
{
  "mcpServers": {
    "synology": {
      "url": "http://localhost:8888/mcp"
    }
  }
}
```

重启 Claude Code，应该能看到 16 个 `mcp__synology__*` 工具。

## 接入 Codex CLI

编辑 `~/.codex/config.toml`：

```toml
[mcp_servers.synology]
url = "http://localhost:8888/mcp"
```

## 故障排查

**1. `docker ps` 显示 unhealthy**

```bash
docker logs mcp-synology | tail -50
```

常见原因：环境变量没设对、端口冲突、镜像编出来有问题。

**2. 容器"健康"但 `tools/call` 都失败**

容器健康只代表 MCP initialize 能响应。**健康 ≠ 能用** —— 实际连 DSM 是在第一次 `tools/call` 时才发生。看后端 log：

```bash
docker logs mcp-synology 2>&1 | grep -A 20 "tools/call"
```

常见错误：凭据错、DSM 不可达、自签名证书（设 `SYNOLOGY_CERT_VERIFY=true` 试试）。

**3. `docker stop` 卡住**

正常情况 5-10s 清场。`docker stop -t 30` 给更长时间。`tini` 转发 SIGTERM 给 entrypoint，entrypoint `trap` 杀掉后端子进程。

**4. 端口冲突**

```bash
docker rm -f mcp-synology  # 清理残留
# 改用别的端口
docker run -d --name mcp-synology -p 18888:8888 -e MCP_LISTEN_PORT=8888 ... mcp-synology:latest
# 注意: -p 18888:8888 把 host 18888 映射到容器 8888, 不需要改容器内端口
```

**5. 想换 token**

```bash
docker stop mcp-synology
docker rm mcp-synology
docker run -d --name mcp-synology -p 8888:8888 \
  -e BACKEND_TOKEN=新token \
  ... mcp-synology:latest
```

## 镜像结构

```
/app/bin/mcp-ext-server              # C++ MCP HTTP server
/app/plugins/libsynology_tools.so    # 17 个 Synology 工具插件
/app/backend/                        # Python 后端 (uv project)
/entrypoint.sh                       # 启动脚本
```

后端 9000 端口留容器内不暴露。要调试可以直接进容器：

```bash
docker exec -it mcp-synology /bin/sh
curl http://127.0.0.1:9000/health
curl -H "Authorization: Bearer $BACKEND_TOKEN" http://127.0.0.1:9000/tools
```

## 不在镜像里

- `libcalculator.so`、`libwsl_tools.so` —— 镜像只装 `libsynology_tools.so`
- 任何凭据 / 你的 NAS 密码 / token —— 全部用 `docker -e` 注入
````

- [ ] **Step 2: 验证文档完整性**

```bash
cd /home/charles/cpp-mcp
ls -la examples/synology-nas/docker/README.md
grep -E "^## " examples/synology-nas/docker/README.md
# 期望看到: 快速开始 / 环境变量 / 接入 Claude Code / 接入 Codex CLI / 故障排查 / 镜像结构 / 不在镜像里
```

- [ ] **Step 3: 提交**

```bash
cd /home/charles/cpp-mcp
git add examples/synology-nas/docker/README.md
git commit -m "docs(synology-docker): add usage README with Claude/Codex setup"
```

---

## Task 7: 更新 `examples/synology-nas/README.md` 加 Docker 指针

**Goal:** 在主 README 顶部加一段提示，让想直接跑的人跳到 docker 文档。

**Files:**
- Modify: `examples/synology-nas/README.md:1`

**Acceptance Criteria:**
- [ ] 第一行（在原 `# Synology NAS MCP Example` 标题之前）有一段 callout 块指向 `docker/README.md`
- [ ] 原内容一字不动

**Verify:**
```bash
cd /home/charles/cpp-mcp
head -15 examples/synology-nas/README.md
```

**Steps:**

- [ ] **Step 1: 在文件顶部插入 callout**

打开 `examples/synology-nas/README.md`，在第一行 `# Synology NAS MCP Example` 之前插入：

```markdown
> 想直接跑？参考 [docker/README.md](./docker/README.md) 用单一镜像启动，5 分钟上线。
> 想本地编译？继续读下面。

```

⚠️ 注意 callout 块用 markdown blockquote 语法（`>`）和原 README 的其他 blockquote 区分开。

- [ ] **Step 2: 验证**

```bash
cd /home/charles/cpp-mcp
head -10 examples/synology-nas/README.md
# 期望第一行是 "> 想直接跑? 参考 [docker/README.md]..."
# 原 "# Synology NAS MCP Example" 标题在第 4-5 行
```

- [ ] **Step 3: 提交**

```bash
cd /home/charles/cpp-mcp
git add examples/synology-nas/README.md
git commit -m "docs(synology): add pointer to docker quickstart"
```

---

## Task 8: 构建并端到端验证镜像

**Goal:** 用占位符凭据起一个容器，验证：容器能起来、`/health` 200、`tools/list` 返回正好 16 个工具、缺 env 时正确报错、`docker stop` 干净退出。

**Files:** (无新增，纯验证)

**Acceptance Criteria:**
- [ ] `docker run -e <placeholder> -e ...` 容器能在 30s 内变成 healthy
- [ ] `curl http://localhost:8888/mcp` initialize 返回 200
- [ ] `tools/list` 返回**正好 16 个**工具（确认 wsl/calculator 已剥掉）
- [ ] 缺 `SYNOLOGY_HOST` 时容器 exit 2，log 有 `missing required env SYNOLOGY_HOST`
- [ ] `docker stop` 在 10s 内清场（容器状态从 `running` → `exited`）

**Verify:**
```bash
cd /home/charles/cpp-mcp
# 1. 启动
CID=$(docker run -d --rm \
  -e SYNOLOGY_HOST=nas.invalid \
  -e SYNOLOGY_USERNAME=test \
  -e SYNOLOGY_PASSWORD=test \
  -e BACKEND_TOKEN=test-token-1234 \
  -p 18888:8888 \
  mcp-synology:test)
trap "docker stop $CID >/dev/null 2>&1 || true" EXIT

# 2. 等 healthy
for i in $(seq 1 30); do
  status=$(docker inspect -f '{{.State.Health.Status}}' "$CID" 2>/dev/null || echo "starting")
  [ "$status" = "healthy" ] && { echo "OK healthy after ${i}s"; break; }
  sleep 1
done

# 3. MCP initialize
curl -sf -m 3 -X POST http://127.0.0.1:18888/mcp \
  -H "Content-Type: application/json" -H "Accept: application/json, text/event-stream" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"t","version":"1"}}}' \
  >/dev/null && echo "OK initialize" || echo "FAIL initialize"

# 4. tools/list count == 16
SID=$(curl -sf -m 3 -i -X POST http://127.0.0.1:18888/mcp \
  -H "Content-Type: application/json" -H "Accept: application/json, text/event-stream" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"t","version":"1"}}}' \
  | grep -i '^mcp-session-id:' | awk '{print $2}' | tr -d '\r')
curl -sf -m 3 -X POST http://127.0.0.1:18888/mcp \
  -H "Content-Type: application/json" -H "Accept: application/json, text/event-stream" \
  -H "Mcp-Session-Id: $SID" \
  -d '{"jsonrpc":"2.0","method":"notifications/initialized"}' >/dev/null
COUNT=$(curl -sf -m 3 -X POST http://127.0.0.1:18888/mcp \
  -H "Content-Type: application/json" -H "Accept: application/json, text/event-stream" \
  -H "Mcp-Session-Id: $SID" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
  | python3 -c "import sys,json; print(len(json.load(sys.stdin)['result']['tools']))")
echo "tools count: $COUNT (expect 16)"
[ "$COUNT" = "16" ] && echo "OK tool count" || echo "FAIL tool count"

# 5. 缺 env 错误
CID2=$(docker run --rm -e BACKEND_TOKEN=t mcp-synology:test 2>&1)
echo "$CID2" | grep -q "missing required env SYNOLOGY_HOST" && echo "OK missing-env check" || echo "FAIL missing-env check"

# 6. docker stop 时长
docker stop "$CID" 2>&1
trap - EXIT
```

**Steps:**

- [ ] **Step 1: 用占位符凭据启动容器**

```bash
cd /home/charles/cpp-mcp
CID=$(docker run -d --rm \
  -e SYNOLOGY_HOST=nas.invalid \
  -e SYNOLOGY_USERNAME=test \
  -e SYNOLOGY_PASSWORD=test \
  -e BACKEND_TOKEN=test-token-1234 \
  -p 18888:8888 \
  mcp-synology:test)
echo "CID=$CID"
```

- [ ] **Step 2: 等 healthy**

```bash
for i in $(seq 1 30); do
  status=$(docker inspect -f '{{.State.Health.Status}}' "$CID" 2>/dev/null || echo "starting")
  echo "[$i] status=$status"
  [ "$status" = "healthy" ] && break
  sleep 1
done
```

期望 30s 内 `healthy`。

- [ ] **Step 3: MCP initialize**

```bash
curl -sf -m 3 -X POST http://127.0.0.1:18888/mcp \
  -H "Content-Type: application/json" -H "Accept: application/json, text/event-stream" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"t","version":"1"}}}' \
  | python3 -c "import sys,json; r=json.load(sys.stdin); print('protocolVersion:', r['result']['protocolVersion'])"
```

期望输出 `protocolVersion: 2025-11-25`（或类似非空值）。

- [ ] **Step 4: tools/list 计数**

```bash
SID=$(curl -sf -m 3 -i -X POST http://127.0.0.1:18888/mcp \
  -H "Content-Type: application/json" -H "Accept: application/json, text/event-stream" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"t","version":"1"}}}' \
  | grep -i '^mcp-session-id:' | awk '{print $2}' | tr -d '\r')
curl -sf -m 3 -X POST http://127.0.0.1:18888/mcp \
  -H "Content-Type: application/json" -H "Accept: application/json, text/event-stream" \
  -H "Mcp-Session-Id: $SID" \
  -d '{"jsonrpc":"2.0","method":"notifications/initialized"}' >/dev/null

curl -sf -m 3 -X POST http://127.0.0.1:18888/mcp \
  -H "Content-Type: application/json" -H "Accept: application/json, text/event-stream" \
  -H "Mcp-Session-Id: $SID" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
  | python3 -c "import sys,json; tools=json.load(sys.stdin)['result']['tools']; print('count:', len(tools)); [print(' -', t['name']) for t in tools]"
```

期望 `count: 16` 加 16 个 `list_shares`/`list_directory`/.../ `ds_get_statistics` 工具。

- [ ] **Step 5: 缺 env 行为**

```bash
docker run --rm -e BACKEND_TOKEN=t mcp-synology:test 2>&1 | head -3
# 期望: "entrypoint: missing required env SYNOLOGY_HOST"
```

- [ ] **Step 6: docker stop 时长**

```bash
START=$(date +%s)
docker stop "$CID" >/dev/null
END=$(date +%s)
echo "stop took $((END-START))s"
```

期望 < 10s。

- [ ] **Step 7: 不提交任何代码（验证任务）**

此任务只验证，不改文件。

---

## Task 9: 用户真实凭据验证

**Goal:** 用户用自己的真实 NAS 凭据跑一次 `docker run`，确认 `tools/call list_shares` 能从自己的 NAS 拉回真实的共享文件夹清单。

**Files:** (无)

**User Verification Required:**
Before marking this task complete, you MUST call AskUserQuestion:
```yaml
AskUserQuestion:
  question: "用你真实的 NAS 凭据跑完 `docker run` + `tools/call list_shares` 之后，结果如何？"
  header: "真实凭据验证"
  options:
    - label: "成功, 拿到共享目录"
      description: "list_shares 返回了你的真实共享目录 (docker, download, ...), 整套打包对你来说可用"
    - label: "失败, 需要排查"
      description: "容器起来但 list_shares 报错 / 返回空 / 连接失败, 给出 docker logs 关键片段"
```

```json:metadata
{"files": [], "verifyCommand": "", "acceptanceCriteria": ["user confirms real-NAS tool call works end-to-end"], "requiresUserVerification": true, "userVerificationPrompt": "用真实 NAS 凭据跑通 `tools/call list_shares` 了吗？返回的是不是你的真实共享目录？"}
```

**Acceptance Criteria:**
- [ ] 用户用真实凭据启动容器
- [ ] `tools/call list_shares` 真实返回用户的共享目录
- [ ] 用户的 MCP 客户端（Claude Code 或 Codex）能连 `http://localhost:8888/mcp` 看到 16 个工具

**Steps:**

- [ ] **Step 1: 给用户给出复现命令**

把以下命令交给用户执行（或他参考 `examples/synology-nas/docker/README.md` 自跑）：

```bash
docker run -d --name mcp-synology -p 8888:8888 \
  -e SYNOLOGY_HOST=192.168.1.195 \
  -e SYNOLOGY_PORT=5001 \
  -e SYNOLOGY_USERNAME=czh \
  -e SYNOLOGY_PASSWORD=Unsmooth4-Hate8-Plasma5-Unrevised6-Ebony9 \
  -e SYNOLOGY_SECURE=true \
  -e SYNOLOGY_CERT_VERIFY=false \
  -e BACKEND_TOKEN=any-token \
  mcp-synology:test

# 等几秒, 然后:
SID=$(curl -sf -i -X POST http://localhost:8888/mcp \
  -H "Content-Type: application/json" -H "Accept: application/json, text/event-stream" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"t","version":"1"}}}' \
  | grep -i '^mcp-session-id:' | awk '{print $2}' | tr -d '\r')
curl -sf -m 30 -X POST http://localhost:8888/mcp \
  -H "Content-Type: application/json" -H "Accept: application/json, text/event-stream" \
  -H "Mcp-Session-Id: $SID" \
  -d '{"jsonrpc":"2.0","method":"notifications/initialized"}' >/dev/null
curl -sf -m 30 -X POST http://localhost:8888/mcp \
  -H "Content-Type: application/json" -H "Accept: application/json, text/event-stream" \
  -H "Mcp-Session-Id: $SID" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"list_shares","arguments":{}}}' \
  | python3 -m json.tool
```

期望 `result.content[0].text` 里能看到用户 NAS 的真实共享目录清单（之前验证过：docker / download / home / photo / web / web_packages / 共享空间 / 技术资料）。

- [ ] **Step 2: 调用 AskUserQuestion 等待用户确认结果**

按上面 `User Verification Required` 块里的 `AskUserQuestion` 调用。

- [ ] **Step 3: 用户选择"失败"时的处理**

让用户给出 `docker logs mcp-synology` 的关键 30 行 + `list_shares` 的返回。常见原因：
- `SYNOLOGY_PASSWORD` 错 → python 后端报 `Authentication failed`
- `SYNOLOGY_SECURE` 配错 → 连接 refused
- `SYNOLOGY_CERT_VERIFY=true` + 自签名证书 → SSL error
- DSM 端口错 → connection refused

根据报错改 env 后重跑，无需改代码。

---

## Self-Review

1. **Spec coverage**：
   - Goal: ✓ Task 1-7
   - Non-goals (no compose, no CI, no registry, no warmup, no test script): ✓ 全部未引入
   - Architecture (single image, tini, entrypoint, two procs): ✓ Task 4, 5
   - File layout: ✓ Task 3-7
   - Dockerfile 多阶段: ✓ Task 5
   - Env vars: ✓ Task 4
   - Startup sequence + signal handling: ✓ Task 4
   - C++ source changes: ✓ Task 1, 2
   - Documentation: ✓ Task 6, 7
   - Acceptance criteria: ✓ Task 8, 9
   - Out of scope items: 不在 plan 里

2. **Placeholder scan**：无 TBD / TODO / "类似 Task N" 引用。每个 step 都有具体代码或命令。

3. **Type / signature consistency**：
   - `MCP_LISTEN_HOST` / `MCP_LISTEN_PORT` 在 Task 2 引入，Task 4 / 5 / 6 一致使用
   - `BACKEND_TOKEN` 一致
   - `MCP_LISTEN_PORT` 默认 `8888` 在 Task 2/4/5 都一致
   - `BACKEND_PORT` 默认 `9000` 在 Task 4/5 一致

4. **User verification requirement**：YES（spec 验收 #7 "真实凭据下 list_shares 真实返回 Synology 共享目录"）。Task 9 已带 `requiresUserVerification: true` + 完整 `User Verification Required` 块 + `AskUserQuestion` yaml。

✓ Self-review pass。
