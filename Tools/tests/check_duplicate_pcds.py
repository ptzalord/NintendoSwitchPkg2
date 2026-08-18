#!/usr/bin/env python3
"""
check_duplicate_pcds.py – detect duplicate PCD token assignments in an
EDK2 DSC file.  Exits with code 1 and prints offending lines if any
duplicates are found.

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

    section_re = re.compile(r"^\[(Pcds[^]]*)\]", re.IGNORECASE)
    pcd_re = re.compile(r"^\s*(g\w+\.\w+)\s*\|")

    for lineno, line in enumerate(lines, 1):
        stripped = line.strip()
        if stripped.startswith("#"):
            continue
        m = section_re.match(stripped)
        if m:
            section = m.group(1).lower()
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
