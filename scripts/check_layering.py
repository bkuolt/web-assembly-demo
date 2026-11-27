#!/usr/bin/env python3
"""Fails if a module includes a header from a module it must not depend on."""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INCLUDE_RE = re.compile(r'^\s*#\s*include\s+[<"]([^">]+)[>"]', re.MULTILINE)

# module directory (relative to ROOT) -> allowed include prefixes
RULES = {
    "src/core":    {"core"},
    "src/audio":   {"core", "audio"},
    "src/scene":   {"core", "scene"},
    "src/gpu":     {"core", "gpu", "scene"},   # known debt: gpu -> scene (see docs/ENGINE_ARCHITECTURE_PLAN.md)
    "src/text":    {"core", "gpu", "text"},
    "src/ui":      {"core", "gpu", "text", "ui"},
    "src/engine":  {"core", "gpu", "scene", "audio", "text", "ui", "engine", "bgl"},
    "src/wasm":    {"wasm"},
    "apps/viewer": {"core", "gpu", "scene", "audio", "text", "ui", "engine", "bgl", "viewer"},
    "include/bgl": {"core", "bgl"},
    "samples":     {"bgl"},
}
FORBIDDEN_IN_PUBLIC = re.compile(r'webgpu|WGPU|SDL|fastgltf|miniaudio|ft2build|freetype|hb\.h|curl|ktx', re.IGNORECASE)
INTERNAL_PREFIXES = {"core", "audio", "scene", "gpu", "text", "ui", "engine", "wasm", "viewer", "bgl"}


def _resolve_include_prefix(path: Path, inc: str) -> str | None:
    if inc.startswith("."):
        resolved = (path.parent / inc).resolve()
        for prefix in INTERNAL_PREFIXES:
            for root_dir in (ROOT / "src" / prefix, ROOT / "include" / prefix, ROOT / "apps" / prefix):
                try:
                    resolved.relative_to(root_dir.resolve())
                    return prefix
                except ValueError:
                    pass
        return None
    if "/" not in inc:
        return None
    prefix = inc.split("/")[0]
    return prefix if prefix in INTERNAL_PREFIXES else None


def main() -> int:
    errors = []
    search_roots = [ROOT / "src", ROOT / "include", ROOT / "apps"]
    for module, allowed in RULES.items():
        base = ROOT / module
        if not base.exists():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in {".hpp", ".cpp", ".h"}:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for inc in INCLUDE_RE.findall(text):
                prefix = _resolve_include_prefix(path, inc)
                if prefix is None:
                    continue  # same-directory or external/system header
                if prefix not in allowed:
                    errors.append(f"{path.relative_to(ROOT)}: forbidden include \"{inc}\" (module {module})")
            if module == "include/bgl":
                # Check direct and transitive internal headers included from public headers
                stack = [path]
                seen = {path.resolve()}
                while stack:
                    cur = stack.pop()
                    cur_text = cur.read_text(encoding="utf-8", errors="replace")
                    for i, line in enumerate(cur_text.splitlines(), 1):
                        if not line.lstrip().startswith("#include"):
                            continue
                        if FORBIDDEN_IN_PUBLIC.search(line):
                            errors.append(f"{cur.relative_to(ROOT)}:{i}: public header includes backend header (via {path.relative_to(ROOT)})")
                        m = INCLUDE_RE.match(line)
                        if m:
                            inc = m.group(1)
                            for sr in [cur.parent] + search_roots:
                                cand = (sr / inc).resolve()
                                if cand.is_file() and cand not in seen:
                                    seen.add(cand)
                                    stack.append(cand)
                                    break
    for e in errors:
        print(e)
    print(f"layering check: {'FAILED' if errors else 'OK'} ({len(errors)} violation(s))")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
