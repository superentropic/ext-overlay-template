#!/usr/bin/env python3
"""
Integrity Hash Generator for DAD Research Tool

Generates SHA-256 hashes of critical source files that should NOT be modified
to add cheating capabilities (memory writes, input injection, aimbots, etc.).

Usage:
    python tools/generate_hashes.py              # Verify existing hashes
    python tools/generate_hashes.py --generate   # Regenerate hash manifest

The generated header (src/core/integrity_hashes.h) is checked into git.
If a critical file is modified, the build will detect the mismatch.
"""

import hashlib
import os
import sys

# Repository root (parent of tools/)
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Critical files that define the tool's READ-ONLY behavior.
# Modifying these could turn the research tool into a cheat.
#
# WHY each file is critical:
#   process.h/cpp      - Must remain read-only (no WriteProcessMemory)
#   main.cpp           - Main loop must not inject input or modify game state
#   world_to_screen.*  - Projection math must not be used for aim calculation
#   drawing.cpp        - Must not add crosshair/aim overlay elements
#   overlay.cpp        - Must not hook game input or inject clicks
#   entity.h           - Entity struct must not carry write-back fields
CRITICAL_FILES = [
    "src/core/process.h",
    "src/core/process.cpp",
    "src/main.cpp",
    "src/render/world_to_screen.h",
    "src/render/world_to_screen.cpp",
    "src/render/drawing.h",
    "src/render/drawing.cpp",
    "src/render/overlay.h",
    "src/render/overlay.cpp",
    "src/game/entity.h",
]

OUTPUT_HEADER = "src/core/integrity_hashes.h"


def hash_file(filepath: str) -> str:
    """Compute SHA-256 of a file, normalizing line endings to LF."""
    full_path = os.path.join(REPO_ROOT, filepath.replace("/", os.sep))
    if not os.path.exists(full_path):
        print(f"  WARNING: {filepath} not found, skipping")
        return "0" * 64

    with open(full_path, "rb") as f:
        content = f.read()

    # Normalize CRLF -> LF so hashes are consistent across platforms
    content = content.replace(b"\r\n", b"\n")
    return hashlib.sha256(content).hexdigest()


def generate_header(hashes: dict[str, str]) -> str:
    """Generate C++ header with embedded hash constants."""
    lines = [
        "#pragma once",
        "// ============================================================================",
        "//  AUTO-GENERATED -- Do not edit manually!",
        "//  Run: python tools/generate_hashes.py --generate",
        "//",
        "//  SHA-256 hashes of critical source files. These files define the tool's",
        "//  read-only research behavior. If any hash mismatches at build time,",
        "//  it means a critical file was modified -- review the change carefully",
        "//  to ensure no write/inject/aim capabilities were added.",
        "// ============================================================================",
        "",
        "#include <array>",
        "#include <string_view>",
        "#include <utility>",
        "",
        "namespace Integrity {",
        "",
        f"static constexpr size_t FILE_COUNT = {len(hashes)};",
        "",
        "// {filename, expected_sha256}",
        "static constexpr std::array<std::pair<std::string_view, std::string_view>, FILE_COUNT> EXPECTED_HASHES = {{",
    ]

    for filepath, sha in sorted(hashes.items()):
        lines.append(f'    {{"{filepath}", "{sha}"}},')

    lines += [
        "}};",
        "",
        "} // namespace Integrity",
        "",
    ]
    return "\n".join(lines)


def main():
    generate_mode = "--generate" in sys.argv

    print(f"DAD Research Tool — Integrity Hash {'Generator' if generate_mode else 'Verifier'}")
    print(f"Repository root: {REPO_ROOT}")
    print(f"Critical files: {len(CRITICAL_FILES)}")
    print()

    # Compute current hashes
    current_hashes = {}
    for filepath in CRITICAL_FILES:
        sha = hash_file(filepath)
        current_hashes[filepath] = sha
        print(f"  {sha[:16]}... {filepath}")

    if generate_mode:
        # Write the header
        header_content = generate_header(current_hashes)
        output_path = os.path.join(REPO_ROOT, OUTPUT_HEADER.replace("/", os.sep))
        os.makedirs(os.path.dirname(output_path), exist_ok=True)
        with open(output_path, "w", newline="\n") as f:
            f.write(header_content)
        print(f"\nGenerated: {OUTPUT_HEADER}")
        print("Commit this file alongside any approved changes to critical files.")
        return 0
    else:
        # Verify mode: compare against existing header
        header_path = os.path.join(REPO_ROOT, OUTPUT_HEADER.replace("/", os.sep))
        if not os.path.exists(header_path):
            print(f"\nERROR: {OUTPUT_HEADER} not found!")
            print("Run: python tools/generate_hashes.py --generate")
            return 1

        # Parse existing hashes from header
        mismatches = []
        with open(header_path, "r") as f:
            content = f.read()

        for filepath, current_sha in current_hashes.items():
            if current_sha not in content:
                mismatches.append(filepath)

        if mismatches:
            print(f"\nINTEGRITY CHECK FAILED — {len(mismatches)} file(s) modified:")
            for f in mismatches:
                print(f"  MISMATCH: {f}")
            print("\nIf these changes are intentional and do NOT add cheat capabilities:")
            print("  python tools/generate_hashes.py --generate")
            print("Then commit the updated integrity_hashes.h alongside your changes.")
            return 1
        else:
            print("\nAll integrity checks passed.")
            return 0


if __name__ == "__main__":
    sys.exit(main())
