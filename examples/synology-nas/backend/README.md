# Synology API Backend

Standalone HTTP backend for Synology DSM tool calls through
`N4S4/synology-api==0.9.0`.

This service is intentionally not an MCP transport. It exposes metadata and
tool-call HTTP endpoints for a cpp-mcp gateway or `.so` adapter.

## Endpoints

- `GET /health`: unauthenticated service health check.
- `GET /tools`: bearer-token protected MCP-compatible tool schema list.
- `POST /tools/call`: bearer-token protected tool call endpoint.

Tool calls accept this shape:

```json
{
  "name": "list_directory",
  "arguments": {
    "folder_path": "/homes"
  }
}
```

Responses use MCP-style text content:

```json
{
  "content": [
    {
      "type": "text",
      "text": "{\"files\": []}"
    }
  ],
  "isError": false
}
```

## Configuration

Copy `.env.example` to `.env` and set:

- `SYNOLOGY_HOST`
- `SYNOLOGY_PORT`
- `SYNOLOGY_USERNAME`
- `SYNOLOGY_PASSWORD`
- `SYNOLOGY_SECURE`
- `SYNOLOGY_CERT_VERIFY`
- `BACKEND_TOKEN`
- `BACKEND_HOST` default: `127.0.0.1`
- `BACKEND_PORT` default: `9000`

## Run

```bash
uv run synology-api-backend
```

Then call:

```bash
curl http://127.0.0.1:9000/health
curl -H "Authorization: Bearer $BACKEND_TOKEN" http://127.0.0.1:9000/tools
```

## Tests

```bash
uv run --extra dev pytest
```

The unit tests use fake FileStation and Download Station clients. They do not
connect to a real NAS.
