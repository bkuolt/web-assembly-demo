/**
 * @file app.js
 * @brief bgl-Engine Web Viewer UI Coordinator, Frame Scheduler & Bootstrap
 */
(() => {
  'use strict';

  const {
    BglLogger,
    BglNotifier,
    BglState,
    BglUI,
    BglModelCache,
    BglLoadingScreen,
    BglNetwork,
    BglWasmBridge,
    BglGltf,
    BglWebGpuRenderer,
  } = globalThis;

  // ============================================================================
  // Model Ingestion Coordinator
  // ============================================================================
  function updateCameraDropdown(gltf) {
    const select = document.getElementById('camera-select');
    if (!select) return;
    select.innerHTML = '<option value="-1">Default Interactive Camera</option>';
    if (BglState.wasmExports && BglState.wasmExports.bgl_get_camera_count) {
      const count = BglState.wasmExports.bgl_get_camera_count();
      for (let i = 0; i < count; ++i) {
        const opt = document.createElement('option');
        opt.value = String(i);
        let name = `glTF Camera #${i}`;
        if (gltf && gltf.cameras && gltf.cameras[i] && gltf.cameras[i].name) {
          name = gltf.cameras[i].name;
        }
        opt.textContent = name;
        select.appendChild(opt);
      }
    } else if (gltf && gltf.cameras && gltf.cameras.length > 0) {
      gltf.cameras.forEach((cam, i) => {
        const opt = document.createElement('option');
        opt.value = String(i);
        opt.textContent = cam.name || `glTF Camera #${i}`;
        select.appendChild(opt);
      });
    }
  }

  function updateAnimationDropdown() {
    const select = document.getElementById('animation-select');
    const playBtn = document.getElementById('btn-anim-play');
    const animSection = document.getElementById('animation-controls-group');
    if (!select) return;
    const anims = BglWebGpuRenderer.animations || [];
    select.innerHTML = '';
    if (anims.length === 0) {
      const opt = document.createElement('option');
      opt.value = '-1';
      opt.textContent = 'No Animations';
      select.appendChild(opt);
      select.disabled = true;
      if (playBtn) playBtn.disabled = true;
      if (animSection) animSection.style.opacity = '0.6';
      return;
    }
    select.disabled = false;
    if (playBtn) {
      playBtn.disabled = false;
      playBtn.textContent = BglWebGpuRenderer.animationPlaying ? '⏸ Pause' : '▶ Play';
      playBtn.classList.toggle('active', BglWebGpuRenderer.animationPlaying);
    }
    if (animSection) animSection.style.opacity = '1';
    anims.forEach((anim, idx) => {
      const opt = document.createElement('option');
      opt.value = String(idx);
      opt.textContent = `${anim.name} (${anim.duration.toFixed(2)}s)`;
      if (idx === BglWebGpuRenderer.activeAnimationIndex) opt.selected = true;
      select.appendChild(opt);
    });
  }

  function onAnimationSelected(value) {
    const idx = parseInt(value, 10);
    BglWebGpuRenderer.setAnimation(idx);
    if (BglState.wasmExports && BglState.wasmExports.bgl_set_animation) {
      BglState.wasmExports.bgl_set_animation(idx);
    }
    const playBtn = document.getElementById('btn-anim-play');
    if (playBtn) {
      playBtn.textContent = BglWebGpuRenderer.animationPlaying ? '⏸ Pause' : '▶ Play';
      playBtn.classList.toggle('active', BglWebGpuRenderer.animationPlaying);
    }
  }

  function toggleAnimationPlayback() {
    const next = !BglWebGpuRenderer.animationPlaying;
    BglWebGpuRenderer.setAnimationPlaying(next);
    if (BglState.wasmExports && BglState.wasmExports.bgl_set_animation_playing) {
      BglState.wasmExports.bgl_set_animation_playing(next ? 1 : 0);
    }
    const playBtn = document.getElementById('btn-anim-play');
    if (playBtn) {
      playBtn.textContent = next ? '⏸ Pause' : '▶ Play';
      playBtn.classList.toggle('active', next);
    }
  }

  function onAnimationSpeedSlider(value) {
    const spd = Math.max(0.0, Math.min(3.0, parseFloat(value) || 0.0));
    BglWebGpuRenderer.animationSpeed = spd;
    const label = document.getElementById('val-anim-speed');
    if (label) label.textContent = `${spd.toFixed(2)}x`;
  }

  window.updateAnimationDropdown = updateAnimationDropdown;
  window.onAnimationSelected = onAnimationSelected;
  window.toggleAnimationPlayback = toggleAnimationPlayback;
  window.onAnimationSpeedSlider = onAnimationSpeedSlider;

  const BglFrameScheduler = {
    _started: false,
    _rendererStep: null,
    _navStep: null,
    _gamepadStep: null,
    _fpsStep: null,

    registerRenderer(fn) { this._rendererStep = fn; this.start(); },
    registerNav(fn) { this._navStep = fn; this.start(); },
    registerGamepad(fn) { this._gamepadStep = fn; this.start(); },
    registerFps(fn) { this._fpsStep = fn; this.start(); },

    start() {
      if (this._started || typeof requestAnimationFrame !== 'function') return;
      this._started = true;
      const tick = (now) => {
        if (this._navStep) this._navStep(now);
        if (this._gamepadStep) this._gamepadStep(now);
        if (this._rendererStep) this._rendererStep(now);
        if (this._fpsStep) this._fpsStep(now);
        requestAnimationFrame(tick);
      };
      requestAnimationFrame(tick);
    }
  };

  async function ingestModel(name, arrayBuffer) {
    BglLogger.timeStart('Model-Ingest');
    BglLogger.log('glTF', `Ingesting 3D model '${name}' (${arrayBuffer.byteLength.toLocaleString()} bytes)...`);

    // 1. Pass to WebAssembly C++ engine
    BglWasmBridge.sendBufferToWasm(name, arrayBuffer);

    // 2. Parse glTF (via Web Worker when available) for live inspector & telemetry
    try {
      const gltf = BglGltf.extractJsonAsync ? await BglGltf.extractJsonAsync(arrayBuffer) : BglGltf.extractJson(arrayBuffer);
      BglState.currentGltf = gltf;
      BglState.currentModelBuffer = arrayBuffer;
      BglState.currentModelName = name;

      const modelLabel = BglUI.modelNameLabel || document.getElementById('model-name-label');
      const countBadge = BglUI.nodeCountBadge || document.getElementById('node-count-badge');
      if (modelLabel) modelLabel.textContent = name;
      const nodes = gltf.nodes || [];
      if (countBadge) countBadge.textContent = `${nodes.length} Nodes`;

      BglGltf.renderTreeDiagram(gltf);
      BglGltf.renderJsonInspector(gltf);
      BglGltf.updateTelemetry(gltf);
      BglUI.setActiveModelButton(name);
      updateCameraDropdown(gltf);

      // Forward to active 3D renderer (WebGPU)
      if (BglWebGpuRenderer.isActive) {
        BglWebGpuRenderer.loadModel(gltf, arrayBuffer, name);
      }

      BglLogger.timeEnd('glTF', 'Model-Ingest', `Model '${name}' loaded and inspected`);
      BglNotifier.success(
        'Model Loaded Successfully',
        `'${name}' is active in the engine (${nodes.length} nodes, ${(arrayBuffer.byteLength / (1024 * 1024)).toFixed(2)} MB).`
      );
    } catch (parseErr) {
      BglLogger.warn('glTF', `Could not parse JSON inspector data for '${name}':`, parseErr);
      BglNotifier.warning(
        'Hierarchy Inspector Notice',
        `3D Model buffer loaded into engine, but glTF JSON inspection was unavailable: ${parseErr.message}`
      );
    }
  }

  // ============================================================================
  // Remote Model Fetching & Preset Loading
  // ============================================================================
  async function loadModelFromUrl(url) {
    BglLogger.log('Network', `Requesting model download from URL: ${url}`);
    const cleanName = url.split(/[?#]/)[0].split('/').pop() || 'Model.glb';
    
    // Check if model exists in browser cache
    const cachedBuffer = await BglModelCache.get(url);
    if (cachedBuffer) {
      BglLogger.log('Cache', `Instant load for cached asset: ${cleanName}`);
      BglLoadingScreen.show(cleanName, url);
      BglLoadingScreen.update(100, `Loading from Browser Storage...`, cachedBuffer.byteLength, cachedBuffer.byteLength, 'Instant', true);
      BglUI.setProgress(100, `Instant Cache: ${cleanName}`);
      
      try {
        await ingestModel(cleanName, cachedBuffer);
        BglNotifier.success('Loaded from Cache', `'${cleanName}' was instantly restored from offline browser cache.`);
      } catch (err) {
        BglLogger.error('Engine', `Error ingesting cached asset: ${err.message}`);
        BglNotifier.error('Cache Ingestion Error', `Could not load '${cleanName}' from cache: ${err.message}`);
      } finally {
        BglLoadingScreen.hide();
      }
      return;
    }

    // Remote or local network fetch
    BglLoadingScreen.show(cleanName, url);
    try {
      BglUI.setProgress(10, `Connecting to ${url}...`);
      const signal = BglLoadingScreen.abortController ? BglLoadingScreen.abortController.signal : null;
      
      const arrayBuffer = await BglNetwork.fetchWithProgress(url, (info) => {
        BglUI.setProgress(info.percent, info.msg);
        BglLoadingScreen.update(
          info.percent,
          info.msg,
          info.transferredBytes,
          info.totalBytes,
          info.speedMbS,
          false
        );
      }, signal);

      // Cache the downloaded buffer for future loads
      await BglModelCache.put(url, arrayBuffer);

      BglLoadingScreen.update(100, `Ingesting into WebGPU Engine...`, arrayBuffer.byteLength, arrayBuffer.byteLength, '0.0', false);
      await ingestModel(cleanName, arrayBuffer);
      BglUI.setProgress(100, `Ready: ${cleanName}`);
    } catch (err) {
      if (err.message && err.message.includes('aborted')) {
        BglLogger.warn('Network', 'Download cancelled by user.');
        return;
      }
      BglLogger.error('Network', `Failed to load model from '${url}':`, err);
      BglNotifier.error(
        'Model Download Failed',
        `Could not retrieve 3D model from "${url}". Please verify the URL, CORS permissions, and network connectivity.`,
        err.message
      );
      BglUI.setProgress(100, `Failed to load: ${cleanName}`);
    } finally {
      BglLoadingScreen.hide();
    }
  }
  window.loadModelFromUrl = loadModelFromUrl;

  async function loadPresetModel(fileName) {
    const url = `assets/${fileName}`;
    BglLogger.log('Engine', `User triggered load of preset asset '${fileName}'`);
    await loadModelFromUrl(url);
  }
  window.loadPresetModel = loadPresetModel;

  function onModelSelectChanged(url) {
    if (url) {
      loadModelFromUrl(url);
    }
  }
  window.onModelSelectChanged = onModelSelectChanged;

  function promptCustomModelUrl() {
    const url = prompt('Enter 3D Model URL (glTF 2.0 Binary .glb or JSON .gltf):', 'https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/DamagedHelmet/glTF-Binary/DamagedHelmet.glb');
    if (url && url.trim()) {
      loadModelFromUrl(url.trim());
    }
  }
  window.promptCustomModelUrl = promptCustomModelUrl;

  function triggerFileInput() {
    const input = document.getElementById('manual-file-input');
    if (input) input.click();
  }
  window.triggerFileInput = triggerFileInput;

  function onManualFileSelected(event) {
    if (event.target.files && event.target.files.length > 0) {
      const file = event.target.files[0];
      BglLogger.log('UI', `Manual file selected: ${file.name} (${file.size.toLocaleString()} bytes)`);

      const reader = new FileReader();
      reader.onload = async (evt) => {
        try {
          await ingestModel(file.name, evt.target.result);
          await BglModelCache.put(`local://${file.name}`, evt.target.result);
        } catch (err) {
          BglNotifier.error('File Ingestion Error', `Could not ingest '${file.name}': ${err.message}`);
        }
      };
      reader.onerror = () => {
        BglNotifier.error('File Read Failed', `Failed to read file '${file.name}'.`);
      };
      reader.readAsArrayBuffer(file);
    }
  }
  window.onManualFileSelected = onManualFileSelected;

  function clearModelCache() {
    BglModelCache.clear();
  }
  window.clearModelCache = clearModelCache;

  function cancelModelDownload() {
    BglLoadingScreen.cancel();
  }
  window.cancelModelDownload = cancelModelDownload;

  /**
   * Fast startup model loading with auto-detection.
   */
  async function loadDefaultModel() {
    const params = new URLSearchParams(window.location.search);
    const customUrl = params.get('model') || params.get('asset') || params.get('url');

    if (customUrl) {
      BglLogger.log('Engine', `Detected query parameter model override: ${customUrl}`);
      await loadModelFromUrl(customUrl);
      return;
    }

    // Default: prioritize fast startup with ToyCar.glb (~5.1 MB), with Sponza available via quick button
    try {
      BglLogger.log('Engine', 'Loading default lightweight scene: assets/ToyCar.glb');
      await loadModelFromUrl("assets/ToyCar.glb");
    } catch (toyCarErr) {
      BglLogger.warn('Engine', `ToyCar.glb unavailable (${toyCarErr.message}), attempting assets/Sponza.glb...`);
      try {
        await loadModelFromUrl("assets/Sponza.glb");
      } catch (sponzaErr) {
        BglLogger.warn('Engine', 'No default local asset found in assets/. Waiting for user drag-and-drop.');
        BglUI.setProgress(100, 'Ready: Drop a glTF or GLB file onto the viewport.');
      }
    }
  }

  // ============================================================================
  // Browser Compatibility Verification
  // ============================================================================
  function dismissCompatWarning() {
    const modal = document.getElementById('compat-warning-modal');
    if (modal) modal.style.display = 'none';
  }
  window.dismissCompatWarning = dismissCompatWarning;

  function showCompatibilityWarning(issues, warnings) {
    const modal = document.getElementById('compat-warning-modal');
    const content = document.getElementById('compat-modal-content');
    if (content) {
      content.innerHTML = '';

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

    if (modal) modal.style.display = 'flex';
    BglNotifier.warning(
      'Compatibility Advisory',
      `Some engine capabilities may be limited: ${issues.map(i => i.feature).join(', ') || 'Warnings reported'}`
    );
  }

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
        detail: 'This browser environment does not support WebAssembly. The native C++ engine cannot run without WebAssembly support.'
      });
    }

    // 2. Secure Context Check
    if (window.isSecureContext === false) {
      warnings.push({
        feature: 'Insecure Context (HTTP)',
        detail: 'The application is served over insecure HTTP. WebGPU requires a Secure Context (HTTPS or http://localhost).'
      });
    }

    // 3. WebGPU Support Check & Pipeline Initialization
    const hasGpuApi = Boolean(navigator && ('gpu' in navigator) && navigator.gpu);
    let gpuOperational = false;

    if (hasGpuApi) {
      BglLogger.log('Engine', 'Attempting native WebGPU Cook-Torrance PBR pipeline initialization...');
      try {
        gpuOperational = await BglWebGpuRenderer.init();
      } catch (e) {
        BglLogger.warn('WebGPU', 'Exception during WebGPU initialization:', e.message || e);
        gpuOperational = false;
      }
    }

    if (gpuOperational) {
      BglState.backend = 'webgpu';
      const vendorStr = (window.cachedGpuInfo && window.cachedGpuInfo.vendor) ? ` (${window.cachedGpuInfo.vendor})` : '';
      const sub = document.querySelector('.hud-subtitle');
      if (sub) sub.textContent = `Native WebGPU PBR & Wasm Runtime${vendorStr}`;
      const pill = document.getElementById('hud-backend-pill');
      if (pill) {
        pill.textContent = 'WebGPU PBR';
        pill.className = 'badge badge-webgpu';
        pill.title = 'Hardware-accelerated native WebGPU Cook-Torrance PBR pipeline';
      }
      BglNotifier.success(
        'WebGPU PBR Engine Operational',
        `Native hardware WebGPU Cook-Torrance PBR pipeline active${vendorStr}.`
      );
      return true;
    }

    issues.push({
      feature: 'WebGPU (Web Graphics API)',
      detail: 'This application requires WebGPU for high-performance Cook-Torrance PBR rendering. Please use a WebGPU-enabled browser such as Google Chrome (113+), Microsoft Edge (113+), or Mozilla Firefox (with dom.webgpu.enabled).'
    });

    if (issues.length > 0) {
      showCompatibilityWarning(issues, warnings);
      return false;
    }

    return true;
  }

  function getActiveRenderer() {
    if (BglWebGpuRenderer && BglWebGpuRenderer.isActive) return BglWebGpuRenderer;
    return null;
  }

  // ============================================================================
  // Engine Control Toggles (Exposed to HTML onclick handlers)
  // ============================================================================
  function toggleBBox() {
    BglState.isBboxActive = !BglState.isBboxActive;
    if (BglUI.btnBbox) {
      BglUI.btnBbox.classList.toggle('active', BglState.isBboxActive);
      BglUI.btnBbox.style.background = BglState.isBboxActive ? 'rgba(2, 132, 199, 0.25)' : '#1e293b';
    }
    const activeRenderer = getActiveRenderer();
    if (activeRenderer) {
      activeRenderer.wireframe = BglState.isBboxActive;
    }
    BglLogger.log('Engine', `Toggle Bounding Boxes: ${BglState.isBboxActive ? 'ENABLED' : 'DISABLED'}`);
    if (BglState.wasmExports && BglState.wasmExports.bgl_toggle_bbox) {
      BglState.wasmExports.bgl_toggle_bbox();
    }
  }
  window.toggleBBox = toggleBBox;

  function toggleQuadtree() {
    BglState.isQuadtreeActive = !BglState.isQuadtreeActive;
    if (BglUI.btnQuadtree) {
      BglUI.btnQuadtree.classList.toggle('active', BglState.isQuadtreeActive);
      BglUI.btnQuadtree.style.background = BglState.isQuadtreeActive ? 'rgba(2, 132, 199, 0.25)' : '#1e293b';
    }
    BglLogger.log('Engine', `Toggle Quadtree Bounds: ${BglState.isQuadtreeActive ? 'ENABLED' : 'DISABLED'}`);
    if (BglState.wasmExports && BglState.wasmExports.bgl_toggle_quadtree) {
      BglState.wasmExports.bgl_toggle_quadtree();
    }
  }
  window.toggleQuadtree = toggleQuadtree;

  function toggleSkybox() {
    BglState.isSkyboxActive = !BglState.isSkyboxActive;
    if (BglUI.btnSkybox) {
      BglUI.btnSkybox.classList.toggle('active', BglState.isSkyboxActive);
      BglUI.btnSkybox.style.background = BglState.isSkyboxActive ? 'rgba(2, 132, 199, 0.25)' : '#1e293b';
    }
    BglLogger.log('Engine', `Toggle Skybox: ${BglState.isSkyboxActive ? 'ENABLED' : 'DISABLED'}`);
    if (BglState.wasmExports && BglState.wasmExports.bgl_toggle_skybox) {
      BglState.wasmExports.bgl_toggle_skybox();
    }
  }
  window.toggleSkybox = toggleSkybox;

  function toggleHUD() {
    BglState.isHudActive = !BglState.isHudActive;
    if (BglUI.btnHud) {
      BglUI.btnHud.classList.toggle('active', BglState.isHudActive);
      BglUI.btnHud.style.background = BglState.isHudActive ? 'rgba(2, 132, 199, 0.25)' : '#1e293b';
    }
    BglLogger.log('Engine', `Toggle Debug HUD: ${BglState.isHudActive ? 'ENABLED' : 'DISABLED'}`);
    if (BglState.wasmExports && BglState.wasmExports.bgl_toggle_debug_overlay) {
      BglState.wasmExports.bgl_toggle_debug_overlay();
    }
  }
  window.toggleHUD = toggleHUD;

  function toggleBump() {
    BglState.isBumpActive = !BglState.isBumpActive;
    if (BglUI.btnBump) {
      BglUI.btnBump.classList.toggle('active', BglState.isBumpActive);
      BglUI.btnBump.style.background = BglState.isBumpActive ? 'rgba(2, 132, 199, 0.25)' : '#1e293b';
    }
    BglLogger.log('Engine', `Toggle Bump & Normal Mapping: ${BglState.isBumpActive ? 'ENABLED' : 'DISABLED'}`);
    if (BglState.wasmExports && BglState.wasmExports.bgl_toggle_bump) {
      BglState.wasmExports.bgl_toggle_bump();
    }
  }
  window.toggleBump = toggleBump;

  function toggleMipmaps() {
    BglState.isMipmapsActive = !BglState.isMipmapsActive;
    if (BglUI.btnMipmaps) {
      BglUI.btnMipmaps.classList.toggle('active', BglState.isMipmapsActive);
      BglUI.btnMipmaps.style.background = BglState.isMipmapsActive ? 'rgba(2, 132, 199, 0.25)' : '#1e293b';
    }
    BglLogger.log('Engine', `Toggle 16x Anisotropic Mipmapping: ${BglState.isMipmapsActive ? 'ENABLED' : 'DISABLED'}`);
    if (BglState.wasmExports && BglState.wasmExports.bgl_toggle_mipmaps) {
      BglState.wasmExports.bgl_toggle_mipmaps();
    }
  }
  window.toggleMipmaps = toggleMipmaps;

  function toggleShadows() {
    BglState.isShadowsActive = !BglState.isShadowsActive;
    if (BglUI.btnShadows) {
      BglUI.btnShadows.classList.toggle('active', BglState.isShadowsActive);
      BglUI.btnShadows.style.background = BglState.isShadowsActive ? 'rgba(2, 132, 199, 0.25)' : '#1e293b';
    }
    BglLogger.log('Engine', `Toggle PCF Shadow Map: ${BglState.isShadowsActive ? 'ENABLED' : 'DISABLED'}`);
    if (BglState.wasmExports && BglState.wasmExports.bgl_toggle_shadows) {
      BglState.wasmExports.bgl_toggle_shadows();
    }
  }
  window.toggleShadows = toggleShadows;

  function toggleReflections() {
    BglState.isReflectionsActive = !BglState.isReflectionsActive;
    if (BglUI.btnReflections) {
      BglUI.btnReflections.classList.toggle('active', BglState.isReflectionsActive);
      BglUI.btnReflections.style.background = BglState.isReflectionsActive ? 'rgba(2, 132, 199, 0.25)' : '#1e293b';
    }
    BglLogger.log('Engine', `Toggle Cubemap IBL Reflections: ${BglState.isReflectionsActive ? 'ENABLED' : 'DISABLED'}`);
    if (BglState.wasmExports && BglState.wasmExports.bgl_toggle_reflections) {
      BglState.wasmExports.bgl_toggle_reflections();
    }
  }
  window.toggleReflections = toggleReflections;

  function toggleAO() {
    BglState.isAOActive = !BglState.isAOActive;
    if (BglUI.btnAO) {
      BglUI.btnAO.classList.toggle('active', BglState.isAOActive);
      BglUI.btnAO.style.background = BglState.isAOActive ? 'rgba(2, 132, 199, 0.25)' : '#1e293b';
    }
    BglLogger.log('Engine', `Toggle Ambient Occlusion: ${BglState.isAOActive ? 'ENABLED' : 'DISABLED'}`);
    if (BglState.wasmExports && BglState.wasmExports.bgl_toggle_ao) {
      BglState.wasmExports.bgl_toggle_ao();
    }
  }
  window.toggleAO = toggleAO;

  function togglePOM() {
    BglState.isPomActive = !BglState.isPomActive;
    if (BglUI.btnPom) {
      BglUI.btnPom.classList.toggle('active', BglState.isPomActive);
      BglUI.btnPom.style.background = BglState.isPomActive ? 'rgba(2, 132, 199, 0.25)' : '#1e293b';
    }
    BglLogger.log('Engine', `Toggle Parallax Occlusion Mapping (POM): ${BglState.isPomActive ? 'ENABLED' : 'DISABLED'}`);
    if (BglState.wasmExports && BglState.wasmExports.bgl_toggle_pom) {
      BglState.wasmExports.bgl_toggle_pom();
    }
  }
  window.togglePOM = togglePOM;

  function toggleLensFlare() {
    BglState.isFlareActive = !BglState.isFlareActive;
    if (BglUI.btnFlare) {
      BglUI.btnFlare.classList.toggle('active', BglState.isFlareActive);
      BglUI.btnFlare.style.background = BglState.isFlareActive ? 'rgba(2, 132, 199, 0.25)' : '#1e293b';
    }
    BglLogger.log('Engine', `Toggle Sun Lens Flares: ${BglState.isFlareActive ? 'ENABLED' : 'DISABLED'}`);
    if (BglState.wasmExports && BglState.wasmExports.bgl_toggle_lens_flare) {
      BglState.wasmExports.bgl_toggle_lens_flare();
    }
  }
  window.toggleLensFlare = toggleLensFlare;

  function toggleSSAO() {
    BglState.isSsaoActive = !BglState.isSsaoActive;
    if (BglUI.btnSSAO) {
      BglUI.btnSSAO.classList.toggle('active', BglState.isSsaoActive);
      BglUI.btnSSAO.style.background = BglState.isSsaoActive ? 'rgba(2, 132, 199, 0.25)' : '#1e293b';
    }
    BglLogger.log('Engine', `Toggle Screen-Space Ambient Occlusion (SSAO/GTAO): ${BglState.isSsaoActive ? 'ENABLED' : 'DISABLED'}`);
    if (BglState.wasmExports && BglState.wasmExports.bgl_toggle_ssao) {
      BglState.wasmExports.bgl_toggle_ssao();
    }
  }
  window.toggleSSAO = toggleSSAO;

  function toggleBloom() {
    BglState.isBloomActive = !BglState.isBloomActive;
    if (BglUI.btnBloom) {
      BglUI.btnBloom.classList.toggle('active', BglState.isBloomActive);
      BglUI.btnBloom.style.background = BglState.isBloomActive ? 'rgba(2, 132, 199, 0.25)' : '#1e293b';
    }
    BglLogger.log('Engine', `Toggle Emissive HDR Bloom: ${BglState.isBloomActive ? 'ENABLED' : 'DISABLED'}`);
    if (BglState.wasmExports && BglState.wasmExports.bgl_toggle_bloom) {
      BglState.wasmExports.bgl_toggle_bloom();
    }
  }
  window.toggleBloom = toggleBloom;

  function onSsaoSlider(val) {
    const num = Number(val);
    BglState.ssaoIntensity = num;
    const label = document.getElementById('val-ssao-intensity');
    if (label) label.textContent = `${num.toFixed(2)}×`;
    if (BglState.wasmExports && BglState.wasmExports.bgl_set_ssao_intensity) {
      BglState.wasmExports.bgl_set_ssao_intensity(num);
    }
  }
  window.onSsaoSlider = onSsaoSlider;

  function onBloomSlider(val) {
    const num = Number(val);
    BglState.bloomIntensity = num;
    const label = document.getElementById('val-bloom-intensity');
    if (label) label.textContent = `${num.toFixed(2)}×`;
    if (BglState.wasmExports && BglState.wasmExports.bgl_set_bloom_intensity) {
      BglState.wasmExports.bgl_set_bloom_intensity(num);
    }
  }
  window.onBloomSlider = onBloomSlider;

  function onAoSlider(val) {
    const num = Number(val);
    BglState.aoIntensity = num;
    const label = document.getElementById('val-ao-intensity');
    if (label) label.textContent = `${num.toFixed(2)}×`;
    if (BglState.wasmExports && BglState.wasmExports.bgl_set_ao_intensity) {
      BglState.wasmExports.bgl_set_ao_intensity(num);
    }
  }
  window.onAoSlider = onAoSlider;

  function onBumpSlider(val) {
    const num = Number(val);
    BglState.bumpScale = num;
    const label = document.getElementById('val-bump-scale');
    if (label) label.textContent = `${num.toFixed(2)}×`;
    if (BglState.wasmExports && BglState.wasmExports.bgl_set_bump_scale) {
      BglState.wasmExports.bgl_set_bump_scale(num);
    }
  }
  window.onBumpSlider = onBumpSlider;

  function onPomSlider(val) {
    const num = Number(val);
    BglState.pomScale = num;
    const label = document.getElementById('val-pom-scale');
    if (label) label.textContent = num.toFixed(3);
    if (BglState.wasmExports && BglState.wasmExports.bgl_set_parallax_scale) {
      BglState.wasmExports.bgl_set_parallax_scale(num);
    }
  }
  window.onPomSlider = onPomSlider;

  function onFlareSlider(val) {
    const num = Number(val);
    BglState.flareIntensity = num;
    const label = document.getElementById('val-flare-intensity');
    if (label) label.textContent = `${num.toFixed(2)}×`;
    if (BglState.wasmExports && BglState.wasmExports.bgl_set_lens_flare_intensity) {
      BglState.wasmExports.bgl_set_lens_flare_intensity(num);
    }
  }
  window.onFlareSlider = onFlareSlider;

  // ============================================================================
  // Atmospheric Weather Controls (Rain, Snow, Fog, Smoke)
  // ============================================================================

  function setWeatherMode(mode) {
    const validModes = ['none', 'rain', 'snow', 'fog', 'smoke'];
    if (!validModes.includes(mode)) mode = 'none';
    BglState.weatherMode = mode;

    const btnNone = document.getElementById('btn-weather-none') || (BglUI && BglUI.btnWeatherNone);
    const btnRain = document.getElementById('btn-weather-rain') || (BglUI && BglUI.btnWeatherRain);
    const btnSnow = document.getElementById('btn-weather-snow') || (BglUI && BglUI.btnWeatherSnow);
    const btnFog = document.getElementById('btn-weather-fog') || (BglUI && BglUI.btnWeatherFog);
    const btnSmoke = document.getElementById('btn-weather-smoke') || (BglUI && BglUI.btnWeatherSmoke);

    const applyChipState = (btn, active) => {
      if (!btn) return;
      btn.classList.toggle('active', active);
      btn.style.background = active ? 'rgba(2, 132, 199, 0.25)' : '#1e293b';
    };

    applyChipState(btnNone, mode === 'none');
    applyChipState(btnRain, mode === 'rain');
    applyChipState(btnSnow, mode === 'snow');
    applyChipState(btnFog, mode === 'fog');
    applyChipState(btnSmoke, mode === 'smoke');

    BglLogger.log('Engine', `Atmospheric Weather Mode set to: ${mode.toUpperCase()}`);
    if (BglState.wasmExports && BglState.wasmExports.bgl_set_weather_mode) {
      const modeMap = { none: 0, rain: 1, snow: 2, fog: 3, smoke: 4 };
      BglState.wasmExports.bgl_set_weather_mode(modeMap[mode] || 0);
    }
    if (typeof BglNotifier !== 'undefined' && BglNotifier.info) {
      const titles = {
        none: 'Clear Skies',
        rain: 'Rain Storm',
        snow: 'Snowfall',
        fog: 'Atmospheric Fog',
        smoke: 'Volumetric Smoke'
      };
      const msgs = {
        none: 'Weather simulation disabled.',
        rain: 'Simulating rain precipitation, droplet ripples & wet surface glaze.',
        snow: 'Simulating drifting snowfall flakes & upward surface accumulation.',
        fog: 'Simulating exponential height/distance mist banks & anisotropic Mie scattering.',
        smoke: 'Simulating rising turbulent FBM smoke plumes & warm ember glow.'
      };
      BglNotifier.info(titles[mode], msgs[mode]);
    }
  }
  window.setWeatherMode = setWeatherMode;

  function toggleRain() {
    setWeatherMode(BglState.weatherMode === 'rain' ? 'none' : 'rain');
  }
  window.toggleRain = toggleRain;

  function toggleSnow() {
    setWeatherMode(BglState.weatherMode === 'snow' ? 'none' : 'snow');
  }
  window.toggleSnow = toggleSnow;

  function toggleFog() {
    setWeatherMode(BglState.weatherMode === 'fog' ? 'none' : 'fog');
  }
  window.toggleFog = toggleFog;

  function toggleSmoke() {
    setWeatherMode(BglState.weatherMode === 'smoke' ? 'none' : 'smoke');
  }
  window.toggleSmoke = toggleSmoke;

  function onWeatherIntensitySlider(val) {
    const num = Math.max(0.0, Math.min(2.0, Number(val)));
    BglState.weatherIntensity = num;
    const label = document.getElementById('val-weather-intensity');
    if (label) label.textContent = `${num.toFixed(2)}×`;
    if (BglState.wasmExports && BglState.wasmExports.bgl_set_weather_intensity) {
      BglState.wasmExports.bgl_set_weather_intensity(num);
    }
  }
  window.onWeatherIntensitySlider = onWeatherIntensitySlider;

  function onWindSlider(val) {
    const num = Math.max(-1.0, Math.min(1.0, Number(val)));
    BglState.windStrength = num;
    const label = document.getElementById('val-wind-strength');
    if (label) label.textContent = `${num >= 0 ? '+' : ''}${num.toFixed(2)}`;
    if (BglState.wasmExports && BglState.wasmExports.bgl_set_wind_strength) {
      BglState.wasmExports.bgl_set_wind_strength(num);
    }
  }
  window.onWindSlider = onWindSlider;

  function focusSelection() {
    BglLogger.log('Engine', 'Focusing camera on selection / scene bounds');
    const activeRenderer = getActiveRenderer();
    if (activeRenderer && activeRenderer.modelBounds) {
      const mb = activeRenderer.modelBounds;
      if (mb.isSponza && mb.minBound && mb.maxBound) {
        const dy = mb.maxBound[1] - mb.minBound[1];
        activeRenderer.camera.target = [mb.center[0], mb.minBound[1] + dy * 0.26, mb.center[2]];
        activeRenderer.camera.distance = 8.5;
      } else {
        activeRenderer.camera.target = [...mb.center];
        activeRenderer.camera.distance = mb.radius * 2.2;
      }
    }
    if (BglState.wasmExports && BglState.wasmExports.bgl_camera_focus_selection) {
      BglState.wasmExports.bgl_camera_focus_selection();
    }
  }
  window.focusSelection = focusSelection;

  function frameAll() {
    BglLogger.log('Engine', 'Framing all scene geometry');
    const activeRenderer = getActiveRenderer();
    if (activeRenderer && activeRenderer.modelBounds) {
      const mb = activeRenderer.modelBounds;
      if (mb.isSponza && mb.minBound && mb.maxBound) {
        const dy = mb.maxBound[1] - mb.minBound[1];
        activeRenderer.camera.target = [mb.center[0], mb.minBound[1] + dy * 0.26, mb.center[2]];
        activeRenderer.camera.distance = 8.5;
      } else {
        activeRenderer.camera.target = [...mb.center];
        activeRenderer.camera.distance = mb.radius * 2.2;
      }
    }
    if (BglState.wasmExports && BglState.wasmExports.bgl_camera_frame_all) {
      BglState.wasmExports.bgl_camera_frame_all();
    }
  }
  window.frameAll = frameAll;

  function toggleCameraMode() {
    BglState.isFlyMode = !BglState.isFlyMode;
    const btn = document.getElementById('btn-cam-mode');
    if (btn) btn.textContent = BglState.isFlyMode ? 'Mode: Fly [C]' : 'Mode: Orbit [C]';
    BglLogger.log('Engine', `Camera Navigation Mode: ${BglState.isFlyMode ? 'FLY' : 'ORBIT'}`);
    if (BglState.wasmExports && BglState.wasmExports.bgl_toggle_camera_mode) {
      BglState.wasmExports.bgl_toggle_camera_mode();
    }
  }
  window.toggleCameraMode = toggleCameraMode;

  function toggleTurntable() {
    BglState.isTurntableActive = !BglState.isTurntableActive;
    const btn = document.getElementById('btn-turntable');
    if (btn) btn.style.background = BglState.isTurntableActive ? '#0284c7' : '#475569';
    const activeRenderer = getActiveRenderer();
    if (activeRenderer) {
      activeRenderer.turntable = BglState.isTurntableActive;
    }
    BglLogger.log('Engine', `Turntable Autorotation: ${BglState.isTurntableActive ? 'ENABLED' : 'DISABLED'}`);
    if (BglState.wasmExports && BglState.wasmExports.bgl_toggle_turntable) {
      BglState.wasmExports.bgl_toggle_turntable();
    }
  }
  window.toggleTurntable = toggleTurntable;

  function onTurntableSpeedSlider(val) {
    const num = Number(val);
    const label = document.getElementById('val-turntable-speed');
    if (label) label.textContent = `${num.toFixed(2)} rad/s`;
    const activeRenderer = getActiveRenderer();
    if (activeRenderer) {
      activeRenderer.turntableSpeed = num;
    }
    if (BglState.wasmExports && BglState.wasmExports.bgl_set_turntable_speed) {
      BglState.wasmExports.bgl_set_turntable_speed(num);
    }
  }
  window.onTurntableSpeedSlider = onTurntableSpeedSlider;

  function onTimeOfDaySlider(val) {
    let num = Number(val) % 24.0;
    if (num < 0.0) num += 24.0;
    const hrs = Math.floor(num) % 24;
    const mins = Math.round((num - hrs) * 60) % 60;
    const icon = (num >= 7.0 && num < 17.5) ? '☀️' : ((num >= 5.5 && num < 19.5) ? '🌅' : '🌙');
    const label = document.getElementById('val-time-of-day');
    if (label) label.textContent = `${String(hrs).padStart(2, '0')}:${String(mins).padStart(2, '0')} ${icon}`;
    BglState.timeOfDay = num;
    if (BglState.wasmExports && BglState.wasmExports.bgl_set_time_of_day) {
      BglState.wasmExports.bgl_set_time_of_day(num);
    }
  }
  window.onTimeOfDaySlider = onTimeOfDaySlider;

  function toggleDayNightCycle() {
    BglState.isDayNightCycleActive = !BglState.isDayNightCycleActive;
    const btn = document.getElementById('btn-day-night') || (BglUI && BglUI.btnDayNight);
    if (btn) {
      btn.classList.toggle('active', BglState.isDayNightCycleActive);
      btn.style.background = BglState.isDayNightCycleActive ? 'rgba(2, 132, 199, 0.25)' : '#1e293b';
    }
    BglLogger.log('Engine', `Automatic Day/Night Cycle: ${BglState.isDayNightCycleActive ? 'ENABLED' : 'DISABLED'}`);
    if (BglState.wasmExports && BglState.wasmExports.bgl_toggle_day_night_cycle) {
      BglState.wasmExports.bgl_toggle_day_night_cycle();
    }
    if (typeof BglNotifier !== 'undefined' && BglNotifier.info) {
      BglNotifier.info(
        BglState.isDayNightCycleActive ? 'Day/Night Cycle Active' : 'Day/Night Cycle Paused',
        BglState.isDayNightCycleActive
          ? `Continuous sun & moon diurnal progression (${BglState.dayNightSpeed.toFixed(1)} h/s).`
          : 'Time of day locked at current position.'
      );
    }
  }
  window.toggleDayNightCycle = toggleDayNightCycle;

  function onDayNightSpeedSlider(val) {
    const num = Math.max(0.05, Math.min(12.0, Number(val)));
    BglState.dayNightSpeed = num;
    const label = document.getElementById('val-day-night-speed');
    if (label) label.textContent = `${num.toFixed(2)} h/s`;
    if (BglState.wasmExports && BglState.wasmExports.bgl_set_day_night_speed) {
      BglState.wasmExports.bgl_set_day_night_speed(num);
    }
  }
  window.onDayNightSpeedSlider = onDayNightSpeedSlider;

  const DEBUG_VIEW_NAMES = ['Lit', 'LOD Map', 'Normals', 'PBR', 'UV Grid'];

  function updateDebugViewUi() {
    const mode = BglState.debugViewMode || 0;
    const btnHeatmap = document.getElementById('btn-lod-heatmap') || (BglUI && BglUI.btnLodHeatmap);
    if (btnHeatmap) {
      const isHeat = (mode === 1);
      btnHeatmap.classList.toggle('active', isHeat);
      btnHeatmap.style.background = isHeat ? 'rgba(2, 132, 199, 0.25)' : '#1e293b';
    }
    const btnView = document.getElementById('btn-debug-view') || (BglUI && BglUI.btnDebugView);
    if (btnView) {
      const isDiag = (mode !== 0);
      btnView.classList.toggle('active', isDiag);
      btnView.style.background = isDiag ? 'rgba(2, 132, 199, 0.25)' : '#1e293b';
      btnView.textContent = `View: ${DEBUG_VIEW_NAMES[mode] || 'Lit'} [F9]`;
    }
  }

  function toggleLod() {
    BglState.isLodActive = !BglState.isLodActive;
    const btn = document.getElementById('btn-lod') || (BglUI && BglUI.btnLod);
    if (btn) {
      btn.classList.toggle('active', BglState.isLodActive);
      btn.style.background = BglState.isLodActive ? 'rgba(2, 132, 199, 0.25)' : '#1e293b';
    }
    BglLogger.log('Engine', `Multi-Level Mesh & Shader LOD System: ${BglState.isLodActive ? 'ENABLED' : 'DISABLED'}`);
    if (BglState.wasmExports && BglState.wasmExports.bgl_toggle_lod) {
      BglState.wasmExports.bgl_toggle_lod();
    }
  }
  window.toggleLod = toggleLod;

  function toggleAutoLod() {
    BglState.isAutoLodActive = !BglState.isAutoLodActive;
    const btn = document.getElementById('btn-auto-lod') || (BglUI && BglUI.btnAutoLod);
    if (btn) {
      btn.classList.toggle('active', BglState.isAutoLodActive);
      btn.style.background = BglState.isAutoLodActive ? 'rgba(2, 132, 199, 0.25)' : '#1e293b';
    }
    BglLogger.log('Engine', `Adaptive 60 FPS Auto-LOD: ${BglState.isAutoLodActive ? 'ENABLED' : 'DISABLED'}`);
    if (BglState.wasmExports && BglState.wasmExports.bgl_toggle_auto_lod) {
      BglState.wasmExports.bgl_toggle_auto_lod();
    }
  }
  window.toggleAutoLod = toggleAutoLod;

  function onLodBiasSlider(val) {
    const num = Math.max(0.25, Math.min(4.0, Number(val)));
    BglState.lodBias = num;
    const label = document.getElementById('val-lod-bias');
    if (label) label.textContent = `${num.toFixed(2)}×`;
    const slider = document.getElementById('slider-lod-bias');
    if (slider) slider.value = String(num);
    if (BglState.wasmExports && BglState.wasmExports.bgl_set_lod_bias) {
      BglState.wasmExports.bgl_set_lod_bias(num);
    }
  }
  window.onLodBiasSlider = onLodBiasSlider;

  function setDebugView(mode) {
    const clamped = Math.max(0, Math.min(4, Number(mode) | 0));
    BglState.debugViewMode = clamped;
    updateDebugViewUi();
    BglLogger.log('Engine', `Diagnostic Debug View: ${DEBUG_VIEW_NAMES[clamped]}`);
    if (BglState.wasmExports && BglState.wasmExports.bgl_set_debug_view) {
      BglState.wasmExports.bgl_set_debug_view(clamped);
    }
  }
  window.setDebugView = setDebugView;

  function cycleDebugView() {
    const next = ((BglState.debugViewMode || 0) + 1) % 5;
    setDebugView(next);
  }
  window.cycleDebugView = cycleDebugView;

  function toggleLodHeatmap() {
    const next = (BglState.debugViewMode === 1) ? 0 : 1;
    setDebugView(next);
  }
  window.toggleLodHeatmap = toggleLodHeatmap;

  function saveBookmark(slot) {
    if (BglWebGpuRenderer && BglWebGpuRenderer.isActive && BglWebGpuRenderer.camera) {
      BglState.cameraBookmarks = BglState.cameraBookmarks || {};
      const cam = BglWebGpuRenderer.camera;
      BglState.cameraBookmarks[slot] = {
        target: cam.target.slice(),
        distance: cam.distance,
        yaw: cam.yaw,
        pitch: cam.pitch
      };
    }
    if (BglState.wasmExports && BglState.wasmExports.bgl_camera_save_bookmark) {
      BglState.wasmExports.bgl_camera_save_bookmark(slot);
    }
    if (BglState.cameraBookmarks?.[slot] || BglState.wasmExports?.bgl_camera_save_bookmark) {
      BglNotifier.show({
        type: 'success',
        title: 'Bookmark Saved',
        message: `Saved viewpoint to slot #${slot + 1}`
      });
    }
  }
  window.saveBookmark = saveBookmark;

  function restoreBookmark(slot) {
    const saved = BglState.cameraBookmarks?.[slot];
    if (saved && BglWebGpuRenderer && BglWebGpuRenderer.camera) {
      const cam = BglWebGpuRenderer.camera;
      cam.target[0] = saved.target[0];
      cam.target[1] = saved.target[1];
      cam.target[2] = saved.target[2];
      cam.distance = Math.max(cam.minDistance, Math.min(cam.maxDistance, saved.distance));
      cam.yaw = saved.yaw;
      cam.pitch = Math.max(-1.45, Math.min(1.45, saved.pitch));
    }
    if (BglState.wasmExports && BglState.wasmExports.bgl_camera_restore_bookmark) {
      BglState.wasmExports.bgl_camera_restore_bookmark(slot);
    }
    if (saved || BglState.wasmExports?.bgl_camera_restore_bookmark) {
      BglLogger.log('Engine', `Restored viewpoint bookmark #${slot + 1}`);
    }
  }
  window.restoreBookmark = restoreBookmark;

  function onCameraSelect(val) {
    const idx = parseInt(val, 10);
    if (BglState.wasmExports && BglState.wasmExports.bgl_select_camera) {
      BglState.wasmExports.bgl_select_camera(idx);
      BglLogger.log('Engine', `Selected camera index: ${idx}`);
    }
  }
  window.onCameraSelect = onCameraSelect;

  function selectTreeNodeByIndex(idx) {
    if (idx < 0) return;
    const row = document.querySelector(`.tree-node-row[data-node-index="${idx}"]`);
    if (row) {
      document.querySelectorAll('.tree-node-row').forEach(r => r.classList.remove('selected'));
      row.classList.add('selected');
      let parent = row.closest('.tree-children');
      while (parent) {
        parent.style.display = 'block';
        const caret = parent.parentElement ? parent.parentElement.querySelector('.tree-toggle-caret') : null;
        if (caret) caret.classList.add('open');
        parent = parent.parentElement ? parent.parentElement.closest('.tree-children') : null;
      }
      row.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
      if (BglState.currentGltf && BglState.currentGltf.nodes && BglState.currentGltf.nodes[idx]) {
        BglGltf.showNodeDetails(idx, BglState.currentGltf.nodes[idx], BglState.currentGltf);
      }
    }
  }
  window.selectTreeNodeByIndex = selectTreeNodeByIndex;

  function toggleHudMinimize() {
    const hud = document.getElementById('ui-hud');
    const btn = document.getElementById('btn-minimize-hud');
    if (!hud) return;
    const isMin = hud.classList.toggle('minimized');
    if (btn) {
      btn.textContent = isMin ? '+' : '―';
      btn.title = isMin ? 'Expand HUD (O)' : 'Minimize HUD (O)';
    }
    BglLogger.log('UI', `HUD panel: ${isMin ? 'MINIMIZED' : 'EXPANDED'}`);
  }
  window.toggleHudMinimize = toggleHudMinimize;

  function toggleFeaturesSection() {
    const container = document.getElementById('features-chips-container');
    const chevron = document.getElementById('features-chevron');
    if (!container) return;
    const isHidden = container.style.display === 'none';
    container.style.display = isHidden ? 'grid' : 'none';
    if (chevron) chevron.textContent = isHidden ? '▾' : '▸';
  }
  window.toggleFeaturesSection = toggleFeaturesSection;

  function toggleTelemetrySection() {
    const card = document.getElementById('aabb-telemetry-card');
    const chevron = document.getElementById('telemetry-chevron');
    if (!card) return;
    const isHidden = card.style.display === 'none';
    card.style.display = isHidden ? 'block' : 'none';
    if (chevron) chevron.textContent = isHidden ? '▾' : '▸';
  }
  window.toggleTelemetrySection = toggleTelemetrySection;

  function toggleTuningSection() {
    const container = document.getElementById('tuning-sliders-container');
    const chevron = document.getElementById('tuning-chevron');
    if (!container) return;
    const isHidden = container.style.display === 'none';
    container.style.display = isHidden ? 'flex' : 'none';
    if (chevron) chevron.textContent = isHidden ? '▾' : '▸';
  }
  window.toggleTuningSection = toggleTuningSection;

  function toggleTreePanel() {
    if (!BglUI.treePanel || !BglUI.treePill) return;
    const isHidden = BglUI.treePanel.style.display === 'none';
    BglUI.treePanel.style.display = isHidden ? 'flex' : 'none';
    BglUI.treePill.style.display = isHidden ? 'none' : 'flex';
    BglLogger.log('UI', `Scene Hierarchy Panel: ${isHidden ? 'OPEN' : 'CLOSED'}`);
  }
  window.toggleTreePanel = toggleTreePanel;

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
    BglLogger.log('UI', `Switched inspector view to: ${tab}`);
  }
  window.switchTab = switchTab;

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

  // ============================================================================
  // Global Event Listeners (Keyboard, Drag & Drop, Error Handlers)
  // ============================================================================
  function registerEventListeners() {
    // Continuous WASD & Arrow Key Navigation
    const activeNavKeys = new Set();
    const navKeyCodes = new Set([
      'KeyW', 'KeyS', 'KeyA', 'KeyD',
      'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight',
      'Equal', 'Minus', 'NumpadAdd', 'NumpadSubtract'
    ]);

    window.addEventListener('keydown', (e) => {
      if (document.activeElement && (document.activeElement.tagName === 'INPUT' || document.activeElement.tagName === 'TEXTAREA')) return;

      if (e.key >= '1' && e.key <= '9') {
        const slot = parseInt(e.key, 10) - 1;
        if (e.ctrlKey) {
          e.preventDefault();
          saveBookmark(slot);
        } else {
          restoreBookmark(slot);
        }
        return;
      }

      if (navKeyCodes.has(e.code)) {
        activeNavKeys.add(e.code);
        if (['KeyW', 'KeyS', 'KeyA', 'KeyD', 'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight'].includes(e.code)) {
          e.preventDefault();
        }
      }

      if (e.ctrlKey || e.metaKey || e.altKey) return;
      const key = e.key.toUpperCase();
      switch (key) {
        case 'F': focusSelection(); break;
        case 'HOME': frameAll(); break;
        case 'C': toggleCameraMode(); break;
        case 'T': toggleTurntable(); break;
        case 'B': toggleBBox(); break;
        case 'Z': toggleBloom(); break;
        case 'Q': toggleQuadtree(); break;
        case 'H':
          if (BglState.wasmExports && BglState.wasmExports.bgl_toggle_occlusion) {
            BglState.wasmExports.bgl_toggle_occlusion();
          }
          break;
        case 'K': toggleSkybox(); break;
        case 'O':
        case 'F3': toggleHUD(); break;
        case 'N': toggleTreePanel(); break;
        case 'V': toggleBump(); break;
        case 'P': togglePOM(); break;
        case 'J': toggleLensFlare(); break;
        case 'L': toggleMipmaps(); break;
        case 'M': toggleShadows(); break;
        case 'R': toggleReflections(); break;
        case 'U': toggleAO(); break;
        case 'G': toggleSSAO(); break;
        case 'X': toggleRain(); break;
        case 'Y': toggleSnow(); break;
        case 'F6': toggleFog(); break;
        case 'F7': toggleSmoke(); break;
        case 'I':
        case 'F8': toggleDayNightCycle(); break;
        case 'F9': cycleDebugView(); break;
        case 'F10': toggleLodHeatmap(); break;
        case 'F11': toggleLod(); break;
        case 'F12': toggleAutoLod(); break;
        case ';': onLodBiasSlider((BglState.lodBias || 1.0) - 0.15); break;
        case '\'': onLodBiasSlider((BglState.lodBias || 1.0) + 0.15); break;
        case '+':
        case '=':
          if (!e.repeat) {
            if (BglWebGpuRenderer && BglWebGpuRenderer.isActive && BglWebGpuRenderer.camera) {
              const cam = BglWebGpuRenderer.camera;
              cam.distance = Math.max(cam.minDistance, Math.min(cam.maxDistance, cam.distance * 0.9));
            }
            if (BglState.wasmExports && BglState.wasmExports.bgl_camera_zoom) BglState.wasmExports.bgl_camera_zoom(0.8);
          }
          break;
        case '-':
        case '_':
          if (!e.repeat) {
            if (BglWebGpuRenderer && BglWebGpuRenderer.isActive && BglWebGpuRenderer.camera) {
              const cam = BglWebGpuRenderer.camera;
              cam.distance = Math.max(cam.minDistance, Math.min(cam.maxDistance, cam.distance * 1.1));
            }
            if (BglState.wasmExports && BglState.wasmExports.bgl_camera_zoom) BglState.wasmExports.bgl_camera_zoom(-0.8);
          }
          break;
        case 'ESCAPE': dismissCompatWarning(); break;
        default: break;
      }
    });

    window.addEventListener('keyup', (e) => {
      if (navKeyCodes.has(e.code)) {
        activeNavKeys.delete(e.code);
      }
    });

    // High frequency smooth keyboard navigation loop (WASD & Arrows & Zoom)
    let lastNavTime = performance.now();
    function navLoop(now) {
      const dt = Math.min((now - lastNavTime) / 1000, 0.1);
      lastNavTime = now;

      if (activeNavKeys.size > 0) {
        const baseScale = (BglWebGpuRenderer && BglWebGpuRenderer.camera)
          ? Math.max(BglWebGpuRenderer.camera.distance * 1.5, 1.0)
          : 4.0;
        const moveSpeed = baseScale * dt;
        let fwd = 0;
        let right = 0;
        let zoom = 0;

        if (activeNavKeys.has('KeyW') || activeNavKeys.has('ArrowUp')) fwd += moveSpeed;
        if (activeNavKeys.has('KeyS') || activeNavKeys.has('ArrowDown')) fwd -= moveSpeed;
        if (activeNavKeys.has('KeyD') || activeNavKeys.has('ArrowRight')) right += moveSpeed;
        if (activeNavKeys.has('KeyA') || activeNavKeys.has('ArrowLeft')) right -= moveSpeed;

        if (activeNavKeys.has('Equal') || activeNavKeys.has('NumpadAdd')) zoom += 3.5 * dt;
        if (activeNavKeys.has('Minus') || activeNavKeys.has('NumpadSubtract')) zoom -= 3.5 * dt;

        if (BglWebGpuRenderer && BglWebGpuRenderer.isActive && BglWebGpuRenderer.camera) {
          const cam = BglWebGpuRenderer.camera;
          if (fwd !== 0 || right !== 0) {
            const cy = Math.cos(cam.yaw);
            const sy = Math.sin(cam.yaw);
            cam.target[0] += (-sy * fwd + cy * right);
            cam.target[2] += (-cy * fwd - sy * right);
          }
          if (zoom !== 0) {
            const scale = Math.max(0.5, 1.0 - zoom * 0.25);
            cam.distance = Math.max(cam.minDistance, Math.min(cam.maxDistance, cam.distance * scale));
          }
        }
        if (BglState.wasmExports) {
          if ((fwd !== 0 || right !== 0) && BglState.wasmExports.bgl_camera_move) {
            BglState.wasmExports.bgl_camera_move(fwd, right, 0);
          }
          if (zoom !== 0 && BglState.wasmExports.bgl_camera_zoom) {
            BglState.wasmExports.bgl_camera_zoom(zoom);
          }
        }
      }
    }
    BglFrameScheduler.registerNav(navLoop);

    // 3D Canvas Click-Picking and Touch Gestures
    const canvas = document.getElementById('canvas');
    if (canvas) {
      let isDragging = false;
      let startX = 0;
      let startY = 0;

      canvas.addEventListener('mousedown', (e) => {
        isDragging = false;
        startX = e.clientX;
        startY = e.clientY;
      });

      canvas.addEventListener('mousemove', (e) => {
        if (Math.hypot(e.clientX - startX, e.clientY - startY) > 6) {
          isDragging = true;
        }
      });

      canvas.addEventListener('click', (e) => {
        if (isDragging) return;
        const rect = canvas.getBoundingClientRect();
        const x = e.clientX - rect.left;
        const y = e.clientY - rect.top;
        if (BglState.wasmExports && BglState.wasmExports.bgl_pick_node) {
          const hitIdx = BglState.wasmExports.bgl_pick_node(x, y, rect.width, rect.height);
          if (hitIdx >= 0) {
            BglState.selectedNodeIndex = hitIdx;
            selectTreeNodeByIndex(hitIdx);
            BglNotifier.show({
              type: 'info',
              title: 'Object Selected',
              message: `Picked node #${hitIdx} in 3D scene`
            });
          } else {
            BglState.selectedNodeIndex = -1;
            if (BglState.wasmExports.bgl_clear_selection) {
              BglState.wasmExports.bgl_clear_selection();
            }
            document.querySelectorAll('.tree-node-row').forEach(r => r.classList.remove('selected'));
          }
        }
      });

      // Touch Controls (Pinch-to-zoom & Two-finger Pan)
      let touchLastPos = { x: 0, y: 0 };
      let touchStartDist = 0;
      let touchLastMid = { x: 0, y: 0 };

      canvas.addEventListener('touchstart', (e) => {
        if (e.touches.length === 1) {
          touchLastPos = { x: e.touches[0].clientX, y: e.touches[0].clientY };
        } else if (e.touches.length === 2) {
          touchStartDist = Math.hypot(
            e.touches[0].clientX - e.touches[1].clientX,
            e.touches[0].clientY - e.touches[1].clientY
          );
          touchLastMid = {
            x: (e.touches[0].clientX + e.touches[1].clientX) * 0.5,
            y: (e.touches[0].clientY + e.touches[1].clientY) * 0.5
          };
        }
      }, { passive: false });

      canvas.addEventListener('touchmove', (e) => {
        e.preventDefault();
        if (e.touches.length === 1 && BglState.wasmExports) {
          const dx = e.touches[0].clientX - touchLastPos.x;
          const dy = e.touches[0].clientY - touchLastPos.y;
          touchLastPos = { x: e.touches[0].clientX, y: e.touches[0].clientY };
          if (BglState.wasmExports.bgl_camera_orbit) {
            BglState.wasmExports.bgl_camera_orbit(dx * 0.005, -dy * 0.005);
          }
        } else if (e.touches.length === 2 && BglState.wasmExports) {
          const curDist = Math.hypot(
            e.touches[0].clientX - e.touches[1].clientX,
            e.touches[0].clientY - e.touches[1].clientY
          );
          const deltaDist = curDist - touchStartDist;
          touchStartDist = curDist;
          if (Math.abs(deltaDist) > 0.5 && BglState.wasmExports.bgl_camera_zoom) {
            BglState.wasmExports.bgl_camera_zoom(deltaDist * 0.04);
          }

          const curMid = {
            x: (e.touches[0].clientX + e.touches[1].clientX) * 0.5,
            y: (e.touches[0].clientY + e.touches[1].clientY) * 0.5
          };
          const dMidX = curMid.x - touchLastMid.x;
          const dMidY = curMid.y - touchLastMid.y;
          touchLastMid = curMid;
          if ((Math.abs(dMidX) > 0.5 || Math.abs(dMidY) > 0.5) && BglState.wasmExports.bgl_camera_pan) {
            BglState.wasmExports.bgl_camera_pan(-dMidX * 0.005, dMidY * 0.005);
          }
        }
      }, { passive: false });
    }

    // Web Gamepad API Polling Loop (PS5 DualSense & Gamepad detection)
    let lastGamepadConnected = false;
    const gamepadBadge = BglUI.gamepadBadge || document.getElementById('gamepad-badge');
    const gamepadLabel = BglUI.gamepadLabel || document.getElementById('gamepad-label');

    function pollGamepad() {
      if (navigator.getGamepads) {
        const gamepads = navigator.getGamepads();
        let activePad = null;
        for (const gp of gamepads) {
          if (gp && gp.connected) {
            activePad = gp;
            break;
          }
        }

        if (activePad && BglState.wasmExports) {
          const isDualSense = /dualsense|054c|ps5|playstation/i.test(activePad.id);
          const padTitle = isDualSense ? '🎮 PS5 DualSense Controller' : `🎮 ${activePad.id.split('(')[0].trim() || 'Gamepad'}`;

          if (!lastGamepadConnected) {
            lastGamepadConnected = true;
            if (gamepadBadge) gamepadBadge.style.display = 'inline-flex';
            if (gamepadLabel) gamepadLabel.textContent = isDualSense ? '🎮 PS5 DualSense Active' : `${padTitle} Active`;
            BglNotifier.show({
              type: 'success',
              title: isDualSense ? 'PS5 Controller Detected' : 'Gamepad Connected',
              message: `${padTitle} connected. PS5 cinematic response curve active.`
            });
            BglLogger.log('Engine', `Gamepad connected: ${activePad.id} (DualSense: ${isDualSense})`);
          }

          // Apply deadzone and PS5 exponential sensitivity curve
          const deadzone = 0.12;
          let lx = Math.abs(activePad.axes[0]) > deadzone ? activePad.axes[0] : 0;
          let ly = Math.abs(activePad.axes[1]) > deadzone ? activePad.axes[1] : 0;
          let rx = Math.abs(activePad.axes[2]) > deadzone ? activePad.axes[2] : 0;
          let ry = Math.abs(activePad.axes[3]) > deadzone ? activePad.axes[3] : 0;

          // PS5 response curve: power 2.1 curve for fine aim + fast turn
          const rMag = Math.hypot(rx, ry);
          if (rMag > 0.001) {
            const curvedMag = Math.pow(Math.min(rMag, 1.0), 2.1);
            rx = (rx / rMag) * curvedMag;
            ry = (ry / rMag) * curvedMag * 0.70; // Stabilize pitch axis
          }

          const lt = activePad.buttons[6] ? activePad.buttons[6].value : 0;
          const rt = activePad.buttons[7] ? activePad.buttons[7].value : 0;
          const btnA = activePad.buttons[0] && activePad.buttons[0].pressed ? 1 : 0;
          const btnB = activePad.buttons[1] && activePad.buttons[1].pressed ? 1 : 0;
          const btnX = activePad.buttons[2] && activePad.buttons[2].pressed ? 1 : 0;
          const btnY = activePad.buttons[3] && activePad.buttons[3].pressed ? 1 : 0;

          if (BglState.wasmExports.bgl_gamepad_state) {
            BglState.wasmExports.bgl_gamepad_state(lx, ly, rx, ry, lt, rt, btnA, btnB, btnX, btnY);
          }
        } else if (lastGamepadConnected) {
          lastGamepadConnected = false;
          if (gamepadBadge) gamepadBadge.style.display = 'none';
          BglNotifier.show({
            type: 'info',
            title: 'Gamepad Disconnected',
            message: 'Game controller has been disconnected.'
          });
          BglLogger.log('Engine', 'Gamepad disconnected');
        }
      }
    }
    BglFrameScheduler.registerGamepad(pollGamepad);

    // Viewport Resize
    window.addEventListener('resize', () => {
      BglLogger.log('Engine', `Window resized to ${window.innerWidth} × ${window.innerHeight}`);
      if (BglState.wasmExports && BglState.wasmExports.bgl_resize) {
        BglState.wasmExports.bgl_resize(window.innerWidth, window.innerHeight);
      }
    });

    // Drag and Drop Handling
    if (BglUI.dropZone) {
      window.addEventListener('dragover', (e) => {
        e.preventDefault();
        BglUI.dropZone.classList.add('active');
        if (BglUI.dropText) BglUI.dropText.style.display = 'block';
      });

      window.addEventListener('dragleave', (e) => {
        if (e.relatedTarget === null) {
          BglUI.dropZone.classList.remove('active');
          if (BglUI.dropText) BglUI.dropText.style.display = 'none';
        }
      });

      window.addEventListener('drop', async (e) => {
        e.preventDefault();
        BglUI.dropZone.classList.remove('active');
        if (BglUI.dropText) BglUI.dropText.style.display = 'none';

        if (e.dataTransfer && e.dataTransfer.files.length > 0) {
          const file = e.dataTransfer.files[0];
          BglLogger.log('UI', `File dropped: ${file.name} (${file.size.toLocaleString()} bytes)`);

          const isGltf = file.name.endsWith('.glb') || file.name.endsWith('.gltf');
          if (!isGltf) {
            BglNotifier.warning(
              'Unsupported File Format',
              `'${file.name}' does not appear to be a .glb or .gltf file. Attempting to parse anyway.`
            );
          }

          const reader = new FileReader();
          reader.onload = async (evt) => {
            try {
              await ingestModel(file.name, evt.target.result);
            } catch (err) {
              BglNotifier.error('File Ingestion Error', `Could not ingest '${file.name}': ${err.message}`);
            }
          };
          reader.onerror = () => {
            BglNotifier.error('File Read Failed', `Failed to read dropped file '${file.name}'.`);
          };
          reader.readAsArrayBuffer(file);
        } else if (e.dataTransfer) {
          const rawUri = (e.dataTransfer.getData('text/uri-list') || e.dataTransfer.getData('text/plain') || '').trim();
          const urlLine = rawUri.split(/\r?\n/).find(line => line.trim() && !line.trim().startsWith('#'));
          if (urlLine && (/^https?:\/\//i.test(urlLine.trim()) || /^data:/i.test(urlLine.trim()))) {
            BglLogger.log('UI', `URL dropped: ${urlLine.trim()}`);
            await loadModelFromUrl(urlLine.trim());
          }
        }
      });
    }

    // Global Unhandled Errors
    window.addEventListener('error', (evt) => {
      BglLogger.error('Unhandled', evt.message || 'Script error', evt.filename, evt.lineno);
    });

    window.addEventListener('unhandledrejection', (evt) => {
      BglLogger.error('UnhandledPromise', evt.reason ? evt.reason.message || evt.reason : 'Promise rejected');
    });
  }

  // ============================================================================
  // Application Bootstrap & Global Module Exports
  // ============================================================================
  window.BglState = BglState;
  window.BglWebGpuRenderer = BglWebGpuRenderer;
  window.BglGltf = BglGltf;
  window.BglLogger = BglLogger;
  window.BglModelCache = BglModelCache;
  window.getActiveRenderer = getActiveRenderer;

  async function bootstrap() {
    BglLogger.banner();
    registerEventListeners();

    // Start real-time UI performance telemetry ticker inside unified frame scheduler
    let lastTime = performance.now();
    let frameCount = 0;
    const fpsPill = BglUI.hudFpsPill || document.getElementById('hud-fps-pill');
    function tickFps(now) {
      frameCount++;
      const nowTime = (typeof now === 'number') ? now : performance.now();
      const elapsed = nowTime - lastTime;
      if (elapsed >= 500) {
        const fps = (frameCount * 1000) / elapsed;
        frameCount = 0;
        lastTime = nowTime;
        if (fpsPill) {
          fpsPill.textContent = `${fps.toFixed(1)} FPS`;
          if (fps >= 55) {
            fpsPill.style.background = 'rgba(16, 185, 129, 0.15)';
            fpsPill.style.color = '#34d399';
            fpsPill.style.borderColor = 'rgba(16, 185, 129, 0.35)';
          } else if (fps >= 30) {
            fpsPill.style.background = 'rgba(234, 179, 8, 0.15)';
            fpsPill.style.color = '#facc15';
            fpsPill.style.borderColor = 'rgba(234, 179, 8, 0.35)';
          } else {
            fpsPill.style.background = 'rgba(244, 63, 94, 0.15)';
            fpsPill.style.color = '#fb7185';
            fpsPill.style.borderColor = 'rgba(244, 63, 94, 0.35)';
          }
        }
      }
    }
    BglFrameScheduler.registerFps(tickFps);

    // Initialize Cache Storage UI status
    BglModelCache.updateCacheStatusUI().catch(e => BglLogger.warn('Cache', 'Initial cache check failed:', e));

    // 1. Verify graphics capabilities and initialize 3D renderer
    const isCompat = await checkBrowserCompatibility();

    // 2. Initialize WebAssembly runtime bridge
    try {
      await BglWasmBridge.init();
    } catch (wasmErr) {
      BglLogger.warn('Wasm', 'WebAssembly runtime bridge notice:', wasmErr);
    }

    // 3. Ingest default 3D model into the active pipeline
    await loadDefaultModel();
    await BglModelCache.updateCacheStatusUI().catch(() => {});

    // Expose engine handles for telemetry, testing, and debugging
    window.BglState = BglState;
    window.BglWebGpuRenderer = BglWebGpuRenderer;
    window.BglGltf = BglGltf;
    window.BglLogger = BglLogger;
    window.BglModelCache = BglModelCache;
    window.getActiveRenderer = getActiveRenderer;

    BglLogger.log('Engine', `Bootstrap completed successfully (Backend: ${BglState.backend}).`);
  }

  // Run on DOM ready
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', bootstrap);
  } else {
    bootstrap();
  }
})();
