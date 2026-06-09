from __future__ import annotations

import base64
import io
import json
import mimetypes
import tempfile
from collections.abc import Callable, Mapping
from pathlib import PurePosixPath
from typing import Any

from .concurrency import ToolConcurrency


JSONDict = dict[str, Any]


def _schema(properties: JSONDict, required: list[str] | None = None) -> JSONDict:
    return {
        "type": "object",
        "properties": properties,
        "required": required or [],
        "additionalProperties": False,
    }


def _string(description: str) -> JSONDict:
    return {"type": "string", "description": description}


def _integer(description: str) -> JSONDict:
    return {"type": "integer", "description": description}


def _boolean(description: str) -> JSONDict:
    return {"type": "boolean", "description": description}


def _array(description: str) -> JSONDict:
    return {"type": "array", "items": {"type": "string"}, "description": description}


def _enum(values: list[str], description: str) -> JSONDict:
    return {"type": "string", "enum": values, "description": description}


def _string_or_array(description: str) -> JSONDict:
    return {
        "oneOf": [{"type": "string"}, {"type": "array", "items": {"type": "string"}}],
        "description": description,
    }


FILESTATION_READ_TOOLS = {
    "list_shares",
    "list_directory",
    "get_file_info",
    "search_files",
    "get_file_content",
}
FILESTATION_WRITE_TOOLS = {
    "create_file",
    "create_directory",
    "delete",
    "rename_file",
    "move_file",
}
DOWNLOAD_STATION_WRITE_TOOLS = {
    "ds_create_task",
    "ds_pause_tasks",
    "ds_resume_tasks",
    "ds_delete_tasks",
}
DOWNLOAD_STATION_READ_TOOLS = {
    "ds_get_info",
    "ds_list_tasks",
    "ds_get_statistics",
}
READ_TOOLS = FILESTATION_READ_TOOLS | DOWNLOAD_STATION_READ_TOOLS
WRITE_TOOLS = FILESTATION_WRITE_TOOLS | DOWNLOAD_STATION_WRITE_TOOLS


