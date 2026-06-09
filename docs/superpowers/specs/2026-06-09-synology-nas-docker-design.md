# Synology NAS MCP Docker 化设计

## Goal

把 `examples/synology-nas/` 整条链路（mcp-ext-server + libsynology_tools.so + Python 后端 + 配置）打包成**单一 OCI 镜像**，让最终用户用一条 `docker run` 就能起起来，之后把 MCP 客户端指到 `http://localhost:8888/mcp` 即可看到 16 个 Synology 工具。

目标读者：会用 docker 跑容器、但不想碰 cmake / uv / C++ 编译的开发者或运维。

## Non-goals

- **不**改 Synology 插件的功能或对外协议
- **不**改 python 后端的代码或接口
- **不**做 CI / registry 推送 / 多架构（arm64 等）镜像
- **不**做 DSM warmup —— 容器"健康"不等于"能用"
- **不**打包 wsl_tools / calculator 插件（用户已确认只装 synology 插件）
- **不**提供 docker-compose 多服务编排（用户已确认走单一镜像路线）
- **不**做凭据持久化（用户已确认只接受 `docker -e` 环境变量注入）
- **不**写 docker-test.sh（用户已确认不写测试脚本）

## Architecture

```
┌────────────────────────────────────────────┐
│  mcp-synology:latest (~180 MB)             │
│                                            │
│  tini (PID 1)                              │
│   └─ /entrypoint.sh                        │
│       ├─ uv run synology-api-backend :9000 │  (Python 后端, 内部端口)
│       └─ exec mcp-ext-server        :8888  │  (MCP HTTP, 唯一对外端口)
│                                            │
│  包含:                                     │
│    /app/bin/mcp-ext-server        (~1.1 MB)│
│    /app/plugins/libsynology_tools.so       │
│    /app/backend/  (uv project, 含 .env)    │
│    /entrypoint.sh                          │
└────────────────────────────────────────────┘
```

关键设计点：

- **单一镜像**：用户视角是一个 `docker run`、一个暴露端口（8888）、一组 `-e` 凭据
- **后端端口 9000 留容器内**：不导出到 host（防止绕过 ext-server 直连后端）
- **tini 做 PID 1**：转发 SIGTERM，确保 `docker stop` 时两个进程都能干净退出
- **HEALTHCHECK 走 MCP initialize**：Docker 编排能感知"起来了"；不依赖 DSM

## File Layout

新增文件全部位于 `examples/synology-nas/docker/`：

```
examples/synology-nas/docker/
├── Dockerfile              # 多阶段：builder → runtime
├── entrypoint.sh           # 校验 env、起后端、exec ext-server
├── .dockerignore           # 排除 build/、.venv/、.git/、specs/ 等
├── .env.example            # 列出全部环境变量（无真实凭据）
└── README.md               # 镜像说明、docker run 示例、Claude/Codex 接入
```

要修改的最小集合：

| 文件 | 改动 |
|---|---|
| `ext/server/CMakeLists.txt` | 加 `MCP_BUILD_WSL_PLUGIN`（默认 ON）和 `MCP_BUILD_CALCULATOR_PLUGIN`（默认 ON）两个 option；OFF 时跳过 `add_library(calculator ...)` 和 `add_library(wsl_tools ...)`。这样 docker 构建传 OFF 时只编出 synology_tools 这一个 .so |
| `ext/server/src/main.cpp`（或 `plugin_loader.cpp`）| 新增 `MCP_LISTEN_HOST` / `MCP_LISTEN_PORT` 环境变量支持（默认 `127.0.0.1` / `8888`），Dockerfile 里 override 成 `0.0.0.0` / `8888`。约 5 行改动 |
| `examples/synology-nas/README.md` | 顶部加一段提示：想直接用，参考 `docker/README.md`；想本地编译，继续读下面 |

不动的文件：

