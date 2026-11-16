#!/usr/bin/env python3
"""
Doxygen input filter for GitHub Flavored Markdown files.

Translates GitHub math delimiters ($...$, $$...$$) and GFM alert callouts
(> [!NOTE], etc.) into Doxygen / MathJax syntax on stdout while leaving
fenced code blocks and inline code spans untouched.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

_FENCE_RE = re.compile(r"^[ \t]{0,3}(`{3,}|~{3,})(.*)$")
_ALERT_RE = re.compile(r"^([ \t]*>[ \t]*)\[!(NOTE|TIP|IMPORTANT|WARNING|CAUTION)\][ \t]*$")
_INLINE_OR_CODE_RE = re.compile(
    r"(`+[^`]*?`+)"
    r"|(?<!\\)\$\$(.+?)(?<!\\)\$\$"
    r"|(?<![\\$])\$(?!\s)([^$\n]+?)(?<!\s)(?<!\\)\$(?!\$)"
)


def _transform_inline(line: str) -> str:
    def _replace(match: re.Match[str]) -> str:
        if match.group(1) is not None:
            return match.group(1)
        if match.group(2) is not None:
            return f"\\f[{match.group(2)}\\f]"
        return f"\\f${match.group(3)}\\f$"

    return _INLINE_OR_CODE_RE.sub(_replace, line)


def filter_markdown(text: str) -> str:
    out_lines: list[str] = []
    in_fence = False
    fence_char = ""
    fence_len = 0
    in_block_math = False

    for raw_line in text.splitlines(keepends=True):
        line = raw_line.rstrip("\r\n")
        eol = raw_line[len(line) :]

        if not in_block_math:
            fence_match = _FENCE_RE.match(line)
            if fence_match:
                marker = fence_match.group(1)
                if not in_fence:
                    in_fence = True
                    fence_char = marker[0]
                    fence_len = len(marker)
                elif marker[0] == fence_char and len(marker) >= fence_len and not fence_match.group(2).strip():
                    in_fence = False
                out_lines.append(raw_line)
                continue

        if in_fence:
            out_lines.append(raw_line)
            continue

        stripped = line.strip()
        if stripped == "$$":
            indent = line[: len(line) - len(line.lstrip())]
            in_block_math = not in_block_math
            tag = "\\f[" if in_block_math else "\\f]"
            out_lines.append(f"{indent}{tag}{eol}")
            continue

        if in_block_math:
            out_lines.append(raw_line)
            continue

        if stripped.startswith("$$") and stripped.endswith("$$") and len(stripped) > 4:
            indent = line[: len(line) - len(line.lstrip())]
            inner = stripped[2:-2]
            out_lines.append(f"{indent}\\f[{inner}\\f]{eol}")
            continue

        alert_match = _ALERT_RE.match(line)
        if alert_match:
            label = alert_match.group(2).capitalize()
            out_lines.append(f"{alert_match.group(1)}**{label}:**{eol}")
            continue

        out_lines.append(f"{_transform_inline(line)}{eol}")

    return "".join(out_lines)


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        sys.stdout.write(filter_markdown(sys.stdin.read()))
        return 0
    content = Path(argv[1]).read_text(encoding="utf-8")
    sys.stdout.write(filter_markdown(content))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