TOOL_SCHEMAS: list[JSONDict] = [
    {
        "name": "list_shares",
        "description": "List Synology FileStation shared folders.",
        "inputSchema": _schema(
            {
                "additional": _string_or_array("Additional fields to include."),
                "offset": _integer("Pagination offset."),
                "limit": _integer("Maximum number of shares to return."),
                "sort_by": _string("Sort field."),
                "sort_direction": _enum(["asc", "desc"], "Sort direction."),
                "onlywritable": _boolean("Only return writable shares."),
            }
        ),
    },
    {
        "name": "list_directory",
        "description": "List files and folders in a FileStation directory.",
        "inputSchema": _schema(
            {
                "folder_path": _string("Directory path, including shared folder."),
                "offset": _integer("Pagination offset."),
                "limit": _integer("Maximum number of entries to return."),
                "sort_by": _string("Sort field."),
                "sort_direction": _enum(["asc", "desc"], "Sort direction."),
                "pattern": _string("Filename pattern filter."),
                "filetype": _enum(["all", "file", "dir"], "Entry type filter."),
                "goto_path": _string("Path used by DSM goto pagination."),
                "additional": _string_or_array("Additional fields to include."),
            },
            required=["folder_path"],
        ),
    },
    {
        "name": "get_file_info",
        "description": "Get metadata for one or more FileStation paths.",
        "inputSchema": _schema(
            {
                "path": _string_or_array("File or folder path, or a list of paths."),
                "additional": _string_or_array("Additional fields to include."),
            },
            required=["path"],
        ),
    },
    {
        "name": "search_files",
        "description": "Start a FileStation search and optionally fetch the first result page.",
        "inputSchema": _schema(
            {
                "folder_path": _string("Folder path to search."),
                "recursive": _boolean("Search recursively."),
                "pattern": _string("Filename pattern."),
                "extension": _string("File extension filter."),
                "filetype": _enum(["all", "file", "dir"], "Entry type filter."),
                "size_from": _integer("Minimum size in bytes."),
                "size_to": _integer("Maximum size in bytes."),
                "mtime_from": _integer("Minimum modified timestamp."),
                "mtime_to": _integer("Maximum modified timestamp."),
                "crtime_from": _integer("Minimum created timestamp."),
                "crtime_to": _integer("Maximum created timestamp."),
                "atime_from": _integer("Minimum accessed timestamp."),
                "atime_to": _integer("Maximum accessed timestamp."),
                "owner": _string("Owner filter."),
                "group": _string("Group filter."),
                "fetch_results": _boolean("Fetch the first result page after creating the task."),
                "offset": _integer("Result pagination offset."),
                "limit": _integer("Maximum number of result entries."),
                "sort_by": _string("Result sort field."),
                "sort_direction": _enum(["asc", "desc"], "Result sort direction."),
                "additional": _string_or_array("Additional result fields to include."),
            },
            required=["folder_path"],
        ),
    },
    {
        "name": "get_file_content",
        "description": "Read a FileStation file and return text or base64 content.",
        "inputSchema": _schema(
            {
                "path": _string("File path to read."),
                "chunk_size": _integer("Download chunk size."),
                "decode": _boolean("Decode UTF-8 text when possible."),
            },
            required=["path"],
        ),
    },
    {
        "name": "create_file",
        "description": "Create or overwrite a text/base64 file through FileStation upload.",
        "inputSchema": _schema(
            {
                "path": _string("Full destination file path, including shared folder."),
                "content": _string("File content."),
                "encoding": _enum(["utf-8", "base64"], "Input content encoding."),
                "create_parents": _boolean("Create missing parent folders."),
                "overwrite": _boolean("Overwrite an existing file."),
            },
            required=["path", "content"],
        ),
    },
    {
        "name": "create_directory",
        "description": "Create a FileStation directory.",
        "inputSchema": _schema(
            {
                "folder_path": _string("Parent folder path."),
                "name": _string_or_array("Directory name, or names for bulk create."),
                "force_parent": _boolean("Create missing parent folders."),
                "additional": _string_or_array("Additional fields to include."),
            },
            required=["folder_path", "name"],
        ),
    },
    {
        "name": "delete",
        "description": "Delete a FileStation file or folder.",
        "inputSchema": _schema(
            {
                "path": _string_or_array("File or folder path, or paths."),
                "recursive": _boolean("Delete folders recursively."),
                "search_taskid": _string("Delete paths from an existing search task."),
            },
            required=["path"],
        ),
    },
    {
        "name": "rename_file",
        "description": "Rename a FileStation file or folder.",
        "inputSchema": _schema(
            {
                "path": _string_or_array("Current file or folder path."),
                "name": _string_or_array("New name."),
                "additional": _string_or_array("Additional fields to include."),
                "search_taskid": _string("Rename paths from an existing search task."),
            },
            required=["path", "name"],
        ),
    },
    {
        "name": "move_file",
        "description": "Move a FileStation file or folder to another directory.",
        "inputSchema": _schema(
            {
                "path": _string_or_array("Source file/folder path or paths."),
                "dest_folder_path": _string_or_array("Destination folder path or paths."),
                "overwrite": _boolean("Overwrite existing destination entries."),
                "accurate_progress": _boolean("Ask DSM for accurate progress."),
                "search_taskid": _string("Move paths from an existing search task."),
            },
            required=["path", "dest_folder_path"],
        ),
    },
    {
        "name": "ds_get_info",
        "description": "Get Download Station service information.",
        "inputSchema": _schema({}),
    },
    {
        "name": "ds_list_tasks",
        "description": "List Download Station tasks.",
        "inputSchema": _schema(
            {
                "additional": _string_or_array("Additional task fields."),
                "offset": _integer("Pagination offset."),
                "limit": _integer("Maximum number of tasks."),
            }
        ),
    },
    {
        "name": "ds_create_task",
        "description": "Create a Download Station task from a URL or local torrent file path.",
        "inputSchema": _schema(
            {
                "url": _string("Download URL."),
                "file_path": _string("Local torrent/NZB file path visible to the backend."),
                "destination": _string("Download destination on the NAS."),
            }
        ),
    },
    {
        "name": "ds_pause_tasks",
        "description": "Pause one or more Download Station tasks.",
        "inputSchema": _schema({"task_ids": _array("Task IDs to pause.")}, required=["task_ids"]),
    },
    {
        "name": "ds_resume_tasks",
        "description": "Resume one or more Download Station tasks.",
        "inputSchema": _schema({"task_ids": _array("Task IDs to resume.")}, required=["task_ids"]),
    },
    {
        "name": "ds_delete_tasks",
        "description": "Delete one or more Download Station tasks.",
        "inputSchema": _schema(
            {
                "task_ids": _array("Task IDs to delete."),
                "force": _boolean("Force delete completed/incomplete files."),
            },
            required=["task_ids"],
        ),
    },
    {
        "name": "ds_get_statistics",
        "description": "Get Download Station transfer statistics.",
        "inputSchema": _schema({}),
    },
]


