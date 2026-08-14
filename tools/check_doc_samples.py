#!/usr/bin/env python3
"""Compile-gate for the docs: run `dragon check` over every fenced ```dragon block.

A block is skipped when its first line is `# doc: no-check`, when it contains
placeholder ellipses ("..."), or (heuristic) when an import names a module that
exists neither under stdlib/ nor packages/, meaning the snippet references
reader-authored files that do not exist in this repo.

Usage: check_doc_samples.py <dragon-binary> <docs-dir>
Exits non-zero if any checked block fails to compile.
"""

import re
import subprocess
import sys
import tempfile
from pathlib import Path

FENCE_OPEN = re.compile(r"^```dragon\s*$")
IMPORT_RE = re.compile(r"^\s*(?:from|import)\s+([A-Za-z_][A-Za-z0-9_]*)")


def known_module_roots(repo: Path) -> set:
    """Top-level module names resolvable from stdlib/ or packages/."""
    roots = set()
    for base in (repo / "stdlib", repo / "packages"):
        if not base.is_dir():
            continue
        for entry in base.iterdir():
            if entry.is_dir():
                roots.add(entry.name)
            elif entry.suffix == ".dr":
                roots.add(entry.stem)
    return roots


def dragon_blocks(md_path: Path):
    """Yield (start_line, code) for each fenced ```dragon block in the file."""
    inside = False
    start = 0
    block = []
    for lineno, line in enumerate(md_path.read_text(encoding="utf-8").splitlines(), 1):
        if not inside:
            if FENCE_OPEN.match(line):
                inside = True
                start = lineno + 1
                block = []
        elif line.strip() == "```":
            inside = False
            yield start, "\n".join(block) + "\n"
        else:
            block.append(line)


def skip_reason(code: str, roots: set):
    lines = code.splitlines()
    if not code.strip():
        return "empty block"
    if lines[0].strip() == "# doc: no-check":
        return "no-check marker"
    if "..." in code or "…" in code:
        return "placeholder ellipsis"
    for line in lines:
        m = IMPORT_RE.match(line)
        if m and m.group(1) not in roots:
            return "imports unknown module '%s'" % m.group(1)
    return None


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: check_doc_samples.py <dragon-binary> <docs-dir>", file=sys.stderr)
        return 2
    dragon = Path(sys.argv[1])
    docs = Path(sys.argv[2])
    repo = Path(__file__).resolve().parent.parent
    roots = known_module_roots(repo)

    checked = 0
    skipped = 0
    failures = []
    with tempfile.TemporaryDirectory(prefix="dragon-doc-samples-") as td:
        sample = Path(td) / "sample.dr"
        for md in sorted(docs.glob("*.md")):
            for start, code in dragon_blocks(md):
                reason = skip_reason(code, roots)
                if reason is not None:
                    skipped += 1
                    continue
                sample.write_text(code, encoding="utf-8")
                try:
                    proc = subprocess.run(
                        [str(dragon), "check", str(sample),
                         "-I", str(repo / "packages")],
                        capture_output=True, text=True, timeout=60)
                except subprocess.TimeoutExpired:
                    failures.append((md.name, start, "dragon check timed out"))
                    continue
                checked += 1
                if proc.returncode != 0:
                    detail = (proc.stderr or proc.stdout).strip()
                    failures.append((md.name, start, detail))

    for name, start, detail in failures:
        print("FAIL %s:%d\n%s\n" % (name, start, detail), file=sys.stderr)
    print("docs samples: %d checked, %d skipped, %d failed"
          % (checked, skipped, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
