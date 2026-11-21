#!/usr/bin/env python3
"""What is new in a HomeVPN build, taken from homevpn/changelog.json (build number -> new / improved / fixed).
Usage: homevpn/changelog.py <build number> [--markdown]
Prints the changes of the build as JSON, or as release notes with --markdown. A build without changes is an error:
the app shows this list in the update window, so every build needs one."""
import json
import sys
from pathlib import Path

CHANGELOG = Path(__file__).with_name("changelog.json")
TITLES = {"new": "Новое", "improved": "Улучшено", "fixed": "Исправлено"}
MAX_LINES = 30
MAX_LINE_LENGTH = 300


def changes_of(build_number: str) -> dict[str, list[str]]:
    entry = json.loads(CHANGELOG.read_text()).get(build_number)
    if not isinstance(entry, dict) or set(entry) - set(TITLES):
        sys.exit(f"{CHANGELOG.name}: describe build {build_number} with the keys {', '.join(TITLES)}")
    changes = {kind: entry.get(kind, []) for kind in TITLES}
    for lines in changes.values():
        is_list_of_lines = isinstance(lines, list) and all(isinstance(line, str) and line.strip() for line in lines)
        if not is_list_of_lines or len(lines) > MAX_LINES or any(len(line) > MAX_LINE_LENGTH for line in lines):
            sys.exit(f"{CHANGELOG.name}: build {build_number} needs lists of at most {MAX_LINES} lines"
                     f" of at most {MAX_LINE_LENGTH} characters")
    if not any(changes.values()):
        sys.exit(f"{CHANGELOG.name}: build {build_number} has no changes")
    return changes


def markdown(changes: dict[str, list[str]]) -> str:
    sections = [f"### {TITLES[kind]}\n\n" + "\n".join(f"- {line}" for line in lines)
                for kind, lines in changes.items() if lines]
    return "\n\n".join(sections) + "\n"


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3) or sys.argv[2:] not in ([], ["--markdown"]):
        sys.exit(__doc__)
    found = changes_of(sys.argv[1])
    print(markdown(found) if len(sys.argv) == 3 else json.dumps(found, ensure_ascii=False, indent=2), end="")
