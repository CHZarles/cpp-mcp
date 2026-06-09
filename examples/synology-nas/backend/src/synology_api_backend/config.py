from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping

from dotenv import load_dotenv


TRUE_VALUES = {"1", "true", "yes", "y", "on"}
FALSE_VALUES = {"0", "false", "no", "n", "off"}


@dataclass(frozen=True)
class Settings:
    synology_host: str
    synology_port: int
    synology_username: str
    synology_password: str
    backend_token: str
    synology_secure: bool = False
    synology_cert_verify: bool = False
    backend_host: str = "127.0.0.1"
    backend_port: int = 9000
    max_concurrent_reads: int = 8
    dsm_version: int = 7

    @classmethod
    def from_env(
        cls,
        env: Mapping[str, str] | None = None,
        dotenv_path: str | Path | None = None,
    ) -> "Settings":
        if env is None:
            load_dotenv(dotenv_path=dotenv_path)
            env = os.environ

        missing = [
            name
            for name in (
                "SYNOLOGY_HOST",
                "SYNOLOGY_PORT",
                "SYNOLOGY_USERNAME",
                "SYNOLOGY_PASSWORD",
                "BACKEND_TOKEN",
            )
            if not env.get(name)
        ]
        if missing:
            raise ValueError(f"Missing required environment variables: {', '.join(missing)}")

        return cls(
            synology_host=env["SYNOLOGY_HOST"],
            synology_port=_parse_int(env["SYNOLOGY_PORT"], "SYNOLOGY_PORT"),
            synology_username=env["SYNOLOGY_USERNAME"],
            synology_password=env["SYNOLOGY_PASSWORD"],
            synology_secure=_parse_bool(env.get("SYNOLOGY_SECURE", "false"), "SYNOLOGY_SECURE"),
            synology_cert_verify=_parse_bool(
                env.get("SYNOLOGY_CERT_VERIFY", "false"),
                "SYNOLOGY_CERT_VERIFY",
            ),
            backend_token=env["BACKEND_TOKEN"],
            backend_host=env.get("BACKEND_HOST", "127.0.0.1"),
            backend_port=_parse_int(env.get("BACKEND_PORT", "9000"), "BACKEND_PORT"),
            max_concurrent_reads=_parse_int(
                env.get("MAX_CONCURRENT_READS", "8"),
                "MAX_CONCURRENT_READS",
            ),
            dsm_version=_parse_int(env.get("SYNOLOGY_DSM_VERSION", "7"), "SYNOLOGY_DSM_VERSION"),
        )


def _parse_bool(value: str, name: str) -> bool:
    normalized = value.strip().lower()
    if normalized in TRUE_VALUES:
        return True
    if normalized in FALSE_VALUES:
        return False
    raise ValueError(f"{name} must be a boolean value")


def _parse_int(value: str, name: str) -> int:
    try:
        return int(value)
    except ValueError as exc:
        raise ValueError(f"{name} must be an integer") from exc
