#!/usr/bin/env bash
set -euo pipefail

backend_url="${SYNOLOGY_BACKEND_URL:-http://127.0.0.1:9000}"
token="${BACKEND_TOKEN:-${SYNOLOGY_BACKEND_TOKEN:-}}"

if [[ -z "${token}" ]]; then
    echo "Set BACKEND_TOKEN or SYNOLOGY_BACKEND_TOKEN before running the smoke test." >&2
    exit 2
fi

curl --fail --silent --show-error "${backend_url}/health"
echo
curl --fail --silent --show-error \
    -H "Authorization: Bearer ${token}" \
    "${backend_url}/tools"
echo
