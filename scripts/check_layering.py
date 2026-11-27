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


def main() -> int:
    errors = []
    for module, allowed in RULES.items():
        base = ROOT / module
        if not base.exists():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in {".hpp", ".cpp", ".h"}:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for inc in INCLUDE_RE.findall(text):
                prefix = inc.split("/")[0] if "/" in inc else None
                if prefix is None or prefix not in INTERNAL_PREFIXES:
                    continue  # same-directory or external/system header
                if prefix not in allowed:
                    errors.append(f"{path.relative_to(ROOT)}: forbidden include \"{inc}\" (module {module})")
            if module == "include/bgl":
                for i, line in enumerate(text.splitlines(), 1):
                    if line.lstrip().startswith("#include") and FORBIDDEN_IN_PUBLIC.search(line):
                        errors.append(f"{path.relative_to(ROOT)}:{i}: public header includes backend header")
    for e in errors:
        print(e)
    print(f"layering check: {'FAILED' if errors else 'OK'} ({len(errors)} violation(s))")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
