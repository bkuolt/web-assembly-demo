/**
 * @file app.js
 * @brief bgl-Engine Web Viewer Client Runtime
 *
 * Provides:
 * - High-performance WebAssembly streaming compilation & instantiation
 * - Real-time streaming network asset downloading with byte counters and transfer speeds
 * - Comprehensive structured console logging with millisecond timestamps and color badges
 * - User-facing toast notification banner system for error handling and telemetry
 * - Live glTF 2.0 / GLB scene graph hierarchy tree and collapsible JSON inspector
 * - Spatial AABB telemetry and WebGPU engine control bridge
 */

(() => {
  'use strict';

  // ============================================================================
  // Structured Logger Module
  // ============================================================================
  const BglLogger = {
    badgeStyles: {
      Engine:  'background: #0284c7; color: #ffffff; font-weight: 700; border-radius: 3px; padding: 2px 6px;',
      Wasm:    'background: #8b5cf6; color: #ffffff; font-weight: 700; border-radius: 3px; padding: 2px 6px;',
      WebGPU:  'background: #10b981; color: #042f2e; font-weight: 700; border-radius: 3px; padding: 2px 6px;',
      Network: 'background: #06b6d4; color: #083344; font-weight: 700; border-radius: 3px; padding: 2px 6px;',
      glTF:    'background: #f59e0b; color: #451a03; font-weight: 700; border-radius: 3px; padding: 2px 6px;',
      Perf:    'background: #ec4899; color: #ffffff; font-weight: 700; border-radius: 3px; padding: 2px 6px;',
      UI:      'background: #64748b; color: #ffffff; font-weight: 700; border-radius: 3px; padding: 2px 6px;',
      Error:   'background: #ef4444; color: #ffffff; font-weight: 700; border-radius: 3px; padding: 2px 6px;',
      Warning: 'background: #f59e0b; color: #000000; font-weight: 700; border-radius: 3px; padding: 2px 6px;',
    },

    timers: new Map(),

    formatTimestamp() {
      const d = new Date();
      const pad = (n, s = 2) => String(n).padStart(s, '0');
      return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}.${pad(d.getMilliseconds(), 3)}`;
    },

    log(category, message, ...args) {
      const style = this.badgeStyles[category] || this.badgeStyles.Engine;
      const ts = this.formatTimestamp();
      console.log(`%c[${category}]%c ${ts} - ${message}`, style, 'color: inherit;', ...args);
    },

    warn(category, message, ...args) {
      const style = this.badgeStyles.Warning;
      const ts = this.formatTimestamp();
      console.warn(`%c[${category} ⚠️]%c ${ts} - ${message}`, style, 'color: inherit;', ...args);
    },

    error(category, message, ...args) {
      const style = this.badgeStyles.Error;
      const ts = this.formatTimestamp();
      console.error(`%c[${category} ✕]%c ${ts} - ${message}`, style, 'color: inherit;', ...args);
    },

    timeStart(label) {
      this.timers.set(label, performance.now());
    },

    timeEnd(category, label, message) {
      const start = this.timers.get(label);
      if (start !== undefined) {
        const elapsed = (performance.now() - start).toFixed(2);
        this.timers.delete(label);
        this.log(category || 'Perf', `${message || label} (took ${elapsed} ms)`);
        return elapsed;
      }
      return null;
    },

    banner() {
      console.log(
        '%c bgl-Engine %c WebAssembly & WebGPU 3D Runtime v0.9.11 ',
        'background: #0284c7; color: #fff; font-weight: 900; font-size: 13px; border-radius: 4px 0 0 4px; padding: 4px 8px;',
        'background: #0f172a; color: #38bdf8; font-weight: 600; font-size: 13px; border-radius: 0 4px 4px 0; border: 1px solid #0284c7; padding: 3px 8px;'
      );
      this.log('Engine', `Client Platform: ${navigator.userAgent}`);
      this.log('Engine', `Viewport Dimensions: ${window.innerWidth} × ${window.innerHeight} (DPR: ${window.devicePixelRatio || 1})`);
      this.log('Engine', `Secure Context: ${window.isSecureContext ? 'YES (HTTPS/localhost)' : 'NO (Insecure HTTP)'}`);
    }
  };

  // ============================================================================
  // User-Facing Toast Notification System
  // ============================================================================
  const BglNotifier = {
    container: document.getElementById('user-toast-container'),

    icons: {
      error: '✕',
      warning: '⚠️',
      success: '✓',
      info: 'ℹ️'
    },

    /**
     * Show a modern floating toast notification to the user.
     * @param {Object} options
     * @param {'error'|'warning'|'success'|'info'} options.type
     * @param {string} options.title
     * @param {string} options.message
     * @param {string} [options.details]
     * @param {number} [options.durationMs]
     */
    show({ type = 'info', title = '', message = '', details = '', durationMs = 5000 }) {
      if (!this.container) {
        this.container = document.getElementById('user-toast-container');
        if (!this.container) return;
      }

      const toast = document.createElement('div');
      toast.className = `toast toast-${type}`;
      toast.setAttribute('role', type === 'error' ? 'alert' : 'status');

      const iconSpan = document.createElement('span');
      iconSpan.className = 'toast-icon';
      iconSpan.textContent = this.icons[type] || '•';

      const bodyDiv = document.createElement('div');
      bodyDiv.className = 'toast-body';

      if (title) {
        const titleDiv = document.createElement('div');
        titleDiv.className = 'toast-title';
        titleDiv.textContent = title;
        bodyDiv.appendChild(titleDiv);
      }

      const msgDiv = document.createElement('div');
      msgDiv.className = 'toast-message';
      msgDiv.textContent = message;
      bodyDiv.appendChild(msgDiv);

      if (details) {
        const detailsPre = document.createElement('pre');
        detailsPre.className = 'toast-details';
        detailsPre.textContent = typeof details === 'string' ? details : JSON.stringify(details, null, 2);
        bodyDiv.appendChild(detailsPre);
      }

      const closeBtn = document.createElement('button');
      closeBtn.className = 'toast-close-btn';
      closeBtn.setAttribute('aria-label', 'Dismiss notification');
      closeBtn.textContent = '✕';

      toast.appendChild(iconSpan);
      toast.appendChild(bodyDiv);
      toast.appendChild(closeBtn);

      this.container.appendChild(toast);

      let timeoutId = null;
      const dismiss = () => {
        if (toast.classList.contains('toast-hiding')) return;
        toast.classList.add('toast-hiding');
        setTimeout(() => {
          if (toast.parentNode) toast.parentNode.removeChild(toast);
        }, 260);
      };

      closeBtn.addEventListener('click', (e) => {
        e.stopPropagation();
        if (timeoutId) clearTimeout(timeoutId);
        dismiss();
      });

      if (durationMs > 0) {
        timeoutId = setTimeout(dismiss, durationMs);
        toast.addEventListener('mouseenter', () => {
          if (timeoutId) clearTimeout(timeoutId);
        });
        toast.addEventListener('mouseleave', () => {
          timeoutId = setTimeout(dismiss, 2000);
        });
      }

      // Also mirror to structured console logger
      if (type === 'error') {
        BglLogger.error('Notification', `${title}: ${message}`, details || '');
      } else if (type === 'warning') {
        BglLogger.warn('Notification', `${title}: ${message}`, details || '');
      } else {
        BglLogger.log('Notification', `${title}: ${message}`);
      }
    },

    error(title, message, details = '') {
      this.show({ type: 'error', title, message, details, durationMs: 8000 });
      BglUI.setStatusText(`Error: ${message}`, true);
    },

    warning(title, message, details = '') {
      this.show({ type: 'warning', title, message, details, durationMs: 6000 });
      BglUI.setStatusText(`Warning: ${message}`);
    },

    success(title, message, details = '') {
      this.show({ type: 'success', title, message, details, durationMs: 4000 });
      BglUI.setStatusText(message);
    },

    info(title, message, details = '') {
      this.show({ type: 'info', title, message, details, durationMs: 3500 });
    }
  };
  window.BglNotifier = BglNotifier;

  // ============================================================================
  // Application State
  // ============================================================================
  const BglState = {
    backend: 'webgpu',
    wasmExports: null,
    wasmMemory: null,
    currentGltf: null,
    currentModelBuffer: null,
    currentModelName: "ToyCar.glb",
    selectedNodeIndex: null,

    isBboxActive: true,
    isQuadtreeActive: false,
    isSkyboxActive: true,
    isHudActive: true,
    isBumpActive: true,
    isPomActive: true,
    isMipmapsActive: true,
    isShadowsActive: true,
    isReflectionsActive: true,
    isAOActive: true,
    isSsaoActive: true,
    isBloomActive: true,
    isFlareActive: true,
    aoIntensity: 1.0,
    ssaoIntensity: 0.75,
    bloomIntensity: 0.65,
    pomScale: 0.035,
    flareIntensity: 0.75,
  };

  // ============================================================================
  // UI & DOM Controller
  // ============================================================================
  const BglUI = {
    pBar: document.getElementById('progress-bar'),
    sText: document.getElementById('status-text'),
    dropZone: document.getElementById('drop-zone'),
    dropText: document.getElementById('drop-text'),
    treePanel: document.getElementById('node-tree-panel'),
    treePill: document.getElementById('tree-toggle-pill'),

    btnBbox: document.getElementById('btn-bbox'),
    btnQuadtree: document.getElementById('btn-quadtree'),
    btnSkybox: document.getElementById('btn-skybox'),
    btnHud: document.getElementById('btn-hud'),
    btnBump: document.getElementById('btn-bump'),
    btnPom: document.getElementById('btn-pom'),
    btnMipmaps: document.getElementById('btn-mipmaps'),
    btnShadows: document.getElementById('btn-shadows'),
    btnReflections: document.getElementById('btn-reflections'),
    btnAO: document.getElementById('btn-ao'),
    btnSSAO: document.getElementById('btn-ssao'),
    btnBloom: document.getElementById('btn-bloom'),
    btnFlare: document.getElementById('btn-flare'),

    btnToyCar: document.getElementById('btn-load-toycar'),
    btnSponza: document.getElementById('btn-load-sponza'),

    elDims: document.getElementById('aabb-dims'),
    elMin: document.getElementById('aabb-min'),
    elMax: document.getElementById('aabb-max'),
    elCenter: document.getElementById('aabb-center'),
    elPrims: document.getElementById('aabb-prims'),

    setProgress(percent, msg) {
      if (this.pBar) {
        const p = Math.min(100, Math.max(0, percent));
        this.pBar.style.width = `${p}%`;
        const container = this.pBar.parentElement;
        if (container) {
          if (p >= 100) {
            setTimeout(() => { container.classList.add('ready'); }, 500);
          } else {
            container.classList.remove('ready');
          }
        }
      }
      if (this.sText && msg) {
        this.sText.textContent = msg;
        this.sText.style.color = '#cbd5e1';
      }
    },

    setStatusText(msg, isError = false) {
      if (this.sText) {
        this.sText.textContent = msg;
        this.sText.style.color = isError ? '#fb7185' : '#cbd5e1';
      }
    },

    setActiveModelButton(modelName) {
      if (this.btnToyCar) {
        this.btnToyCar.classList.toggle('active', modelName.includes('ToyCar'));
      }
      if (this.btnSponza) {
        this.btnSponza.classList.toggle('active', modelName.includes('Sponza'));
      }
      const select = document.getElementById('model-select');
      if (select) {
        let matched = false;
        for (const opt of select.options) {
          if (opt.value && (opt.value.includes(modelName) || opt.textContent.includes(modelName))) {
            select.value = opt.value;
            matched = true;
            break;
          }
        }
        if (!matched && modelName) {
          let customOpt = Array.from(select.options).find(o => o.value === modelName);
          if (!customOpt) {
            customOpt = document.createElement('option');
            customOpt.value = modelName;
            customOpt.textContent = `Custom: ${modelName}`;
            select.appendChild(customOpt);
          }
          select.value = modelName;
        }
      }
    }
  };

  // ============================================================================
  // Browser Storage Offline Model Cache (CacheStorage API with Memory Fallback)
  // ============================================================================
  const BglModelCache = {
    CACHE_NAME: 'bgl-gltf-models-v1',
    memoryFallback: new Map(),

    async getCache() {
      if ('caches' in window) {
        try {
          return await window.caches.open(this.CACHE_NAME);
        } catch (e) {
          BglLogger.warn('Cache', 'window.caches access error:', e);
        }
      }
      return null;
    },

    async has(url) {
      try {
        const cache = await this.getCache();
        if (cache) {
          const match = await cache.match(url);
          return !!match;
        }
      } catch (e) {
        // fallback
      }
      return this.memoryFallback.has(url);
    },

    async get(url) {
      try {
        const cache = await this.getCache();
        if (cache) {
          const match = await cache.match(url);
          if (match) {
            BglLogger.log('Cache', `CacheStorage HIT for model: ${url}`);
            return await match.arrayBuffer();
          }
        }
      } catch (e) {
        BglLogger.warn('Cache', `Failed reading from CacheStorage for ${url}:`, e);
      }
      if (this.memoryFallback.has(url)) {
        BglLogger.log('Cache', `Memory Cache HIT for model: ${url}`);
        return this.memoryFallback.get(url).slice(0);
      }
      return null;
    },

    async put(url, arrayBuffer) {
      try {
        const cache = await this.getCache();
        if (cache) {
          const response = new Response(arrayBuffer.slice(0), {
            headers: {
              'Content-Type': 'model/gltf-binary',
              'Content-Length': String(arrayBuffer.byteLength),
              'X-Bgl-Cached-At': new Date().toISOString()
            }
          });
          await cache.put(url, response);
          BglLogger.log('Cache', `Successfully cached model (${(arrayBuffer.byteLength / (1024 * 1024)).toFixed(2)} MB): ${url}`);
          await this.updateCacheStatusUI();
          return;
        }
      } catch (e) {
        BglLogger.warn('Cache', `CacheStorage put failed for ${url}:`, e);
      }
      this.memoryFallback.set(url, arrayBuffer.slice(0));
      await this.updateCacheStatusUI();
    },

    async clear() {
      try {
        if ('caches' in window) {
          await window.caches.delete(this.CACHE_NAME);
        }
        this.memoryFallback.clear();
        BglLogger.log('Cache', 'Model cache cleared successfully.');
        await this.updateCacheStatusUI();
        BglNotifier.success('Cache Cleared', 'All locally cached 3D models were purged from browser storage.');
      } catch (e) {
        BglLogger.error('Cache', 'Error clearing model cache:', e);
        BglNotifier.error('Cache Clear Error', 'Failed to purge model cache: ' + e.message);
      }
    },

    async getCachedCount() {
      try {
        const cache = await this.getCache();
        if (cache) {
          const keys = await cache.keys();
          return keys.length;
        }
      } catch (e) {
        // fallback
      }
      return this.memoryFallback.size;
    },

    async updateCacheStatusUI() {
      const count = await this.getCachedCount();
      const indicator = document.getElementById('cache-indicator-pill');
      if (indicator) {
        if (count > 0) {
          indicator.textContent = `⚡ ${count} Model${count > 1 ? 's' : ''} Cached`;
          indicator.classList.add('has-cache');
        } else {
          indicator.textContent = `⚡ Browser Storage Ready`;
          indicator.classList.remove('has-cache');
        }
      }
      await this.updateDropdownBadges();
    },

    async updateDropdownBadges() {
      const select = document.getElementById('model-select');
      if (!select) return;
      for (const opt of select.options) {
        if (!opt.value) continue;
        const isCached = await this.has(opt.value);
        const baseLabel = opt.getAttribute('data-label') || opt.textContent.replace(' ⚡', '');
        opt.setAttribute('data-label', baseLabel);
        opt.textContent = isCached ? `${baseLabel} ⚡` : baseLabel;
      }
    }
  };

  // ============================================================================
  // Dedicated Model Loading & Streaming Progress Screen
  // ============================================================================
  const BglLoadingScreen = {
    screen: null,
    title: null,
    source: null,
    bar: null,
    status: null,
    pct: null,
    statSize: null,
    statSpeed: null,
    statCache: null,
    abortController: null,

    initDom() {
      this.screen = document.getElementById('model-loading-screen');
      this.title = document.getElementById('loading-model-name');
      this.source = document.getElementById('loading-source-label');
      this.bar = document.getElementById('loading-bar-fill');
      this.status = document.getElementById('loading-status-text');
      this.pct = document.getElementById('loading-pct-badge');
      this.statSize = document.getElementById('loading-stat-size');
      this.statSpeed = document.getElementById('loading-stat-speed');
      this.statCache = document.getElementById('loading-stat-cache');
    },

    show(modelName, sourceUrl) {
      if (!this.screen) this.initDom();
      if (!this.screen) return;

      const cleanName = modelName || (sourceUrl ? sourceUrl.split(/[?#]/)[0].split('/').pop() : 'Model.glb');
      const cleanSource = (sourceUrl && sourceUrl.includes('githubusercontent.com'))
        ? 'Khronos glTF-Sample-Assets (GitHub)'
        : (sourceUrl && sourceUrl.startsWith('assets/') ? 'Local Engine Asset' : 'Remote 3D Asset');

      if (this.title) this.title.textContent = `Loading ${cleanName}`;
      if (this.source) this.source.textContent = cleanSource;
      if (this.bar) this.bar.style.width = '0%';
      if (this.pct) this.pct.textContent = '0%';
      if (this.status) this.status.textContent = 'Connecting to asset server...';
      if (this.statSize) this.statSize.textContent = '0.0 / 0.0 MB';
      if (this.statSpeed) this.statSpeed.textContent = '0.0 MB/s';
      if (this.statCache) this.statCache.textContent = 'Streaming';

      this.screen.style.display = 'flex';
      this.abortController = new AbortController();
    },

    update(pct, text, transferredBytes = 0, totalBytes = 0, speedMbS = '0.0', isCached = false) {
      if (!this.screen) this.initDom();
      const roundedPct = Math.min(100, Math.max(0, Math.round(pct)));
      if (this.bar) this.bar.style.width = `${roundedPct}%`;
      if (this.pct) this.pct.textContent = `${roundedPct}%`;
      if (this.status && text) this.status.textContent = text;

      if (this.statSize) {
        if (totalBytes > 0) {
          const curMb = (transferredBytes / (1024 * 1024)).toFixed(1);
          const totMb = (totalBytes / (1024 * 1024)).toFixed(1);
          this.statSize.textContent = `${curMb} / ${totMb} MB`;
        } else if (transferredBytes > 0) {
          this.statSize.textContent = `${(transferredBytes / (1024 * 1024)).toFixed(1)} MB`;
        }
      }

      if (this.statSpeed && speedMbS !== undefined) {
        this.statSpeed.textContent = `${speedMbS} MB/s`;
      }

      if (this.statCache) {
        this.statCache.textContent = isCached ? 'Cached (Offline)' : 'Streaming';
      }
    },

    hide() {
      if (!this.screen) this.initDom();
      if (this.screen) {
        this.screen.style.display = 'none';
      }
      this.abortController = null;
    },

    cancel() {
      if (this.abortController) {
        this.abortController.abort();
        this.abortController = null;
      }
      this.hide();
      BglUI.setProgress(0, 'Download cancelled by user.');
      BglNotifier.warning('Download Cancelled', 'Model download was cancelled by user.');
    }
  };

  // ============================================================================
  // High-Speed Streaming Network Fetcher
  // ============================================================================
  const BglNetwork = {
    /**
     * Download binary resource with real-time streaming progress.
     * @param {string} url
     * @param {function(object): void} onProgress ({ percent, msg, transferredBytes, totalBytes, speedMbS })
     * @param {AbortSignal} [signal]
     * @returns {Promise<ArrayBuffer>}
     */
    async fetchWithProgress(url, onProgress, signal) {
      const startTime = performance.now();
      BglLogger.log('Network', `Initiating fetch for resource: ${url}`);

      let response;
      try {
        response = await fetch(url, { signal });
      } catch (networkErr) {
        if (signal && signal.aborted) {
          throw new Error('Download aborted by user.');
        }
        throw new Error(`Network connection error fetching '${url}': ${networkErr.message}`);
      }

      if (!response.ok) {
        throw new Error(`HTTP ${response.status} (${response.statusText || 'Not Found'}) while fetching '${url}'`);
      }

      const contentLengthHeader = response.headers.get('content-length');
      const totalBytes = contentLengthHeader ? parseInt(contentLengthHeader, 10) : 0;
      const totalMbStr = totalBytes > 0 ? (totalBytes / (1024 * 1024)).toFixed(1) : 'unknown';

      BglLogger.log('Network', `HTTP ${response.status} OK. Content-Length: ${totalBytes.toLocaleString()} bytes (${totalMbStr} MB)`);

      // Fallback for browsers or responses without ReadableStream body
      if (!response.body || !window.ReadableStream) {
        BglLogger.log('Network', 'Streaming not supported on this response body, falling back to arrayBuffer()');
        const buf = await response.arrayBuffer();
        if (onProgress) {
          onProgress({
            percent: 100,
            msg: `Downloaded ${(buf.byteLength / (1024 * 1024)).toFixed(1)} MB`,
            transferredBytes: buf.byteLength,
            totalBytes: buf.byteLength,
            speedMbS: '0.0'
          });
        }
        return buf;
      }

      const reader = response.body.getReader();
      const chunks = [];
      let receivedBytes = 0;
      let lastReportTime = startTime;

      while (true) {
        const { done, value } = await reader.read();
        if (done) break;

        chunks.push(value);
        receivedBytes += value.length;

        const now = performance.now();
        // Report progress every 50ms to maintain 60fps UI
        if (now - lastReportTime > 50 || receivedBytes === totalBytes) {
          lastReportTime = now;
          const elapsedSec = (now - startTime) / 1000;
          const speedMbS = elapsedSec > 0 ? ((receivedBytes / (1024 * 1024)) / elapsedSec).toFixed(1) : '0.0';
          const receivedMb = (receivedBytes / (1024 * 1024)).toFixed(1);

          if (totalBytes > 0) {
            const percent = Math.min(99, Math.round((receivedBytes / totalBytes) * 100));
            const msg = `Downloading ${url.split('/').pop()}: ${receivedMb} / ${totalMbStr} MB (${percent}%)`;
            if (onProgress) {
              onProgress({
                percent,
                msg,
                transferredBytes: receivedBytes,
                totalBytes,
                speedMbS
              });
            }
          } else {
            const msg = `Downloading ${url.split('/').pop()}: ${receivedMb} MB received`;
            if (onProgress) {
              onProgress({
                percent: 50,
                msg,
                transferredBytes: receivedBytes,
                totalBytes: 0,
                speedMbS
              });
            }
          }
        }
      }

      // Combine chunks into a single ArrayBuffer
      const combined = new Uint8Array(receivedBytes);
      let offset = 0;
      for (const chunk of chunks) {
        combined.set(chunk, offset);
        offset += chunk.length;
      }

      const totalElapsed = ((performance.now() - startTime) / 1000).toFixed(2);
      const avgSpeed = (totalElapsed > 0 ? (receivedBytes / (1024 * 1024)) / parseFloat(totalElapsed) : 0).toFixed(1);
      BglLogger.log('Network', `Finished transfer of ${url.split('/').pop()}: ${(receivedBytes / (1024 * 1024)).toFixed(2)} MB in ${totalElapsed}s (avg ${avgSpeed} MB/s)`);

      return combined.buffer;
    }
  };

  // ============================================================================
  // WebAssembly Runtime Bridge
  // ============================================================================
  const BglWasmBridge = {
    /**
     * Compile and instantiate the WebAssembly runtime engine.
     * Uses streaming compilation (WebAssembly.instantiateStreaming) for fastest load times.
     */
    async init() {
      BglLogger.timeStart('Wasm-Init');
      BglUI.setProgress(20, 'Preparing WebAssembly runtime environment...');

      const isCompat = await checkBrowserCompatibility();
      if (!isCompat) {
        BglLogger.warn('Wasm', 'Browser compatibility checks reported issues.');
      }

      if (typeof WebAssembly === 'undefined' || !WebAssembly.instantiate) {
        const msg = 'WebAssembly is not supported or execution is disabled in this browser.';
        BglNotifier.error('WebAssembly Unsupported', msg);
        BglUI.setProgress(100, msg);
        return false;
      }

      try {
        const memory = new WebAssembly.Memory({ initial: 256, maximum: 1024 });
        BglState.wasmMemory = memory;

        const importObject = {
          env: {
            memory: memory
          }
        };

        const wasmUrl = "dist/bgl_engine.wasm";
        let instantiateResult = null;

        // Attempt 1: High-speed streaming compilation
        if (typeof WebAssembly.instantiateStreaming === 'function') {
          try {
            BglLogger.log('Wasm', `Streaming WebAssembly compilation via WebAssembly.instantiateStreaming('${wasmUrl}')...`);
            BglUI.setProgress(40, 'Streaming & compiling WebAssembly module in background...');
            instantiateResult = await WebAssembly.instantiateStreaming(fetch(wasmUrl), importObject);
            BglLogger.log('Wasm', 'WebAssembly streaming compilation succeeded.');
          } catch (streamErr) {
            BglLogger.warn('Wasm', `instantiateStreaming failed (${streamErr.message}), falling back to ArrayBuffer download...`);
          }
        }

        // Attempt 2: ArrayBuffer fallback (e.g. servers without application/wasm MIME header)
        if (!instantiateResult) {
          BglUI.setProgress(50, 'Fetching WebAssembly binary buffer...');
          const response = await fetch(wasmUrl);
          if (!response.ok) {
            throw new Error(`HTTP ${response.status} (${response.statusText}) while fetching '${wasmUrl}'`);
          }
          const bytes = await response.arrayBuffer();
          BglLogger.log('Wasm', `Compiling WebAssembly from ArrayBuffer (${bytes.byteLength} bytes)...`);
          BglUI.setProgress(75, 'Compiling WebAssembly module...');
          instantiateResult = await WebAssembly.instantiate(bytes, importObject);
        }

        BglState.wasmExports = instantiateResult.instance.exports;
        BglLogger.log('Wasm', 'WebAssembly instance successfully created.');

        if (BglState.wasmExports && BglState.wasmExports.bgl_init) {
          BglLogger.log('Wasm', `Calling bgl_init(${window.innerWidth}, ${window.innerHeight})...`);
          BglState.wasmExports.bgl_init(window.innerWidth, window.innerHeight);
        }

        BglLogger.timeEnd('Wasm', 'Wasm-Init', 'WebAssembly engine runtime initialized');
        BglUI.setProgress(100, 'WebAssembly engine initialized and ready.');
        return true;
      } catch (err) {
        BglLogger.error('Wasm', 'Initialization failure:', err);
        BglNotifier.error(
          'WebAssembly Engine Initialization Failed',
          `Could not instantiate the engine runtime: ${err.message}`,
          err.stack || String(err)
        );
        BglUI.setProgress(100, 'WebAssembly initialization failed.');
        return false;
      }
    },

    /**
     * Push raw model buffer into C++ WebAssembly heap memory.
     */
    sendBufferToWasm(name, arrayBuffer) {
      if (!BglState.wasmExports) {
        BglLogger.warn('Wasm', 'Cannot send buffer: WebAssembly runtime not active.');
        return false;
      }

      try {
        const uint8 = new Uint8Array(arrayBuffer);
        const bufPtr = BglState.wasmExports.malloc(uint8.length);
        if (!bufPtr) {
          throw new Error(`Wasm malloc failed to allocate ${uint8.length} bytes`);
        }

        const heap = new Uint8Array(BglState.wasmMemory.buffer);
        heap.set(uint8, bufPtr);

        const nameBytes = new TextEncoder().encode(name + '\0');
        const namePtr = BglState.wasmExports.malloc(nameBytes.length);
        if (!namePtr) {
          BglState.wasmExports.free(bufPtr);
          throw new Error(`Wasm malloc failed to allocate ${nameBytes.length} bytes for name`);
        }

        heap.set(nameBytes, namePtr);
        BglState.wasmExports.bgl_load_file_buffer(namePtr, bufPtr, uint8.length);

        BglState.wasmExports.free(namePtr);
        BglState.wasmExports.free(bufPtr);
        BglLogger.log('Wasm', `Successfully ingested model '${name}' (${uint8.length.toLocaleString()} bytes) into engine memory`);
        return true;
      } catch (err) {
        BglLogger.error('Wasm', `Error feeding buffer '${name}' to Wasm:`, err);
        BglNotifier.error('Engine Buffer Ingestion Error', err.message);
        return false;
      }
    }
  };

  // ============================================================================
  // glTF 2.0 / GLB Binary Parser & Hierarchy Inspector
  // ============================================================================
  const BglGltf = {
    /**
     * Extract JSON text from GLB binary container or plain text glTF.
     */
    extractJson(arrayBuffer) {
      if (!arrayBuffer || arrayBuffer.byteLength < 4) {
        throw new Error('Buffer is empty or too short to be a valid 3D model.');
      }

      const dataView = new DataView(arrayBuffer);
      const magic = dataView.getUint32(0, true);

      // Binary GLB check: magic 0x46546C67 ("glTF")
      if (magic === 0x46546C67) {
        const version = dataView.getUint32(4, true);
        const totalLength = dataView.getUint32(8, true);
        BglLogger.log('glTF', `GLB container detected: Version ${version}, Total Length: ${totalLength.toLocaleString()} bytes`);

        const chunkLength = dataView.getUint32(12, true);
        const chunkType = dataView.getUint32(16, true);

        // Chunk 0 must be JSON (0x4E4F534A -> "JSON")
        if (chunkType === 0x4E4F534A) {
          const jsonBytes = new Uint8Array(arrayBuffer, 20, chunkLength);
          const jsonStr = new TextDecoder().decode(jsonBytes);
          return JSON.parse(jsonStr);
        } else {
          throw new Error(`Invalid GLB: First chunk type 0x${chunkType.toString(16)} is not JSON (0x4E4F534A).`);
        }
      }

      // Plain JSON glTF text
      const jsonStr = new TextDecoder().decode(new Uint8Array(arrayBuffer));
      return JSON.parse(jsonStr);
    },

    /**
     * Compute and display spatial AABB telemetry from glTF accessors.
     */
    updateTelemetry(gltf) {
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

  // ============================================================================
  // High-Performance WebGL 2 (GLES 3.0) Fallback 3D Renderer
  // ============================================================================
  const BglWebGlRenderer = {
    gl: null,
    canvas: null,
    program: null,
    isActive: false,
    primitives: [],
    defaultTexture: null,

    camera: {
      target: [0.0, 0.0, 0.0],
      distance: 3.5,
      yaw: 0.45,
      pitch: 0.35,
      fov: (45 * Math.PI) / 180,
      near: 0.05,
      far: 500.0,
      minDistance: 0.2,
      maxDistance: 100.0,
    },

    isDragging: false,
    lastMouseX: 0,
    lastMouseY: 0,
    dragButton: 0,
    wireframe: false,
    turntable: false,
    turntableSpeed: 0.35,
    lastFrameTime: performance.now(),
    modelBounds: null,
    uniforms: {},

    mat4: {
      create() {
        const out = new Float32Array(16);
        out[0] = 1; out[5] = 1; out[10] = 1; out[15] = 1;
        return out;
      },
      perspective(out, fovy, aspect, near, far) {
        const f = 1.0 / Math.tan(fovy / 2);
        const nf = 1.0 / (near - far);
        out[0] = f / aspect; out[1] = 0; out[2] = 0; out[3] = 0;
        out[4] = 0; out[5] = f; out[6] = 0; out[7] = 0;
        out[8] = 0; out[9] = 0; out[10] = (far + near) * nf; out[11] = -1;
        out[12] = 0; out[13] = 0; out[14] = (2 * far * near) * nf; out[15] = 0;
        return out;
      },
      lookAt(out, eye, target, up) {
        let z0 = eye[0] - target[0], z1 = eye[1] - target[1], z2 = eye[2] - target[2];
        let len = 1 / Math.hypot(z0, z1, z2);
        z0 *= len; z1 *= len; z2 *= len;

        let x0 = up[1] * z2 - up[2] * z1, x1 = up[2] * z0 - up[0] * z2, x2 = up[0] * z1 - up[1] * z0;
        len = Math.hypot(x0, x1, x2);
        if (len > 0.00001) { len = 1 / len; x0 *= len; x1 *= len; x2 *= len; }

        let y0 = z1 * x2 - z2 * x1, y1 = z2 * x0 - z0 * x2, y2 = z0 * x1 - z1 * x0;

        out[0] = x0; out[1] = y0; out[2] = z0; out[3] = 0;
        out[4] = x1; out[5] = y1; out[6] = z1; out[7] = 0;
        out[8] = x2; out[9] = y2; out[10] = z2; out[11] = 0;
        out[12] = -(x0 * eye[0] + x1 * eye[1] + x2 * eye[2]);
        out[13] = -(y0 * eye[0] + y1 * eye[1] + y2 * eye[2]);
        out[14] = -(z0 * eye[0] + z1 * eye[1] + z2 * eye[2]);
        out[15] = 1;
        return out;
      },
      multiply(out, a, b) {
        const a00 = a[0], a01 = a[1], a02 = a[2], a03 = a[3];
        const a10 = a[4], a11 = a[5], a12 = a[6], a13 = a[7];
        const a20 = a[8], a21 = a[9], a22 = a[10], a23 = a[11];
        const a30 = a[12], a31 = a[13], a32 = a[14], a33 = a[15];

        let b0 = b[0], b1 = b[1], b2 = b[2], b3 = b[3];
        out[0] = b0 * a00 + b1 * a10 + b2 * a20 + b3 * a30;
        out[1] = b0 * a01 + b1 * a11 + b2 * a21 + b3 * a31;
        out[2] = b0 * a02 + b1 * a12 + b2 * a22 + b3 * a32;
        out[3] = b0 * a03 + b1 * a13 + b2 * a23 + b3 * a33;

        b0 = b[4]; b1 = b[5]; b2 = b[6]; b3 = b[7];
        out[4] = b0 * a00 + b1 * a10 + b2 * a20 + b3 * a30;
        out[5] = b0 * a01 + b1 * a11 + b2 * a21 + b3 * a31;
        out[6] = b0 * a02 + b1 * a12 + b2 * a22 + b3 * a32;
        out[7] = b0 * a03 + b1 * a13 + b2 * a23 + b3 * a33;

        b0 = b[8]; b1 = b[9]; b2 = b[10]; b3 = b[11];
        out[8] = b0 * a00 + b1 * a10 + b2 * a20 + b3 * a30;
        out[9] = b0 * a01 + b1 * a11 + b2 * a21 + b3 * a31;
        out[10] = b0 * a02 + b1 * a12 + b2 * a22 + b3 * a32;
        out[11] = b0 * a03 + b1 * a13 + b2 * a23 + b3 * a33;

        b0 = b[12]; b1 = b[13]; b2 = b[14]; b3 = b[15];
        out[12] = b0 * a00 + b1 * a10 + b2 * a20 + b3 * a30;
        out[13] = b0 * a01 + b1 * a11 + b2 * a21 + b3 * a31;
        out[14] = b0 * a02 + b1 * a12 + b2 * a22 + b3 * a32;
        out[15] = b0 * a03 + b1 * a13 + b2 * a23 + b3 * a33;
        return out;
      },
      normalMatrix(out, m4) {
        out[0] = m4[0]; out[1] = m4[1]; out[2] = m4[2];
        out[3] = m4[4]; out[4] = m4[5]; out[5] = m4[6];
        out[6] = m4[8]; out[7] = m4[9]; out[8] = m4[10];
        return out;
      }
    },

    init() {
      this.canvas = document.getElementById('canvas');
      if (!this.canvas) return false;

      try {
        const gl = this.canvas.getContext('webgl2', {
          antialias: true,
          alpha: false,
          depth: true,
          stencil: false,
          powerPreference: 'high-performance'
        });
        if (!gl) {
          BglLogger.warn('WebGL', 'WebGL 2 context creation returned null.');
          return false;
        }

        this.gl = gl;
        this.isActive = true;

        if (!this.initShaders()) {
          this.isActive = false;
          return false;
        }

        this.initEvents();
        this.startLoop();
        BglLogger.log('WebGL', 'WebGL 2 fallback pipeline initialized successfully.');

        if (BglState.currentGltf && BglState.currentModelBuffer) {
          BglLogger.log('WebGL', `Immediately loading pre-parsed model '${BglState.currentModelName}' into WebGL 2.`);
          this.loadModel(BglState.currentGltf, BglState.currentModelBuffer, BglState.currentModelName);
        }

        return true;
      } catch (err) {
        BglLogger.error('WebGL', 'Exception initializing WebGL 2:', err);
        return false;
      }
    },

    initShaders() {
      const gl = this.gl;
      const vsSrc = `#version 300 es
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUv;

uniform mat4 uModel;
uniform mat4 uViewProj;
uniform mat3 uNormalMatrix;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUv;

void main() {
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    vWorldPos = worldPos.xyz;
    vNormal = normalize(uNormalMatrix * aNormal);
    vUv = aUv;
    gl_Position = uViewProj * worldPos;
}
`;

      const fsSrc = `#version 300 es
precision highp float;

in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUv;

uniform vec3 uCameraPos;
uniform vec3 uLightDir;
uniform vec4 uBaseColorFactor;
uniform sampler2D uBaseColorTexture;
uniform bool uHasBaseColorTexture;
uniform bool uWireframe;

out vec4 fragColor;

void main() {
    if (uWireframe) {
        fragColor = vec4(0.22, 0.74, 0.97, 1.0);
        return;
    }

    vec4 baseColor = uBaseColorFactor;
    if (uHasBaseColorTexture) {
        baseColor *= texture(uBaseColorTexture, vUv);
    }
    if (baseColor.a < 0.1) discard;

    vec3 N = normalize(vNormal);
    vec3 L = normalize(uLightDir);
    vec3 V = normalize(uCameraPos - vWorldPos);
    vec3 H = normalize(L + V);

    float NdotL = max(dot(N, L), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float spec = pow(NdotH, 32.0) * 0.35;

    vec3 ambient = vec3(0.18, 0.20, 0.25) * baseColor.rgb;
    vec3 diffuse = baseColor.rgb * vec3(1.0, 0.98, 0.94) * NdotL;
    vec3 specular = vec3(1.0) * spec * (NdotL > 0.0 ? 1.0 : 0.0);

    vec3 color = ambient + diffuse + specular;
    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));

    fragColor = vec4(color, baseColor.a);
}
`;

      const compile = (type, src) => {
        const s = gl.createShader(type);
        gl.shaderSource(s, src);
        gl.compileShader(s);
        if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
          const log = gl.getShaderInfoLog(s);
          gl.deleteShader(s);
          throw new Error(log);
        }
        return s;
      };

      try {
        const vs = compile(gl.VERTEX_SHADER, vsSrc);
        const fs = compile(gl.FRAGMENT_SHADER, fsSrc);
        const prog = gl.createProgram();
        gl.attachShader(prog, vs);
        gl.attachShader(prog, fs);
        gl.linkProgram(prog);
        if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
          const log = gl.getProgramInfoLog(prog);
          gl.deleteProgram(prog);
          throw new Error(log);
        }

        this.program = prog;
        this.uniforms = {
          uModel: gl.getUniformLocation(prog, 'uModel'),
          uViewProj: gl.getUniformLocation(prog, 'uViewProj'),
          uNormalMatrix: gl.getUniformLocation(prog, 'uNormalMatrix'),
          uCameraPos: gl.getUniformLocation(prog, 'uCameraPos'),
          uLightDir: gl.getUniformLocation(prog, 'uLightDir'),
          uBaseColorFactor: gl.getUniformLocation(prog, 'uBaseColorFactor'),
          uBaseColorTexture: gl.getUniformLocation(prog, 'uBaseColorTexture'),
          uHasBaseColorTexture: gl.getUniformLocation(prog, 'uHasBaseColorTexture'),
          uWireframe: gl.getUniformLocation(prog, 'uWireframe'),
        };

        const defaultTex = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, defaultTex);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([255, 255, 255, 255]));
        this.defaultTexture = defaultTex;

        return true;
      } catch (err) {
        BglLogger.error('WebGL', 'Shader compilation error:', err);
        return false;
      }
    },

    initEvents() {
      const canvas = this.canvas;
      canvas.addEventListener('contextmenu', (e) => e.preventDefault());

      canvas.addEventListener('mousedown', (e) => {
        this.isDragging = true;
        this.lastMouseX = e.clientX;
        this.lastMouseY = e.clientY;
        this.dragButton = e.button;
      });

      window.addEventListener('mousemove', (e) => {
        if (!this.isDragging) return;
        const dx = e.clientX - this.lastMouseX;
        const dy = e.clientY - this.lastMouseY;
        this.lastMouseX = e.clientX;
        this.lastMouseY = e.clientY;

        if (this.dragButton === 0) {
          this.camera.yaw += dx * 0.006;
          this.camera.pitch = Math.max(-1.45, Math.min(1.45, this.camera.pitch - dy * 0.006));
        } else if (this.dragButton === 2) {
          const factor = this.camera.distance * 0.0015;
          const cosY = Math.cos(this.camera.yaw);
          const sinY = Math.sin(this.camera.yaw);
          this.camera.target[0] -= (cosY * dx) * factor;
          this.camera.target[1] += dy * factor;
          this.camera.target[2] -= (sinY * dx) * factor;
        }
      });

      window.addEventListener('mouseup', () => {
        this.isDragging = false;
      });

      canvas.addEventListener('wheel', (e) => {
        e.preventDefault();
        const factor = e.deltaY > 0 ? 1.1 : 0.9;
        this.camera.distance = Math.max(this.camera.minDistance, Math.min(this.camera.maxDistance, this.camera.distance * factor));
      }, { passive: false });

      // Mobile touch controls (Samsung S22, smartphones, tablets)
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
        if (e.touches.length === 1) {
          const dx = e.touches[0].clientX - touchLastPos.x;
          const dy = e.touches[0].clientY - touchLastPos.y;
          touchLastPos = { x: e.touches[0].clientX, y: e.touches[0].clientY };
          this.camera.yaw += dx * 0.006;
          this.camera.pitch = Math.max(-1.45, Math.min(1.45, this.camera.pitch - dy * 0.006));
        } else if (e.touches.length === 2) {
          const curDist = Math.hypot(
            e.touches[0].clientX - e.touches[1].clientX,
            e.touches[0].clientY - e.touches[1].clientY
          );
          const deltaDist = curDist - touchStartDist;
          touchStartDist = curDist;
          if (Math.abs(deltaDist) > 0.5) {
            const factor = deltaDist > 0 ? 0.96 : 1.04;
            this.camera.distance = Math.max(this.camera.minDistance, Math.min(this.camera.maxDistance, this.camera.distance * factor));
          }

          const curMid = {
            x: (e.touches[0].clientX + e.touches[1].clientX) * 0.5,
            y: (e.touches[0].clientY + e.touches[1].clientY) * 0.5
          };
          const dMidX = curMid.x - touchLastMid.x;
          const dMidY = curMid.y - touchLastMid.y;
          touchLastMid = curMid;
          if (Math.abs(dMidX) > 0.5 || Math.abs(dMidY) > 0.5) {
            const factor = this.camera.distance * 0.002;
            const cosY = Math.cos(this.camera.yaw);
            const sinY = Math.sin(this.camera.yaw);
            this.camera.target[0] -= (cosY * dMidX) * factor;
            this.camera.target[1] += dMidY * factor;
            this.camera.target[2] -= (sinY * dMidX) * factor;
          }
        }
      }, { passive: false });
    },

    loadModel(gltf, arrayBuffer, modelName) {
      if (!this.gl || !gltf) return;
      const gl = this.gl;

      for (const p of this.primitives) {
        if (p.vao) gl.deleteVertexArray(p.vao);
      }
      this.primitives = [];

      let binBuffer = null;
      const dataView = new DataView(arrayBuffer);
      if (dataView.getUint32(0, true) === 0x46546C67) {
        const jsonChunkLen = dataView.getUint32(12, true);
        const binHeaderOffset = 20 + jsonChunkLen;
        if (binHeaderOffset + 8 <= arrayBuffer.byteLength) {
          const binLen = dataView.getUint32(binHeaderOffset, true);
          binBuffer = arrayBuffer.slice(binHeaderOffset + 8, binHeaderOffset + 8 + binLen);
        }
      }

      const getBufferViewData = (viewIdx) => {
        if (!gltf.bufferViews || !gltf.bufferViews[viewIdx]) return null;
        const bv = gltf.bufferViews[viewIdx];
        const offset = bv.byteOffset || 0;
        const length = bv.byteLength;
        const src = binBuffer || arrayBuffer;
        return src.slice(offset, offset + length);
      };

      const getAccessorData = (accIdx) => {
        if (!gltf.accessors || !gltf.accessors[accIdx]) return null;
        const acc = gltf.accessors[accIdx];
        if (acc.bufferView === undefined) return null;
        const bvData = getBufferViewData(acc.bufferView);
        if (!bvData) return null;
        const byteOffset = acc.byteOffset || 0;

        let TypedArray = Float32Array;
        if (acc.componentType === 5126) TypedArray = Float32Array;
        else if (acc.componentType === 5123) TypedArray = Uint16Array;
        else if (acc.componentType === 5125) TypedArray = Uint32Array;
        else if (acc.componentType === 5121) TypedArray = Uint8Array;
        else if (acc.componentType === 5122) TypedArray = Int16Array;

        const numComp = (acc.type === 'SCALAR' ? 1 : acc.type === 'VEC2' ? 2 : acc.type === 'VEC3' ? 3 : 4);
        return new TypedArray(bvData, byteOffset, acc.count * numComp);
      };

      // Compute world transforms for scene graph nodes
      const meshNodeMatrices = new Map();
      if (gltf.nodes) {
        const computeNodeMatrix = (nodeIdx, parentMat) => {
          const node = gltf.nodes[nodeIdx];
          if (!node) return;
          let localMat = this.mat4.create();
          if (node.matrix) {
            for (let i = 0; i < 16; ++i) localMat[i] = node.matrix[i];
          } else {
            const t = node.translation || [0, 0, 0];
            const r = node.rotation || [0, 0, 0, 1];
            const s = node.scale || [1, 1, 1];
            const x2 = r[0] + r[0], y2 = r[1] + r[1], z2 = r[2] + r[2];
            const xx = r[0] * x2, xy = r[0] * y2, xz = r[0] * z2;
            const yy = r[1] * y2, yz = r[1] * z2, zz = r[2] * z2;
            const wx = r[3] * x2, wy = r[3] * y2, wz = r[3] * z2;

            localMat[0] = (1 - (yy + zz)) * s[0];
            localMat[1] = (xy + wz) * s[0];
            localMat[2] = (xz - wy) * s[0];
            localMat[3] = 0;

            localMat[4] = (xy - wz) * s[1];
            localMat[5] = (1 - (xx + zz)) * s[1];
            localMat[6] = (yz + wx) * s[1];
            localMat[7] = 0;

            localMat[8] = (xz + wy) * s[2];
            localMat[9] = (yz - wx) * s[2];
            localMat[10] = (1 - (xx + yy)) * s[2];
            localMat[11] = 0;

            localMat[12] = t[0];
            localMat[13] = t[1];
            localMat[14] = t[2];
            localMat[15] = 1;
          }

          let worldMat = this.mat4.create();
          if (parentMat) {
            this.mat4.multiply(worldMat, parentMat, localMat);
          } else {
            worldMat = localMat;
          }

          if (node.mesh !== undefined) {
            if (!meshNodeMatrices.has(node.mesh)) {
              meshNodeMatrices.set(node.mesh, []);
            }
            meshNodeMatrices.get(node.mesh).push(worldMat);
          }

          if (node.children) {
            for (const childIdx of node.children) {
              computeNodeMatrix(childIdx, worldMat);
            }
          }
        };

        const sceneIdx = gltf.scene || 0;
        const rootNodes = (gltf.scenes && gltf.scenes[sceneIdx] && gltf.scenes[sceneIdx].nodes)
          ? gltf.scenes[sceneIdx].nodes
          : gltf.nodes.map((_, i) => i);

        for (const rootIdx of rootNodes) {
          computeNodeMatrix(rootIdx, null);
        }
      }

      let minBound = [Infinity, Infinity, Infinity];
      let maxBound = [-Infinity, -Infinity, -Infinity];

      if (gltf.meshes) {
        gltf.meshes.forEach((mesh, meshIdx) => {
          if (!mesh.primitives) return;
          const nodeMats = meshNodeMatrices.get(meshIdx) || [this.mat4.create()];

          for (const prim of mesh.primitives) {
            if (!prim.attributes || prim.attributes.POSITION === undefined) continue;

            const posData = getAccessorData(prim.attributes.POSITION);
            if (!posData) continue;

            const normData = prim.attributes.NORMAL !== undefined ? getAccessorData(prim.attributes.NORMAL) : null;
            const uvData = prim.attributes.TEXCOORD_0 !== undefined ? getAccessorData(prim.attributes.TEXCOORD_0) : null;
            const indexData = prim.indices !== undefined ? getAccessorData(prim.indices) : null;

            for (const worldMatrix of nodeMats) {
              for (let i = 0; i < posData.length; i += 3) {
                const lx = posData[i], ly = posData[i + 1], lz = posData[i + 2];
                const wx = worldMatrix[0] * lx + worldMatrix[4] * ly + worldMatrix[8] * lz + worldMatrix[12];
                const wy = worldMatrix[1] * lx + worldMatrix[5] * ly + worldMatrix[9] * lz + worldMatrix[13];
                const wz = worldMatrix[2] * lx + worldMatrix[6] * ly + worldMatrix[10] * lz + worldMatrix[14];
                minBound[0] = Math.min(minBound[0], wx); maxBound[0] = Math.max(maxBound[0], wx);
                minBound[1] = Math.min(minBound[1], wy); maxBound[1] = Math.max(maxBound[1], wy);
                minBound[2] = Math.min(minBound[2], wz); maxBound[2] = Math.max(maxBound[2], wz);
              }
            }

            const vao = gl.createVertexArray();
            gl.bindVertexArray(vao);

            const pBuf = gl.createBuffer();
            gl.bindBuffer(gl.ARRAY_BUFFER, pBuf);
            gl.bufferData(gl.ARRAY_BUFFER, posData, gl.STATIC_DRAW);
            gl.enableVertexAttribArray(0);
            gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);

            if (normData) {
              const nBuf = gl.createBuffer();
              gl.bindBuffer(gl.ARRAY_BUFFER, nBuf);
              gl.bufferData(gl.ARRAY_BUFFER, normData, gl.STATIC_DRAW);
              gl.enableVertexAttribArray(1);
              gl.vertexAttribPointer(1, 3, gl.FLOAT, false, 0, 0);
            }

            if (uvData) {
              const uBuf = gl.createBuffer();
              gl.bindBuffer(gl.ARRAY_BUFFER, uBuf);
              gl.bufferData(gl.ARRAY_BUFFER, uvData, gl.STATIC_DRAW);
              gl.enableVertexAttribArray(2);
              gl.vertexAttribPointer(2, 2, gl.FLOAT, false, 0, 0);
            }

            let indexType = gl.UNSIGNED_SHORT;
            let indexCount = 0;
            if (indexData) {
              const iBuf = gl.createBuffer();
              gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, iBuf);
              gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, indexData, gl.STATIC_DRAW);
              indexCount = indexData.length;
              indexType = (indexData instanceof Uint32Array) ? gl.UNSIGNED_INT : gl.UNSIGNED_SHORT;
            }

            gl.bindVertexArray(null);

            let baseColorFactor = [1.0, 1.0, 1.0, 1.0];
            let texture = null;
            if (prim.material !== undefined && gltf.materials && gltf.materials[prim.material]) {
              const mat = gltf.materials[prim.material];
              if (mat.pbrMetallicRoughness) {
                if (mat.pbrMetallicRoughness.baseColorFactor) {
                  baseColorFactor = mat.pbrMetallicRoughness.baseColorFactor;
                }
                const texInfo = mat.pbrMetallicRoughness.baseColorTexture;
                if (texInfo && gltf.textures && gltf.textures[texInfo.index]) {
                  const texObj = gltf.textures[texInfo.index];
                  if (texObj.source !== undefined && gltf.images && gltf.images[texObj.source]) {
                    const imgObj = gltf.images[texObj.source];
                    if (imgObj.bufferView !== undefined) {
                      const imgBytes = getBufferViewData(imgObj.bufferView);
                      if (imgBytes) {
                        const blob = new Blob([imgBytes], { type: imgObj.mimeType || 'image/png' });
                        createImageBitmap(blob).then((bmp) => {
                          const t = gl.createTexture();
                          gl.bindTexture(gl.TEXTURE_2D, t);
                          gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, bmp);
                          gl.generateMipmap(gl.TEXTURE_2D);
                          gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_LINEAR);
                          gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
                          for (const pr of nodeMats) {
                            pr._texRef = t;
                          }
                        }).catch(() => {});
                      }
                    }
                  }
                }
              }
            }

            for (const worldMatrix of nodeMats) {
              const primRecord = {
                vao,
                indexCount,
                indexType,
                vertexCount: posData.length / 3,
                hasIndices: !!indexData,
                baseColorFactor,
                texture,
                modelMatrix: worldMatrix,
                get currentTexture() { return this._texRef || this.texture; }
              };
              this.primitives.push(primRecord);
            }
          }
        });
      }

      if (minBound[0] <= maxBound[0]) {
        const cx = (minBound[0] + maxBound[0]) * 0.5;
        const cy = (minBound[1] + maxBound[1]) * 0.5;
        const cz = (minBound[2] + maxBound[2]) * 0.5;
        const dx = maxBound[0] - minBound[0];
        const dy = maxBound[1] - minBound[1];
        const dz = maxBound[2] - minBound[2];
        const radius = Math.max(Math.hypot(dx, dy, dz) * 0.5, 0.5);

        this.camera.target = [cx, cy, cz];
        this.camera.distance = radius * 2.2;
        this.camera.near = Math.max(0.01, radius * 0.02);
        this.camera.far = Math.max(100.0, radius * 30.0);
        this.modelBounds = { center: [cx, cy, cz], radius };
      }

      BglLogger.log('WebGL', `Loaded '${modelName}' into WebGL 2 fallback pipeline (${this.primitives.length} primitives).`);
    },

    startLoop() {
      const render = (time) => {
        if (!this.isActive || !this.gl) return;
        const dt = (time - this.lastFrameTime) * 0.001;
        this.lastFrameTime = time;

        if (this.turntable) {
          this.camera.yaw += this.turntableSpeed * dt;
        }

        this.draw();
        requestAnimationFrame(render);
      };
      requestAnimationFrame(render);
    },

    draw() {
      const gl = this.gl;
      if (!gl || !this.program) return;

      const dpr = Math.min(window.devicePixelRatio || 1, 2);
      const w = Math.round((this.canvas.clientWidth || window.innerWidth) * dpr);
      const h = Math.round((this.canvas.clientHeight || window.innerHeight) * dpr);
      if (this.canvas.width !== w || this.canvas.height !== h) {
        this.canvas.width = w;
        this.canvas.height = h;
      }

      gl.viewport(0, 0, w, h);
      gl.enable(gl.DEPTH_TEST);
      gl.enable(gl.CULL_FACE);
      gl.cullFace(gl.BACK);

      gl.clearColor(0.04, 0.06, 0.10, 1.0);
      gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

      gl.useProgram(this.program);

      const cy = Math.cos(this.camera.yaw);
      const sy = Math.sin(this.camera.yaw);
      const cp = Math.cos(this.camera.pitch);
      const sp = Math.sin(this.camera.pitch);

      const eye = [
        this.camera.target[0] + this.camera.distance * cp * sy,
        this.camera.target[1] + this.camera.distance * sp,
        this.camera.target[2] + this.camera.distance * cp * cy,
      ];

      const proj = this.mat4.create();
      this.mat4.perspective(proj, this.camera.fov, w / Math.max(1, h), this.camera.near, this.camera.far);

      const view = this.mat4.create();
      this.mat4.lookAt(view, eye, this.camera.target, [0, 1, 0]);

      const viewProj = this.mat4.create();
      this.mat4.multiply(viewProj, proj, view);

      gl.uniformMatrix4fv(this.uniforms.uViewProj, false, viewProj);
      gl.uniform3fv(this.uniforms.uCameraPos, eye);

      const tod = BglState.timeOfDay || 14.0;
      const sunAngle = ((tod - 6.0) / 18.0) * Math.PI;
      const lightDir = [Math.cos(sunAngle) * 0.7, Math.sin(sunAngle), 0.35];
      gl.uniform3fv(this.uniforms.uLightDir, lightDir);
      gl.uniform1i(this.uniforms.uWireframe, this.wireframe ? 1 : 0);

      const normalMat = new Float32Array(9);
      for (const prim of this.primitives) {
        gl.uniformMatrix4fv(this.uniforms.uModel, false, prim.modelMatrix);
        this.mat4.normalMatrix(normalMat, prim.modelMatrix);
        gl.uniformMatrix3fv(this.uniforms.uNormalMatrix, false, normalMat);

        gl.uniform4fv(this.uniforms.uBaseColorFactor, prim.baseColorFactor);

        const activeTex = prim.currentTexture || prim.texture;
        if (activeTex) {
          gl.activeTexture(gl.TEXTURE0);
          gl.bindTexture(gl.TEXTURE_2D, activeTex);
          gl.uniform1i(this.uniforms.uBaseColorTexture, 0);
          gl.uniform1i(this.uniforms.uHasBaseColorTexture, 1);
        } else {
          gl.activeTexture(gl.TEXTURE0);
          gl.bindTexture(gl.TEXTURE_2D, this.defaultTexture);
          gl.uniform1i(this.uniforms.uBaseColorTexture, 0);
          gl.uniform1i(this.uniforms.uHasBaseColorTexture, 0);
        }

        gl.bindVertexArray(prim.vao);
        if (prim.hasIndices) {
          gl.drawElements(this.wireframe ? gl.LINES : gl.TRIANGLES, prim.indexCount, prim.indexType, 0);
        } else {
          gl.drawArrays(this.wireframe ? gl.LINES : gl.TRIANGLES, 0, prim.vertexCount);
        }
      }
      gl.bindVertexArray(null);
    }
  };

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

  async function ingestModel(name, arrayBuffer) {
    BglLogger.timeStart('Model-Ingest');
    BglLogger.log('glTF', `Ingesting 3D model '${name}' (${arrayBuffer.byteLength.toLocaleString()} bytes)...`);

    // 1. Pass to WebAssembly C++ engine
    BglWasmBridge.sendBufferToWasm(name, arrayBuffer);

    // 2. Parse glTF for live inspector & telemetry
    try {
      const gltf = BglGltf.extractJson(arrayBuffer);
      BglState.currentGltf = gltf;
      BglState.currentModelBuffer = arrayBuffer;
      BglState.currentModelName = name;

      const modelLabel = document.getElementById('model-name-label');
      const countBadge = document.getElementById('node-count-badge');
      if (modelLabel) modelLabel.textContent = name;
      const nodes = gltf.nodes || [];
      if (countBadge) countBadge.textContent = `${nodes.length} Nodes`;

      BglGltf.renderTreeDiagram(gltf);
      BglGltf.renderJsonInspector(gltf);
      BglGltf.updateTelemetry(gltf);
      BglUI.setActiveModelButton(name);
      updateCameraDropdown(gltf);

      // Forward to WebGL 2 renderer if running in compatibility mode
      if (BglWebGlRenderer.isActive) {
        BglWebGlRenderer.loadModel(gltf, arrayBuffer, name);
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

    // 3. WebGPU Support Check with Automatic WebGL 2 Fallback
    const urlParams = new URLSearchParams(window.location.search);
    const forceWebGl = urlParams.has('webgl') || urlParams.get('backend') === 'webgl' || urlParams.get('backend') === 'webgl2';
    const forceWebGpu = urlParams.get('backend') === 'webgpu';

    const hasGpuApi = Boolean(navigator && ('gpu' in navigator) && navigator.gpu);
    let gpuOperational = false;

    if (hasGpuApi && !forceWebGl) {
      try {
        // Test WebGPU canvas context creation on isolated test canvas without locking the main canvas
        const testCanvas = document.createElement('canvas');
        testCanvas.width = 1;
        testCanvas.height = 1;
        const testCtx = testCanvas.getContext('webgpu');
        if (!testCtx) {
          throw new Error('canvas.getContext("webgpu") returned null');
        }

        const adapter = await navigator.gpu.requestAdapter();
        if (!adapter) {
          throw new Error('navigator.gpu.requestAdapter() returned null');
        }

        // Test actual device creation (often fails on mobile GPUs like Samsung S22 Exynos/Adreno)
        const device = await adapter.requestDevice();
        if (!device) {
          throw new Error('adapter.requestDevice() returned null');
        }

        gpuOperational = true;
        if (adapter.info) {
          window.cachedGpuInfo = adapter.info;
          BglLogger.log('WebGPU', `Acquired GPU Adapter: ${adapter.info.vendor || 'Unknown'} - ${adapter.info.architecture || ''} (${adapter.info.description || ''})`);
        } else {
          BglLogger.log('WebGPU', 'Acquired compatible WebGPU hardware adapter.');
        }

        if (device.destroy) {
          device.destroy();
        }
      } catch (e) {
        BglLogger.warn('WebGPU', 'Hardware WebGPU validation failed, activating WebGL 2 fallback:', e.message || e);
        gpuOperational = false;
      }
    } else if (forceWebGl) {
      BglLogger.log('Engine', 'WebGL 2 forced via query parameter (?webgl or ?backend=webgl).');
      gpuOperational = false;
    }

    if (!gpuOperational) {
      BglLogger.log('Engine', 'Initializing high-compatibility WebGL 2 fallback pipeline...');
      const webglOk = BglWebGlRenderer.init();
      if (webglOk) {
        BglState.backend = 'webgl2';
        const sub = document.querySelector('.hud-subtitle');
        if (sub) sub.textContent = 'WebGL 2 & Wasm (Compatibility Mode)';
        BglNotifier.info(
          'WebGL 2 Active',
          'Running in high-compatibility WebGL 2 mode for mobile and legacy hardware.'
        );
        return true;
      }
      issues.push({
        feature: '3D Graphics Acceleration (WebGPU / WebGL 2)',
        detail: 'Neither WebGPU nor WebGL 2 could be initialized on this browser. Verify hardware graphics acceleration settings in your browser.'
      });
    }

    if (issues.length > 0 || warnings.length > 0) {
      showCompatibilityWarning(issues, warnings);
      return false;
    }

    // Clear and prominent startup capability check notification
    const vendorStr = (window.cachedGpuInfo && window.cachedGpuInfo.vendor) ? `(${window.cachedGpuInfo.vendor})` : '';
    BglNotifier.success(
      'WebGPU & WebAssembly Operational',
      `System verified: Native WebAssembly SIMD runtime and WebGPU graphics pipeline active ${vendorStr}.`
    );
    return true;
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
    if (BglWebGlRenderer.isActive) {
      BglWebGlRenderer.wireframe = BglState.isBboxActive;
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

  function focusSelection() {
    BglLogger.log('Engine', 'Focusing camera on selection / scene bounds');
    if (BglWebGlRenderer.isActive && BglWebGlRenderer.modelBounds) {
      BglWebGlRenderer.camera.target = [...BglWebGlRenderer.modelBounds.center];
      BglWebGlRenderer.camera.distance = BglWebGlRenderer.modelBounds.radius * 2.2;
    }
    if (BglState.wasmExports && BglState.wasmExports.bgl_camera_focus_selection) {
      BglState.wasmExports.bgl_camera_focus_selection();
    }
  }
  window.focusSelection = focusSelection;

  function frameAll() {
    BglLogger.log('Engine', 'Framing all scene geometry');
    if (BglWebGlRenderer.isActive && BglWebGlRenderer.modelBounds) {
      BglWebGlRenderer.camera.target = [...BglWebGlRenderer.modelBounds.center];
      BglWebGlRenderer.camera.distance = BglWebGlRenderer.modelBounds.radius * 2.2;
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
    if (BglWebGlRenderer.isActive) {
      BglWebGlRenderer.turntable = BglState.isTurntableActive;
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
    if (BglWebGlRenderer.isActive) {
      BglWebGlRenderer.turntableSpeed = num;
    }
    if (BglState.wasmExports && BglState.wasmExports.bgl_set_turntable_speed) {
      BglState.wasmExports.bgl_set_turntable_speed(num);
    }
  }
  window.onTurntableSpeedSlider = onTurntableSpeedSlider;

  function onTimeOfDaySlider(val) {
    const num = Number(val);
    const hrs = Math.floor(num);
    const mins = Math.round((num - hrs) * 60);
    const label = document.getElementById('val-time-of-day');
    if (label) label.textContent = `${String(hrs).padStart(2, '0')}:${String(mins).padStart(2, '0')}`;
    if (BglWebGlRenderer.isActive) {
      BglState.timeOfDay = num;
    }
    if (BglState.wasmExports && BglState.wasmExports.bgl_set_time_of_day) {
      BglState.wasmExports.bgl_set_time_of_day(num);
    }
  }
  window.onTimeOfDaySlider = onTimeOfDaySlider;

  function saveBookmark(slot) {
    if (BglState.wasmExports && BglState.wasmExports.bgl_camera_save_bookmark) {
      BglState.wasmExports.bgl_camera_save_bookmark(slot);
      BglNotifier.show({
        type: 'success',
        title: 'Bookmark Saved',
        message: `Saved viewpoint to slot #${slot + 1}`
      });
    }
  }
  window.saveBookmark = saveBookmark;

  function restoreBookmark(slot) {
    if (BglState.wasmExports && BglState.wasmExports.bgl_camera_restore_bookmark) {
      BglState.wasmExports.bgl_camera_restore_bookmark(slot);
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
    const row = document.querySelector(`.tree-node-row[data-node-idx="${idx}"]`);
    if (row) {
      document.querySelectorAll('.tree-node-row').forEach(r => r.classList.remove('selected'));
      row.classList.add('selected');
      let parent = row.closest('.tree-children');
      while (parent) {
        parent.style.display = 'block';
        const caret = parent.parentElement ? parent.parentElement.querySelector('.tree-caret') : null;
        if (caret) caret.classList.add('open');
        parent = parent.parentElement ? parent.parentElement.closest('.tree-children') : null;
      }
      row.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
      if (BglState.currentGltf && BglState.currentGltf.nodes && BglState.currentGltf.nodes[idx]) {
        BglTreeViewer.showNodeDetails(idx, BglState.currentGltf.nodes[idx], BglState.currentGltf);
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
        case '+':
        case '=':
          if (BglState.wasmExports && BglState.wasmExports.bgl_camera_zoom) BglState.wasmExports.bgl_camera_zoom(0.8);
          break;
        case '-':
        case '_':
          if (BglState.wasmExports && BglState.wasmExports.bgl_camera_zoom) BglState.wasmExports.bgl_camera_zoom(-0.8);
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

      if (activeNavKeys.size > 0 && BglState.wasmExports) {
        const moveSpeed = 4.0 * dt;
        let fwd = 0;
        let right = 0;
        let zoom = 0;

        if (activeNavKeys.has('KeyW') || activeNavKeys.has('ArrowUp')) fwd += moveSpeed;
        if (activeNavKeys.has('KeyS') || activeNavKeys.has('ArrowDown')) fwd -= moveSpeed;
        if (activeNavKeys.has('KeyD') || activeNavKeys.has('ArrowRight')) right += moveSpeed;
        if (activeNavKeys.has('KeyA') || activeNavKeys.has('ArrowLeft')) right -= moveSpeed;

        if (activeNavKeys.has('Equal') || activeNavKeys.has('NumpadAdd')) zoom += 3.5 * dt;
        if (activeNavKeys.has('Minus') || activeNavKeys.has('NumpadSubtract')) zoom -= 3.5 * dt;

        if ((fwd !== 0 || right !== 0) && BglState.wasmExports.bgl_camera_move) {
          BglState.wasmExports.bgl_camera_move(fwd, right, 0);
        }
        if (zoom !== 0 && BglState.wasmExports.bgl_camera_zoom) {
          BglState.wasmExports.bgl_camera_zoom(zoom);
        }
      }
      requestAnimationFrame(navLoop);
    }
    requestAnimationFrame(navLoop);

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
    const gamepadBadge = document.getElementById('gamepad-badge');
    const gamepadLabel = document.getElementById('gamepad-label');

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
      requestAnimationFrame(pollGamepad);
    }
    requestAnimationFrame(pollGamepad);

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
  // Application Bootstrap
  // ============================================================================
  async function bootstrap() {
    BglLogger.banner();
    registerEventListeners();

    // Start real-time UI performance telemetry ticker
    let lastTime = performance.now();
    let frameCount = 0;
    const fpsPill = document.getElementById('hud-fps-pill');
    function tickFps() {
      frameCount++;
      const now = performance.now();
      const elapsed = now - lastTime;
      if (elapsed >= 500) {
        const fps = (frameCount * 1000) / elapsed;
        frameCount = 0;
        lastTime = now;
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
      requestAnimationFrame(tickFps);
    }
    requestAnimationFrame(tickFps);

    // Initialize Cache Storage UI status
    BglModelCache.updateCacheStatusUI().catch(e => BglLogger.warn('Cache', 'Initial cache check failed:', e));

    // 1. Verify graphics capabilities and select backend (WebGPU vs WebGL 2)
    const isCompat = await checkBrowserCompatibility();

    // 2. If WebGPU is confirmed, initialize WebAssembly runtime
    if (BglState.backend === 'webgpu') {
      try {
        await BglWasmBridge.init();
      } catch (wasmErr) {
        BglLogger.warn('Wasm', 'WebGPU Wasm runtime failed to start, falling back to WebGL 2:', wasmErr);
        BglState.backend = 'webgl2';
        BglWebGlRenderer.init();
      }
    }

    // 3. Ingest default 3D model into the active pipeline
    await loadDefaultModel();
    await BglModelCache.updateCacheStatusUI().catch(() => {});
    BglLogger.log('Engine', `Bootstrap completed successfully (Backend: ${BglState.backend}).`);
  }

  // Run on DOM ready
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', bootstrap);
  } else {
    bootstrap();
  }
})();
