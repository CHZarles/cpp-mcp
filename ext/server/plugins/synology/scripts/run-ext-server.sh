#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../../../../.." && pwd)"

export SYNOLOGY_BACKEND_URL="${SYNOLOGY_BACKEND_URL:-http://127.0.0.1:9000}"
export SYNOLOGY_BACKEND_TIMEOUT="${SYNOLOGY_BACKEND_TIMEOUT:-120}"

if [[ -z "${SYNOLOGY_BACKEND_TOKEN:-}" ]]; then
    echo "SYNOLOGY_BACKEND_TOKEN is required and must match backend BACKEND_TOKEN." >&2
    exit 2
fi

server="${repo_root}/build/ext/server/mcp-ext-server"
if [[ ! -x "${server}" ]]; then
    echo "Missing ${server}. Build with:" >&2
    echo "  cmake -B build -DMCP_BUILD_EXT=ON -DMCP_BUILD_SYNOLOGY_EXAMPLE=ON" >&2
    echo "  cmake --build build --target mcp-ext-server synology_tools" >&2
    exit 2
fi

exec "${server}"
