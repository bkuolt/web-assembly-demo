#!/usr/bin/env bash
set -euo pipefail

OUT_DIR="${1:-_site}"
DOCS_DIR="${2:-build/docs/html}"

rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}/dist" "${OUT_DIR}/assets" "${OUT_DIR}/js" "${OUT_DIR}/web"

# 1. Root site: WebGPU/WASM viewer directly at /
cp -f web/index.html "${OUT_DIR}/index.html"
cp -f web/style.css "${OUT_DIR}/style.css"
cp -f web/app.js "${OUT_DIR}/app.js"
cp -f web/gltf_worker.js "${OUT_DIR}/gltf_worker.js"
cp -rf web/js/* "${OUT_DIR}/js/"
if [[ -d web/dist ]] && [[ -n "$(ls -A web/dist 2>/dev/null)" ]]; then
  cp -rf web/dist/* "${OUT_DIR}/dist/"
fi
if [[ -d web/assets ]] && [[ -n "$(ls -A web/assets 2>/dev/null)" ]]; then
  cp -rf web/assets/* "${OUT_DIR}/assets/"
fi

# 2. Compatibility redirect for legacy /web/index.html subpath (avoids duplicating 60MB of GLB assets)
cat > "${OUT_DIR}/web/index.html" <<'EOF'
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta http-equiv="refresh" content="0; url=../index.html">
  <title>Redirecting to bgl-Engine Web Viewer</title>
  <script>
    window.location.replace("../index.html" + window.location.search + window.location.hash);
  </script>
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
  <meta http-equiv="refresh" content="0; url=../../docs/index.html">
  <title>Redirecting to bgl-Engine API Docs</title>
  <script>
    window.location.replace("../../docs/index.html" + window.location.search + window.location.hash);
  </script>
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