class SynologyToolDispatcher:
    def __init__(
        self,
        client_manager: Any,
        concurrency: ToolConcurrency | None = None,
    ) -> None:
        self.client_manager = client_manager
        self.concurrency = concurrency or ToolConcurrency()
        self._handlers: dict[str, Callable[[Mapping[str, Any]], Any]] = {
            "list_shares": self._list_shares,
            "list_directory": self._list_directory,
            "get_file_info": self._get_file_info,
            "search_files": self._search_files,
            "get_file_content": self._get_file_content,
            "create_file": self._create_file,
            "create_directory": self._create_directory,
            "delete": self._delete,
            "rename_file": self._rename_file,
            "move_file": self._move_file,
            "ds_get_info": self._ds_get_info,
            "ds_list_tasks": self._ds_list_tasks,
            "ds_create_task": self._ds_create_task,
            "ds_pause_tasks": self._ds_pause_tasks,
            "ds_resume_tasks": self._ds_resume_tasks,
            "ds_delete_tasks": self._ds_delete_tasks,
            "ds_get_statistics": self._ds_get_statistics,
        }

    def call(self, name: str, arguments: Mapping[str, Any] | None = None) -> JSONDict:
        if name not in self._handlers:
            return mcp_error(f"Unknown tool: {name}")

        arguments = arguments or {}
        if not isinstance(arguments, Mapping):
            return mcp_error("Tool arguments must be an object")

        gate = self.concurrency.run_write if name in WRITE_TOOLS else self.concurrency.run_read
        try:
            result = gate(self._handlers[name], arguments)
        except Exception as exc:  # noqa: BLE001 - convert wrapper failures to MCP-style errors.
            return mcp_error(f"{type(exc).__name__}: {exc}")
        return mcp_success(result)

    def _file_station(self) -> Any:
        return self.client_manager.get_file_station()

    def _download_station(self) -> Any:
        return self.client_manager.get_download_station()

    def _cert_verify(self) -> bool:
        settings = getattr(self.client_manager, "settings", None)
        return bool(getattr(settings, "synology_cert_verify", False))

    def _list_shares(self, arguments: Mapping[str, Any]) -> Any:
        return self._file_station().get_list_share(
            **_pick(arguments, "additional", "offset", "limit", "sort_by", "sort_direction", "onlywritable")
        )

    def _list_directory(self, arguments: Mapping[str, Any]) -> Any:
        return self._file_station().get_file_list(
            **_pick_required(arguments, "folder_path"),
            **_pick(
                arguments,
                "offset",
                "limit",
                "sort_by",
                "sort_direction",
                "pattern",
                "filetype",
                "goto_path",
                "additional",
            ),
        )

    def _get_file_info(self, arguments: Mapping[str, Any]) -> Any:
        kwargs = _pick_required(arguments, "path")
        if "additional" in arguments:
            kwargs["additional_param"] = arguments["additional"]
        return self._file_station().get_file_info(**kwargs)

    def _search_files(self, arguments: Mapping[str, Any]) -> Any:
        fs = self._file_station()
        start_kwargs = _pick_required(arguments, "folder_path")
        start_kwargs.update(
            _pick(
                arguments,
                "recursive",
                "pattern",
                "extension",
                "filetype",
                "size_from",
                "size_to",
                "mtime_from",
                "mtime_to",
                "crtime_from",
                "crtime_to",
                "atime_from",
                "atime_to",
                "owner",
                "group",
            )
        )
        start_result = fs.search_start(**start_kwargs)
        if arguments.get("fetch_results", True) is False:
            return start_result

        task_id = _extract_task_id(start_result)
        if task_id is None:
            return {"task": start_result, "results": None}

        result_kwargs: JSONDict = {"task_id": task_id}
        result_kwargs.update(
            _pick(
                arguments,
                "filetype",
                "limit",
                "sort_by",
                "sort_direction",
                "offset",
                "additional",
            )
        )
        return {"task": start_result, "results": fs.get_search_list(**result_kwargs)}

    def _get_file_content(self, arguments: Mapping[str, Any]) -> JSONDict:
        path = _required(arguments, "path")
        raw = self._file_station().get_file(
            path=path,
            mode="serve",
            verify=self._cert_verify(),
            **_pick(arguments, "chunk_size"),
        )
        content_bytes = _to_bytes(raw)
        decode = arguments.get("decode", True)
        mime_type, _ = mimetypes.guess_type(str(path))
        if decode:
            try:
                return {
                    "path": path,
                    "content": content_bytes.decode("utf-8"),
                    "encoding": "utf-8",
                    "mime_type": mime_type,
                }
            except UnicodeDecodeError:
                pass
        return {
            "path": path,
            "content": base64.b64encode(content_bytes).decode("ascii"),
            "encoding": "base64",
            "mime_type": mime_type,
        }

    def _create_file(self, arguments: Mapping[str, Any]) -> Any:
        path = str(_required(arguments, "path"))
        content = _required(arguments, "content")
        encoding = arguments.get("encoding", "utf-8")
        content_bytes = _decode_content(content, encoding)
        dest_path, filename = _split_remote_file_path(path)

        with tempfile.TemporaryDirectory(prefix="synology-api-backend-") as tmpdir:
            local_path = f"{tmpdir}/{filename}"
            with open(local_path, "wb") as file:
                file.write(content_bytes)
            return self._file_station().upload_file(
                dest_path=dest_path,
                file_path=local_path,
                create_parents=arguments.get("create_parents", True),
                overwrite=arguments.get("overwrite", True),
                verify=self._cert_verify(),
                progress_bar=False,
            )

    def _create_directory(self, arguments: Mapping[str, Any]) -> Any:
        kwargs = _pick_required(arguments, "folder_path", "name")
        kwargs["force_parent"] = arguments.get("force_parent", False)
        if "additional" in arguments:
            kwargs["additional"] = arguments["additional"]
        return self._file_station().create_folder(**kwargs)

    def _delete(self, arguments: Mapping[str, Any]) -> Any:
        kwargs = _pick_required(arguments, "path")
        kwargs["recursive"] = arguments.get("recursive", False)
        if "search_taskid" in arguments:
            kwargs["search_taskid"] = arguments["search_taskid"]
        return self._file_station().delete_blocking_function(**kwargs)

    def _rename_file(self, arguments: Mapping[str, Any]) -> Any:
        kwargs = _pick_required(arguments, "path", "name")
        kwargs.update(_pick(arguments, "additional", "search_taskid"))
        return self._file_station().rename_folder(**kwargs)

    def _move_file(self, arguments: Mapping[str, Any]) -> Any:
        kwargs = _pick_required(arguments, "path", "dest_folder_path")
        kwargs["remove_src"] = True
        kwargs["overwrite"] = arguments.get("overwrite", False)
        kwargs["accurate_progress"] = arguments.get("accurate_progress", True)
        if "search_taskid" in arguments:
            kwargs["search_taskid"] = arguments["search_taskid"]
        return self._file_station().start_copy_move(**kwargs)

    def _ds_get_info(self, arguments: Mapping[str, Any]) -> Any:
        _ensure_no_unexpected(arguments)
        return self._download_station().get_info()

    def _ds_list_tasks(self, arguments: Mapping[str, Any]) -> Any:
        kwargs: JSONDict = {}
        if "additional" in arguments:
            kwargs["additional_param"] = arguments["additional"]
        kwargs.update(_pick(arguments, "offset", "limit"))
        return self._download_station().tasks_list(**kwargs)

    def _ds_create_task(self, arguments: Mapping[str, Any]) -> Any:
        return self._download_station().create_task(
            url=arguments.get("url"),
            file_path=arguments.get("file_path"),
            destination=arguments.get("destination", ""),
        )

    def _ds_pause_tasks(self, arguments: Mapping[str, Any]) -> Any:
        return self._download_station().pause_task(task_id=_task_ids(arguments))

    def _ds_resume_tasks(self, arguments: Mapping[str, Any]) -> Any:
        return self._download_station().resume_task(task_id=_task_ids(arguments))

    def _ds_delete_tasks(self, arguments: Mapping[str, Any]) -> Any:
        return self._download_station().delete_task(
            task_id=_task_ids(arguments),
            force=arguments.get("force", False),
        )

    def _ds_get_statistics(self, arguments: Mapping[str, Any]) -> Any:
        _ensure_no_unexpected(arguments)
        return self._download_station().get_statistic_info()


