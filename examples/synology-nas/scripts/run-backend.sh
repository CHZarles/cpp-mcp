#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../../.." && pwd)"
backend_dir="${repo_root}/examples/synology-nas/backend"

exec uv run --project "${backend_dir}" synology-api-backend

