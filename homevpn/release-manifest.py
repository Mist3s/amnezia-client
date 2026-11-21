#!/usr/bin/env python3
"""Writes homevpn-release.json into a folder with release files: the name, sha256 and purpose of each file.
The backend reads it to publish the files of a GitHub release.
Usage: homevpn/release-manifest.py <version> <folder> [<changes file>]
An APK needs the description that homevpn/android-apk.sh puts next to it (<apk name>.json).
The changes file is the output of homevpn/changelog.py: what is new in this build."""
import hashlib
import json
import sys
from pathlib import Path

MANIFEST_NAME = "homevpn-release.json"
PLATFORM_BY_SUFFIX = {".apk": "android", ".exe": "windows", ".run": "linux"}


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        while chunk := stream.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def describe(path: Path) -> dict:
    details = path.with_name(path.name + ".json")
    if path.suffix == ".apk" and not details.exists():
        sys.exit(f"{path.name}: no {details.name} next to it")
    extra = json.loads(details.read_text()) if details.exists() else {}
    return {"name": path.name, "sha256": sha256_of(path), "platform": PLATFORM_BY_SUFFIX[path.suffix], **extra}


def main(version: str, folder: str, changes_file: str | None = None):
    files = sorted(path for path in Path(folder).iterdir() if path.suffix in PLATFORM_BY_SUFFIX)
    if not files:
        sys.exit(f"{folder}: no release files")
    manifest = {"version": version, "files": [describe(path) for path in files]}
    if changes_file:
        manifest["changes"] = json.loads(Path(changes_file).read_text())
    text = json.dumps(manifest, ensure_ascii=False, indent=2) + "\n"
    Path(folder, MANIFEST_NAME).write_text(text)
    print(text, end="")


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    main(*sys.argv[1:])