def mcp_success(data: Any) -> JSONDict:
    return {
        "content": [
            {
                "type": "text",
                "text": json.dumps(data, ensure_ascii=False, default=_json_default),
            }
        ],
        "isError": False,
    }


def mcp_error(message: str) -> JSONDict:
    return {"content": [{"type": "text", "text": message}], "isError": True}


def _json_default(value: Any) -> Any:
    if isinstance(value, bytes):
        return base64.b64encode(value).decode("ascii")
    if isinstance(value, io.BytesIO):
        return base64.b64encode(value.getvalue()).decode("ascii")
    return str(value)


def _pick(arguments: Mapping[str, Any], *names: str) -> JSONDict:
    return {name: arguments[name] for name in names if name in arguments}


def _pick_required(arguments: Mapping[str, Any], *names: str) -> JSONDict:
    return {name: _required(arguments, name) for name in names}


def _required(arguments: Mapping[str, Any], name: str) -> Any:
    if name not in arguments:
        raise ValueError(f"Missing required argument: {name}")
    return arguments[name]


def _ensure_no_unexpected(arguments: Mapping[str, Any]) -> None:
    if arguments:
        names = ", ".join(sorted(arguments))
        raise ValueError(f"Unexpected arguments: {names}")


def _extract_task_id(result: Any) -> str | None:
    if isinstance(result, Mapping):
        for key in ("taskid", "task_id"):
            value = result.get(key)
            if isinstance(value, str):
                return value
        data = result.get("data")
        if isinstance(data, Mapping):
            for key in ("taskid", "task_id"):
                value = data.get(key)
                if isinstance(value, str):
                    return value
    return None


