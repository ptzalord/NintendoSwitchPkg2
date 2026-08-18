#!/usr/bin/env python3
"""
check_duplicate_pcds.py – detect duplicate PCD token assignments in an
EDK2 DSC file.  Exits with code 1 and prints offending lines if any
duplicates are found.

Per-component <PcdsFixedAtBuild> override blocks (those that appear inside
a module `{ ... }` stanza in [Components]) are intentional and are excluded
from the duplicate check.

Usage:
    python check_duplicate_pcds.py NintendoSwitch.dsc
"""
import re
import sys
from collections import defaultdict


def check_duplicates(dsc_path: str) -> int:
    """Return 0 if no duplicates found, 1 otherwise."""
    section = ""
    # Map section_type -> { token_name -> [line_numbers] }
    seen: dict[str, dict[str, list[int]]] = defaultdict(lambda: defaultdict(list))

    with open(dsc_path, encoding="utf-8") as f:
        lines = f.readlines()

    pcd_re = re.compile(r"^\s*(g\w+\.\w+)\s*\|")

    in_components = False
    brace_depth = 0  # depth inside a component { ... } override block

    for lineno, line in enumerate(lines, 1):
        stripped = line.strip()
        if stripped.startswith("#"):
            continue

        # Track whether we are in a [Components.*] section.
        m = re.match(r"^\[([^\]]+)\]", stripped)
        if m:
            tag = m.group(1).strip().lower()
            if tag.startswith("components"):
                in_components = True
                section = ""   # component blocks don't have a Pcds section header
            elif tag.startswith("pcds"):
                in_components = False
                section = tag
            else:
                in_components = False
                section = ""
            brace_depth = 0
            continue

        # Inside [Components], track { } depth to detect per-module overrides.
        if in_components:
            brace_depth += stripped.count("{") - stripped.count("}")
            brace_depth = max(brace_depth, 0)
            # Skip: we don't check PCDs inside component override blocks.
            continue

        if not section:
            continue

        m = pcd_re.match(stripped)
        if m:
            token = m.group(1)
            seen[section][token].append(lineno)

    duplicates_found = False
    for section_name, tokens in sorted(seen.items()):
        for token, linenos in sorted(tokens.items()):
            if len(linenos) > 1:
                print(
                    f"DUPLICATE PCD in [{section_name}]: {token} "
                    f"at lines {linenos}"
                )
                duplicates_found = True

    if duplicates_found:
        print(f"\nError: duplicate PCD assignments found in {dsc_path}")
        return 1

    print(f"OK: no duplicate PCD assignments in {dsc_path}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <path/to/file.dsc>")
        sys.exit(2)
    sys.exit(check_duplicates(sys.argv[1]))
