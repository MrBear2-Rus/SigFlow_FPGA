"""Stage baseline ZIP dependencies without changing already checked-out files."""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import zipfile
from pathlib import Path


def digest(path: Path) -> str:
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--wx-source", type=Path, required=True)
    parser.add_argument("--wx-build-lib", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    report: dict = {"archives": [], "wx": {}}
    for name in ("3rd.zip", "external.zip", "tools.zip"):
        archive = root / name
        added = skipped = 0
        with zipfile.ZipFile(archive) as source:
            for entry in source.infolist():
                target = (root / entry.filename).resolve()
                if not target.is_relative_to(root):
                    raise ValueError(f"Archive escapes workspace: {entry.filename}")
                if entry.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                if target.exists():
                    skipped += 1
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                with source.open(entry) as src, target.open("wb") as dst:
                    shutil.copyfileobj(src, dst)
                added += 1
        report["archives"].append({"file": name, "sha256": digest(archive), "added": added, "existing_preserved": skipped})
    wx_root = root / "3rd/wx-mingw"
    shutil.copytree(args.wx_source / "include", wx_root / "include", dirs_exist_ok=True)
    shutil.copytree(args.wx_build_lib, wx_root / "lib", dirs_exist_ok=True)
    # wx CMake builds include '-Windows' in import-library names; the baseline's
    # documented prebuilt-library finder expects the unsuffixed stem.
    aliases = []
    for lib in (wx_root / "lib").glob("*-Windows.dll.a"):
        alias = lib.with_name(lib.name.replace("-Windows.dll.a", ".dll.a"))
        shutil.copy2(lib, alias)
        aliases.append({"source": lib.name, "alias": alias.name, "sha256": digest(lib)})
    report["wx"] = {"source_headers": str(args.wx_source), "source_libraries": str(args.wx_build_lib), "aliases": aliases,
                    "note": "Reused local wxWidgets 3.2.9 MinGW GCC 14.2 build; original deployment is read-only."}
    output = root / ".work/k0"
    output.mkdir(parents=True, exist_ok=True)
    (output / "dependencies.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"archives": report["archives"], "wx_aliases": len(aliases)}, indent=2))


if __name__ == "__main__":
    main()
