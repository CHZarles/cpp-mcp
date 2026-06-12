import json

import pytest
from starlette.testclient import TestClient

from synology_api_backend.app import create_app
from synology_api_backend.config import Settings
from synology_api_backend.concurrency import ToolConcurrency
from synology_api_backend.tools import (
    DOWNLOAD_STATION_WRITE_TOOLS,
    FILESTATION_READ_TOOLS,
    FILESTATION_WRITE_TOOLS,
    TOOL_SCHEMAS,
    SynologyToolDispatcher,
)


class RecordingConcurrency(ToolConcurrency):
    def __init__(self):
        super().__init__(max_concurrent_reads=8)
        self.read_entries = 0
        self.write_entries = 0

    def run_read(self, func, *args, **kwargs):
        self.read_entries += 1
        return func(*args, **kwargs)

    def run_write(self, func, *args, **kwargs):
        self.write_entries += 1
        return func(*args, **kwargs)


class FakeFileStation:
    def __init__(self):
        self.calls = []

    def get_list_share(self, **kwargs):
        self.calls.append(("get_list_share", kwargs))
        return {"shares": [{"name": "media"}]}

    def get_file_list(self, **kwargs):
        self.calls.append(("get_file_list", kwargs))
        return {"files": [{"path": "/video/movie.mkv"}]}

    def get_file_info(self, **kwargs):
        self.calls.append(("get_file_info", kwargs))
        return {"files": [{"path": kwargs["path"], "size": 42}]}

    def search_start(self, **kwargs):
        self.calls.append(("search_start", kwargs))
        return {"taskid": "search-1"}

    def get_search_list(self, **kwargs):
        self.calls.append(("get_search_list", kwargs))
        return {"files": [{"path": "/video/found.txt"}]}

    def get_file(self, **kwargs):
        self.calls.append(("get_file", kwargs))
        return b"hello from nas"

    def upload_file(self, **kwargs):
        self.calls.append(("upload_file", kwargs))
        return {"uploaded": True}

    def create_folder(self, **kwargs):
        self.calls.append(("create_folder", kwargs))
        return {"folder": kwargs["name"]}

    def delete_blocking_function(self, **kwargs):
        self.calls.append(("delete_blocking_function", kwargs))
        return {"deleted": True}

    def rename_folder(self, **kwargs):
        self.calls.append(("rename_folder", kwargs))
        return {"renamed": kwargs["name"]}

    def start_copy_move(self, **kwargs):
        self.calls.append(("start_copy_move", kwargs))
        return {"taskid": "move-1"}


class FakeDownloadStation:
    def __init__(self):
        self.calls = []

    def get_info(self):
        self.calls.append(("get_info", {}))
        return {"version": "1"}

    def tasks_list(self, **kwargs):
        self.calls.append(("tasks_list", kwargs))
        return {"tasks": [{"id": "dbid_1"}]}

    def create_task(self, **kwargs):
        self.calls.append(("create_task", kwargs))
        return {"task_created": True}

    def pause_task(self, **kwargs):
        self.calls.append(("pause_task", kwargs))
        return {"paused": kwargs["task_id"]}

    def resume_task(self, **kwargs):
        self.calls.append(("resume_task", kwargs))
        return {"resumed": kwargs["task_id"]}

    def delete_task(self, **kwargs):
        self.calls.append(("delete_task", kwargs))
        return {"deleted": kwargs["task_id"]}

    def get_statistic_info(self):
        self.calls.append(("get_statistic_info", {}))
        return {"speed_download": 0}


class FakeClientManager:
    def __init__(self, file_station=None, download_station=None):
        self.file_station = file_station or FakeFileStation()
        self.download_station = download_station or FakeDownloadStation()

    def get_file_station(self):
        return self.file_station

    def get_download_station(self):
        return self.download_station


def app_client(token="secret", dispatcher=None):
    settings = Settings(
        synology_host="nas.local",
        synology_port=5000,
        synology_username="user",
        synology_password="pass",
        backend_token=token,
    )
    dispatcher = dispatcher or SynologyToolDispatcher(
        FakeClientManager(),
        concurrency=RecordingConcurrency(),
    )
    app = create_app(settings=settings, dispatcher=dispatcher)
    return TestClient(app), dispatcher


def auth_header(token="secret"):
    return {"Authorization": f"Bearer {token}"}


def mcp_json(response):
    assert response.status_code == 200
    body = response.json()
    assert body["isError"] is False
    text_items = [item for item in body["content"] if item["type"] == "text"]
    assert text_items
    return json.loads(text_items[0]["text"])


def test_health_works_without_token():
    client, _ = app_client()

    response = client.get("/health")

    assert response.status_code == 200
    assert response.json()["status"] == "ok"


def test_tools_rejects_missing_or_invalid_token():
    client, _ = app_client()

    missing = client.get("/tools")
    invalid = client.get("/tools", headers=auth_header("bad"))

    assert missing.status_code == 401
    assert invalid.status_code == 401