- `examples/synology-nas/scripts/*` 保留 —— 留给原生编译用户
- `examples/synology-nas/backend/src/*` 不动 —— Python 后端原样进镜像
- `examples/synology-nas/backend/tests/*` 不动 —— 留给原生开发，不进 docker 流程

## Dockerfile（多阶段）

### Stage 1: builder

```dockerfile
FROM debian:bookworm-slim AS builder
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src

RUN cmake -B build -DMCP_BUILD_EXT=ON -DMCP_BUILD_SYNOLOGY_EXAMPLE=ON \
              -DMCP_BUILD_WSL_PLUGIN=OFF -DMCP_BUILD_CALCULATOR_PLUGIN=OFF
RUN cmake --build build --target mcp-ext-server synology_tools -j"$(nproc)"
```

只编 `mcp-ext-server` 和 `synology_tools` 两个 target；`WSL/calculator` 的 .so 不生成。

### Stage 2: runtime

```dockerfile
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        tini ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

# uv for Python backend (sync deps in image build)
COPY --from=ghcr.io/astral-sh/uv:latest /uv /usr/local/bin/uv

# C++ artifacts
COPY --from=builder /src/build/ext/server/mcp-ext-server /app/bin/
COPY --from=builder /src/build/plugins/libsynology_tools.so /app/plugins/

# Python backend
COPY examples/synology-nas/backend /app/backend

# entrypoint
COPY examples/synology-nas/docker/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

WORKDIR /app

# Pre-install python deps + project into image (avoid first-run latency)
RUN cd /app/backend && uv sync --frozen

# Defaults — Dockerfile provides fallbacks, runtime -e wins
ENV SYNOLOGY_BACKEND_URL=http://127.0.0.1:9000 \
    BACKEND_PORT=9000 \
    MCP_LISTEN_HOST=0.0.0.0 \
    MCP_LISTEN_PORT=8888 \
    PYTHONUNBUFFERED=1

EXPOSE 8888
HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 \
    CMD curl -fsS -X POST http://127.0.0.1:8888/mcp \
        -H "Content-Type: application/json" -H "Accept: application/json, text/event-stream" \
        -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"healthcheck","version":"1"}}}' \
        || exit 1

ENTRYPOINT ["/usr/bin/tini", "--", "/entrypoint.sh"]
```

## Configuration Surface

### 必填环境变量

| 变量 | 说明 |
|---|---|
| `SYNOLOGY_HOST` | DSM 主机 / IP |
| `SYNOLOGY_USERNAME` | DSM 账号 |
| `SYNOLOGY_PASSWORD` | DSM 密码 |
| `BACKEND_TOKEN` | cpp 插件与 python 后端共享的 bearer（用户随便设） |

### 可选环境变量

| 变量 | 默认 | 说明 |
|---|---|---|
| `SYNOLOGY_PORT` | `5000` | DSM HTTPS 端口（自签名场景常 5001） |
| `SYNOLOGY_SECURE` | `true` | DSM 走 HTTPS |
| `SYNOLOGY_CERT_VERIFY` | `false` | 自签名证书场景关掉校验 |
| `SYNOLOGY_DSM_VERSION` | `7` | DSM 主版本 |
| `BACKEND_HOST` | `127.0.0.1` | python 后端监听 |
| `BACKEND_PORT` | `9000` | python 后端端口 |
| `MCP_LISTEN_HOST` | `0.0.0.0` | ext-server 监听（容器内要 0.0.0.0 才能 `-p`） |
| `MCP_LISTEN_PORT` | `8888` | ext-server 暴露端口 |
| `SYNOLOGY_BACKEND_TIMEOUT` | `120` | cpp 插件等后端超时秒数 |

### entrypoint.sh

