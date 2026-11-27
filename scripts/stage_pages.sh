#!/usr/bin/env bash
set -euo pipefail

OUT_DIR="${1:-_site}"
DOCS_DIR="${2:-build/docs/html}"

if [[ -z "${OUT_DIR}" || "${OUT_DIR}" == "/" || "${OUT_DIR}" == "." || "${OUT_DIR}" == ".." ]]; then
  echo "Refusing unsafe output directory: '${OUT_DIR}'" >&2
  exit 1
fi

rm -rf -- "${OUT_DIR}"
mkdir -p "${OUT_DIR}/dist" "${OUT_DIR}/assets" "${OUT_DIR}/js" "${OUT_DIR}/web"

# 1. Root site: WebGPU/WASM viewer directly at /
cp -f web/index.html "${OUT_DIR}/index.html"
cp -f web/style.css "${OUT_DIR}/style.css"
cp -f web/app.js "${OUT_DIR}/app.js"
cp -f web/gltf_worker.js "${OUT_DIR}/gltf_worker.js"
cp -rf web/js/* "${OUT_DIR}/js/"
if [[ ! -f "web/dist/bgl_engine.wasm" ]]; then
  echo "Required WebAssembly artifact missing: web/dist/bgl_engine.wasm" >&2
  exit 1
fi
cp -rf web/dist/* "${OUT_DIR}/dist/"
if [[ ! -d web/assets ]] || [[ -z "$(ls -A web/assets 2>/dev/null)" ]]; then
  echo "Required web assets directory missing or empty: web/assets" >&2
  exit 1
fi
cp -rf web/assets/* "${OUT_DIR}/assets/"

# 2. Compatibility redirect for legacy /web/index.html subpath (avoids duplicating 60MB of GLB assets)
cat > "${OUT_DIR}/web/index.html" <<'EOF'
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="referrer" content="no-referrer">
  <meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none';">
  <meta http-equiv="refresh" content="0; url=../index.html">
  <title>Redirecting to bgl-Engine Web Viewer</title>
</head>
<body style="background:#0b0f19;color:#f1f5f9;font-family:sans-serif;display:flex;justify-content:center;align-items:center;height:100vh;">
  <p>Redirecting to <a href="../index.html" style="color:#38bdf8;">bgl-Engine Web Viewer</a>...</p>
</body>
</html>
EOF

# 3. Optional Doxygen HTML API documentation under /docs/
if [[ -d "${DOCS_DIR}" ]] && [[ -n "$(ls -A "${DOCS_DIR}" 2>/dev/null)" ]]; then
  mkdir -p "${OUT_DIR}/docs" "${OUT_DIR}/web/docs"
  cp -rf "${DOCS_DIR}/"* "${OUT_DIR}/docs/"
  cat > "${OUT_DIR}/web/docs/index.html" <<'EOF'
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="referrer" content="no-referrer">
  <meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none';">
  <meta http-equiv="refresh" content="0; url=../../docs/index.html">
  <title>Redirecting to bgl-Engine API Docs</title>
</head>
<body>
  <p>Redirecting to <a href="../../docs/index.html">API Documentation</a>...</p>
</body>
</html>
EOF
fi

# 4. Prevent Jekyll from ignoring files/directories
touch "${OUT_DIR}/.nojekyll"

echo "Staged GitHub Pages site at '${OUT_DIR}' ($(du -sh "${OUT_DIR}" | cut -f1))."
