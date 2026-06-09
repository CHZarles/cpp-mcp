from __future__ import annotations

import hmac
from typing import Any

from starlette.applications import Starlette
from starlette.concurrency import run_in_threadpool
from starlette.requests import Request
from starlette.responses import JSONResponse, Response
from starlette.routing import Route

from .client import SynologyClientManager
from .concurrency import ToolConcurrency
from .config import Settings
from .tools import TOOL_SCHEMAS, SynologyToolDispatcher, mcp_error


def create_app(
    settings: Settings | None = None,
    dispatcher: SynologyToolDispatcher | None = None,
) -> Starlette:
    settings = settings or Settings.from_env()
    if dispatcher is None:
        client_manager = SynologyClientManager(settings)
        dispatcher = SynologyToolDispatcher(
            client_manager,
            concurrency=ToolConcurrency(settings.max_concurrent_reads),
        )

    async def health(request: Request) -> JSONResponse:
        return JSONResponse({"status": "ok", "service": "synology-api-backend"})

    async def tools(request: Request) -> JSONResponse:
        auth_response = _require_bearer(request, settings.backend_token)
        if auth_response is not None:
            return auth_response
        return JSONResponse({"tools": TOOL_SCHEMAS})

    async def call_tool(request: Request) -> JSONResponse:
        auth_response = _require_bearer(request, settings.backend_token)
        if auth_response is not None:
            return auth_response

        try:
            body = await request.json()
        except Exception:  # noqa: BLE001 - invalid JSON should be a client error.
            return JSONResponse({"error": "Invalid JSON body"}, status_code=400)

        validation_error = _validate_tool_call_body(body)
        if validation_error is not None:
            return JSONResponse({"error": validation_error}, status_code=400)

        result = await run_in_threadpool(dispatcher.call, body["name"], body.get("arguments", {}))
        return JSONResponse(result)

    app = Starlette(
        debug=False,
        routes=[
            Route("/health", health, methods=["GET"]),
            Route("/tools", tools, methods=["GET"]),
            Route("/tools/call", call_tool, methods=["POST"]),
        ],
    )
    app.state.settings = settings
    app.state.dispatcher = dispatcher
    return app


def _require_bearer(request: Request, expected_token: str) -> Response | None:
    auth = request.headers.get("authorization", "")
    prefix = "Bearer "
    if not auth.startswith(prefix):
        return _unauthorized()

    token = auth[len(prefix) :]
    if not hmac.compare_digest(token, expected_token):
        return _unauthorized()
    return None


def _unauthorized() -> JSONResponse:
    return JSONResponse(
        mcp_error("Unauthorized"),
        status_code=401,
        headers={"WWW-Authenticate": "Bearer"},
    )


def _validate_tool_call_body(body: Any) -> str | None:
    if not isinstance(body, dict):
        return "Request body must be an object"
    if not isinstance(body.get("name"), str) or not body["name"]:
        return "Field 'name' must be a non-empty string"
    if "arguments" in body and not isinstance(body["arguments"], dict):
        return "Field 'arguments' must be an object"
    return None
