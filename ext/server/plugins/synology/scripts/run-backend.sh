#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
plugin_dir="$(cd "${script_dir}/.." && pwd)"
backend_dir="${plugin_dir}/backend"

exec uv run --project "${backend_dir}" synology-api-backend
