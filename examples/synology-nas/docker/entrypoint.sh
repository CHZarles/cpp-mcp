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
