"""Stage the repository's Markdown docs into website/_staging for MkDocs.

The docs live at the repository root next to the code, so MkDocs cannot use
the root as docs_dir. This script copies the doc pages into a staging folder
and rewrites relative links:

* links to another staged page are re-pointed at its staged location
  (e.g. .claude/skills/x/SKILL.md -> skills/x.md);
* a link to a directory that has a README.md is pointed at that README;
* links to anything else in the repo (source files, examples, scripts) are
  turned into absolute links to the file on GitHub.

Usage: python website/stage.py
"""

from __future__ import annotations

import os
import posixpath
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = Path(__file__).resolve().parent / "_staging"
REPO_URL = os.environ.get("DOCS_REPO_URL", "https://github.com/biswasray/distributed-model")
BRANCH = os.environ.get("DOCS_BRANCH", "main")


def page_map() -> dict[str, str]:
    """Source path (repo-relative, posix) -> staged path."""
    pages = {
        "README.md": "index.md",
        "HOW_TO_RUN.md": "HOW_TO_RUN.md",
        "distributed-qwen-cluster.md": "distributed-qwen-cluster.md",
        "CLAUDE.md": "CLAUDE.md",
    }
    for sub in ("architecture", "adr"):
        for p in sorted((ROOT / sub).glob("*.md")):
            rel = p.relative_to(ROOT).as_posix()
            pages[rel] = rel.replace("README.md", "index.md")
    for p in sorted((ROOT / ".claude" / "skills").glob("*/SKILL.md")):
        pages[p.relative_to(ROOT).as_posix()] = f"skills/{p.parent.name}.md"
    return pages


LINK_RE = re.compile(r"(\]\()([^)\s]+)(\))")
FENCE_RE = re.compile(r"^\s*(```|~~~)")


def github_slugs(src: str) -> set[str]:
    """Return the GitHub anchors of duplicate headings ("x-1", "x-2", ...) in a file."""
    counts: dict[str, int] = {}
    dups: set[str] = set()
    in_fence = False
    for line in (ROOT / src).read_text(encoding="utf-8").splitlines():
        if FENCE_RE.match(line):
            in_fence = not in_fence
        m = None if in_fence else re.match(r"^#{1,6}\s+(.*?)\s*#*\s*$", line)
        if not m:
            continue
        text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", m.group(1))
        slug = re.sub(r"[^\w\- ]", "", text.lower()).replace(" ", "-")
        n = counts.get(slug, 0)
        counts[slug] = n + 1
        if n:
            dups.add(f"{slug}-{n}")
    return dups


def fix_duplicate_anchor(target_src: str, frag: str) -> str:
    """GitHub suffixes duplicate heading anchors with -N; Python-Markdown uses _N."""
    if frag and frag[1:] in github_slugs(target_src):
        base, _, n = frag.rpartition("-")
        return f"{base}_{n}"
    return frag


def rewrite_target(target: str, src: str, pages: dict[str, str]) -> str:
    if re.match(r"^[a-z][a-z0-9+.-]*:", target, re.I) or target.startswith(("#", "/")):
        return target
    path, _, frag = target.partition("#")
    frag = f"#{frag}" if frag else ""
    resolved = posixpath.normpath(posixpath.join(posixpath.dirname(src), path))

    if (ROOT / resolved).is_dir() and f"{resolved}/README.md" in pages:
        resolved = f"{resolved}/README.md"

    if resolved in pages:
        staged_src = pages[src]
        new = posixpath.relpath(pages[resolved], posixpath.dirname(staged_src) or ".")
        return new + fix_duplicate_anchor(resolved, frag)

    kind = "tree" if (ROOT / resolved).is_dir() else "blob"
    return f"{REPO_URL}/{kind}/{BRANCH}/{resolved}{frag}"


def rewrite(text: str, src: str, pages: dict[str, str]) -> str:
    out, in_fence = [], False
    for line in text.splitlines(keepends=True):
        if FENCE_RE.match(line):
            in_fence = not in_fence
        if not in_fence:
            line = LINK_RE.sub(lambda m: m.group(1) + rewrite_target(m.group(2), src, pages) + m.group(3), line)
        out.append(line)
    return "".join(out)


def main() -> int:
    pages = page_map()
    if OUT.exists():
        shutil.rmtree(OUT)
    for src, dst in pages.items():
        text = (ROOT / src).read_text(encoding="utf-8")
        target = OUT / dst
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(rewrite(text, src, pages), encoding="utf-8", newline="\n")
    extra = Path(__file__).resolve().parent / "assets"
    if extra.is_dir():
        shutil.copytree(extra, OUT / "assets")
    print(f"staged {len(pages)} pages into {OUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
