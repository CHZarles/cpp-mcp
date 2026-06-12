from __future__ import annotations

import threading
from collections.abc import Callable
from typing import TypeVar


T = TypeVar("T")


class ToolConcurrency:
    def __init__(self, max_concurrent_reads: int = 8) -> None:
        if max_concurrent_reads < 1:
            raise ValueError("max_concurrent_reads must be at least 1")
        self._read_semaphore = threading.BoundedSemaphore(max_concurrent_reads)
        self._write_lock = threading.Lock()

    def run_read(self, func: Callable[..., T], *args: object, **kwargs: object) -> T:
        with self._read_semaphore:
            return func(*args, **kwargs)

    def run_write(self, func: Callable[..., T], *args: object, **kwargs: object) -> T:
        with self._write_lock:
            return func(*args, **kwargs)