def _to_bytes(value: Any) -> bytes:
    if isinstance(value, bytes):
        return value
    if isinstance(value, bytearray):
        return bytes(value)
    if isinstance(value, io.BytesIO):
        return value.getvalue()
    if isinstance(value, str):
        return value.encode("utf-8")
    if value is None:
        return b""
    return str(value).encode("utf-8")


def _decode_content(content: Any, encoding: str) -> bytes:
    if not isinstance(content, str):
        raise ValueError("content must be a string")
    if encoding == "utf-8":
        return content.encode("utf-8")
    if encoding == "base64":
        return base64.b64decode(content, validate=True)
    raise ValueError("encoding must be 'utf-8' or 'base64'")


def _split_remote_file_path(path: str) -> tuple[str, str]:
    remote_path = PurePosixPath(path)
    filename = remote_path.name
    if not filename or filename in {".", ".."}:
        raise ValueError("path must include a file name")
    parent = str(remote_path.parent)
    if parent == ".":
        raise ValueError("path must include a parent folder")
    return parent, filename


def _task_ids(arguments: Mapping[str, Any]) -> str:
    task_ids = _required(arguments, "task_ids")
    if isinstance(task_ids, str):
        return task_ids
    if isinstance(task_ids, list) and all(isinstance(item, str) for item in task_ids):
        if not task_ids:
            raise ValueError("task_ids must not be empty")
        return ",".join(task_ids)
    raise ValueError("task_ids must be a string or list of strings")
