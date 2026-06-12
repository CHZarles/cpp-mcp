# mcp-synology Docker

把本地预编译好的 `mcp-ext-server`、`libsynology_tools.so` 插件和 Synology Python 后端打进一个运行时 OCI 镜像。

这个镜像不是源码自构建镜像：Dockerfile 不安装 `build-essential`、`cmake`、`git`，也不在镜像里编译 C++。C++ 二进制先在宿主机编好，再从 `build/` 拷进镜像。

跑起来后，从 MCP 客户端连 `http://localhost:8888/mcp` 即可看到 17 个 Synology 工具（FileStation 10 个 + Download Station 7 个）。

## 快速开始

```bash
# 1. clone 源码（必须，因为 Dockerfile 用整个 cpp-mcp 目录作为 build context）
git clone https://github.com/your-org/cpp-mcp.git
cd cpp-mcp

# 2. 本地编译 C++ 二进制
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DMCP_BUILD_EXT=ON \
  -DMCP_BUILD_SYNOLOGY_EXAMPLE=ON \
  -DMCP_BUILD_CALCULATOR_PLUGIN=OFF
cmake --build build --target mcp-ext-server synology_tools -j"$(nproc)"

# 3. 构建运行时镜像
docker build -f ext/server/plugins/synology/docker/Dockerfile -t mcp-synology:latest .

# 4. 启动容器
docker run -d --name mcp-synology -p 8888:8888 \
  -e SYNOLOGY_HOST=192.168.1.195 \
  -e SYNOLOGY_PORT=5001 \
  -e SYNOLOGY_USERNAME=czh \
  -e SYNOLOGY_PASSWORD=你的密码 \
  -e SYNOLOGY_SECURE=true \
  -e SYNOLOGY_CERT_VERIFY=false \
  mcp-synology:latest

# 5. 验证
docker logs mcp-synology
curl -X POST http://localhost:8888/mcp \
  -H "Content-Type: application/json" -H "Accept: application/json, text/event-stream" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"curl","version":"1"}}}'

# 6. 停止
docker stop mcp-synology
```

Dockerfile 期望这两个文件已经存在：

```text
build/ext/server/mcp-ext-server
build/plugins/libsynology_tools.so
```

如果宿主机是 Ubuntu 24.04 这类较新的 glibc 环境，本地编译产物可能需要 glibc 2.39 以上。当前镜像默认使用 `python:3.12-slim-trixie`，避免把 Ubuntu 24.04 上编出的二进制放进 bookworm 运行时报 `GLIBC_2.39 not found`。

如果 Docker Hub 拉取慢，可以改基础镜像源：

```bash
docker build \
  --build-arg PYTHON_RUNTIME_IMAGE=m.daocloud.io/docker.io/library/python:3.12-slim \
  -f ext/server/plugins/synology/docker/Dockerfile \
  -t mcp-synology:latest \
  .
```

## 环境变量

### 必填

| 变量 | 说明 |
|---|---|
| `SYNOLOGY_HOST` | DSM 主机 / IP |
| `SYNOLOGY_USERNAME` | DSM 账号 |
| `SYNOLOGY_PASSWORD` | DSM 密码 |

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
| `BACKEND_TOKEN` | 自动生成 | cpp 插件和 python 后端之间的容器内部 bearer。正常不要配置；只有调试内部后端鉴权时才需要覆盖 |

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

重启 Claude Code，应该能看到 17 个 `mcp__synology__*` 工具。

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

**5. 想固定内部 token 调试**

正常不需要配置 `BACKEND_TOKEN`，entrypoint 每次启动都会自动生成，并同时写给 Python 后端和 C++ 插件。只有你要直接调容器内的 Python 后端鉴权接口时，才需要手动固定它：

```bash
docker stop mcp-synology
docker rm mcp-synology
docker run -d --name mcp-synology -p 8888:8888 \
  -e BACKEND_TOKEN=调试用token \
  ... mcp-synology:latest
```

## 镜像结构

```
/app/bin/mcp-ext-server              # C++ MCP HTTP server
/app/plugins/libsynology_tools.so    # 17 个 Synology 工具插件
/app/backend/                        # Python 后端和 .venv 依赖
/entrypoint.sh                       # 启动脚本
```

后端 9000 端口留容器内不暴露。要调试可以直接进容器：

```bash
docker exec -it mcp-synology /bin/sh
curl http://127.0.0.1:9000/health
set -a
. /app/backend/.env
set +a
wget -qO- --header="Authorization: Bearer $BACKEND_TOKEN" http://127.0.0.1:9000/tools
```

## 不在镜像里

- `libcalculator.so` —— 镜像只装 `libsynology_tools.so`
- 你的 NAS 密码 —— 用 `docker -e` 或 `--env-file` 注入
- Python 后端内部 token —— 默认由 entrypoint 在容器启动时生成
- C++ 编译工具链 / CMake / Git / cpp-mcp 源码树
- `uv` 运行时 —— Python 依赖在镜像构建阶段同步进 `.venv`，启动时直接运行后端入口
