/**
 * @file js/gltf.js
 * @brief glTF 2.0 / GLB binary parser, scene graph tree builder, and telemetry.
 */
(() => {
  'use strict';

  // ============================================================================
  // glTF 2.0 / GLB Binary Parser & Hierarchy Inspector
  // ============================================================================
  const BglGltf = {
    /**
     * Safely extract JSON text and scene descriptor from a GLB binary container or plain text glTF.
     * Validates GLB headers, chunk boundaries, and handles truncated/corrupted input gracefully.
     * @param {ArrayBuffer} arrayBuffer - Raw glTF/GLB file buffer
     * @returns {Object} Parsed glTF JSON object
     * @throws {Error} If the buffer is empty, truncated, or invalid glTF/GLB format
     */
    extractJson(arrayBuffer) {
      if (!arrayBuffer || arrayBuffer.byteLength < 4) {
        throw new Error('Buffer is empty or too short to be a valid 3D model.');
      }

      const dataView = new DataView(arrayBuffer);
      const magic = dataView.getUint32(0, true);

      // Binary GLB check: magic 0x46546C67 ("glTF")
      if (magic === 0x46546C67) {
        if (arrayBuffer.byteLength < 20) {
          throw new Error(`Truncated GLB container: Header is only ${arrayBuffer.byteLength} bytes (minimum 20 required).`);
        }
        const version = dataView.getUint32(4, true);
        const totalLength = dataView.getUint32(8, true);
        BglLogger.log('glTF', `GLB container detected: Version ${version}, Total Length: ${totalLength.toLocaleString()} bytes`);

        const chunkLength = dataView.getUint32(12, true);
        const chunkType = dataView.getUint32(16, true);

        // Chunk 0 must be JSON (0x4E4F534A -> "JSON")
        if (chunkType !== 0x4E4F534A) {
          throw new Error(`Invalid GLB: First chunk type 0x${chunkType.toString(16)} is not JSON (0x4E4F534A).`);
        }

        if (20 + chunkLength > arrayBuffer.byteLength) {
          throw new Error(`Truncated GLB chunk: JSON chunk claims ${chunkLength} bytes, but file has only ${arrayBuffer.byteLength - 20} payload bytes remaining.`);
        }

        const jsonBytes = new Uint8Array(arrayBuffer, 20, chunkLength);
        const jsonStr = new TextDecoder('utf-8', { fatal: false }).decode(jsonBytes);
        return JSON.parse(jsonStr);
      }

      // Plain JSON glTF text
      const jsonStr = new TextDecoder('utf-8', { fatal: false }).decode(new Uint8Array(arrayBuffer));
      return JSON.parse(jsonStr);
    },

    _worker: null,
    _workerReqId: 0,
    _workerPending: null,

    /**
     * Asynchronously parse glTF/GLB JSON, compute AABB telemetry, and decode embedded data-URI buffers
     * inside a background Web Worker when supported, falling back to synchronous parsing in Node/workerless hosts.
     * @param {ArrayBuffer} arrayBuffer - Raw glTF/GLB file buffer (never detached)
     * @returns {Promise<Object>} Parsed glTF JSON object (with optional non-enumerable _bglEmbeddedBuffers/_bglTelemetry)
     */
    async extractJsonAsync(arrayBuffer) {
      if (typeof Worker === 'undefined') {
        return this.extractJson(arrayBuffer);
      }
      if (!arrayBuffer || arrayBuffer.byteLength < 4) {
        return this.extractJson(arrayBuffer);
      }

      try {
        if (!this._worker) {
          this._worker = new Worker('gltf_worker.js');
          this._workerPending = new Map();
          this._worker.onmessage = (evt) => {
            const msg = evt.data;
            if (!msg || typeof msg.id !== 'number') return;
            const entry = this._workerPending.get(msg.id);
            if (!entry) return;
            this._workerPending.delete(msg.id);
            if (msg.ok) {
              if (Array.isArray(msg.embeddedBuffers) && msg.embeddedBuffers.length > 0) {
                Object.defineProperty(msg.gltf, '_bglEmbeddedBuffers', {
                  value: msg.embeddedBuffers,
                  enumerable: false,
                  configurable: true
                });
              }
              if (msg.telemetry) {
                Object.defineProperty(msg.gltf, '_bglTelemetry', {
                  value: msg.telemetry,
                  enumerable: false,
                  configurable: true
                });
              }
              entry.resolve(msg.gltf);
            } else {
              entry.reject(new Error(msg.error || 'glTF worker parse failure'));
            }
          };
          this._worker.onerror = (err) => {
            const pending = this._workerPending ? Array.from(this._workerPending.values()) : [];
            if (this._workerPending) this._workerPending.clear();
            this._worker = null;
            for (const entry of pending) {
              entry.reject(new Error(err && err.message ? err.message : 'Worker crashed'));
            }
          };
        }

        const dv = new DataView(arrayBuffer);
        const magic = dv.getUint32(0, true);
        let payloadBuffer;
        let cmd = 'parseBuffer';
        if (magic === 0x46546C67 && arrayBuffer.byteLength >= 20) {
          const chunkLength = dv.getUint32(12, true);
          const chunkType = dv.getUint32(16, true);
          if (chunkType === 0x4E4F534A && 20 + chunkLength <= arrayBuffer.byteLength) {
            // Transfer only the JSON chunk slice so the full GLB buffer stays intact for WASM & WebGPU
            payloadBuffer = arrayBuffer.slice(20, 20 + chunkLength);
            cmd = 'parseJsonChunkOnly';
          } else {
            return this.extractJson(arrayBuffer);
          }
        } else {
          payloadBuffer = arrayBuffer.slice(0);
        }

        const reqId = ++this._workerReqId;
        return await new Promise((resolve, reject) => {
          this._workerPending.set(reqId, { resolve, reject });
          this._worker.postMessage({ id: reqId, cmd, buffer: payloadBuffer }, [payloadBuffer]);
        });
      } catch (workerErr) {
        BglLogger.warn('glTF', `Web Worker parse fallback to main thread: ${workerErr.message || workerErr}`);
        return this.extractJson(arrayBuffer);
      }
    },

    /**
     * Compute and display spatial AABB telemetry from glTF accessors.
     */
    updateTelemetry(gltf) {
      if (!gltf || !gltf.accessors) return;
      const precomputed = gltf._bglTelemetry;
      let minB = precomputed ? precomputed.minB : [Infinity, Infinity, Infinity];
      let maxB = precomputed ? precomputed.maxB : [-Infinity, -Infinity, -Infinity];
      let totalPrims = precomputed ? precomputed.totalPrims : 0;
      let totalVerts = precomputed ? precomputed.totalVerts : 0;

      if (!precomputed && gltf.meshes) {
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

        if (BglUI.elDims) BglUI.elDims.textContent = `${dx.toFixed(1)} × ${dy.toFixed(1)} × ${dz.toFixed(1)}`;
        if (BglUI.elMin) BglUI.elMin.textContent = `[${minB.map(n => n.toFixed(1)).join(', ')}]`;
        if (BglUI.elMax) BglUI.elMax.textContent = `[${maxB.map(n => n.toFixed(1)).join(', ')}]`;
        if (BglUI.elCenter) BglUI.elCenter.textContent = `[${cx.toFixed(1)}, ${cy.toFixed(1)}, ${cz.toFixed(1)}]`;
        if (BglUI.elPrims) BglUI.elPrims.textContent = `${totalPrims} Prims (${totalVerts.toLocaleString()} Verts)`;

        BglLogger.log('glTF', `Telemetry updated: ${totalPrims} primitives, ${totalVerts.toLocaleString()} vertices, AABB [${dx.toFixed(1)} × ${dy.toFixed(1)} × ${dz.toFixed(1)}]`);
      }
    },

    /**
     * Render the visual scene graph tree diagram.
     */
    renderTreeDiagram(gltf) {
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

      const buildNodeElement = (nodeIdx) => {
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
          const primCount = meshObj && meshObj.primitives ? meshObj.primitives.length : 1;
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
        row.setAttribute('data-node-idx', String(nodeIdx));
        row.appendChild(badges);

        row.addEventListener('click', (e) => {
          e.stopPropagation();
          document.querySelectorAll('.tree-node-row').forEach(r => r.classList.remove('selected'));
          row.classList.add('selected');
          BglState.selectedNodeIndex = nodeIdx;
          if (BglState.wasmExports && BglState.wasmExports.bgl_select_node) {
            BglState.wasmExports.bgl_select_node(nodeIdx);
          }
          this.showNodeDetails(nodeIdx, node, gltf);
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
      };

      rootIndices.forEach(rIdx => {
        const el = buildNodeElement(rIdx);
        if (el) container.appendChild(el);
      });

      treeRoot.appendChild(container);
    },

    /**
     * Show telemetry for selected tree node.
     */
    showNodeDetails(nodeIdx, node, gltf) {
      const footer = document.getElementById('node-details-footer');
      const title = document.getElementById('footer-node-title');
      const props = document.getElementById('footer-node-props');
      if (!footer || !title || !props) return;

      footer.style.display = 'block';
      title.textContent = `[#${nodeIdx}] ${node.name || 'Unnamed Node'}`;
      props.innerHTML = '';

      const addRow = (k, v) => {
        const row = document.createElement('div');
        row.className = 'footer-kv';
        const kSpan = document.createElement('span');
        kSpan.textContent = `${k}:`;
        const vSpan = document.createElement('span');
        vSpan.textContent = String(v);
        row.appendChild(kSpan);
        row.appendChild(vSpan);
        props.appendChild(row);
      };

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
    },

    /**
     * Interactive Collapsible JSON Inspector.
     */
    renderJsonInspector(gltf) {
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

      jsonRoot.appendChild(this.createJsonElement(nodeData, true));
    },

    createJsonElement(val, isRoot = false) {
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
          line.appendChild(this.createJsonElement(item));
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
          line.appendChild(this.createJsonElement(val[key]));
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
  };


  globalThis.BglGltf = BglGltf;
})();
