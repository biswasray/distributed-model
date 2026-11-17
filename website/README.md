# Docs website (GitHub Pages)

The Markdown docs in this repo (`README.md`, `HOW_TO_RUN.md`, the operator guide, `architecture/`, `adr/`, `CLAUDE.md` and `.claude/skills/`) are published as a static site with [MkDocs Material](https://squidfunk.github.io/mkdocs-material/). The Markdown files stay where they are and remain the source. This folder only holds the build setup.

| File | Purpose |
|---|---|
| `stage.py` | Copies the doc pages into `_staging/` and rewrites links (repo files that aren't pages become GitHub links) |
| `mkdocs.yml` | Site config and navigation. Add new pages (for example a new ADR) to `nav` here |
| `requirements.txt` | Pinned MkDocs versions |
| `../.github/workflows/docs.yml` | Builds on every PR and deploys to Pages on push to `main` |

## Preview locally

```powershell
py -m venv .venv-docs
.\.venv-docs\Scripts\pip install -r website\requirements.txt
py website\stage.py
.\.venv-docs\Scripts\mkdocs serve -f website\mkdocs.yml    # http://127.0.0.1:8000/
```

```sh
python3 -m venv .venv-docs && .venv-docs/bin/pip install -r website/requirements.txt
python3 website/stage.py && .venv-docs/bin/mkdocs serve -f website/mkdocs.yml
```

`mkdocs serve` watches `_staging/`, not the real docs, so re-run `stage.py` after editing a page. Use `mkdocs build --strict` to check for broken links the way CI does.

## Enable GitHub Pages (once)

In the repository on GitHub: **Settings → Pages → Build and deployment → Source: GitHub Actions**. The next push to `main` publishes the site at `https://biswasray.github.io/distributed-model/`.
