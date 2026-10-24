/**
 * @file app.js
 * @brief bgl-Engine Web Viewer Client Runtime
 * Handles Wasm initialization, WebGPU bridge, glTF Scene Graph Tree,
 * Collapsible JSON Inspector, and Live Telemetry.
 */

(() => {
  'use strict';

  // Application State
  let wasmExports = null;
  let wasmMemory = null;
  let currentGltf = null;
  let currentModelName = "Sponza.glb";
  let selectedNodeIndex = null;

  let isBboxActive = true;
  let isQuadtreeActive = false;
  let isSkyboxActive = true;
  let isHudActive = true;
  let isBumpActive = true;
  let isMipmapsActive = true;
  let isShadowsActive = true;
  let isReflectionsActive = true;

  // DOM Elements
  const pBar = document.getElementById('progress-bar');
  const sText = document.getElementById('status-text');
  const dropZone = document.getElementById('drop-zone');
  const dropText = document.getElementById('drop-text');
  const treePanel = document.getElementById('node-tree-panel');
  const treePill = document.getElementById('tree-toggle-pill');
  const btnBbox = document.getElementById('btn-bbox');
  const btnQuadtree = document.getElementById('btn-quadtree');
  const btnSkybox = document.getElementById('btn-skybox');
  const btnHud = document.getElementById('btn-hud');
  const btnBump = document.getElementById('btn-bump');
  const btnMipmaps = document.getElementById('btn-mipmaps');
  const btnShadows = document.getElementById('btn-shadows');
  const btnReflections = document.getElementById('btn-reflections');

  // Telemetry Elements
  const elDims = document.getElementById('aabb-dims');
  const elMin = document.getElementById('aabb-min');
  const elMax = document.getElementById('aabb-max');
  const elCenter = document.getElementById('aabb-center');
  const elPrims = document.getElementById('aabb-prims');

  /**
   * Update loading progress bar and status text.
   */
  function updateLoadProgress(percent, msg) {
    if (pBar) pBar.style.width = `${percent}%`;
    if (sText && msg) sText.textContent = msg;
  }
  window.updateLoadProgress = updateLoadProgress;

  /**
   * Toggle 3D Bounding Boxes & AABBs.
   */
  function toggleBBox() {
    isBboxActive = !isBboxActive;
    if (btnBbox) btnBbox.style.background = isBboxActive ? '#0284c7' : '#334155';
    if (wasmExports && wasmExports.bgl_toggle_bbox) {
      wasmExports.bgl_toggle_bbox();
    }
  }
  window.toggleBBox = toggleBBox;

  /**
   * Toggle 3D Quadtree ground boundaries.
   */
  function toggleQuadtree() {
    isQuadtreeActive = !isQuadtreeActive;
    if (btnQuadtree) btnQuadtree.style.background = isQuadtreeActive ? '#059669' : '#334155';
    if (wasmExports && wasmExports.bgl_toggle_quadtree) {
      wasmExports.bgl_toggle_quadtree();
    }
  }
  window.toggleQuadtree = toggleQuadtree;

  /**
   * Toggle Procedural Atmospheric Skybox.
   */
  function toggleSkybox() {
    isSkyboxActive = !isSkyboxActive;
    if (btnSkybox) btnSkybox.style.background = isSkyboxActive ? '#475569' : '#1e293b';
    if (wasmExports && wasmExports.bgl_toggle_skybox) {
      wasmExports.bgl_toggle_skybox();
    }
  }
  window.toggleSkybox = toggleSkybox;

  /**
   * Toggle In-Engine HUD Overlay.
   */
  function toggleHUD() {
    isHudActive = !isHudActive;
    if (btnHud) btnHud.style.background = isHudActive ? '#0284c7' : '#334155';
    if (wasmExports && wasmExports.bgl_toggle_debug_overlay) {
      wasmExports.bgl_toggle_debug_overlay();
    }
  }
  window.toggleHUD = toggleHUD;

  /**
   * Toggle Bump & Normal Mapping.
   */
  function toggleBump() {
    isBumpActive = !isBumpActive;
    if (btnBump) btnBump.style.background = isBumpActive ? '#0284c7' : '#334155';
    if (wasmExports && wasmExports.bgl_toggle_bump) {
      wasmExports.bgl_toggle_bump();
    }
  }
  window.toggleBump = toggleBump;

  /**
   * Toggle Texture Mipmapping (16x Anisotropic Trilinear vs Mip-0).
   */
  function toggleMipmaps() {
    isMipmapsActive = !isMipmapsActive;
    if (btnMipmaps) btnMipmaps.style.background = isMipmapsActive ? '#0284c7' : '#334155';
    if (wasmExports && wasmExports.bgl_toggle_mipmaps) {
      wasmExports.bgl_toggle_mipmaps();
    }
  }
  window.toggleMipmaps = toggleMipmaps;

  /**
   * Toggle Directional Shadow Mapping (PCF).
   */
  function toggleShadows() {
    isShadowsActive = !isShadowsActive;
    if (btnShadows) btnShadows.style.background = isShadowsActive ? '#0284c7' : '#334155';
    if (wasmExports && wasmExports.bgl_toggle_shadows) {
      wasmExports.bgl_toggle_shadows();
    }
  }
  window.toggleShadows = toggleShadows;

  /**
   * Toggle Environment Cubemap Reflections (IBL).
   */
  function toggleReflections() {
    isReflectionsActive = !isReflectionsActive;
    if (btnReflections) btnReflections.style.background = isReflectionsActive ? '#0284c7' : '#334155';
    if (wasmExports && wasmExports.bgl_toggle_reflections) {
      wasmExports.bgl_toggle_reflections();
    }
  }
  window.toggleReflections = toggleReflections;

  let isAOActive = true;
  const btnAO = document.getElementById('btn-ao');

  /**
   * Toggle Ambient Occlusion (AO).
   */
  function toggleAO() {
    isAOActive = !isAOActive;
    if (btnAO) btnAO.style.background = isAOActive ? '#0284c7' : '#334155';
    if (wasmExports && wasmExports.bgl_toggle_ao) {
      wasmExports.bgl_toggle_ao();
    }
  }
  window.toggleAO = toggleAO;

  /**
   * Toggle Scene Graph Tree Inspector Panel.
   */
  function toggleTreePanel() {
    if (!treePanel || !treePill) return;
    if (treePanel.style.display === 'none') {
      treePanel.style.display = 'flex';
      treePill.style.display = 'none';
    } else {
      treePanel.style.display = 'none';
      treePill.style.display = 'flex';
    }
  }
  window.toggleTreePanel = toggleTreePanel;

  /**
   * Switch between Scene Graph Tree Diagram and JSON Nodes.
   */
  function switchTab(tab) {
    const tabTree = document.getElementById('tab-btn-tree');
    const tabJson = document.getElementById('tab-btn-json');
    const viewTree = document.getElementById('tree-container');
    const viewJson = document.getElementById('json-container');
    if (!tabTree || !tabJson || !viewTree || !viewJson) return;

    if (tab === 'tree') {
      tabTree.classList.add('active');
      tabJson.classList.remove('active');
      viewTree.style.display = 'block';
      viewJson.style.display = 'none';
    } else {
      tabJson.classList.add('active');
      tabTree.classList.remove('active');
      viewJson.style.display = 'block';
      viewTree.style.display = 'none';
    }
  }
  window.switchTab = switchTab;

  /**
   * Calculate and update AABB telemetry metrics for the active glTF asset.
   */
  function updateSceneAabbTelemetry(gltf) {
    if (!gltf || !gltf.accessors) return;
    let minB = [Infinity, Infinity, Infinity];
    let maxB = [-Infinity, -Infinity, -Infinity];
    let totalPrims = 0;
    let totalVerts = 0;

    if (gltf.meshes) {
      gltf.meshes.forEach(m => {
        if (m.primitives) {
          totalPrims += m.primitives.length;
          m.primitives.forEach(p => {
            if (p.attributes && p.attributes.POSITION !== undefined) {
              const acc = gltf.accessors[p.attributes.POSITION];
              if (acc) {
                totalVerts += acc.count || 0;
                if (acc.min && acc.max) {
                  for (let i = 0; i < 3; i++) {
                    minB[i] = Math.min(minB[i], acc.min[i]);
                    maxB[i] = Math.max(maxB[i], acc.max[i]);
                  }
                }
              }
            }
          });
        }
      });
    }

    if (minB[0] <= maxB[0]) {
      const dx = maxB[0] - minB[0];
      const dy = maxB[1] - minB[1];
      const dz = maxB[2] - minB[2];
      const cx = (minB[0] + maxB[0]) * 0.5;
      const cy = (minB[1] + maxB[1]) * 0.5;
      const cz = (minB[2] + maxB[2]) * 0.5;

      if (elDims) elDims.textContent = `${dx.toFixed(1)} × ${dy.toFixed(1)} × ${dz.toFixed(1)}`;
      if (elMin) elMin.textContent = `[${minB.map(n => n.toFixed(1)).join(', ')}]`;
      if (elMax) elMax.textContent = `[${maxB.map(n => n.toFixed(1)).join(', ')}]`;
      if (elCenter) elCenter.textContent = `[${cx.toFixed(1)}, ${cy.toFixed(1)}, ${cz.toFixed(1)}]`;
      if (elPrims) elPrims.textContent = `${totalPrims} Prims (${totalVerts.toLocaleString()} Verts)`;
    }
  }

  /**
   * Extract JSON text from GLB binary chunk or plain JSON glTF.
   */
  function extractGltfJson(arrayBuffer) {
    const dataView = new DataView(arrayBuffer);
    const magic = dataView.getUint32(0, true);
    if (magic === 0x46546C67) { // "glTF"
      const chunkLength = dataView.getUint32(12, true);
      const chunkType = dataView.getUint32(16, true);
      if (chunkType === 0x4E4F534A) { // "JSON"
        const jsonBytes = new Uint8Array(arrayBuffer, 20, chunkLength);
        const jsonStr = new TextDecoder().decode(jsonBytes);
        return JSON.parse(jsonStr);
      }
    }
    const jsonStr = new TextDecoder().decode(new Uint8Array(arrayBuffer));
    return JSON.parse(jsonStr);
  }

  /**
   * Ingest a model buffer into both WebAssembly runtime and the UI inspector.
   */
  function ingestModel(name, arrayBuffer) {
    if (wasmExports) {
      const uint8 = new Uint8Array(arrayBuffer);
      const bufPtr = wasmExports.malloc(uint8.length);
      if (bufPtr) {
        const heap = new Uint8Array(wasmMemory.buffer);
        heap.set(uint8, bufPtr);
        const nameBytes = new TextEncoder().encode(name + '\0');
        const namePtr = wasmExports.malloc(nameBytes.length);
        heap.set(nameBytes, namePtr);
        wasmExports.bgl_load_file_buffer(namePtr, bufPtr, uint8.length);
        wasmExports.free(namePtr);
        wasmExports.free(bufPtr);
        console.log(`[Wasm] Ingested ${name} (${uint8.length} bytes)`);
      }
    }

    try {
      const gltf = extractGltfJson(arrayBuffer);
      currentGltf = gltf;
      currentModelName = name;
      renderTreeAndJson(gltf, name);
    } catch (err) {
      console.warn("Could not parse glTF JSON for inspector:", err);
    }
  }

  /**
   * Populate Tree Diagram and JSON views with parsed glTF.
   */
  function renderTreeAndJson(gltf, modelName) {
    if (!gltf) return;
    const modelLabel = document.getElementById('model-name-label');
    const countBadge = document.getElementById('node-count-badge');
    if (modelLabel) modelLabel.textContent = modelName;
    const nodes = gltf.nodes || [];
    if (countBadge) countBadge.textContent = `${nodes.length} Nodes`;

    renderTreeDiagram(gltf);
    renderJsonInspector(gltf);
    updateSceneAabbTelemetry(gltf);
  }

  /**
   * Render ASCII/Visual Tree Diagram with expand/collapse carats and node icons.
   */
  function renderTreeDiagram(gltf) {
    const treeRoot = document.getElementById('tree-view-root');
    if (!treeRoot) return;
    treeRoot.innerHTML = '';
    const nodes = gltf.nodes || [];

    const isChild = new Array(nodes.length).fill(false);
    nodes.forEach(n => {
      if (n.children) {
        n.children.forEach(cIdx => { isChild[cIdx] = true; });
      }
    });

    let rootIndices = [];
    const activeSceneIdx = gltf.scene || 0;
    if (gltf.scenes && gltf.scenes[activeSceneIdx] && gltf.scenes[activeSceneIdx].nodes) {
      rootIndices = gltf.scenes[activeSceneIdx].nodes;
    } else {
      nodes.forEach((_, idx) => { if (!isChild[idx]) rootIndices.push(idx); });
    }

    const container = document.createElement('div');

    function buildNodeElement(nodeIdx) {
      const node = nodes[nodeIdx];
      if (!node) return null;

      const wrapper = document.createElement('div');
      wrapper.className = 'tree-node-wrapper';
      wrapper.dataset.name = (node.name || `Node_${nodeIdx}`).toLowerCase();

      const row = document.createElement('div');
      row.className = 'tree-node-row';
      row.dataset.nodeIndex = nodeIdx;

      const hasChildren = node.children && node.children.length > 0;
      const caret = document.createElement('span');
      caret.className = 'tree-toggle-caret' + (hasChildren ? ' open' : '');
      caret.textContent = hasChildren ? '▶' : '•';
      row.appendChild(caret);

      const icon = document.createElement('span');
      icon.className = 'tree-node-icon';
      if (node.mesh !== undefined) {
        icon.textContent = '◈';
        icon.title = 'Mesh Geometry';
      } else if (node.camera !== undefined) {
        icon.textContent = '◬';
        icon.title = 'Camera Node';
      } else if (hasChildren) {
        icon.textContent = '◆';
        icon.title = 'Group Node';
      } else {
        icon.textContent = '⊹';
        icon.title = 'Transform Node';
      }
      row.appendChild(icon);

      const label = document.createElement('span');
      label.className = 'tree-node-name';
      label.textContent = node.name || `Node_${nodeIdx}`;
      row.appendChild(label);

      const badges = document.createElement('div');
      badges.className = 'tree-node-badges';
      if (node.mesh !== undefined) {
        const meshBadge = document.createElement('span');
        meshBadge.className = 'badge badge-mesh';
        const meshObj = (gltf.meshes && gltf.meshes[node.mesh]) ? gltf.meshes[node.mesh] : null;
        let primCount = meshObj && meshObj.primitives ? meshObj.primitives.length : 1;
        meshBadge.textContent = `Mesh #${node.mesh} (${primCount}p)`;
        badges.appendChild(meshBadge);

        if (meshObj && meshObj.primitives && meshObj.primitives[0] && meshObj.primitives[0].material !== undefined) {
          const matIdx = meshObj.primitives[0].material;
          const matObj = (gltf.materials && gltf.materials[matIdx]) ? gltf.materials[matIdx] : null;
          const matBadge = document.createElement('span');
          matBadge.className = 'badge badge-mat';
          matBadge.textContent = matObj && matObj.name ? matObj.name : `Mat #${matIdx}`;
          badges.appendChild(matBadge);
        }
      }
      row.appendChild(badges);

      row.addEventListener('click', (e) => {
        e.stopPropagation();
        document.querySelectorAll('.tree-node-row').forEach(r => r.classList.remove('selected'));
        row.classList.add('selected');
        showNodeDetails(nodeIdx, node, gltf);
      });

      wrapper.appendChild(row);

      if (hasChildren) {
        const childrenContainer = document.createElement('div');
        childrenContainer.className = 'tree-children';
        node.children.forEach(cIdx => {
          const childEl = buildNodeElement(cIdx);
          if (childEl) childrenContainer.appendChild(childEl);
        });
        wrapper.appendChild(childrenContainer);

        caret.addEventListener('click', (e) => {
          e.stopPropagation();
          if (childrenContainer.style.display === 'none') {
            childrenContainer.style.display = 'block';
            caret.classList.add('open');
          } else {
            childrenContainer.style.display = 'none';
            caret.classList.remove('open');
          }
        });
      }

      return wrapper;
    }

    rootIndices.forEach(rIdx => {
      const el = buildNodeElement(rIdx);
      if (el) container.appendChild(el);
    });

    treeRoot.appendChild(container);
  }

  /**
   * Display selected node telemetry in footer.
   */
  function showNodeDetails(nodeIdx, node, gltf) {
    const footer = document.getElementById('node-details-footer');
    const title = document.getElementById('footer-node-title');
    const props = document.getElementById('footer-node-props');
    if (!footer || !title || !props) return;

    footer.style.display = 'block';
    title.textContent = `[#${nodeIdx}] ${node.name || 'Unnamed Node'}`;
    props.innerHTML = '';

    function addRow(k, v) {
      const row = document.createElement('div');
      row.className = 'footer-kv';
      const kSpan = document.createElement('span');
      kSpan.textContent = `${k}:`;
      const vSpan = document.createElement('span');
      vSpan.textContent = String(v);
      row.appendChild(kSpan);
      row.appendChild(vSpan);
      props.appendChild(row);
    }

    addRow('Node Index', nodeIdx);
    if (node.mesh !== undefined) {
      const mesh = gltf.meshes ? gltf.meshes[node.mesh] : null;
      addRow('Mesh Index', `${node.mesh} (${mesh ? mesh.name || 'Unnamed' : ''})`);
      if (mesh && mesh.primitives) {
        addRow('Primitives Count', mesh.primitives.length);
        let meshMin = [Infinity, Infinity, Infinity];
        let meshMax = [-Infinity, -Infinity, -Infinity];
        mesh.primitives.forEach((p, pi) => {
          if (p.attributes && p.attributes.POSITION !== undefined) {
            const acc = gltf.accessors ? gltf.accessors[p.attributes.POSITION] : null;
            if (acc) {
              addRow(`Prim #${pi} Vertices`, acc.count.toLocaleString());
              if (acc.min && acc.max) {
                for (let i = 0; i < 3; i++) {
                  meshMin[i] = Math.min(meshMin[i], acc.min[i]);
                  meshMax[i] = Math.max(meshMax[i], acc.max[i]);
                }
                addRow(`Prim #${pi} AABB Min`, `[${acc.min.map(n => n.toFixed(1)).join(', ')}]`);
                addRow(`Prim #${pi} AABB Max`, `[${acc.max.map(n => n.toFixed(1)).join(', ')}]`);
              }
            }
          }
        });
        if (meshMin[0] <= meshMax[0]) {
          const extents = [meshMax[0] - meshMin[0], meshMax[1] - meshMin[1], meshMax[2] - meshMin[2]];
          addRow('Combined AABB Dims', `${extents[0].toFixed(1)} × ${extents[1].toFixed(1)} × ${extents[2].toFixed(1)}`);
        }
      }
    }

    if (node.scale) addRow('Scale', `[${node.scale.map(n => n.toFixed(4)).join(', ')}]`);
    if (node.translation) addRow('Translation', `[${node.translation.map(n => n.toFixed(4)).join(', ')}]`);
    if (node.rotation) addRow('Rotation (quat)', `[${node.rotation.map(n => n.toFixed(3)).join(', ')}]`);
    if (node.children) addRow('Children Count', node.children.length);
  }

  /**
   * Search node filtering.
   */
  function filterNodes(query) {
    const q = query.trim().toLowerCase();
    const escapedQ = (typeof CSS !== 'undefined' && CSS.escape) ? CSS.escape(q) : q.replace(/["\\]/g, '\\$&');
    const wrappers = document.querySelectorAll('.tree-node-wrapper');
    wrappers.forEach(w => {
      if (!q || w.dataset.name.includes(q)) {
        w.style.display = '';
      } else {
        const hasMatchingChild = w.querySelector(`.tree-node-wrapper[data-name*="${escapedQ}"]`);
        w.style.display = hasMatchingChild ? '' : 'none';
      }
    });
  }
  window.filterNodes = filterNodes;

  /**
   * Interactive Collapsible JSON Inspector.
   */
  function renderJsonInspector(gltf) {
    const jsonRoot = document.getElementById('json-view-root');
    if (!jsonRoot) return;
    jsonRoot.innerHTML = '';

    const nodeData = {
      asset: gltf.asset,
      scene: gltf.scene,
      scenes: gltf.scenes,
      nodes: gltf.nodes,
      meshes: gltf.meshes,
      materials: gltf.materials,
      textures: gltf.textures,
      images: gltf.images ? `${gltf.images.length} images` : undefined,
      accessors: gltf.accessors ? `${gltf.accessors.length} accessors` : undefined,
      bufferViews: gltf.bufferViews ? `${gltf.bufferViews.length} bufferViews` : undefined
    };

    jsonRoot.appendChild(createJsonElement(nodeData, true));
  }

  function createJsonElement(val, isRoot = false) {
    if (val === null) {
      const span = document.createElement('span');
      span.className = 'json-null';
      span.textContent = 'null';
      return span;
    }
    if (typeof val === 'string') {
      const span = document.createElement('span');
      span.className = 'json-str';
      span.textContent = `"${val}"`;
      return span;
    }
    if (typeof val === 'number') {
      const span = document.createElement('span');
      span.className = 'json-num';
      span.textContent = val;
      return span;
    }
    if (typeof val === 'boolean') {
      const span = document.createElement('span');
      span.className = 'json-bool';
      span.textContent = val;
      return span;
    }

    if (Array.isArray(val)) {
      const container = document.createElement('div');
      container.className = 'json-block';

      const toggle = document.createElement('span');
      toggle.className = 'json-toggle';
      toggle.textContent = isRoot ? '▼' : '▶';

      const openBracket = document.createTextNode('[');
      const closeBracket = document.createTextNode(']');

      const content = document.createElement('div');
      content.style.marginLeft = '16px';
      content.style.display = isRoot ? 'block' : 'none';

      val.forEach((item, idx) => {
        const line = document.createElement('div');
        line.appendChild(createJsonElement(item));
        if (idx < val.length - 1) line.appendChild(document.createTextNode(','));
        content.appendChild(line);
      });

      const preview = document.createElement('span');
      preview.className = 'json-collapsed-preview';
      preview.style.display = isRoot ? 'none' : 'inline';
      preview.textContent = ` Array(${val.length}) `;

      toggle.onclick = () => {
        const isHidden = content.style.display === 'none';
        content.style.display = isHidden ? 'block' : 'none';
        preview.style.display = isHidden ? 'none' : 'inline';
        toggle.textContent = isHidden ? '▼' : '▶';
      };

      container.appendChild(toggle);
      container.appendChild(openBracket);
      container.appendChild(preview);
      container.appendChild(content);
      container.appendChild(closeBracket);
      return container;
    }

    if (typeof val === 'object') {
      const container = document.createElement('div');
      container.className = 'json-block';

      const toggle = document.createElement('span');
      toggle.className = 'json-toggle';
      toggle.textContent = isRoot ? '▼' : '▶';

      const openBrace = document.createTextNode('{');
      const closeBrace = document.createTextNode('}');

      const content = document.createElement('div');
      content.style.marginLeft = '16px';
      content.style.display = isRoot ? 'block' : 'none';

      const keys = Object.keys(val);
      keys.forEach((key, i) => {
        const line = document.createElement('div');
        const keySpan = document.createElement('span');
        keySpan.className = 'json-key';
        keySpan.textContent = `"${key}": `;
        line.appendChild(keySpan);
        line.appendChild(createJsonElement(val[key]));
        if (i < keys.length - 1) line.appendChild(document.createTextNode(','));
        content.appendChild(line);
      });

      const preview = document.createElement('span');
      preview.className = 'json-collapsed-preview';
      preview.style.display = isRoot ? 'none' : 'inline';
      preview.textContent = ` Object(${keys.length}) `;

      toggle.onclick = () => {
        const isHidden = content.style.display === 'none';
        content.style.display = isHidden ? 'block' : 'none';
        preview.style.display = isHidden ? 'none' : 'inline';
        toggle.textContent = isHidden ? '▼' : '▶';
      };

      container.appendChild(toggle);
      container.appendChild(openBrace);
      container.appendChild(preview);
      container.appendChild(content);
      container.appendChild(closeBrace);
      return container;
    }

    return document.createTextNode(String(val));
  }

  // Keyboard Navigation
  window.addEventListener('keydown', (e) => {
    if (e.ctrlKey || e.metaKey || e.altKey) return;
    if (document.activeElement && (document.activeElement.tagName === 'INPUT' || document.activeElement.tagName === 'TEXTAREA')) return;
    if (e.key === 'b' || e.key === 'B') {
      toggleBBox();
    } else if (e.key === 'q' || e.key === 'Q') {
      toggleQuadtree();
    } else if (e.key === 'k' || e.key === 'K') {
      toggleSkybox();
    } else if (e.key === 'o' || e.key === 'O' || e.key === 'F3') {
      toggleHUD();
    } else if (e.key === 'n' || e.key === 'N') {
      toggleTreePanel();
    } else if (e.key === 'v' || e.key === 'V') {
      toggleBump();
    } else if (e.key === 'l' || e.key === 'L') {
      toggleMipmaps();
    } else if (e.key === 'm' || e.key === 'M') {
      toggleShadows();
    } else if (e.key === 'r' || e.key === 'R') {
      toggleReflections();
    } else if (e.key === 'u' || e.key === 'U') {
      toggleAO();
    } else if (e.key === 'Escape') {
      dismissCompatWarning();
    }
  });

  // Drag and Drop Listeners
  if (dropZone) {
    window.addEventListener('dragover', (e) => {
      e.preventDefault();
      dropZone.classList.add('active');
      if (dropText) dropText.style.display = 'block';
    });

    window.addEventListener('dragleave', (e) => {
      if (e.relatedTarget === null) {
        dropZone.classList.remove('active');
        if (dropText) dropText.style.display = 'none';
      }
    });

    window.addEventListener('drop', async (e) => {
      e.preventDefault();
      dropZone.classList.remove('active');
      if (dropText) dropText.style.display = 'none';

      if (e.dataTransfer && e.dataTransfer.files.length > 0) {
        const file = e.dataTransfer.files[0];
        const reader = new FileReader();
        reader.onload = (evt) => {
          ingestModel(file.name, evt.target.result);
        };
        reader.readAsArrayBuffer(file);
      } else if (e.dataTransfer) {
        const rawUri = (e.dataTransfer.getData('text/uri-list') || e.dataTransfer.getData('text/plain') || '').trim();
        const urlLine = rawUri.split(/\r?\n/).find(line => line.trim() && !line.trim().startsWith('#'));
        if (urlLine && (/^https?:\/\//i.test(urlLine.trim()) || /^data:/i.test(urlLine.trim()))) {
          await loadModelFromUrl(urlLine.trim());
        }
      }
    });
  }

  /**
   * Fetch and ingest a remote 3D model (.glb / .gltf) from an HTTP/HTTPS or data: URL.
   */
  async function loadModelFromUrl(url) {
    try {
      updateLoadProgress(20, `Downloading model from ${url}...`);
      const response = await fetch(url);
      if (!response.ok) {
        throw new Error(`HTTP ${response.status} (${response.statusText})`);
      }
      const arrayBuffer = await response.arrayBuffer();
      const cleanName = url.split(/[?#]/)[0].split('/').pop() || url;
      ingestModel(cleanName, arrayBuffer);
      updateLoadProgress(100, `Loaded ${cleanName} from URL.`);
    } catch (err) {
      console.error(`Failed to load model from URL '${url}':`, err);
      updateLoadProgress(100, `Failed to load URL: ${err.message}`);
    }
  }
  window.loadModelFromUrl = loadModelFromUrl;

  /**
   * Auto-fetch default model on startup (supports ?model=<url> or ?asset=<url>, else Sponza.glb / ToyCar.glb).
   */
  async function loadDefaultModel() {
    const params = new URLSearchParams(window.location.search);
    const customUrl = params.get('model') || params.get('asset') || params.get('url');
    if (customUrl) {
      await loadModelFromUrl(customUrl);
      return;
    }
    try {
      let response = await fetch("assets/Sponza.glb");
      let name = "Sponza.glb";
      if (!response.ok) {
        response = await fetch("assets/ToyCar.glb");
        name = "ToyCar.glb";
      }
      if (response.ok) {
        const arrayBuffer = await response.arrayBuffer();
        ingestModel(name, arrayBuffer);
      }
    } catch (e) {
      console.warn("Could not auto-fetch model for tree inspector:", e);
    }
  }

  /**
   * Dismiss the compatibility warning modal.
   */
  function dismissCompatWarning() {
    const modal = document.getElementById('compat-warning-modal');
    if (modal) modal.style.display = 'none';
  }
  window.dismissCompatWarning = dismissCompatWarning;

  /**
   * Display modal and HUD notification for missing WebAssembly or WebGPU capabilities.
   */
  function showCompatibilityWarning(issues, warnings) {
    const modal = document.getElementById('compat-warning-modal');
    const content = document.getElementById('compat-modal-content');
    if (content) {
      content.innerHTML = '';

      // Critical issues (Wasm / WebGPU missing)
      for (const item of issues) {
        const box = document.createElement('div');
        box.className = 'compat-issue-box';

        const title = document.createElement('div');
        title.className = 'compat-issue-title';
        title.innerHTML = `<span>✕</span> <strong>${item.feature} Not Available</strong>`;

        const desc = document.createElement('div');
        desc.className = 'compat-issue-desc';
        desc.textContent = item.detail;

        box.appendChild(title);
        box.appendChild(desc);
        content.appendChild(box);
      }

      // Warnings (e.g. Insecure context)
      for (const item of warnings) {
        const box = document.createElement('div');
        box.className = 'compat-issue-box warning';

        const title = document.createElement('div');
        title.className = 'compat-issue-title';
        title.innerHTML = `<span>⚠️</span> <strong>${item.feature}</strong>`;

        const desc = document.createElement('div');
        desc.className = 'compat-issue-desc';
        desc.textContent = item.detail;

        box.appendChild(title);
        box.appendChild(desc);
        content.appendChild(box);
      }

      // Troubleshooting recommendations
      const rec = document.createElement('div');
      rec.className = 'compat-recommendations';
      rec.innerHTML = `
        <h5>Troubleshooting &amp; Browser Requirements</h5>
        <ul>
          <li><strong>Supported Browsers:</strong> Google Chrome (113+), Microsoft Edge (113+), Firefox (120+ with <code>dom.webgpu.enabled</code> in <code>about:config</code>), Safari (18+).</li>
          <li><strong>Secure Context:</strong> WebGPU requires HTTPS or <code>http://localhost</code>. Non-secure HTTP origins block WebGPU APIs by browser security policy.</li>
          <li><strong>Hardware Acceleration:</strong> Ensure hardware graphics acceleration is enabled in browser preferences (<code>Settings &rarr; System &rarr; Use graphics acceleration when available</code>).</li>
          <li><strong>GPU Drivers:</strong> Ensure modern Vulkan, Metal, or Direct3D 12 drivers are installed for your graphics hardware.</li>
        </ul>
      `;
      content.appendChild(rec);
    }

    if (modal) {
      modal.style.display = 'flex';
    }

    if (sText) {
      const missingList = issues.map(i => i.feature).join(', ') || 'Browser Compatibility Warning';
      sText.innerHTML = `<span style="color: #fb7185; font-weight: 600;">⚠️ Warning: ${missingList} unavailable</span>`;
    }
    console.warn("[bgl-Engine] Browser Compatibility Warning:", { issues, warnings });
  }

  /**
   * Verify browser support for WebAssembly and WebGPU.
   */
  async function checkBrowserCompatibility() {
    const issues = [];
    const warnings = [];

    // 1. WebAssembly Support Check
    const hasWasm = (typeof WebAssembly === 'object' &&
                     typeof WebAssembly.instantiate === 'function' &&
                     typeof WebAssembly.Memory === 'function');
    if (!hasWasm) {
      issues.push({
        feature: 'WebAssembly',
        detail: 'This browser environment does not support WebAssembly or execution has been disabled. The native C++ engine cannot run without WebAssembly support.'
      });
    }

    // 2. Secure Context Check (WebGPU prerequisite)
    if (window.isSecureContext === false) {
      warnings.push({
        feature: 'Insecure Context (HTTP)',
        detail: 'The application is served over insecure HTTP. WebGPU requires a Secure Context (HTTPS or http://localhost) according to the W3C specification.'
      });
    }

    // 3. WebGPU Support Check
    const hasGpuApi = Boolean(navigator && ('gpu' in navigator) && navigator.gpu);
    if (!hasGpuApi) {
      issues.push({
        feature: 'WebGPU',
        detail: 'The WebGPU API (navigator.gpu) is not available or disabled in this browser. Please use an updated WebGPU-compatible browser such as Chrome 113+, Edge 113+, or Firefox Nightly.'
      });
    } else {
      // 4. WebGPU Adapter Acquisition Check
      try {
        const adapter = await navigator.gpu.requestAdapter();
        if (!adapter) {
          issues.push({
            feature: 'WebGPU Graphics Adapter',
            detail: 'WebGPU API is present, but no compatible hardware GPU adapter could be acquired. Verify browser hardware acceleration settings and GPU driver installation.'
          });
        }
      } catch (e) {
        issues.push({
          feature: 'WebGPU Adapter Error',
          detail: `An error occurred while requesting the WebGPU adapter: ${e && e.message ? e.message : e}`
        });
      }
    }

    if (issues.length > 0 || warnings.length > 0) {
      showCompatibilityWarning(issues, warnings);
      return false;
    }
    return true;
  }

  /**
   * Instantiate WebAssembly Runtime.
   */
  async function initWasm() {
    const isCompat = await checkBrowserCompatibility();
    if (typeof WebAssembly === 'undefined' || !WebAssembly.instantiate) {
      updateLoadProgress(100, "WebAssembly nicht unterstützt.");
      return;
    }

    updateLoadProgress(30, "Fetching WebAssembly runtime module...");
    try {
      const memory = new WebAssembly.Memory({ initial: 256, maximum: 1024 });
      wasmMemory = memory;
      const response = await fetch("dist/bgl_engine.wasm");
      const bytes = await response.arrayBuffer();
      updateLoadProgress(70, "Compiling WebAssembly module...");
      const result = await WebAssembly.instantiate(bytes, {
        env: {
          memory: memory
        }
      });
      wasmExports = result.instance.exports;
      if (wasmExports && wasmExports.bgl_init) {
        wasmExports.bgl_init(window.innerWidth, window.innerHeight);
      }
      updateLoadProgress(100, "WebAssembly engine initialized and ready.");
      console.log("[Wasm] bgl-engine initialized successfully");
    } catch (err) {
      console.error("Wasm initialization error:", err);
      updateLoadProgress(100, "WebAssembly runtime loaded.");
    }
  }

  // Startup
  initWasm();
  loadDefaultModel();
})();

