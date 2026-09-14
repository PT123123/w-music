#!/usr/bin/env python3
"""Install an online-source adapter into the external provider directory.

The app never reads adapters from the repository: they live outside it, so a
site config can be added, edited or removed without touching git.

    python tools/new_provider.py my-source            # from the bundled template
    python tools/new_provider.py my-source path/to.json

Writes to %LOCALAPPDATA%\\w-music\\providers\\<id>.json on Windows
($XDG_DATA_HOME/w-music/providers/<id>.json elsewhere), then validates the file
with the same rules the app applies at load time.
"""

from __future__ import annotations

import json
import os
import shutil
import sys
from pathlib import Path

REQUIRED_TOP = ("id", "baseUrl", "search")


def provider_dir() -> Path:
    override = os.environ.get("WMUSIC_PROVIDER_DIR")
    if override:
        return Path(override.split(os.pathsep)[0])
    if os.name == "nt":
        base = os.environ.get("LOCALAPPDATA") or str(Path.home() / "AppData" / "Local")
        return Path(base) / "w-music" / "providers"
    base = os.environ.get("XDG_DATA_HOME") or str(Path.home() / ".local" / "share")
    return Path(base) / "w-music" / "providers"


def validate(data: dict) -> list[str]:
    problems = [f'missing top-level "{key}"' for key in REQUIRED_TOP if key not in data]
    search = data.get("search")
    if isinstance(search, dict):
        if not search.get("url"):
            problems.append('search step has no "url"')
        elif not (search.get("itemPattern") or search.get("listPath") or search.get("fields")):
            problems.append('search step needs "itemPattern", "listPath" or "fields"')
        for field in search.get("fields", []) or []:
            if field.get("source") not in ("json", "regex", "static", None):
                problems.append(f'unknown source "{field.get("source")}" for field "{field.get("key")}"')
    return problems


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__.strip())
        return 2

    adapter_id = argv[1]
    source = Path(argv[2]) if len(argv) > 2 else Path(__file__).resolve().parent.parent / "adapters" / "template.json.example"
    if not source.is_file():
        print(f"template not found: {source}")
        return 1

    try:
        data = json.loads(source.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        print(f"invalid JSON in {source}: {error}")
        return 1

    data["id"] = adapter_id
    data.setdefault("name", adapter_id)

    problems = validate(data)
    if problems:
        print("adapter would not load:")
        for problem in problems:
            print(f"  - {problem}")
        return 1

    target_dir = provider_dir()
    target_dir.mkdir(parents=True, exist_ok=True)
    target = target_dir / f"{adapter_id}.json"
    if target.exists():
        shutil.copy(target, target.with_suffix(".json.bak"))
        print(f"existing file backed up as {target.with_suffix('.json.bak').name}")

    target.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"installed: {target}")
    print("Restart w-music (or press 重新加载 on the discover page) to pick it up.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
