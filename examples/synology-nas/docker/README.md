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
