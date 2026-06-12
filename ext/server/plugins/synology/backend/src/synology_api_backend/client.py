from __future__ import annotations

import threading
from dataclasses import dataclass
from typing import Protocol

from .config import Settings


class FileStationLike(Protocol):
    pass


class DownloadStationLike(Protocol):
    pass


@dataclass(frozen=True)
class SynologyClients:
    file_station: FileStationLike
    download_station: DownloadStationLike


class SynologyClientManager:
    """Lazy single-NAS N4S4 client manager.

    N4S4/synology-api keeps session state on shared wrapper/session objects. This
    manager intentionally creates one FileStation and one DownloadStation client
    for a single configured NAS and protects initialization with a process-local
    lock.
    """

    def __init__(self, settings: Settings) -> None:
        self.settings = settings
        self._clients: SynologyClients | None = None
        self._lock = threading.Lock()

    def get_file_station(self) -> FileStationLike:
        return self._get_clients().file_station

    def get_download_station(self) -> DownloadStationLike:
        return self._get_clients().download_station

    def warm_up(self) -> None:
        self._get_clients()

    def _get_clients(self) -> SynologyClients:
        if self._clients is None:
            with self._lock:
                if self._clients is None:
                    self._clients = self._create_clients()
        return self._clients

    def _create_clients(self) -> SynologyClients:
        from synology_api.downloadstation import DownloadStation
        from synology_api.filestation import FileStation

        common_args = {
            "ip_address": self.settings.synology_host,
            "port": str(self.settings.synology_port),
            "username": self.settings.synology_username,
            "password": self.settings.synology_password,
            "secure": self.settings.synology_secure,
            "cert_verify": self.settings.synology_cert_verify,
            "dsm_version": self.settings.dsm_version,
            "debug": False,
            "interactive_output": False,
        }
        return SynologyClients(
            file_station=FileStation(**common_args),
            download_station=DownloadStation(**common_args),
        )