def test_tools_returns_expected_schema_names():
    client, _ = app_client()

    response = client.get("/tools", headers=auth_header())

    assert response.status_code == 200
    names = {tool["name"] for tool in response.json()["tools"]}
    assert names == {tool["name"] for tool in TOOL_SCHEMAS}
    assert {"list_shares", "create_file", "ds_create_task"} <= names


def test_unknown_tool_returns_mcp_error():
    client, _ = app_client()

    response = client.post(
        "/tools/call",
        headers=auth_header(),
        json={"name": "missing_tool", "arguments": {}},
    )

    assert response.status_code == 200
    assert response.json()["isError"] is True
    assert "Unknown tool" in response.json()["content"][0]["text"]


def test_successful_tool_result_returns_mcp_text_content():
    client, dispatcher = app_client()

    response = client.post(
        "/tools/call",
        headers=auth_header(),
        json={"name": "list_shares", "arguments": {"onlywritable": True}},
    )

    assert mcp_json(response) == {"shares": [{"name": "media"}]}
    assert dispatcher.client_manager.file_station.calls == [
        ("get_list_share", {"onlywritable": True})
    ]


def test_filestation_read_tools_use_read_gate():
    file_station = FakeFileStation()
    concurrency = RecordingConcurrency()
    dispatcher = SynologyToolDispatcher(
        FakeClientManager(file_station=file_station),
        concurrency=concurrency,
    )

    result = dispatcher.call("list_directory", {"folder_path": "/video", "limit": 5})

    assert result["isError"] is False
    assert concurrency.read_entries == 1
    assert concurrency.write_entries == 0
    assert file_station.calls == [
        ("get_file_list", {"folder_path": "/video", "limit": 5})
    ]


def test_filestation_write_tools_use_write_lock():
    file_station = FakeFileStation()
    concurrency = RecordingConcurrency()
    dispatcher = SynologyToolDispatcher(
        FakeClientManager(file_station=file_station),
        concurrency=concurrency,
    )

    result = dispatcher.call("create_directory", {"folder_path": "/video", "name": "new"})

    assert result["isError"] is False
    assert concurrency.read_entries == 0
    assert concurrency.write_entries == 1
    assert file_station.calls == [
        (
            "create_folder",
            {"folder_path": "/video", "name": "new", "force_parent": False},
        )
    ]


@pytest.mark.parametrize(
    ("name", "arguments", "expected_call"),
    [
        (
            "ds_list_tasks",
            {"additional": ["detail"], "offset": 2, "limit": 10},
            ("tasks_list", {"additional_param": ["detail"], "offset": 2, "limit": 10}),
        ),
        (
            "ds_create_task",
            {"url": "https://example.test/file.iso", "destination": "/downloads"},
            (
                "create_task",
                {
                    "url": "https://example.test/file.iso",
                    "destination": "/downloads",
                    "file_path": None,
                },
            ),
        ),
        (
            "ds_pause_tasks",
            {"task_ids": ["dbid_1", "dbid_2"]},
            ("pause_task", {"task_id": "dbid_1,dbid_2"}),
        ),
        (
            "ds_resume_tasks",
            {"task_ids": ["dbid_1"]},
            ("resume_task", {"task_id": "dbid_1"}),
        ),
        (
            "ds_delete_tasks",
            {"task_ids": ["dbid_1"], "force": True},
            ("delete_task", {"task_id": "dbid_1", "force": True}),
        ),
    ],
)
def test_download_station_tool_argument_mapping(name, arguments, expected_call):
    download_station = FakeDownloadStation()
    dispatcher = SynologyToolDispatcher(
        FakeClientManager(download_station=download_station),
        concurrency=RecordingConcurrency(),
    )

    result = dispatcher.call(name, arguments)

    assert result["isError"] is False
    assert download_station.calls == [expected_call]


def test_wrapper_exceptions_convert_to_mcp_error():
    class BrokenFileStation(FakeFileStation):
        def get_list_share(self, **kwargs):
            raise RuntimeError("DSM refused the call")

    dispatcher = SynologyToolDispatcher(
        FakeClientManager(file_station=BrokenFileStation()),
        concurrency=RecordingConcurrency(),
    )

    result = dispatcher.call("list_shares", {})

    assert result["isError"] is True
    assert "DSM refused the call" in result["content"][0]["text"]


def test_tool_sets_match_declared_scope():
    schema_names = {tool["name"] for tool in TOOL_SCHEMAS}

    assert FILESTATION_READ_TOOLS == {
        "list_shares",
        "list_directory",
        "get_file_info",
        "search_files",
        "get_file_content",
    }
    assert FILESTATION_WRITE_TOOLS == {
        "create_file",
        "create_directory",
        "delete",
        "rename_file",
        "move_file",
    }
    assert DOWNLOAD_STATION_WRITE_TOOLS == {
        "ds_create_task",
        "ds_pause_tasks",
        "ds_resume_tasks",
        "ds_delete_tasks",
    }
    assert FILESTATION_READ_TOOLS | FILESTATION_WRITE_TOOLS | {
        "ds_get_info",
        "ds_list_tasks",
        "ds_get_statistics",
    } | DOWNLOAD_STATION_WRITE_TOOLS == schema_names
