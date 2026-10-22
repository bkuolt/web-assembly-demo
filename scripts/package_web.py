#!/usr/bin/env python3
"""
scripts/package_web.py
Packages a minimal, self-contained zip distribution of the bgl-Engine Web viewer.
Excludes large benchmark scenes (e.g. Sponza.glb 53MB), debug maps, and intermediate artifacts.
"""

import os
import sys
import argparse
import hashlib
import zipfile
from pathlib import Path

def compute_sha256(filepath: Path) -> str:
    sha = hashlib.sha256()
    with open(filepath, "rb") as f:
        while chunk := f.read(65536):
            sha.update(chunk)
    return sha.hexdigest()

def package_web_app(output_zip: Path, include_sponza: bool = False):
    project_root = Path(__file__).resolve().parent.parent
    web_dir = project_root / "web"

    if not web_dir.exists():
        print(f"Error: Web directory '{web_dir}' does not exist.", file=sys.stderr)
        sys.exit(1)

    output_zip.parent.mkdir(parents=True, exist_ok=True)

    # Core essential files required for the Web viewer
    files_to_pack = [
        (web_dir / "index.html", "index.html"),
        (web_dir / "style.css", "style.css"),
        (web_dir / "app.js", "app.js"),
    ]

    # Precompiled WebAssembly binary
    wasm_file = web_dir / "dist" / "bgl_engine.wasm"
    if wasm_file.exists():
        files_to_pack.append((wasm_file, "dist/bgl_engine.wasm"))

    # Lightweight glTF demo model (ToyCar.glb ~5.1 MB)
    toycar_file = web_dir / "assets" / "ToyCar.glb"
    if toycar_file.exists():
        files_to_pack.append((toycar_file, "assets/ToyCar.glb"))

    if include_sponza:
        sponza_file = web_dir / "assets" / "Sponza.glb"
        if sponza_file.exists():
            files_to_pack.append((sponza_file, "assets/Sponza.glb"))

    print("=" * 80)
    print(" Packaging bgl-Engine Minimal Web Viewer Distribution")
    print("=" * 80)

    total_uncompressed = 0
    packed_entries = []

    with zipfile.ZipFile(output_zip, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for src_path, arc_name in files_to_pack:
            if not src_path.exists():
                print(f"  [MISSING] {src_path}", file=sys.stderr)
                continue

            file_size = src_path.stat().st_size
            total_uncompressed += file_size
            zf.write(src_path, arc_name)
            packed_entries.append((arc_name, file_size))
            print(f"  + {arc_name:<30} ({file_size / (1024 * 1024):.2f} MB / {file_size:,} bytes)")

    zip_size = output_zip.stat().st_size
    checksum = compute_sha256(output_zip)
    ratio = (1.0 - (zip_size / total_uncompressed)) * 100.0 if total_uncompressed > 0 else 0.0

    print("-" * 80)
    print(f"Output Archive:       {output_zip}")
    print(f"Total Files Packed:   {len(packed_entries)}")
    print(f"Uncompressed Size:    {total_uncompressed / (1024 * 1024):.2f} MB ({total_uncompressed:,} bytes)")
    print(f"Compressed Zip Size:  {zip_size / (1024 * 1024):.2f} MB ({zip_size:,} bytes) [{ratio:.1f}% reduction]")
    print(f"SHA-256 Checksum:     {checksum}")
    print("=" * 80)
    print("Minimal web app package created successfully!\n")

def main():
    parser = argparse.ArgumentParser(description="Package minimal bgl-Engine Web viewer zip distribution.")
    parser.add_argument("-o", "--output", type=Path, default=None,
                        help="Path to output zip file (default: dist/bgl_web_app_minimal.zip)")
    parser.add_argument("--include-sponza", action="store_true",
                        help="Optionally include large Sponza.glb asset (53 MB)")

    args = parser.parse_args()

    project_root = Path(__file__).resolve().parent.parent
    output_path = args.output or (project_root / "dist" / "bgl_web_app_minimal.zip")

    package_web_app(output_path, include_sponza=args.include_sponza)

if __name__ == "__main__":
    main()

