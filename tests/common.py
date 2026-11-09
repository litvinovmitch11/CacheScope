from __future__ import annotations

import subprocess
from pathlib import Path
from typing import Iterable


def run(
    command: Iterable[object],
    *,
    cwd: Path | None = None,
    input_text: str | None = None,
    env: dict[str, str] | None = None,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(item) for item in command],
        cwd=cwd,
        input=input_text,
        env=env,
        check=check,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def require_tool(directory: Path, name: str) -> Path:
    path = directory / name
    if not path.is_file():
        raise SystemExit(f"required tool is missing: {path}")
    return path