```sh
#!/bin/sh
set -eu

# 1. 校验必填
for v in SYNOLOGY_HOST SYNOLOGY_USERNAME SYNOLOGY_PASSWORD BACKEND_TOKEN; do
    eval "[ -n \"\${$v:-}\" ]" || { echo "entrypoint: missing required env $v" >&2; exit 2; }
done

# 2. 写临时 .env 喂 python 后端（chmod 600 防 ps 偷看）
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
BACKEND_PORT=${BACKEND_PORT:-9000}
EOF
chmod 600 /app/backend/.env

# 3. 拉起后端 (项目已在 build 时 uv sync 进镜像, 这里 --no-sync 跳过检查)
cd /app/backend
uv run --no-sync synology-api-backend &
BACKEND_PID=$!
trap "kill $BACKEND_PID 2>/dev/null || true" TERM INT

# 4. 等 /health
for i in $(seq 1 30); do
    if wget -qO- "http://127.0.0.1:${BACKEND_PORT:-9000}/health" >/dev/null 2>&1; then
        break
    fi
    if ! kill -0 "$BACKEND_PID" 2>/dev/null; then
        echo "entrypoint: backend died before becoming healthy" >&2
        exit 1
    fi
    sleep 1
done

# 最后再检查一次, 避免刚好 30 次循环都没起来但 break 出来
if ! wget -qO- "http://127.0.0.1:${BACKEND_PORT:-9000}/health" >/dev/null 2>&1; then
    echo "entrypoint: backend not healthy after 30s" >&2
    kill "$BACKEND_PID" 2>/dev/null || true
    exit 1
fi

# 5. exec ext-server
export SYNOLOGY_BACKEND_URL="http://127.0.0.1:${BACKEND_PORT:-9000}"
export SYNOLOGY_BACKEND_TOKEN="${BACKEND_TOKEN}"
export MCP_LISTEN_HOST="${MCP_LISTEN_HOST:-0.0.0.0}"
export MCP_LISTEN_PORT="${MCP_LISTEN_PORT:-8888}"
exec /app/bin/mcp-ext-server
```

## Startup Sequence & Error Handling

```
1. entrypoint 校验 4 个必填 env
   └─ 缺一: stderr "missing required env X", exit 2
             → 容器退出, docker logs 能看到

2. 写 /app/backend/.env (chmod 600)
   └─ 写文件失败: exit 1

3. uv run synology-api-backend &  (后台)
   └─ uv 自身启动失败: 进程立即退
   └─ 进入循环, kill -0 检测到后端已死 → stderr, exit 1

4. 等 /health 最多 30s (1s 一次)
   └─ 30s 仍未起: kill 后端, exit 1
   └─ 中途后端挂: kill -0 检测到, exit 1
   └─ 起来: 跳出循环

5. exec mcp-ext-server
   └─ 绑定 8888 失败: ext-server 自己退, 容器退
   └─ 正常: 容器留在前台
```

**信号路径**：
- `docker stop` → tini → entrypoint.sh 收到 SIGTERM
- `trap` 触发 `kill $BACKEND_PID`
- ext-server 因为 `exec` 也直接收到 SIGTERM
- 期望 5-10s 内清场

**DSM 凭据错误场景**（故意保持简单）：
- 容器正常起来（`/health` 不需要 DSM）
- 第一次 `tools/call` 时 python 后端抛 Synology 异常
- 异常从 mcp-ext-server 透传给客户端
- 用户从 `docker logs` 看到完整 traceback
- **不**做 DSM warmup —— NAS 临时不可达不应让容器起不来

**HEALTHCHECK 语义**：
- 30s 间隔、3s 超时、启动 10s 宽限期
- 测的是 MCP initialize 200 OK，**不**测 DSM
- 容器"健康"≠ "能用"，文档里说清

## C++ Source Changes (Minimal)

### `ext/server/CMakeLists.txt`

加两个 option，默认 ON（保持现有行为不变）：

