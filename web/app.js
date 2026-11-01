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
        '%c bgl-Engine %c WebAssembly & WebGPU 3D Runtime v0.9.14 ',
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
    bumpScale: 1.5,
    pomScale: 0.035,
    flareIntensity: 0.75,
    weatherMode: 'none',
    weatherIntensity: 1.0,
    windStrength: 0.30,
    timeOfDay: 14.0,
    isDayNightCycleActive: false,
    dayNightSpeed: 1.0,
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
    btnDayNight: document.getElementById('btn-day-night'),
    btnWeatherNone: document.getElementById('btn-weather-none'),
    btnWeatherRain: document.getElementById('btn-weather-rain'),
    btnWeatherSnow: document.getElementById('btn-weather-snow'),
    btnWeatherFog: document.getElementById('btn-weather-fog'),
    btnWeatherSmoke: document.getElementById('btn-weather-smoke'),

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
      if (!BglState.wasmExports || !BglState.wasmExports.malloc) {
        return false;
      }

      try {
        const uint8 = new Uint8Array(arrayBuffer);
        // If buffer exceeds available Wasm memory, dynamically grow heap if within limits
        if (BglState.wasmMemory) {
          const curBytes = BglState.wasmMemory.buffer.byteLength;
          if (uint8.length + 65536 > curBytes) {
            const pagesNeeded = Math.ceil((uint8.length + 65536 - curBytes) / 65536) + 16;
            try {
              BglState.wasmMemory.grow(pagesNeeded);
            } catch (growErr) {
              // Maximum memory limit reached
            }
          }
        }

        const bufPtr = BglState.wasmExports.malloc(uint8.length);
        if (!bufPtr) {
          BglLogger.log('Wasm', `Wasm heap memory limit reached for '${name}' (${(uint8.length / (1024 * 1024)).toFixed(1)} MB). Model renders directly via WebGPU.`);
          return false;
        }

        const heap = new Uint8Array(BglState.wasmMemory.buffer);
        heap.set(uint8, bufPtr);

        const nameBytes = new TextEncoder().encode(name + '\0');
        const namePtr = BglState.wasmExports.malloc(nameBytes.length);
        if (!namePtr) {
          BglState.wasmExports.free(bufPtr);
          return false;
        }

        heap.set(nameBytes, namePtr);
        BglState.wasmExports.bgl_load_file_buffer(namePtr, bufPtr, uint8.length);

        BglState.wasmExports.free(namePtr);
        BglState.wasmExports.free(bufPtr);
        BglLogger.log('Wasm', `Successfully ingested model '${name}' (${uint8.length.toLocaleString()} bytes) into engine memory`);
        return true;
      } catch (err) {
        BglLogger.warn('Wasm', `Notice feeding buffer '${name}' to Wasm:`, err.message || err);
        return false;
      }
    }
  };

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
  // Native WebGPU 3D Renderer (WGSL Shaders & Hardware Acceleration)
  // ============================================================================
  /**
   * @typedef {Object} WebGpuPrimitiveRecord
   * @property {GPUBuffer} vertexBuffer - Interleaved vertex buffer [pos(3), norm(3), uv(2)]
   * @property {GPUBuffer|null} indexBuffer - 4-byte aligned index buffer (uint16/uint32)
   * @property {number} indexCount - Number of indices to draw
   * @property {'uint16'|'uint32'} indexFormat - WebGPU index buffer format
   * @property {number} vertexCount - Number of vertices (if unindexed)
   * @property {boolean} hasIndices - Whether primitive uses indexed draw call
   * @property {GPUBuffer} uniformBuffer - 256-byte aligned per-primitive UBO
   * @property {GPUBindGroup} bindGroup - Bind group containing UBO, sampler, and texture view
   * @property {number[]} baseColorFactor - RGBA base color multiplier [r, g, b, a]
   * @property {boolean} doubleSided - Whether primitive requires backface culling disabled
   * @property {boolean} hasTexture - Whether diffuse texture is bound
   * @property {GPUTexture|null} texture - Associated diffuse GPUTexture
   * @property {Float32Array} modelMatrix - 4x4 world transform matrix
   */

  /**
   * Hardware-accelerated native WebGPU 3D renderer.
   * Utilizes WGSL shaders, dynamic depth textures, zero-copy buffer uploads,
   * 4-byte index buffer alignment, and zero-allocation frame rendering.
   */
  const BglWebGpuRenderer = {
    adapter: null,
    device: null,
    context: null,
    canvas: null,
    format: null,
    pipeline: null,
    pipelineDoubleSided: null,
    depthTexture: null,
    depthTextureView: null,
    defaultWhiteTexture: null,
    defaultWhiteTextureView: null,
    defaultMrTexture: null,
    defaultMrTextureView: null,
    defaultNormalTexture: null,
    defaultNormalTextureView: null,
    defaultBlackTexture: null,
    defaultBlackTextureView: null,
    defaultSampler: null,
    bindGroupLayout: null,
    pipelineLayout: null,
    isActive: false,
    primitives: [],
    textureCache: new Map(),

    // Preallocated scratch objects for zero-allocation 60/120 FPS rendering
    _scratchEye: new Float32Array(3),
    _scratchLightDir: new Float32Array(3),
    _scratchProj: null,
    _scratchView: null,
    _scratchViewProj: null,
    _scratchNormalMat: new Float32Array(16),
    _scratchUboBuffer: new ArrayBuffer(512),
    _scratchUboF32: null,
    _scratchUboU32: null,

    camera: {
      target: [0.0, 0.0, 0.0],
      distance: 3.5,
      yaw: 0.45,
      pitch: 0.35,
      fov: (45 * Math.PI) / 180,
      near: 0.05,
      far: 500.0,
      minDistance: 0.0001,
      maxDistance: 1000.0,
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
      normalMatrix4(out, m) {
        const a00 = m[0], a01 = m[1], a02 = m[2];
        const a10 = m[4], a11 = m[5], a12 = m[6];
        const a20 = m[8], a21 = m[9], a22 = m[10];

        const b01 = a22 * a11 - a12 * a21;
        const b11 = -a22 * a10 + a12 * a20;
        const b21 = a21 * a10 - a11 * a20;

        let det = a00 * b01 + a01 * b11 + a02 * b21;
        if (!det) {
          out[0] = 1; out[1] = 0; out[2] = 0; out[3] = 0;
          out[4] = 0; out[5] = 1; out[6] = 0; out[7] = 0;
          out[8] = 0; out[9] = 0; out[10] = 1; out[11] = 0;
          out[12] = 0; out[13] = 0; out[14] = 0; out[15] = 1;
          return out;
        }
        det = 1.0 / det;

        out[0] = b01 * det;
        out[1] = (-a22 * a01 + a02 * a21) * det;
        out[2] = (a12 * a01 - a02 * a11) * det;
        out[3] = 0;

        out[4] = b11 * det;
        out[5] = (a22 * a00 - a02 * a20) * det;
        out[6] = (-a12 * a00 + a02 * a10) * det;
        out[7] = 0;

        out[8] = b21 * det;
        out[9] = (-a21 * a00 + a01 * a20) * det;
        out[10] = (a11 * a00 - a01 * a10) * det;
        out[11] = 0;

        out[12] = 0;
        out[13] = 0;
        out[14] = 0;
        out[15] = 1;
        return out;
      }
    },

    getShaderWgsl() {
      return `
struct Uniforms {
    viewProj : mat4x4<f32>,
    model : mat4x4<f32>,
    normalMatrix : mat4x4<f32>,
    cameraPos : vec4<f32>,
    lightDir : vec4<f32>,
    baseColorFactor : vec4<f32>,
    pbrFactors : vec4<f32>,      // x=metallic, y=roughness, z=normalScale, w=occlusionStrength
    emissiveFactor : vec4<f32>,  // xyz=emissive, w=alphaCutoff
    uvTransform : vec4<f32>,     // xy=scale, zw=offset
    flags : vec4<u32>,           // x=texFlags, y=wireframe, z=doubleSided, w=alphaMode
    params : vec4<f32>,          // x=bumpScale, y=pomScale, z=isPomActive, w=isBumpActive
    weather : vec4<f32>,         // x=weatherMode (0=none, 1=rain, 2=snow), y=intensity, z=wind, w=timeSec
};

@group(0) @binding(0) var<uniform> ubo : Uniforms;
@group(0) @binding(1) var defaultSampler : sampler;
@group(0) @binding(2) var baseColorTexture : texture_2d<f32>;
@group(0) @binding(3) var metallicRoughnessTexture : texture_2d<f32>;
@group(0) @binding(4) var normalTexture : texture_2d<f32>;
@group(0) @binding(5) var emissiveTexture : texture_2d<f32>;
@group(0) @binding(6) var occlusionTexture : texture_2d<f32>;

struct VertexInput {
    @location(0) position : vec3<f32>,
    @location(1) normal : vec3<f32>,
    @location(2) uv : vec2<f32>,
    @location(3) tangent : vec4<f32>,
};

struct VertexOutput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) worldPos : vec3<f32>,
    @location(1) normal : vec3<f32>,
    @location(2) uv : vec2<f32>,
    @location(3) tangent : vec4<f32>,
};

struct MipVertexOutput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) uv : vec2<f32>,
};

@vertex
fn vs_mip(@builtin(vertex_index) vIdx : u32) -> MipVertexOutput {
    var out : MipVertexOutput;
    let x = f32(i32(vIdx << 1u) & 2) * 2.0 - 1.0;
    let y = f32(i32(vIdx & 2u)) * 2.0 - 1.0;
    out.clipPos = vec4<f32>(x, y, 0.0, 1.0);
    out.uv = vec2<f32>(x * 0.5 + 0.5, 0.5 - y * 0.5);
    return out;
}

@fragment
fn fs_mip(in : MipVertexOutput) -> @location(0) vec4<f32> {
    return textureSample(baseColorTexture, defaultSampler, in.uv);
}

const PI : f32 = 3.141592653589793;

fn distributionGGX(NdotH : f32, roughness : f32) -> f32 {
    let a = roughness * roughness;
    let a2 = a * a;
    let d = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

fn visibilitySmithGGX(NdotL : f32, NdotV : f32, roughness : f32) -> f32 {
    let a = roughness * roughness;
    let a2 = a * a;
    let ggxL = NdotL + sqrt(a2 + (1.0 - a2) * (NdotL * NdotL));
    let ggxV = NdotV + sqrt(a2 + (1.0 - a2) * (NdotV * NdotV));
    return 1.0 / max(ggxL * ggxV, 0.0001);
}

fn fresnelSchlick(VdotH : f32, F0 : vec3<f32>) -> vec3<f32> {
    return F0 + (vec3<f32>(1.0) - F0) * pow(clamp(1.0 - VdotH, 0.0, 1.0), 5.0);
}

fn fresnelSchlickRoughness(NdotV : f32, F0 : vec3<f32>, roughness : f32) -> vec3<f32> {
    return F0 + (max(vec3<f32>(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - NdotV, 0.0, 1.0), 5.0);
}

fn getTBN(worldPos : vec3<f32>, geomNormal : vec3<f32>, uv : vec2<f32>, tangent : vec4<f32>, dp1 : vec3<f32>, dp2 : vec3<f32>, duv1 : vec2<f32>, duv2 : vec2<f32>) -> mat3x3<f32> {
    let N = normalize(geomNormal);
    let orthoT = tangent.xyz - N * dot(N, tangent.xyz);
    let orthoLenSq = dot(orthoT, orthoT);
    if (abs(tangent.w) > 0.001 && orthoLenSq > 1e-6) {
        let T = orthoT * inverseSqrt(orthoLenSq);
        let B = normalize(cross(N, T) * sign(tangent.w));
        return mat3x3<f32>(T, B, N);
    }
    let dp2perp = cross(dp2, N);
    let dp1perp = cross(N, dp1);
    let T = dp2perp * duv1.x + dp1perp * duv2.x;
    let B = dp2perp * duv1.y + dp1perp * duv2.y;
    let maxLenSq = max(dot(T, T), dot(B, B));
    if (maxLenSq > 1e-18) {
        let invmax = inverseSqrt(maxLenSq);
        return mat3x3<f32>(normalize(T * invmax), normalize(B * invmax), N);
    }
    let up = select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0), abs(N.y) < 0.999);
    let fallbackT = normalize(cross(up, N));
    let fallbackB = normalize(cross(N, fallbackT));
    return mat3x3<f32>(fallbackT, fallbackB, N);
}

fn getSurfaceHeight(uv : vec2<f32>, duv1 : vec2<f32>, duv2 : vec2<f32>) -> f32 {
    var height = 0.5;
    if ((ubo.flags.x & 16u) != 0u) {
        height = textureSampleGrad(occlusionTexture, defaultSampler, uv, duv1, duv2).r;
    } else if ((ubo.flags.x & 1u) != 0u) {
        let col = textureSampleGrad(baseColorTexture, defaultSampler, uv, duv1, duv2).rgb;
        height = dot(col, vec3<f32>(0.299, 0.587, 0.114));
    }
    return clamp(height, 0.0, 1.0);
}

fn parallaxOcclusionMapping(
    initialUV : vec2<f32>,
    viewDirTangent : vec3<f32>,
    duv1 : vec2<f32>,
    duv2 : vec2<f32>,
    scale : f32
) -> vec2<f32> {
    let minLayers : f32 = 8.0;
    let maxLayers : f32 = 32.0;
    let numLayers = mix(maxLayers, minLayers, clamp(abs(viewDirTangent.z), 0.0, 1.0));
    let layerDepth = 1.0 / numLayers;
    var currentLayerDepth : f32 = 0.0;

    let P = (viewDirTangent.xy / max(abs(viewDirTangent.z), 0.15)) * scale;
    let deltaUV = P / numLayers;

    var currentUV = initialUV;
    var currentHeight = getSurfaceHeight(currentUV, duv1, duv2);

    var prevUV = currentUV;
    var prevHeight = currentHeight;
    var prevLayerDepth = currentLayerDepth;

    for (var i : i32 = 0; i < 32; i = i + 1) {
        if (f32(i) >= numLayers || currentLayerDepth >= (1.0 - currentHeight)) {
            break;
        }
        prevUV = currentUV;
        prevHeight = currentHeight;
        prevLayerDepth = currentLayerDepth;

        currentUV = currentUV - deltaUV;
        currentHeight = getSurfaceHeight(currentUV, duv1, duv2);
        currentLayerDepth = currentLayerDepth + layerDepth;
    }

    let afterDepth = currentLayerDepth - (1.0 - currentHeight);
    let beforeDepth = (1.0 - prevHeight) - prevLayerDepth;
    let weight = afterDepth / max(afterDepth + beforeDepth, 0.0001);
    return mix(currentUV, prevUV, clamp(weight, 0.0, 1.0));
}

fn computeParallaxShadow(
    displacedUV : vec2<f32>,
    lightDirTangent : vec3<f32>,
    duv1 : vec2<f32>,
    duv2 : vec2<f32>,
    scale : f32
) -> f32 {
    let initialHeight = getSurfaceHeight(displacedUV, duv1, duv2);
    if (lightDirTangent.z <= 0.0) {
        return 0.25;
    }
    let numSamples : f32 = 8.0;
    let stepSize = (1.0 - initialHeight) / numSamples;
    let deltaUV = (lightDirTangent.xy / max(lightDirTangent.z, 0.15)) * scale / numSamples;

    var currentLayerHeight = initialHeight;
    var currentUV = displacedUV;
    var shadowFactor = 1.0;

    for (var i : i32 = 0; i < 8; i = i + 1) {
        currentUV = currentUV + deltaUV;
        currentLayerHeight = currentLayerHeight + stepSize;
        let sampledHeight = getSurfaceHeight(currentUV, duv1, duv2);
        if (sampledHeight > currentLayerHeight) {
            let diff = sampledHeight - currentLayerHeight;
            shadowFactor = min(shadowFactor, 1.0 - diff * 0.85);
        }
    }
    return clamp(shadowFactor, 0.25, 1.0);
}

fn srgbToLinear(c : vec3<f32>) -> vec3<f32> {
    return pow(c, vec3<f32>(2.2));
}

fn linearToSrgb(c : vec3<f32>) -> vec3<f32> {
    return pow(c, vec3<f32>(1.0 / 2.2));
}

fn tonemapKhronosPbrNeutral(color : vec3<f32>) -> vec3<f32> {
    let startCompression = 0.8 - 0.04;
    let desaturation = 0.15;
    let x = min(color.r, min(color.g, color.b));
    let offset = select(0.04, x - 6.25 * x * x, x < 0.08);
    var c = color - offset;
    let peak = max(c.r, max(c.g, c.b));
    if (peak < startCompression) {
        return color;
    }
    let d = 1.0 - startCompression;
    let newPeak = 1.0 - d * d / (peak + d - startCompression);
    c = c * (newPeak / peak);
    let g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(c, vec3<f32>(newPeak), g);
}

// ============================================================================
// Procedural Weather Effects (Atmospheric Rain & Snow WGSL Subroutines)
// ============================================================================

fn hash31(p : vec3<f32>) -> f32 {
    let h = fract(sin(dot(p, vec3<f32>(12.9898, 78.233, 45.164))) * 43758.5453123);
    return h;
}

// Procedural dynamic raindrops ripples on surface
fn calculateRainRipples(pos : vec2<f32>, time : f32) -> vec3<f32> {
    let scale = 14.0;
    let uvCoord = pos * scale;
    let iuv = floor(uvCoord);
    let fuv = fract(uvCoord);
    var rippleVec = vec2<f32>(0.0);
    for (var y = -1; y <= 1; y = y + 1) {
        for (var x = -1; x <= 1; x = x + 1) {
            let g = vec2<f32>(f32(x), f32(y));
            let h = fract(sin(dot(iuv + g, vec2<f32>(127.1, 311.7))) * 43758.5453);
            let t = fract(time * 2.2 + h);
            let center = g + vec2<f32>(0.5) + vec2<f32>(sin(h * 6.28), cos(h * 6.28)) * 0.25;
            let d = length(fuv - center);
            let wave = sin(d * 24.0 - t * 14.0) * exp(-d * 4.5) * (1.0 - t);
            let dir = normalize(fuv - center + 1e-4);
            rippleVec = rippleVec + dir * wave * 0.5;
        }
    }
    return normalize(vec3<f32>(rippleVec, 1.0));
}

// Volumetric atmospheric rain streaks
fn calculateRainStreaks(worldPos : vec3<f32>, cameraPos : vec3<f32>, time : f32, wind : f32, intensity : f32) -> vec3<f32> {
    let rayDist = length(worldPos - cameraPos);
    var streakAccum : f32 = 0.0;
    for (var i = 0; i < 2; i = i + 1) {
        let scale = select(4.0, 8.0, i == 1);
        let speed = select(22.0, 32.0, i == 1);
        let windVec = vec3<f32>(wind * 3.5, -speed, wind * 1.5);
        let samplePos = (worldPos - windVec * time) * scale;
        let cell = floor(vec3<f32>(samplePos.x, samplePos.y * 0.08, samplePos.z));
        let f = fract(vec3<f32>(samplePos.x, samplePos.y * 0.08, samplePos.z));
        let h = hash31(cell);
        let dX = abs(f.x - 0.5);
        let dZ = abs(f.z - 0.5);
        let dY = abs(f.y - 0.5);
        let streak = smoothstep(0.08, 0.0, dX) * smoothstep(0.08, 0.0, dZ) * smoothstep(0.45, 0.0, dY);
        let activeDrop = step(0.70, h);
        streakAccum = streakAccum + streak * activeDrop;
    }
    let mist = (1.0 - exp(-rayDist * 0.08)) * 0.15 * intensity;
    let mistColor = vec3<f32>(0.65, 0.72, 0.82) * mist;
    let streakColor = vec3<f32>(0.85, 0.92, 1.0) * (streakAccum * intensity * 0.75);
    return streakColor + mistColor;
}

// Volumetric atmospheric drifting snow flakes
fn calculateSnowFlakes(worldPos : vec3<f32>, cameraPos : vec3<f32>, time : f32, wind : f32, intensity : f32) -> vec3<f32> {
    let rayDist = length(worldPos - cameraPos);
    var flakeAccum : f32 = 0.0;
    for (var i = 0; i < 2; i = i + 1) {
        let scale = select(3.5, 6.5, i == 1);
        let fallSpeed = select(2.4, 3.8, i == 1);
        let freq = select(2.0, 3.2, i == 1);
        let wobbleX = sin(time * freq + worldPos.y * 2.5 + f32(i)) * 0.35;
        let wobbleZ = cos(time * (freq * 0.85) + worldPos.x * 2.0) * 0.3;
        let driftVec = vec3<f32>(wind * 1.8 * time + wobbleX, -fallSpeed * time, wind * 0.8 * time + wobbleZ);
        let samplePos = (worldPos - driftVec) * scale;
        let cell = floor(samplePos);
        let f = fract(samplePos) - 0.5;
        let h = hash31(cell + vec3<f32>(17.3, 41.7, 83.1));
        let dist = length(f);
        let flake = smoothstep(0.20, 0.02, dist) * step(0.66, h);
        flakeAccum = flakeAccum + flake;
    }
    let winterFog = (1.0 - exp(-rayDist * 0.06)) * 0.18 * intensity;
    let winterFogColor = vec3<f32>(0.82, 0.88, 0.96) * winterFog;
    let flakeColor = vec3<f32>(0.96, 0.98, 1.0) * (flakeAccum * intensity * 0.85);
    return flakeColor + winterFogColor;
}

fn noise3D(p : vec3<f32>) -> f32 {
    let i = floor(p);
    let f = fract(p);
    let u = f * f * (3.0 - 2.0 * f);
    let n000 = hash31(i + vec3<f32>(0.0, 0.0, 0.0));
    let n100 = hash31(i + vec3<f32>(1.0, 0.0, 0.0));
    let n010 = hash31(i + vec3<f32>(0.0, 1.0, 0.0));
    let n110 = hash31(i + vec3<f32>(1.0, 1.0, 0.0));
    let n001 = hash31(i + vec3<f32>(0.0, 0.0, 1.0));
    let n101 = hash31(i + vec3<f32>(1.0, 0.0, 1.0));
    let n011 = hash31(i + vec3<f32>(0.0, 1.0, 1.0));
    let n111 = hash31(i + vec3<f32>(1.0, 1.0, 1.0));
    let nx00 = mix(n000, n100, u.x);
    let nx10 = mix(n010, n110, u.x);
    let nx01 = mix(n001, n101, u.x);
    let nx11 = mix(n011, n111, u.x);
    let nxy0 = mix(nx00, nx10, u.y);
    let nxy1 = mix(nx01, nx11, u.y);
    return mix(nxy0, nxy1, u.z);
}

fn fbm3D(p : vec3<f32>) -> f32 {
    var val = 0.0;
    var amp = 0.5;
    var pos = p;
    for (var i = 0; i < 4; i = i + 1) {
        val = val + amp * noise3D(pos);
        pos = pos * 2.03 + vec3<f32>(13.7, 29.3, 7.1);
        amp = amp * 0.5;
    }
    return val;
}

fn evaluateDiurnalSunRadiance(solarElev : f32) -> vec3<f32> {
    let noonSun = vec3<f32>(1.0, 0.97, 0.92) * 2.6;
    let goldenSun = vec3<f32>(1.0, 0.48, 0.18) * 1.9;
    let nightMoon = vec3<f32>(0.28, 0.44, 0.78) * 0.48;
    if (solarElev >= 0.35) {
        let t = clamp((solarElev - 0.35) / 0.65, 0.0, 1.0);
        return mix(goldenSun, noonSun, t);
    } else if (solarElev >= -0.10) {
        let t = clamp((solarElev + 0.10) / 0.45, 0.0, 1.0);
        return mix(nightMoon, goldenSun, t);
    }
    return nightMoon;
}

fn evaluateDiurnalAmbientScale(solarElev : f32) -> vec3<f32> {
    let dayScale = vec3<f32>(1.0, 1.0, 1.0);
    let duskScale = vec3<f32>(0.82, 0.60, 0.52);
    let nightScale = vec3<f32>(0.16, 0.22, 0.38);
    if (solarElev >= 0.25) {
        let t = clamp((solarElev - 0.25) / 0.75, 0.0, 1.0);
        return mix(duskScale, dayScale, t);
    } else if (solarElev >= -0.15) {
        let t = clamp((solarElev + 0.15) / 0.40, 0.0, 1.0);
        return mix(nightScale, duskScale, t);
    }
    return nightScale;
}

fn calculateFogEffect(sceneColor : vec3<f32>, worldPos : vec3<f32>, cameraPos : vec3<f32>, sunDir : vec3<f32>, solarElev : f32, time : f32, wind : f32, intensity : f32) -> vec3<f32> {
    let toFrag = worldPos - cameraPos;
    let dist = length(toFrag);
    let viewRay = toFrag / max(dist, 1e-4);

    let windOffset = vec3<f32>(wind * time * 0.6, -time * 0.08, wind * time * 0.3);
    let mistNoise = fbm3D(worldPos * 0.28 - windOffset);
    let heightFalloff = exp(-max(worldPos.y + 2.0, 0.0) * 0.12);
    let density = (0.045 + 0.055 * heightFalloff * (0.65 + 0.7 * mistNoise)) * intensity;
    let fogFactor = clamp(1.0 - exp(-dist * density), 0.0, 0.96);

    let cosTheta = max(dot(viewRay, sunDir), 0.0);
    let mieScatter = pow(cosTheta, 6.0) * 0.45;

    let dayFogColor = vec3<f32>(0.74, 0.81, 0.89);
    let duskFogColor = vec3<f32>(0.82, 0.58, 0.46);
    let nightFogColor = vec3<f32>(0.12, 0.17, 0.28);
    var baseFogColor = dayFogColor;
    if (solarElev < 0.25 && solarElev >= -0.15) {
        baseFogColor = mix(nightFogColor, duskFogColor, clamp((solarElev + 0.15) / 0.40, 0.0, 1.0));
    } else if (solarElev < -0.15) {
        baseFogColor = nightFogColor;
    } else {
        baseFogColor = mix(duskFogColor, dayFogColor, clamp((solarElev - 0.25) / 0.75, 0.0, 1.0));
    }

    let scatterTint = select(vec3<f32>(0.35, 0.50, 0.80), vec3<f32>(1.0, 0.82, 0.55), solarElev >= 0.0);
    let finalFogColor = baseFogColor + scatterTint * mieScatter;
    return mix(sceneColor, finalFogColor, fogFactor);
}

fn calculateSmokePlumes(sceneColor : vec3<f32>, worldPos : vec3<f32>, cameraPos : vec3<f32>, time : f32, wind : f32, intensity : f32) -> vec3<f32> {
    let dist = length(worldPos - cameraPos);
    let flow = vec3<f32>(wind * time * 1.1, -time * 1.35, wind * time * 0.45);
    let p1 = worldPos * 0.42 + flow;
    let warp = vec3<f32>(
        fbm3D(p1 + vec3<f32>(5.2, 1.3, 2.8)),
        fbm3D(p1 + vec3<f32>(1.7, 9.2, 4.1)),
        fbm3D(p1 + vec3<f32>(8.3, 2.8, 6.4))
    ) * 1.35;
    let rawTurbulence = fbm3D(p1 + warp);

    let heightEnvelope = exp(-max(worldPos.y + 3.0, 0.0) * 0.08);
    let distEnvelope = 1.0 - exp(-dist * 0.085);
    let plumeShape = smoothstep(0.32, 0.78, rawTurbulence);
    let smokeOpacity = clamp((plumeShape * 0.75 + distEnvelope * 0.35) * heightEnvelope * intensity, 0.0, 0.94);

    let sootColor = vec3<f32>(0.15, 0.16, 0.18);
    let ashHighlight = vec3<f32>(0.34, 0.35, 0.38);
    let emberGlow = vec3<f32>(0.95, 0.38, 0.08) * exp(-max(worldPos.y + 1.5, 0.0) * 0.45) * pow(plumeShape, 2.5) * 0.55 * clamp(intensity, 0.0, 1.5);
    let smokeColor = mix(sootColor, ashHighlight, rawTurbulence) + emberGlow;

    return mix(sceneColor, smokeColor, smokeOpacity);
}

@vertex
fn vs_main(in : VertexInput) -> VertexOutput {
    var out : VertexOutput;
    let worldPos4 = ubo.model * vec4<f32>(in.position, 1.0);
    out.worldPos = worldPos4.xyz;
    let n = (ubo.normalMatrix * vec4<f32>(in.normal, 0.0)).xyz;
    if (dot(n, n) > 0.0001) {
        out.normal = normalize(n);
    } else {
        out.normal = vec3<f32>(0.0, 1.0, 0.0);
    }
    let worldTan = (ubo.model * vec4<f32>(in.tangent.xyz, 0.0)).xyz;
    let tanLen = length(worldTan);
    let normTan = select(vec3<f32>(1.0, 0.0, 0.0), worldTan / tanLen, tanLen > 1e-8);
    out.tangent = vec4<f32>(normTan, in.tangent.w);
    out.uv = in.uv * ubo.uvTransform.xy + ubo.uvTransform.zw;
    out.clipPos = ubo.viewProj * worldPos4;
    return out;
}

@fragment
fn fs_main(in : VertexOutput, @builtin(front_facing) frontFacing : bool) -> @location(0) vec4<f32> {
    let dp1 = dpdx(in.worldPos);
    let dp2 = dpdy(in.worldPos);
    let duv1 = dpdx(in.uv);
    let duv2 = dpdy(in.uv);

    // Wireframe debug override
    if (ubo.flags.y != 0u) {
        return vec4<f32>(0.22, 0.74, 0.97, 1.0);
    }

    // 1. Initial Geometry Normal & Orthonormal TBN Basis
    var geomNormal = in.normal;
    if (dot(geomNormal, geomNormal) > 0.0001) {
        geomNormal = normalize(geomNormal);
    } else {
        geomNormal = vec3<f32>(0.0, 1.0, 0.0);
    }
    let V = normalize(ubo.cameraPos.xyz - in.worldPos);
    if (!frontFacing || (ubo.flags.z != 0u && dot(geomNormal, V) < 0.0)) {
        geomNormal = -geomNormal;
    }

    let TBN = getTBN(in.worldPos, geomNormal, in.uv, in.tangent, dp1, dp2, duv1, duv2);
    let invTBN = transpose(TBN);
    let V_tangent = invTBN * V;

    // 2. Parallax Occlusion Mapping (POM) & Surface Displacement
    var uv = in.uv;
    var pomShadow = 1.0;
    let bumpScale = ubo.params.x;
    let pomScale = ubo.params.y;
    let isPomActive = ubo.params.z > 0.5;
    let isBumpActive = ubo.params.w > 0.5;

    if (isPomActive && isBumpActive) {
        let isNormalMapped = (ubo.flags.x & 4u) != 0u;
        let effPomScale = select(pomScale, pomScale * 0.14, isNormalMapped);
        uv = parallaxOcclusionMapping(in.uv, V_tangent, duv1, duv2, effPomScale);
        let L0_tangent = invTBN * normalize(ubo.lightDir.xyz);
        pomShadow = computeParallaxShadow(uv, L0_tangent, duv1, duv2, effPomScale);
    }

    // Sample all PBR material maps using explicit screen-space gradients (uniformity-safe & anisotropic-correct)
    let baseTexSample = textureSampleGrad(baseColorTexture, defaultSampler, uv, duv1, duv2);
    let mrTexSample = textureSampleGrad(metallicRoughnessTexture, defaultSampler, uv, duv1, duv2);
    let normTexSample = textureSampleGrad(normalTexture, defaultSampler, uv, duv1, duv2);
    let emTexSample = textureSampleGrad(emissiveTexture, defaultSampler, uv, duv1, duv2);
    let occTexSample = textureSampleGrad(occlusionTexture, defaultSampler, uv, duv1, duv2);

    // 3. Base Color & Alpha
    var baseColor = ubo.baseColorFactor;
    if ((ubo.flags.x & 1u) != 0u) {
        baseColor = baseColor * vec4<f32>(srgbToLinear(baseTexSample.rgb), baseTexSample.a);
    } else {
        baseColor = vec4<f32>(srgbToLinear(baseColor.rgb), baseColor.a);
    }

    // Alpha Cutoff test (glTF 2.0 alphaMode)
    let alphaMode = ubo.flags.w;
    let alphaCutoff = ubo.emissiveFactor.w;
    if (alphaMode == 1u && baseColor.a < alphaCutoff) {
        discard;
    } else if (baseColor.a < 0.01) {
        discard;
    }

    // 4. High-Detail Sculptural Bump & Normal Mapping
    var N = geomNormal;
    if (isBumpActive) {
        if ((ubo.flags.x & 4u) != 0u) { // HasNormalTex
            let normalSample = normTexSample.xyz * 2.0 - 1.0;
            let totalScale = ubo.pbrFactors.z * bumpScale;
            let tangentNormal = normalize(vec3<f32>(normalSample.xy * totalScale, normalSample.z));
            N = normalize(TBN * tangentNormal);
        } else if ((ubo.flags.x & 1u) != 0u || (ubo.flags.x & 16u) != 0u) {
            // Surface Gradient Bump Mapping from BaseColor Luminance and Occlusion Cavity
            let texDims = vec2<f32>(textureDimensions(baseColorTexture, 0));
            let texelStep = 1.0 / clamp(texDims, vec2<f32>(64.0), vec2<f32>(2048.0));
            let lumaWeights = vec3<f32>(0.299, 0.587, 0.114);

            let hR = dot(textureSampleGrad(baseColorTexture, defaultSampler, uv + vec2<f32>(texelStep.x, 0.0), duv1, duv2).rgb, lumaWeights);
            let hL = dot(textureSampleGrad(baseColorTexture, defaultSampler, uv - vec2<f32>(texelStep.x, 0.0), duv1, duv2).rgb, lumaWeights);
            let hU = dot(textureSampleGrad(baseColorTexture, defaultSampler, uv + vec2<f32>(0.0, texelStep.y), duv1, duv2).rgb, lumaWeights);
            let hD = dot(textureSampleGrad(baseColorTexture, defaultSampler, uv - vec2<f32>(0.0, texelStep.y), duv1, duv2).rgb, lumaWeights);

            var dHdU = (hR - hL);
            var dHdV = (hU - hD);

            if ((ubo.flags.x & 16u) != 0u) {
                let occR = textureSampleGrad(occlusionTexture, defaultSampler, uv + vec2<f32>(texelStep.x, 0.0), duv1, duv2).r;
                let occL = textureSampleGrad(occlusionTexture, defaultSampler, uv - vec2<f32>(texelStep.x, 0.0), duv1, duv2).r;
                let occU = textureSampleGrad(occlusionTexture, defaultSampler, uv + vec2<f32>(0.0, texelStep.y), duv1, duv2).r;
                let occD = textureSampleGrad(occlusionTexture, defaultSampler, uv - vec2<f32>(0.0, texelStep.y), duv1, duv2).r;
                dHdU = dHdU + (occR - occL) * 0.75;
                dHdV = dHdV + (occU - occD) * 0.75;
            }

            let bumpStrength = 2.0 * bumpScale;
            let tangentBump = normalize(vec3<f32>(-dHdU * bumpStrength, -dHdV * bumpStrength, 1.0));
            N = normalize(TBN * tangentBump);
        }
    }

    // 5. Micro-Cavity Factor & Horizon Occlusion
    let cavityFactor = clamp(dot(geomNormal, N), 0.0, 1.0);

    // Weather Parameters
    let weatherMode = ubo.weather.x;
    let weatherIntensity = ubo.weather.y;
    let windStrength = ubo.weather.z;
    let weatherTime = ubo.weather.w;

    if (weatherMode == 1.0 && weatherIntensity > 0.001 && geomNormal.y > 0.15) {
        // Rain droplet ripples on upward-facing surfaces
        let rippleTan = calculateRainRipples(in.uv * 5.0 + in.worldPos.xz * 2.5, weatherTime);
        let rippleWeight = smoothstep(0.15, 0.75, geomNormal.y) * clamp(weatherIntensity * 0.55, 0.0, 0.65);
        N = normalize(mix(N, TBN * rippleTan, rippleWeight));
    } else if (weatherMode == 2.0 && weatherIntensity > 0.001 && geomNormal.y > 0.2) {
        // Snow settles and flattens crevices upward
        let snowAccum = smoothstep(0.2, 0.75, geomNormal.y) * clamp(weatherIntensity, 0.0, 1.5);
        N = normalize(mix(N, vec3<f32>(0.0, 1.0, 0.0), snowAccum * 0.6));
    }

    // 6. Metallic & Roughness with Micro-Roughness Relief
    var metallic = ubo.pbrFactors.x;
    var roughness = ubo.pbrFactors.y;
    if ((ubo.flags.x & 2u) != 0u) { // HasMrTex
        roughness = roughness * mrTexSample.g;
        metallic = metallic * mrTexSample.b;
    }
    metallic = clamp(metallic, 0.0, 1.0);
    // Ridge polish & crevice matte modulation for heightened tactile plasticity
    roughness = clamp(roughness * mix(1.15, 0.85, cavityFactor), 0.04, 1.0);

    // Weather surface material modulation (wet absorption glaze, crystalline snow, fog condensation, smoke soot)
    if (weatherMode == 1.0 && weatherIntensity > 0.001) {
        roughness = mix(roughness, 0.08, clamp(weatherIntensity * 0.85, 0.0, 0.88));
    } else if (weatherMode == 2.0 && weatherIntensity > 0.001) {
        let snowAccum = smoothstep(0.2, 0.75, geomNormal.y) * clamp(weatherIntensity, 0.0, 1.5);
        if (snowAccum > 0.001) {
            roughness = mix(roughness, 0.70, snowAccum * 0.9);
            metallic = mix(metallic, 0.0, snowAccum * 0.95);
        }
    } else if (weatherMode == 3.0 && weatherIntensity > 0.001) {
        roughness = mix(roughness, 0.30, clamp(weatherIntensity * 0.35, 0.0, 0.45));
    } else if (weatherMode == 4.0 && weatherIntensity > 0.001) {
        roughness = mix(roughness, 0.88, clamp(weatherIntensity * 0.4, 0.0, 0.55));
    }

    // 7. Ambient Occlusion with Micro-Cavity Crevice Deepening
    var ao = 1.0;
    if ((ubo.flags.x & 16u) != 0u) { // HasOcclusionTex
        ao = mix(1.0, occTexSample.r, ubo.pbrFactors.w);
    }
    ao = ao * mix(0.5, 1.0, cavityFactor * cavityFactor);

    // 8. Emissive
    var emissive = ubo.emissiveFactor.rgb;
    if ((ubo.flags.x & 8u) != 0u) { // HasEmissiveTex
        emissive = emissive * srgbToLinear(emTexSample.rgb);
    }

    // 9. PBR Vectors & Dielectric/Metallic Reflectance Setup
    let NdotV = max(dot(N, V), 0.001);
    let F0 = mix(vec3<f32>(0.04), baseColor.rgb, metallic);
    var diffuseColor = baseColor.rgb * (1.0 - metallic);

    if (weatherMode == 1.0 && weatherIntensity > 0.001) {
        let wetWeight = clamp(weatherIntensity * 0.8, 0.0, 0.75);
        diffuseColor = mix(diffuseColor, diffuseColor * 0.65, wetWeight * (1.0 - metallic * 0.5));
    } else if (weatherMode == 2.0 && weatherIntensity > 0.001) {
        let snowAccum = smoothstep(0.2, 0.75, geomNormal.y) * clamp(weatherIntensity, 0.0, 1.5);
        if (snowAccum > 0.001) {
            let crispSnowColor = vec3<f32>(0.93, 0.95, 0.99);
            diffuseColor = mix(diffuseColor, crispSnowColor, snowAccum * 0.95);
        }
    }

    // 10. Direct Lighting Evaluation with Diurnal Sun/Moon Cycle & Horizon Micro-Shadowing
    var directRadiance = vec3<f32>(0.0);
    let solarElev = ubo.lightDir.w;
    let diurnalAmbientScale = evaluateDiurnalAmbientScale(solarElev);

    // Light 0: Sun / Moon Key Light
    let L0 = normalize(ubo.lightDir.xyz);
    let NdotL0 = max(dot(N, L0), 0.0);
    let geomNdotL0 = max(dot(geomNormal, L0), 0.0);
    let horizon0 = clamp(geomNdotL0 * 3.0, 0.0, 1.0);
    let light0Shadow = pomShadow * horizon0;

    if (NdotL0 > 0.0 && light0Shadow > 0.001) {
        let H0 = normalize(L0 + V);
        let NdotH0 = max(dot(N, H0), 0.0);
        let VdotH0 = max(dot(V, H0), 0.0);

        let D = distributionGGX(NdotH0, roughness);
        let Vis = visibilitySmithGGX(NdotL0, NdotV, roughness);
        let F = fresnelSchlick(VdotH0, F0);
        let specularBRDF = D * Vis * F;

        let kD = (vec3<f32>(1.0) - F) * (1.0 - metallic);
        let diffuseBRDF = kD * (diffuseColor / PI);

        let sunRadiance = evaluateDiurnalSunRadiance(solarElev);
        directRadiance = directRadiance + (diffuseBRDF + specularBRDF) * sunRadiance * (NdotL0 * light0Shadow);
    }

    // Light 1: Cool Sky Fill Light
    let L1 = normalize(vec3<f32>(-L0.x * 0.7, 0.5, -L0.z * 0.7));
    let NdotL1 = max(dot(N, L1), 0.0);
    let geomNdotL1 = max(dot(geomNormal, L1), 0.0);
    let horizon1 = clamp(geomNdotL1 * 3.0, 0.0, 1.0);

    if (NdotL1 > 0.0 && horizon1 > 0.001) {
        let H1 = normalize(L1 + V);
        let NdotH1 = max(dot(N, H1), 0.0);
        let VdotH1 = max(dot(V, H1), 0.0);

        let D = distributionGGX(NdotH1, roughness);
        let Vis = visibilitySmithGGX(NdotL1, NdotV, roughness);
        let F = fresnelSchlick(VdotH1, F0);
        let specularBRDF = D * Vis * F;

        let kD = (vec3<f32>(1.0) - F) * (1.0 - metallic);
        let diffuseBRDF = kD * (diffuseColor / PI);

        let fillRadiance = vec3<f32>(0.35, 0.45, 0.60) * 0.8 * diurnalAmbientScale;
        directRadiance = directRadiance + (diffuseBRDF + specularBRDF) * fillRadiance * (NdotL1 * horizon1);
    }

    // Snow crystalline micro-sparkle under direct key illumination
    if (weatherMode == 2.0 && weatherIntensity > 0.001 && geomNormal.y > 0.2) {
        let snowAccum = smoothstep(0.2, 0.75, geomNormal.y) * clamp(weatherIntensity, 0.0, 1.5);
        let sparkleCoord = floor(in.worldPos * 75.0);
        let sparkleNoise = fract(sin(dot(sparkleCoord, vec3<f32>(27.1, 59.3, 113.7))) * 43758.5453);
        let sparkle = pow(sparkleNoise, 16.0) * 3.5 * snowAccum * max(dot(N, L0), 0.0);
        directRadiance = directRadiance + vec3<f32>(sparkle);
    }

    // 11. Ambient Lighting (Hemispheric Diffuse Irradiance + GGX Environment Specular Reflection)
    let hemiWeight = N.y * 0.5 + 0.5;
    let groundAmbient = vec3<f32>(0.12, 0.11, 0.10) * diurnalAmbientScale;
    let skyAmbient = vec3<f32>(0.35, 0.45, 0.60) * diurnalAmbientScale;
    let irradiance = mix(groundAmbient, skyAmbient, hemiWeight) * 0.9;

    let F_env = fresnelSchlickRoughness(NdotV, F0, roughness);
    let kD_env = (vec3<f32>(1.0) - F_env) * (1.0 - metallic);
    let ambientDiffuse = kD_env * diffuseColor * irradiance;

    // Specular Reflection (Roughness-dependent environmental reflection modulated by cavity factor)
    let R = reflect(-V, N);
    let refHemi = R.y * 0.5 + 0.5;
    let groundReflect = vec3<f32>(0.10, 0.09, 0.08) * diurnalAmbientScale;
    let skyReflect = vec3<f32>(0.55, 0.70, 0.95) * diurnalAmbientScale;
    let envReflectCol = mix(groundReflect, skyReflect, refHemi);
    let roughnessAttenuation = 1.0 - roughness * 0.6;
    let ambientSpecular = envReflectCol * F_env * roughnessAttenuation * cavityFactor;

    let ambientColor = (ambientDiffuse + ambientSpecular) * ao;

    // 12. Total Radiance in Linear HDR Space
    var totalColor = directRadiance + ambientColor + emissive;

    // Atmospheric Weather Effects (Rain, Snow, Fog, Smoke)
    if (weatherMode == 1.0 && weatherIntensity > 0.001) {
        totalColor = totalColor + calculateRainStreaks(in.worldPos, ubo.cameraPos.xyz, weatherTime, windStrength, weatherIntensity);
    } else if (weatherMode == 2.0 && weatherIntensity > 0.001) {
        totalColor = totalColor + calculateSnowFlakes(in.worldPos, ubo.cameraPos.xyz, weatherTime, windStrength, weatherIntensity);
    } else if (weatherMode == 3.0 && weatherIntensity > 0.001) {
        totalColor = calculateFogEffect(totalColor, in.worldPos, ubo.cameraPos.xyz, L0, solarElev, weatherTime, windStrength, weatherIntensity);
    } else if (weatherMode == 4.0 && weatherIntensity > 0.001) {
        totalColor = calculateSmokePlumes(totalColor, in.worldPos, ubo.cameraPos.xyz, weatherTime, windStrength, weatherIntensity);
    }

    // 13. HDR Tonemapping (Khronos PBR Neutral) & sRGB Display Encoding
    totalColor = tonemapKhronosPbrNeutral(totalColor);
    let finalColor = linearToSrgb(totalColor);

    return vec4<f32>(finalColor, baseColor.a);
}
`;
    },

    async init() {
      if (this.isActive && this.device) return true;
      this.canvas = document.getElementById('canvas');
      if (!this.canvas) return false;

      if (!navigator.gpu) {
        BglLogger.warn('WebGPU', 'navigator.gpu is unavailable in this environment.');
        return false;
      }

      try {
        this.adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
        if (!this.adapter) {
          BglLogger.warn('WebGPU', 'navigator.gpu.requestAdapter() returned null.');
          return false;
        }

        this.device = await this.adapter.requestDevice();
        if (!this.device) {
          BglLogger.warn('WebGPU', 'adapter.requestDevice() returned null.');
          return false;
        }

        if (this.device.lost) {
          this.device.lost.then((info) => {
            BglLogger.error('WebGPU', `Device lost: ${info.reason || 'Unknown'} - ${info.message || ''}`);
            this.isActive = false;
          });
        }

        if (this.adapter.info) {
          window.cachedGpuInfo = this.adapter.info;
          BglLogger.log('WebGPU', `Acquired GPU Adapter: ${this.adapter.info.vendor || 'Unknown'} - ${this.adapter.info.architecture || ''} (${this.adapter.info.description || ''})`);
        } else {
          BglLogger.log('WebGPU', 'Acquired compatible WebGPU hardware adapter.');
        }

        this.context = this.canvas.getContext('webgpu');
        if (!this.context) {
          BglLogger.warn('WebGPU', 'canvas.getContext("webgpu") returned null.');
          return false;
        }

        this.format = navigator.gpu.getPreferredCanvasFormat();
        this.context.configure({
          device: this.device,
          format: this.format,
          alphaMode: 'opaque'
        });

        // 1x1 default white texture [255, 255, 255, 255]
        this.defaultWhiteTexture = this.device.createTexture({
          size: [1, 1, 1],
          format: 'rgba8unorm',
          usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST
        });
        this.device.queue.writeTexture(
          { texture: this.defaultWhiteTexture },
          new Uint8Array([255, 255, 255, 255]),
          { bytesPerRow: 4, rowsPerImage: 1 },
          [1, 1, 1]
        );
        this.defaultWhiteTextureView = this.defaultWhiteTexture.createView();

        // 1x1 default metallic-roughness texture [255, 255, 255, 255] (R=1, G=Roughness=1, B=Metallic=1)
        this.defaultMrTexture = this.device.createTexture({
          size: [1, 1, 1],
          format: 'rgba8unorm',
          usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST
        });
        this.device.queue.writeTexture(
          { texture: this.defaultMrTexture },
          new Uint8Array([255, 255, 255, 255]),
          { bytesPerRow: 4, rowsPerImage: 1 },
          [1, 1, 1]
        );
        this.defaultMrTextureView = this.defaultMrTexture.createView();

        // 1x1 default normal texture [128, 128, 255, 255] (tangent normal [0, 0, 1])
        this.defaultNormalTexture = this.device.createTexture({
          size: [1, 1, 1],
          format: 'rgba8unorm',
          usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST
        });
        this.device.queue.writeTexture(
          { texture: this.defaultNormalTexture },
          new Uint8Array([128, 128, 255, 255]),
          { bytesPerRow: 4, rowsPerImage: 1 },
          [1, 1, 1]
        );
        this.defaultNormalTextureView = this.defaultNormalTexture.createView();

        // 1x1 default black texture [0, 0, 0, 255] (emissive default)
        this.defaultBlackTexture = this.device.createTexture({
          size: [1, 1, 1],
          format: 'rgba8unorm',
          usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST
        });
        this.device.queue.writeTexture(
          { texture: this.defaultBlackTexture },
          new Uint8Array([0, 0, 0, 255]),
          { bytesPerRow: 4, rowsPerImage: 1 },
          [1, 1, 1]
        );
        this.defaultBlackTextureView = this.defaultBlackTexture.createView();

        // Default 16x anisotropic trilinear repeat sampler (glTF 2.0 default wrapS/wrapT = REPEAT 10497)
        this.defaultSampler = this.device.createSampler({
          addressModeU: 'repeat',
          addressModeV: 'repeat',
          addressModeW: 'repeat',
          magFilter: 'linear',
          minFilter: 'linear',
          mipmapFilter: 'linear',
          maxAnisotropy: 16
        });

        this.mipmapSampler = this.device.createSampler({
          addressModeU: 'clamp-to-edge',
          addressModeV: 'clamp-to-edge',
          magFilter: 'linear',
          minFilter: 'linear'
        });

        // 7-Entry PBR Bind Group Layout
        this.bindGroupLayout = this.device.createBindGroupLayout({
          entries: [
            {
              binding: 0,
              visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT,
              buffer: { type: 'uniform' }
            },
            {
              binding: 1,
              visibility: GPUShaderStage.FRAGMENT,
              sampler: { type: 'filtering' }
            },
            {
              binding: 2,
              visibility: GPUShaderStage.FRAGMENT,
              texture: { sampleType: 'float', viewDimension: '2d' }
            },
            {
              binding: 3,
              visibility: GPUShaderStage.FRAGMENT,
              texture: { sampleType: 'float', viewDimension: '2d' }
            },
            {
              binding: 4,
              visibility: GPUShaderStage.FRAGMENT,
              texture: { sampleType: 'float', viewDimension: '2d' }
            },
            {
              binding: 5,
              visibility: GPUShaderStage.FRAGMENT,
              texture: { sampleType: 'float', viewDimension: '2d' }
            },
            {
              binding: 6,
              visibility: GPUShaderStage.FRAGMENT,
              texture: { sampleType: 'float', viewDimension: '2d' }
            }
          ]
        });

        this.pipelineLayout = this.device.createPipelineLayout({
          bindGroupLayouts: [this.bindGroupLayout]
        });

        const shaderModule = this.device.createShaderModule({
          code: this.getShaderWgsl()
        });

        const vertexBufferLayout = {
          arrayStride: 48,
          attributes: [
            { shaderLocation: 0, offset: 0, format: 'float32x3' },  // position
            { shaderLocation: 1, offset: 12, format: 'float32x3' }, // normal
            { shaderLocation: 2, offset: 24, format: 'float32x2' }, // uv
            { shaderLocation: 3, offset: 32, format: 'float32x4' }  // tangent
          ]
        };

        this.pipeline = this.device.createRenderPipeline({
          layout: this.pipelineLayout,
          vertex: {
            module: shaderModule,
            entryPoint: 'vs_main',
            buffers: [vertexBufferLayout]
          },
          fragment: {
            module: shaderModule,
            entryPoint: 'fs_main',
            targets: [{ format: this.format }]
          },
          primitive: {
            topology: 'triangle-list',
            cullMode: 'back'
          },
          depthStencil: {
            format: 'depth24plus',
            depthWriteEnabled: true,
            depthCompare: 'less'
          }
        });

        this.pipelineDoubleSided = this.device.createRenderPipeline({
          layout: this.pipelineLayout,
          vertex: {
            module: shaderModule,
            entryPoint: 'vs_main',
            buffers: [vertexBufferLayout]
          },
          fragment: {
            module: shaderModule,
            entryPoint: 'fs_main',
            targets: [{ format: this.format }]
          },
          primitive: {
            topology: 'triangle-list',
            cullMode: 'none'
          },
          depthStencil: {
            format: 'depth24plus',
            depthWriteEnabled: true,
            depthCompare: 'less'
          }
        });

        this.mipmapBindGroupLayout = this.device.createBindGroupLayout({
          entries: [
            { binding: 1, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'filtering' } },
            { binding: 2, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float', viewDimension: '2d' } }
          ]
        });
        this.mipmapPipelineLayout = this.device.createPipelineLayout({
          bindGroupLayouts: [this.mipmapBindGroupLayout]
        });
        this.mipmapPipeline = this.device.createRenderPipeline({
          layout: this.mipmapPipelineLayout,
          vertex: { module: shaderModule, entryPoint: 'vs_mip' },
          fragment: { module: shaderModule, entryPoint: 'fs_mip', targets: [{ format: 'rgba8unorm' }] },
          primitive: { topology: 'triangle-list' }
        });

        // Initialize reusable scratch buffers for zero-allocation rendering
        this._scratchProj = this.mat4.create();
        this._scratchView = this.mat4.create();
        this._scratchViewProj = this.mat4.create();
        this._scratchUboF32 = new Float32Array(this._scratchUboBuffer, 0, 84);
        this._scratchUboU32 = new Uint32Array(this._scratchUboBuffer, 288, 4);

        this.isActive = true;
        this.initEvents();
        this.startLoop();
        BglLogger.log('WebGPU', 'Native WebGPU rendering pipeline initialized successfully.');

        if (BglState.currentGltf && BglState.currentModelBuffer) {
          BglLogger.log('WebGPU', `Immediately loading pre-parsed model '${BglState.currentModelName}' into WebGPU.`);
          this.loadModel(BglState.currentGltf, BglState.currentModelBuffer, BglState.currentModelName);
        }

        return true;
      } catch (err) {
        BglLogger.warn('WebGPU', 'Failed to initialize WebGPU renderer:', err);
        this.isActive = false;
        return false;
      }
    },

    initEvents() {
      const canvas = this.canvas;
      canvas.addEventListener('contextmenu', (e) => e.preventDefault());

      canvas.addEventListener('mousedown', (e) => {
        if (!this.isActive) return;
        this.isDragging = true;
        this.lastMouseX = e.clientX;
        this.lastMouseY = e.clientY;
        this.dragButton = e.button;
      });

      window.addEventListener('mousemove', (e) => {
        if (!this.isActive || !this.isDragging) return;
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
        if (this.isActive) this.isDragging = false;
      });

      canvas.addEventListener('wheel', (e) => {
        if (!this.isActive) return;
        e.preventDefault();
        const factor = e.deltaY > 0 ? 1.1 : 0.9;
        this.camera.distance = Math.max(this.camera.minDistance, Math.min(this.camera.maxDistance, this.camera.distance * factor));
      }, { passive: false });

      // Mobile touch controls (pinch-zoom, multi-touch pan, 1-finger orbit)
      let touchLastPos = { x: 0, y: 0 };
      let touchStartDist = 0;
      let touchLastMid = { x: 0, y: 0 };

      canvas.addEventListener('touchstart', (e) => {
        if (!this.isActive) return;
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
        if (!this.isActive) return;
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

          const factor = this.camera.distance * 0.0015;
          const cosY = Math.cos(this.camera.yaw);
          const sinY = Math.sin(this.camera.yaw);
          this.camera.target[0] -= (cosY * dMidX) * factor;
          this.camera.target[1] += dMidY * factor;
          this.camera.target[2] -= (sinY * dMidX) * factor;
        }
      }, { passive: false });
    },

    loadModel(gltf, arrayBuffer, modelName) {
      if (!this.isActive || !this.device) return;

      // Release previous GPU resources to prevent VRAM memory leak
      for (const prim of this.primitives) {
        if (prim.vertexBuffer && prim.vertexBuffer.destroy) prim.vertexBuffer.destroy();
        if (prim.indexBuffer && prim.indexBuffer.destroy) prim.indexBuffer.destroy();
        if (prim.uniformBuffer && prim.uniformBuffer.destroy) prim.uniformBuffer.destroy();
      }
      for (const entry of this.textureCache.values()) {
        if (entry.texture && entry.texture.destroy) entry.texture.destroy();
      }
      this.textureCache.clear();
      this.primitives = [];

      let binBuffer = null;
      if (arrayBuffer && arrayBuffer.byteLength >= 12) {
        const dv = new DataView(arrayBuffer, 0, 12);
        const magic = dv.getUint32(0, true);
        if (magic === 0x46546C67) {
          let byteOffset = 12;
          while (byteOffset + 8 <= arrayBuffer.byteLength) {
            const chunkDv = new DataView(arrayBuffer, byteOffset, 8);
            const chunkLen = chunkDv.getUint32(0, true);
            const chunkType = chunkDv.getUint32(4, true);
            if (chunkType === 0x004E4942) {
              binBuffer = arrayBuffer.slice(byteOffset + 8, byteOffset + 8 + chunkLen);
              break;
            }
            byteOffset += 8 + chunkLen;
          }
        }
      }

      const embeddedBuffers = [];
      if (gltf.buffers) {
        for (let i = 0; i < gltf.buffers.length; ++i) {
          const buf = gltf.buffers[i];
          if (buf.uri && buf.uri.startsWith('data:')) {
            const commaIdx = buf.uri.indexOf(',');
            if (commaIdx !== -1) {
              const base64Str = buf.uri.slice(commaIdx + 1);
              try {
                const binStr = atob(base64Str);
                const bytes = new Uint8Array(binStr.length);
                for (let b = 0; b < binStr.length; ++b) {
                  bytes[b] = binStr.charCodeAt(b);
                }
                embeddedBuffers[i] = bytes.buffer;
              } catch (e) {
                BglLogger.warn('glTF', 'Failed to decode base64 buffer URI:', e);
              }
            }
          }
        }
      }

      const getBufferViewData = (viewIdx) => {
        if (!gltf.bufferViews || !gltf.bufferViews[viewIdx]) return null;
        const bv = gltf.bufferViews[viewIdx];
        const offset = bv.byteOffset || 0;
        const length = bv.byteLength || 0;
        const bufferIdx = bv.buffer || 0;
        const src = (binBuffer && bufferIdx === 0) ? binBuffer : (embeddedBuffers[bufferIdx] || binBuffer || arrayBuffer);
        if (!src || offset + length > src.byteLength) {
          BglLogger.warn('glTF', `BufferView ${viewIdx} out of bounds: offset ${offset} + length ${length} > ${src ? src.byteLength : 0}`);
          return null;
        }
        return src.slice(offset, offset + length);
      };

      const getAccessorData = (accIdx) => {
        if (!gltf.accessors || !gltf.accessors[accIdx]) return null;
        const acc = gltf.accessors[accIdx];
        if (!acc.count) return null;
        if (acc.bufferView === undefined) return null;
        const bv = gltf.bufferViews ? gltf.bufferViews[acc.bufferView] : null;
        if (!bv) return null;
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
        const elemBytes = TypedArray.BYTES_PER_ELEMENT;
        const stride = bv.byteStride || (numComp * elemBytes);

        if (byteOffset + (acc.count - 1) * stride + (numComp * elemBytes) > bvData.byteLength) {
          BglLogger.warn('glTF', `Accessor ${accIdx} exceeds buffer view byte length.`);
          return null;
        }

        if (stride === numComp * elemBytes && (byteOffset % elemBytes === 0)) {
          return new TypedArray(bvData, byteOffset, acc.count * numComp);
        }

        const totalElems = acc.count * numComp;
        const out = new TypedArray(totalElems);
        const dView = new DataView(bvData);
        for (let i = 0; i < acc.count; ++i) {
          const itemOffset = byteOffset + i * stride;
          for (let c = 0; c < numComp; ++c) {
            const valOffset = itemOffset + c * elemBytes;
            if (valOffset + elemBytes <= dView.byteLength) {
              if (acc.componentType === 5126) out[i * numComp + c] = dView.getFloat32(valOffset, true);
              else if (acc.componentType === 5123) out[i * numComp + c] = dView.getUint16(valOffset, true);
              else if (acc.componentType === 5125) out[i * numComp + c] = dView.getUint32(valOffset, true);
              else if (acc.componentType === 5121) out[i * numComp + c] = dView.getUint8(valOffset);
              else if (acc.componentType === 5122) out[i * numComp + c] = dView.getInt16(valOffset, true);
            }
          }
        }
        return out;
      };

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
            worldMat.set(localMat);
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
            const tanData = prim.attributes.TANGENT !== undefined ? getAccessorData(prim.attributes.TANGENT) : null;
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

            const vertexCount = posData.length / 3;
            const interleaved = new Float32Array(vertexCount * 12);
            for (let i = 0; i < vertexCount; ++i) {
              const base = i * 12;
              const pIdx = i * 3;
              interleaved[base + 0] = posData[pIdx + 0];
              interleaved[base + 1] = posData[pIdx + 1];
              interleaved[base + 2] = posData[pIdx + 2];

              if (normData) {
                interleaved[base + 3] = normData[pIdx + 0];
                interleaved[base + 4] = normData[pIdx + 1];
                interleaved[base + 5] = normData[pIdx + 2];
              } else {
                interleaved[base + 3] = 0.0;
                interleaved[base + 4] = 1.0;
                interleaved[base + 5] = 0.0;
              }

              if (uvData) {
                const uIdx = i * 2;
                interleaved[base + 6] = uvData[uIdx + 0];
                interleaved[base + 7] = uvData[uIdx + 1];
              } else {
                interleaved[base + 6] = 0.0;
                interleaved[base + 7] = 0.0;
              }

              if (tanData) {
                const tIdx = i * 4;
                interleaved[base + 8] = tanData[tIdx + 0];
                interleaved[base + 9] = tanData[tIdx + 1];
                interleaved[base + 10] = tanData[tIdx + 2];
                interleaved[base + 11] = tanData[tIdx + 3];
              } else {
                interleaved[base + 8] = 0.0;
                interleaved[base + 9] = 0.0;
                interleaved[base + 10] = 0.0;
                interleaved[base + 11] = 0.0;
              }
            }

            const vertexBuffer = this.device.createBuffer({
              size: Math.max(48, interleaved.byteLength),
              usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST
            });
            this.device.queue.writeBuffer(vertexBuffer, 0, interleaved);

            let indexBuffer = null;
            let indexFormat = 'uint16';
            let indexCount = 0;
            if (indexData) {
              let gpuIndexData = indexData;
              if (indexData instanceof Uint8Array) {
                gpuIndexData = new Uint16Array(indexData);
                indexFormat = 'uint16';
              } else if (indexData instanceof Uint32Array) {
                indexFormat = 'uint32';
              } else {
                indexFormat = 'uint16';
              }
              indexCount = gpuIndexData.length;

              // Ensure buffer size and write size are strictly multiples of 4 (W3C WebGPU specification requirement)
              const rawByteLength = gpuIndexData.byteLength;
              const alignedByteLength = Math.max(4, Math.ceil(rawByteLength / 4) * 4);
              indexBuffer = this.device.createBuffer({
                size: alignedByteLength,
                usage: GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST
              });

              if (rawByteLength % 4 === 0) {
                this.device.queue.writeBuffer(indexBuffer, 0, gpuIndexData);
              } else {
                const paddedBytes = new Uint8Array(alignedByteLength);
                paddedBytes.set(new Uint8Array(gpuIndexData.buffer, gpuIndexData.byteOffset, rawByteLength));
                this.device.queue.writeBuffer(indexBuffer, 0, paddedBytes);
              }
            }

            // Material parsing (PBR Metallic-Roughness, Normal, Occlusion, Emissive, Alpha, Transform)
            let baseColorFactor = [1.0, 1.0, 1.0, 1.0];
            let metallicFactor = 1.0;
            let roughnessFactor = 1.0;
            let normalScale = 1.0;
            let occlusionStrength = 1.0;
            let emissiveFactor = [0.0, 0.0, 0.0];
            let alphaCutoff = 0.5;
            let alphaMode = 0; // 0=OPAQUE, 1=MASK, 2=BLEND
            let doubleSided = false;
            let uvScale = [1.0, 1.0];
            let uvOffset = [0.0, 0.0];

            if (prim.material !== undefined && gltf.materials && gltf.materials[prim.material]) {
              const mat = gltf.materials[prim.material];
              if (mat.doubleSided) doubleSided = true;
              if (mat.alphaMode === 'MASK') alphaMode = 1;
              else if (mat.alphaMode === 'BLEND') alphaMode = 2;
              if (mat.alphaCutoff !== undefined) alphaCutoff = mat.alphaCutoff;
              if (mat.emissiveFactor) emissiveFactor = mat.emissiveFactor;

              if (mat.pbrMetallicRoughness) {
                const pbr = mat.pbrMetallicRoughness;
                if (pbr.baseColorFactor) baseColorFactor = pbr.baseColorFactor;
                if (pbr.metallicFactor !== undefined) metallicFactor = pbr.metallicFactor;
                if (pbr.roughnessFactor !== undefined) roughnessFactor = pbr.roughnessFactor;
                if (pbr.baseColorTexture && pbr.baseColorTexture.extensions && pbr.baseColorTexture.extensions.KHR_texture_transform) {
                  const tt = pbr.baseColorTexture.extensions.KHR_texture_transform;
                  if (tt.scale) uvScale = [tt.scale[0], tt.scale[1]];
                  if (tt.offset) uvOffset = [tt.offset[0], tt.offset[1]];
                }
              }

              if (mat.normalTexture) {
                if (mat.normalTexture.scale !== undefined) normalScale = mat.normalTexture.scale;
                if (mat.normalTexture.extensions && mat.normalTexture.extensions.KHR_texture_transform) {
                  const tt = mat.normalTexture.extensions.KHR_texture_transform;
                  if (tt.scale) uvScale = [tt.scale[0], tt.scale[1]];
                  if (tt.offset) uvOffset = [tt.offset[0], tt.offset[1]];
                }
              }

              if (mat.occlusionTexture && mat.occlusionTexture.strength !== undefined) {
                occlusionStrength = mat.occlusionTexture.strength;
              }
            }

            const currentPrimRecords = [];
            for (const worldMatrix of nodeMats) {
              const uniformBuffer = this.device.createBuffer({
                size: 512,
                usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST
              });

              const primRecord = {
                vertexBuffer,
                indexBuffer,
                indexCount,
                indexFormat,
                vertexCount,
                hasIndices: !!indexData,
                uniformBuffer,
                bindGroup: null,
                baseColorFactor,
                metallicFactor,
                roughnessFactor,
                normalScale,
                occlusionStrength,
                emissiveFactor,
                alphaCutoff,
                alphaMode,
                doubleSided,
                uvScale,
                uvOffset,
                flags: 0,
                baseColorView: null,
                mrView: null,
                normalView: null,
                emissiveView: null,
                occlusionView: null,
                modelMatrix: worldMatrix,
                updateBindGroup: null
              };

              primRecord.updateBindGroup = () => {
                primRecord.bindGroup = this.device.createBindGroup({
                  layout: this.bindGroupLayout,
                  entries: [
                    { binding: 0, resource: { buffer: primRecord.uniformBuffer } },
                    { binding: 1, resource: this.defaultSampler },
                    { binding: 2, resource: primRecord.baseColorView || this.defaultWhiteTextureView },
                    { binding: 3, resource: primRecord.mrView || this.defaultMrTextureView },
                    { binding: 4, resource: primRecord.normalView || this.defaultNormalTextureView },
                    { binding: 5, resource: primRecord.emissiveView || this.defaultBlackTextureView },
                    { binding: 6, resource: primRecord.occlusionView || this.defaultWhiteTextureView }
                  ]
                });
              };

              primRecord.updateBindGroup();
              currentPrimRecords.push(primRecord);
              this.primitives.push(primRecord);
            }

            // Asynchronously load textures and update bind groups
            if (prim.material !== undefined && gltf.materials && gltf.materials[prim.material]) {
              const mat = gltf.materials[prim.material];

              const loadTextureImage = (texInfo, viewSetter, flagBit) => {
                if (!texInfo || texInfo.index === undefined || !gltf.textures || !gltf.textures[texInfo.index]) return;
                const texObj = gltf.textures[texInfo.index];
                if (texObj.source === undefined || !gltf.images || !gltf.images[texObj.source]) return;
                const imgIdx = texObj.source;
                const imgObj = gltf.images[imgIdx];

                const applyView = (view) => {
                  for (const pr of currentPrimRecords) {
                    viewSetter(pr, view);
                    pr.flags |= flagBit;
                    pr.updateBindGroup();
                  }
                };

                if (this.textureCache.has(imgIdx)) {
                  const cached = this.textureCache.get(imgIdx);
                  if (cached.view) {
                    applyView(cached.view);
                  } else if (cached.waiters) {
                    cached.waiters.push(applyView);
                  }
                  return;
                }

                // Register pending entry immediately to deduplicate concurrent async decodes across primitives
                this.textureCache.set(imgIdx, { texture: null, view: null, waiters: [applyView] });

                const onImageReady = (bmp) => {
                  try {
                    if (!this.device) return;
                    const mipLevelCount = Math.max(1, Math.floor(Math.log2(Math.max(bmp.width || 1, bmp.height || 1))) + 1);
                    const gpuTex = this.device.createTexture({
                      size: [bmp.width, bmp.height, 1],
                      mipLevelCount,
                      format: 'rgba8unorm',
                      usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT
                    });
                    this.device.queue.copyExternalImageToTexture(
                      { source: bmp },
                      { texture: gpuTex, mipLevel: 0 },
                      [bmp.width, bmp.height]
                    );
                    if (mipLevelCount > 1) {
                      this.generateMipmaps(gpuTex, bmp.width, bmp.height, mipLevelCount);
                    }
                    if (typeof bmp.close === 'function') {
                      bmp.close();
                    }
                    const gpuView = gpuTex.createView();
                    const prevEntry = this.textureCache.get(imgIdx);
                    const waiters = (prevEntry && prevEntry.waiters) ? prevEntry.waiters : [applyView];
                    this.textureCache.set(imgIdx, { texture: gpuTex, view: gpuView, waiters: null });
                    for (const fn of waiters) {
                      fn(gpuView);
                    }
                  } catch (texErr) {
                    BglLogger.warn('WebGPU', `Failed to upload texture image #${imgIdx}:`, texErr);
                  }
                };

                const decodeBitmap = (srcObj) => {
                  const opts = { colorSpaceConversion: 'none' };
                  createImageBitmap(srcObj, opts).then(onImageReady).catch(() => {
                    createImageBitmap(srcObj).then(onImageReady).catch((err) => {
                      BglLogger.warn('WebGPU', `createImageBitmap failed for image #${imgIdx}:`, err);
                    });
                  });
                };

                if (imgObj.bufferView !== undefined) {
                  const imgBytes = getBufferViewData(imgObj.bufferView);
                  if (imgBytes) {
                    const blob = new Blob([imgBytes], { type: imgObj.mimeType || 'image/png' });
                    decodeBitmap(blob);
                  }
                } else if (imgObj.uri) {
                  const img = new Image();
                  img.crossOrigin = 'anonymous';
                  img.onload = () => {
                    decodeBitmap(img);
                  };
                  img.src = imgObj.uri;
                }
              };

              if (mat.pbrMetallicRoughness) {
                if (mat.pbrMetallicRoughness.baseColorTexture) {
                  loadTextureImage(mat.pbrMetallicRoughness.baseColorTexture, (pr, v) => { pr.baseColorView = v; }, 1);
                }
                if (mat.pbrMetallicRoughness.metallicRoughnessTexture) {
                  loadTextureImage(mat.pbrMetallicRoughness.metallicRoughnessTexture, (pr, v) => { pr.mrView = v; }, 2);
                }
              }

              if (mat.normalTexture) {
                loadTextureImage(mat.normalTexture, (pr, v) => { pr.normalView = v; }, 4);
              }
              if (mat.emissiveTexture) {
                loadTextureImage(mat.emissiveTexture, (pr, v) => { pr.emissiveView = v; }, 8);
              }
              if (mat.occlusionTexture) {
                loadTextureImage(mat.occlusionTexture, (pr, v) => { pr.occlusionView = v; }, 16);
              }
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
        const rawRadius = Math.hypot(dx, dy, dz) * 0.5;
        const radius = Math.max(rawRadius, 0.0001);

        const isSponza = /sponza/i.test(modelName || '') || (dx > 25.0 && dx < 32.0 && dy > 10.0 && dy < 15.0 && dz > 15.0 && dz < 22.0);
        if (isSponza) {
          this.camera.target = [cx, minBound[1] + dy * 0.25, cz];
          this.camera.distance = 7.2;
          this.camera.yaw = 1.5708;
          this.camera.pitch = 0.06;
        } else {
          this.camera.target = [cx, cy, cz];
          this.camera.distance = radius * 2.2;
        }
        this.camera.minDistance = Math.max(0.0001, radius * 0.02);
        this.camera.maxDistance = Math.max(10.0, radius * 35.0);
        this.camera.near = Math.max(0.0001, radius * 0.008);
        this.camera.far = Math.max(100.0, radius * 50.0);
        this.modelBounds = { center: [cx, cy, cz], radius, isSponza, minBound, maxBound };
      }

      BglLogger.log('WebGPU', `Loaded '${modelName}' into WebGPU rendering pipeline (${this.primitives.length} primitives).`);
    },

    generateMipmaps(gpuTex, width, height, mipLevelCount) {
      if (!this.device || !this.mipmapPipeline || mipLevelCount <= 1 || typeof this.device.createCommandEncoder !== 'function') return;
      try {
        const encoder = this.device.createCommandEncoder();
        for (let mip = 1; mip < mipLevelCount; ++mip) {
          const srcView = gpuTex.createView({ baseMipLevel: mip - 1, mipLevelCount: 1 });
          const dstView = gpuTex.createView({ baseMipLevel: mip, mipLevelCount: 1 });
          const bindGroup = this.device.createBindGroup({
            layout: this.mipmapBindGroupLayout,
            entries: [
              { binding: 1, resource: this.mipmapSampler || this.defaultSampler },
              { binding: 2, resource: srcView }
            ]
          });
          const pass = encoder.beginRenderPass({
            colorAttachments: [{
              view: dstView,
              clearValue: { r: 0, g: 0, b: 0, a: 1 },
              loadOp: 'clear',
              storeOp: 'store'
            }]
          });
          pass.setPipeline(this.mipmapPipeline);
          pass.setBindGroup(0, bindGroup);
          pass.draw(3);
          pass.end();
        }
        this.device.queue.submit([encoder.finish()]);
      } catch (mipErr) {
        BglLogger.warn('WebGPU', 'Mipmap generation notice:', mipErr);
      }
    },

    startLoop() {
      const render = (time) => {
        if (!this.isActive || !this.device || !this.context) return;
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
      if (!this.isActive || !this.device || !this.context) return;

      const dpr = Math.min(window.devicePixelRatio || 1, 2);
      const w = Math.max(1, Math.round((this.canvas.clientWidth || window.innerWidth) * dpr));
      const h = Math.max(1, Math.round((this.canvas.clientHeight || window.innerHeight) * dpr));

      if (this.canvas.width !== w || this.canvas.height !== h || !this.depthTexture) {
        this.canvas.width = w;
        this.canvas.height = h;

        if (this.depthTexture) {
          this.depthTexture.destroy();
        }
        this.depthTexture = this.device.createTexture({
          size: [w, h, 1],
          format: 'depth24plus',
          usage: GPUTextureUsage.RENDER_ATTACHMENT
        });
        this.depthTextureView = this.depthTexture.createView();

        this.context.configure({
          device: this.device,
          format: this.format,
          alphaMode: 'opaque'
        });
      }

      let currentTexture;
      try {
        currentTexture = this.context.getCurrentTexture();
      } catch (texErr) {
        return;
      }

      const nowMs = performance.now();
      const dtSec = this._lastDrawMs ? Math.min((nowMs - this._lastDrawMs) * 0.001, 0.1) : (1.0 / 60.0);
      this._lastDrawMs = nowMs;

      if (typeof BglState !== 'undefined' && BglState.isDayNightCycleActive) {
        const speed = (BglState.dayNightSpeed !== undefined) ? BglState.dayNightSpeed : 1.0;
        let nextTod = ((BglState.timeOfDay !== undefined ? BglState.timeOfDay : 14.0) + speed * dtSec) % 24.0;
        if (nextTod < 0.0) nextTod += 24.0;
        BglState.timeOfDay = nextTod;
        const sliderTod = document.getElementById('slider-time-of-day');
        if (sliderTod) sliderTod.value = nextTod.toFixed(2);
        const labelTod = document.getElementById('val-time-of-day');
        if (labelTod) {
          const hrs = Math.floor(nextTod) % 24;
          const mins = Math.floor((nextTod - Math.floor(nextTod)) * 60) % 60;
          const icon = (nextTod >= 7.0 && nextTod < 17.5) ? '☀️' : ((nextTod >= 5.5 && nextTod < 19.5) ? '🌅' : '🌙');
          labelTod.textContent = `${String(hrs).padStart(2, '0')}:${String(mins).padStart(2, '0')} ${icon}`;
        }
      }

      const tod = (typeof BglState !== 'undefined' && BglState.timeOfDay !== undefined) ? BglState.timeOfDay : 14.0;
      const solarElev = -Math.cos((tod / 24.0) * 2.0 * Math.PI);
      const dayBlend = Math.max(0.0, Math.min(1.0, (solarElev + 0.15) / 0.85));
      let clearR = 0.015 + 0.045 * dayBlend;
      let clearG = 0.022 + 0.068 * dayBlend;
      let clearB = 0.045 + 0.120 * dayBlend;

      const wModeStr = (typeof BglState !== 'undefined' && BglState.weatherMode) ? BglState.weatherMode : 'none';
      const weatherIntensity = (typeof BglState !== 'undefined' && BglState.weatherIntensity !== undefined) ? BglState.weatherIntensity : 1.0;
      if (wModeStr === 'fog' && weatherIntensity > 0.001) {
        const fogMix = Math.min(weatherIntensity * 0.65, 0.85);
        clearR = clearR * (1.0 - fogMix) + (0.42 * dayBlend + 0.06) * fogMix;
        clearG = clearG * (1.0 - fogMix) + (0.48 * dayBlend + 0.08) * fogMix;
        clearB = clearB * (1.0 - fogMix) + (0.55 * dayBlend + 0.12) * fogMix;
      } else if (wModeStr === 'smoke' && weatherIntensity > 0.001) {
        const smokeMix = Math.min(weatherIntensity * 0.70, 0.88);
        clearR = clearR * (1.0 - smokeMix) + 0.12 * smokeMix;
        clearG = clearG * (1.0 - smokeMix) + 0.12 * smokeMix;
        clearB = clearB * (1.0 - smokeMix) + 0.14 * smokeMix;
      }

      const commandEncoder = this.device.createCommandEncoder();
      const renderPassDesc = {
        colorAttachments: [
          {
            view: currentTexture.createView(),
            clearValue: { r: clearR, g: clearG, b: clearB, a: 1.0 },
            loadOp: 'clear',
            storeOp: 'store'
          }
        ],
        depthStencilAttachment: {
          view: this.depthTextureView,
          depthClearValue: 1.0,
          depthLoadOp: 'clear',
          depthStoreOp: 'store'
        }
      };

      const pass = commandEncoder.beginRenderPass(renderPassDesc);

      const cy = Math.cos(this.camera.yaw);
      const sy = Math.sin(this.camera.yaw);
      const cp = Math.cos(this.camera.pitch);
      const sp = Math.sin(this.camera.pitch);

      const eye = this._scratchEye;
      eye[0] = this.camera.target[0] + this.camera.distance * cp * sy;
      eye[1] = this.camera.target[1] + this.camera.distance * sp;
      eye[2] = this.camera.target[2] + this.camera.distance * cp * cy;

      const proj = this._scratchProj;
      this.mat4.perspective(proj, this.camera.fov, w / Math.max(1, h), this.camera.near, this.camera.far);

      const view = this._scratchView;
      this.mat4.lookAt(view, eye, this.camera.target, [0, 1, 0]);

      const viewProj = this._scratchViewProj;
      this.mat4.multiply(viewProj, proj, view);

      const azimuth = (tod / 24.0) * 2.0 * Math.PI;
      const activeElevSin = (solarElev >= 0.0) ? Math.max(solarElev, 0.08) : Math.max(-solarElev * 0.75, 0.15);
      const activeElevCos = Math.sqrt(Math.max(0.0, 1.0 - activeElevSin * activeElevSin));
      const lightDir = this._scratchLightDir;
      lightDir[0] = activeElevCos * Math.sin(azimuth);
      lightDir[1] = activeElevSin;
      lightDir[2] = activeElevCos * Math.cos(azimuth);

      const normalMat = this._scratchNormalMat;
      const uboBuffer = this._scratchUboBuffer;
      const uboF32 = this._scratchUboF32;
      const uboU32 = this._scratchUboU32;

      for (const prim of this.primitives) {
        this.mat4.normalMatrix4(normalMat, prim.modelMatrix);

        uboF32.set(viewProj, 0);
        uboF32.set(prim.modelMatrix, 16);
        uboF32.set(normalMat, 32);
        uboF32[48] = eye[0]; uboF32[49] = eye[1]; uboF32[50] = eye[2]; uboF32[51] = 1.0;
        uboF32[52] = lightDir[0]; uboF32[53] = lightDir[1]; uboF32[54] = lightDir[2]; uboF32[55] = solarElev;
        uboF32[56] = prim.baseColorFactor[0];
        uboF32[57] = prim.baseColorFactor[1];
        uboF32[58] = prim.baseColorFactor[2];
        uboF32[59] = prim.baseColorFactor[3];

        uboF32[60] = prim.metallicFactor;
        uboF32[61] = prim.roughnessFactor;
        uboF32[62] = prim.normalScale;
        uboF32[63] = prim.occlusionStrength;

        uboF32[64] = prim.emissiveFactor[0];
        uboF32[65] = prim.emissiveFactor[1];
        uboF32[66] = prim.emissiveFactor[2];
        uboF32[67] = prim.alphaCutoff;

        uboF32[68] = prim.uvScale[0];
        uboF32[69] = prim.uvScale[1];
        uboF32[70] = prim.uvOffset[0];
        uboF32[71] = prim.uvOffset[1];

        uboU32[0] = prim.flags;
        uboU32[1] = this.wireframe ? 1 : 0;
        uboU32[2] = prim.doubleSided ? 1 : 0;
        uboU32[3] = prim.alphaMode;

        uboF32[76] = (typeof BglState !== 'undefined' && BglState.bumpScale !== undefined) ? BglState.bumpScale : 1.5;
        uboF32[77] = (typeof BglState !== 'undefined' && BglState.pomScale !== undefined) ? BglState.pomScale : 0.035;
        uboF32[78] = (typeof BglState !== 'undefined' && BglState.isPomActive) ? 1.0 : 0.0;
        uboF32[79] = (typeof BglState !== 'undefined' && BglState.isBumpActive !== undefined) ? (BglState.isBumpActive ? 1.0 : 0.0) : 1.0;

        const weatherMode = wModeStr === 'rain' ? 1.0 : (wModeStr === 'snow' ? 2.0 : (wModeStr === 'fog' ? 3.0 : (wModeStr === 'smoke' ? 4.0 : 0.0)));
        const windStrength = (typeof BglState !== 'undefined' && BglState.windStrength !== undefined) ? BglState.windStrength : 0.3;
        const timeSec = (nowMs * 0.001);

        uboF32[80] = weatherMode;
        uboF32[81] = weatherIntensity;
        uboF32[82] = windStrength;
        uboF32[83] = timeSec;

        this.device.queue.writeBuffer(prim.uniformBuffer, 0, uboBuffer);

        const pipeline = prim.doubleSided ? this.pipelineDoubleSided : this.pipeline;
        pass.setPipeline(pipeline);
        pass.setBindGroup(0, prim.bindGroup);
        pass.setVertexBuffer(0, prim.vertexBuffer);

        if (prim.hasIndices) {
          pass.setIndexBuffer(prim.indexBuffer, prim.indexFormat);
          pass.drawIndexed(prim.indexCount);
        } else {
          pass.draw(prim.vertexCount);
        }
      }

      pass.end();
      this.device.queue.submit([commandEncoder.finish()]);
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
        case 'X': toggleRain(); break;
        case 'Y': toggleSnow(); break;
        case 'F6': toggleFog(); break;
        case 'F7': toggleSmoke(); break;
        case 'I':
        case 'F8': toggleDayNightCycle(); break;
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
