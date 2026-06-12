from __future__ import annotations

import uvicorn

from .app import create_app
from .config import Settings


def main() -> None:
    settings = Settings.from_env()
    uvicorn.run(
        create_app(settings),
        host=settings.backend_host,
        port=settings.backend_port,
    )


if __name__ == "__main__":
    main()