```cmake
option(MCP_BUILD_CALCULATOR_PLUGIN "Build the calculator example plugin" ON)
option(MCP_BUILD_WSL_PLUGIN "Build the WSL tools plugin" ON)

if(MCP_BUILD_CALCULATOR_PLUGIN)
    add_library(calculator SHARED plugins/calculator.cpp)
    target_include_directories(calculator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/plugins)
    target_link_libraries(calculator PRIVATE nlohmann_json::nlohmann_json)
    list(APPEND EXT_PLUGIN_TARGETS calculator)
endif()

# WSL 同理, 包在 if(MCP_BUILD_WSL_PLUGIN) ... endif() 里
```

### `ext/server/src/main.cpp`（或 plugin_loader.cpp 中处理监听地址的位置）

读环境变量覆盖默认值：

```cpp
const char* host_c = std::getenv("MCP_LISTEN_HOST");
const char* port_c = std::getenv("MCP_LISTEN_PORT");
std::string host = host_c ? host_c : "127.0.0.1";
int port = port_c ? std::atoi(port_c) : 8888;
```

约 5 行改动，不动现有 API。

## Documentation

### `examples/synology-nas/docker/README.md`

自包含文档，覆盖：

1. **快速开始** —— `git clone` → `cd cpp-mcp` → `docker build -f examples/synology-nas/docker/Dockerfile -t mcp-synology:latest .` → `docker run` 一条命令。强调 build context 必须是 cpp-mcp 项目根（因为 Dockerfile 里用 `COPY . /src` 和 `COPY examples/synology-nas/backend ...`）
2. **环境变量表格** —— 必填 4 个 + 可选若干
3. **接入 Claude Code** —— `~/.claude/settings.json` 片段
4. **接入 Codex CLI** —— `~/.codex/config.toml` 片段
5. **故障排查** —— `docker logs` 指南、健康 vs 可用 的区别

### Claude Code 接入

`~/.claude/settings.json`（项目级用 `.claude/settings.local.json`）：

```json
{
  "mcpServers": {
    "synology": {
      "url": "http://localhost:8888/mcp"
    }
  }
}
```

### Codex CLI 接入

`~/.codex/config.toml`：

```toml
[mcp_servers.synology]
url = "http://localhost:8888/mcp"
```

### `examples/synology-nas/README.md` 改动

顶部加一段提示：

```markdown
> 想直接用？参考 [docker/README.md](./docker/README.md) 用单一镜像启动。
> 想本地编译？继续读下面。
```

不重写其他内容。

## Acceptance Criteria

- [ ] `docker build -f examples/synology-nas/docker/Dockerfile -t mcp-synology:test .` 在 linux/amd64 上成功
- [ ] 镜像大小 < 250 MB
- [ ] `docker run -e SYNOLOGY_HOST=x -e SYNOLOGY_USERNAME=y -e SYNOLOGY_PASSWORD=z -e BACKEND_TOKEN=t -p 8888:8888 mcp-synology:test` 启动后 `docker ps` 显示 healthy（30s 内）
- [ ] 缺任一必填 env → 容器 exit code 非 0，`docker logs` 提示缺哪个
- [ ] `curl http://localhost:8888/mcp` initialize 200 OK
- [ ] `tools/list` 返回**正好 16 个工具**（确认 wsl/calculator 已被剥掉）
- [ ] 真实凭据下 `tools/call list_shares` 真实返回 Synology 共享目录
- [ ] `docker stop` 在 10s 内清场（两进程都收到 SIGTERM 并退出）
- [ ] 镜像里不含任何明文凭据（`docker history` / `docker inspect` 看不到密码）

## Out of Scope (Explicit)

- **不**做 multi-arch 镜像（arm64 等）
- **不**做镜像 push 到 registry（ghcr.io / dockerhub）
- **不**做 GitHub Action 自动 build & test
- **不**做 docker-compose 多服务编排
- **不**做凭据 secret 文件挂载 / Vault 集成
- **不**做 DSM warmup probe
- **不**做容器内日志收集（fluentd / vector 等）
- **不**改 Synology 插件 / Python 后端的对外接口
