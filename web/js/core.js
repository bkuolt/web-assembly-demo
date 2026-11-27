/**
 * @file js/core.js
 * @brief Core state, structured logger, toast notifier, cache, network, and WASM bridge.
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
      console.log('%c[%s]%c %s - %s', style, category, 'color: inherit;', ts, String(message), ...args);
    },

    warn(category, message, ...args) {
      const style = this.badgeStyles.Warning;
      const ts = this.formatTimestamp();
      console.warn('%c[%s ⚠️]%c %s - %s', style, category, 'color: inherit;', ts, String(message), ...args);
    },

    error(category, message, ...args) {
      const style = this.badgeStyles.Error;
      const ts = this.formatTimestamp();
      console.error('%c[%s ✕]%c %s - %s', style, category, 'color: inherit;', ts, String(message), ...args);
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
        '%c bgl-Engine %c WebAssembly & WebGPU 3D Runtime v1.2.0 ',
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
  // Application State & UI Coordinator
  // ============================================================================
  // Keep global BglFrameScheduler removal clean inside app.js by exporting it here.
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
    isLodActive: true,
    isAutoLodActive: false,
    lodBias: 1.0,
    debugViewMode: 0,
  };
  window.BglState = BglState;

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

    elDims: document.getElementById('aabb-dims'),
    elMin: document.getElementById('aabb-min'),
    elMax: document.getElementById('aabb-max'),
    elCenter: document.getElementById('aabb-center'),
    elPrims: document.getElementById('aabb-prims'),
    elLodTelemetry: document.getElementById('lod-telemetry'),
    sliderLodBias: document.getElementById('slider-lod-bias'),
    valLodBias: document.getElementById('val-lod-bias'),
    sliderTimeOfDay: document.getElementById('slider-time-of-day'),
    valTimeOfDay: document.getElementById('val-time-of-day'),
    hudFpsPill: document.getElementById('hud-fps-pill'),
    modelNameLabel: document.getElementById('model-name-label'),
    nodeCountBadge: document.getElementById('node-count-badge'),
    gamepadBadge: document.getElementById('gamepad-badge'),
    gamepadLabel: document.getElementById('gamepad-label'),

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
    MAX_MEMORY_ENTRIES: 4,
    MAX_MEMORY_BYTES: 160 * 1024 * 1024,
    memoryFallback: new Map(),

    _isCacheableUrl(url) {
      if (typeof url !== 'string' || !url || /^local:/i.test(url)) return false;
      return !BglNetwork || BglNetwork.validateAssetUrl(url, { allowData: false, allowCustomHttps: true }).ok;
    },

    _enforceMemoryCap() {
      let total = 0;
      for (const buf of this.memoryFallback.values()) {
        total += buf ? buf.byteLength : 0;
      }
      while (this.memoryFallback.size > this.MAX_MEMORY_ENTRIES || total > this.MAX_MEMORY_BYTES) {
        const oldestKey = this.memoryFallback.keys().next().value;
        if (oldestKey === undefined) break;
        const oldBuf = this.memoryFallback.get(oldestKey);
        this.memoryFallback.delete(oldestKey);
        total -= oldBuf ? oldBuf.byteLength : 0;
      }
    },

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
      if (!this._isCacheableUrl(url)) return false;
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
      if (!this._isCacheableUrl(url)) return null;
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
      if (!this._isCacheableUrl(url) || !arrayBuffer || !(arrayBuffer instanceof ArrayBuffer)) {
        return;
      }
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
      this._enforceMemoryCap();
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
          indicator.textContent = `${count} Model${count > 1 ? 's' : ''} Cached`;
          indicator.classList.add('has-cache');
        } else {
          indicator.textContent = `Browser Storage Ready`;
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
        const baseLabel = opt.getAttribute('data-label') || opt.textContent.replace(' [Cached]', '').replace(' ⚡', '');
        opt.setAttribute('data-label', baseLabel);
        opt.textContent = isCached ? `${baseLabel} [Cached]` : baseLabel;
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
    MAX_MODEL_BYTES: 128 * 1024 * 1024,
    FETCH_TIMEOUT_MS: 60000,
    ALLOWED_QUERY_HOSTS: new Set(['raw.githubusercontent.com']),

    /**
     * Validate asset URLs against strict scheme, credential, path, and CORS allowlist policies.
     * @param {string} rawUrl
     * @param {{ allowData?: boolean, allowCustomHttps?: boolean }} [opts]
     * @returns {{ ok: boolean, url?: string, reason?: string }}
     */
    validateAssetUrl(rawUrl, opts = {}) {
      const { allowData = false, allowCustomHttps = false } = opts || {};
      if (typeof rawUrl !== 'string') {
        return { ok: false, reason: 'URL must be a string' };
      }
      const url = rawUrl.trim();
      if (!url || /[\u0000-\u001F\u007F]/.test(url)) {
        return { ok: false, reason: 'Empty URL or control characters detected' };
      }
      if (/^(?:javascript|vbscript|file|blob|ftp):/i.test(url)) {
        return { ok: false, reason: 'Disallowed URI scheme' };
      }
      if (/^data:/i.test(url)) {
        if (!allowData) {
          return { ok: false, reason: 'Data URI not permitted in this context' };
        }
        const safeDataRe = /^data:(?:application\/octet-stream|model\/gltf-binary|model\/gltf\+json|image\/(?:png|jpeg|webp))(?:[;,]|$)/i;
        if (!safeDataRe.test(url)) {
          return { ok: false, reason: 'Disallowed data URI MIME type' };
        }
        return { ok: true, url };
      }

      const loc = globalThis.location || (typeof window !== 'undefined' ? window.location : null);
      const baseHref = (loc && loc.href)
        ? loc.href
        : (loc && loc.protocol && loc.host ? `${loc.protocol}//${loc.host}${loc.pathname || '/'}` : 'https://localhost/web/index.html');

      let parsed;
      let baseParsed;
      try {
        baseParsed = new URL(baseHref);
        parsed = new URL(url, baseParsed);
      } catch (_) {
        return { ok: false, reason: 'Malformed URL' };
      }

      if (parsed.username || parsed.password) {
        return { ok: false, reason: 'Embedded URL credentials are prohibited' };
      }

      const isLoopback = parsed.hostname === 'localhost' || parsed.hostname === '127.0.0.1' || parsed.hostname === '[::1]';
      if (parsed.protocol !== 'https:' && !(parsed.protocol === 'http:' && isLoopback)) {
        return { ok: false, reason: `Insecure scheme '${parsed.protocol}' is not allowed` };
      }

      const sameOrigin = parsed.origin === baseParsed.origin;
      if (sameOrigin) {
        if (/\/\.\.(\/|$)/.test(url)) {
          return { ok: false, reason: 'Path traversal sequence prohibited' };
        }
        return { ok: true, url: parsed.href };
      }

      if (!allowCustomHttps && !this.ALLOWED_QUERY_HOSTS.has(parsed.hostname.toLowerCase())) {
        return { ok: false, reason: `Untrusted external host '${parsed.hostname}'` };
      }
      return { ok: true, url: parsed.href };
    },

    /**
     * Download binary resource with real-time streaming progress, size cap, and timeout.
     * @param {string} url
     * @param {function(object): void} onProgress ({ percent, msg, transferredBytes, totalBytes, speedMbS })
     * @param {AbortSignal} [signal]
     * @returns {Promise<ArrayBuffer>}
     */
    async fetchWithProgress(url, onProgress, signal) {
      const check = this.validateAssetUrl(url, { allowData: true, allowCustomHttps: true });
      if (!check.ok) {
        throw new Error(`Blocked unsafe asset URL '${url}': ${check.reason}`);
      }
      const safeUrl = check.url;

      const startTime = performance.now();
      BglLogger.log('Network', `Initiating fetch for resource: ${safeUrl}`);

      const fetchCtrl = new AbortController();
      let timedOut = false;
      const timeoutId = setTimeout(() => {
        timedOut = true;
        fetchCtrl.abort();
      }, this.FETCH_TIMEOUT_MS);

      const onExternalAbort = () => fetchCtrl.abort();
      if (signal) {
        if (signal.aborted) {
          clearTimeout(timeoutId);
          throw new Error('Download aborted by user.');
        }
        signal.addEventListener('abort', onExternalAbort, { once: true });
      }

      let response;
      try {
        response = await fetch(safeUrl, {
          signal: fetchCtrl.signal,
          credentials: 'omit',
          mode: 'cors',
          referrerPolicy: 'no-referrer'
        });
      } catch (networkErr) {
        clearTimeout(timeoutId);
        if (signal) signal.removeEventListener('abort', onExternalAbort);
        if (timedOut) {
          throw new Error(`Request timed out after ${this.FETCH_TIMEOUT_MS / 1000}s while fetching '${safeUrl}'.`);
        }
        if (signal && signal.aborted) {
          throw new Error('Download aborted by user.');
        }
        throw new Error(`Network connection error fetching '${safeUrl}': ${networkErr.message}`);
      }

      if (!response.ok) {
        clearTimeout(timeoutId);
        if (signal) signal.removeEventListener('abort', onExternalAbort);
        throw new Error(`HTTP ${response.status} (${response.statusText || 'Not Found'}) while fetching '${safeUrl}'`);
      }

      const contentLengthHeader = response.headers ? response.headers.get('content-length') : null;
      let totalBytes = 0;
      if (contentLengthHeader) {
        const trimmedLen = contentLengthHeader.trim();
        if (!/^\d+$/.test(trimmedLen)) {
          clearTimeout(timeoutId);
          if (signal) signal.removeEventListener('abort', onExternalAbort);
          throw new Error(`Invalid Content-Length header '${trimmedLen}'`);
        }
        totalBytes = Number(trimmedLen);
        if (!Number.isSafeInteger(totalBytes) || totalBytes > this.MAX_MODEL_BYTES) {
          clearTimeout(timeoutId);
          if (signal) signal.removeEventListener('abort', onExternalAbort);
          throw new Error(`Asset exceeds maximum allowed size (${this.MAX_MODEL_BYTES / (1024 * 1024)} MB).`);
        }
      }
      const totalMbStr = totalBytes > 0 ? (totalBytes / (1024 * 1024)).toFixed(1) : 'unknown';

      BglLogger.log('Network', `HTTP ${response.status} OK. Content-Length: ${totalBytes.toLocaleString()} bytes (${totalMbStr} MB)`);

      try {
        // Fallback for browsers or responses without ReadableStream body
        if (!response.body || typeof ReadableStream === 'undefined') {
          BglLogger.log('Network', 'Streaming not supported on this response body, falling back to arrayBuffer()');
          const buf = await response.arrayBuffer();
          if (buf.byteLength > this.MAX_MODEL_BYTES) {
            throw new Error(`Asset exceeds maximum allowed size (${this.MAX_MODEL_BYTES / (1024 * 1024)} MB).`);
          }
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
          if (!value || value.length === 0) continue;

          if (receivedBytes + value.length > this.MAX_MODEL_BYTES) {
            try { await reader.cancel(); } catch (_) {}
            throw new Error(`Asset download exceeded ${this.MAX_MODEL_BYTES / (1024 * 1024)} MB security limit.`);
          }

          chunks.push(value);
          receivedBytes += value.length;

          const now = performance.now();
          // Report progress every 50ms to maintain 60fps UI
          if (now - lastReportTime > 50 || receivedBytes === totalBytes) {
            lastReportTime = now;
            const elapsedSec = (now - startTime) / 1000;
            const speedMbS = elapsedSec > 0 ? ((receivedBytes / (1024 * 1024)) / elapsedSec).toFixed(1) : '0.0';
            const receivedMb = (receivedBytes / (1024 * 1024)).toFixed(1);
            const shortName = safeUrl.split(/[?#]/)[0].split('/').pop() || 'model.glb';

            if (totalBytes > 0) {
              const percent = Math.min(99, Math.round((receivedBytes / totalBytes) * 100));
              const msg = `Downloading ${shortName}: ${receivedMb} / ${totalMbStr} MB (${percent}%)`;
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
              const msg = `Downloading ${shortName}: ${receivedMb} MB received`;
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

        // Combine chunks into a single ArrayBuffer and free chunk references promptly
        const combined = new Uint8Array(receivedBytes);
        let offset = 0;
        for (let i = 0; i < chunks.length; ++i) {
          combined.set(chunks[i], offset);
          offset += chunks[i].length;
          chunks[i] = null;
        }

        const totalElapsed = ((performance.now() - startTime) / 1000).toFixed(2);
        const avgSpeed = (totalElapsed > 0 ? (receivedBytes / (1024 * 1024)) / parseFloat(totalElapsed) : 0).toFixed(1);
        BglLogger.log('Network', `Finished transfer (${(receivedBytes / (1024 * 1024)).toFixed(2)} MB in ${totalElapsed}s, avg ${avgSpeed} MB/s)`);

        return combined.buffer;
      } finally {
        clearTimeout(timeoutId);
        if (signal) signal.removeEventListener('abort', onExternalAbort);
      }
    }
  };

  // ============================================================================
  // Unified Frame Scheduler (Shared across Core/Renderer/App)
  // ============================================================================
  const BglFrameScheduler = {
    _started: false,
    _rendererStep: null,
    _navStep: null,
    _gamepadStep: null,
    _fpsStep: null,
    _lastErrorMs: 0,

    registerRenderer(fn) { this._rendererStep = fn; this.start(); },
    registerNav(fn) { this._navStep = fn; this.start(); },
    registerGamepad(fn) { this._gamepadStep = fn; this.start(); },
    registerFps(fn) { this._fpsStep = fn; this.start(); },

    _safeCall(fn, now) {
      if (!fn) return;
      try {
        fn(now);
      } catch (err) {
        const curMs = performance.now();
        if (curMs - this._lastErrorMs > 2000) {
          this._lastErrorMs = curMs;
          BglLogger.error('FrameScheduler', 'Uncaught frame callback exception:', err && err.message ? err.message : err);
        }
      }
    },

    start() {
      if (this._started || typeof requestAnimationFrame !== 'function') return;
      this._started = true;
      const tick = (now) => {
        this._safeCall(this._navStep, now);
        this._safeCall(this._gamepadStep, now);
        this._safeCall(this._rendererStep, now);
        this._safeCall(this._fpsStep, now);
        requestAnimationFrame(tick);
      };
      requestAnimationFrame(tick);
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
        if (uint8.length >= 64 * 1024 * 1024) {
          BglLogger.log('Wasm', `Buffer '${name}' exceeds 64 MiB WASM heap ceiling. Model renders directly via WebGPU.`);
          return false;
        }

        const bufPtr = BglState.wasmExports.malloc(uint8.length);
        if (!bufPtr) {
          BglLogger.log('Wasm', `Wasm heap memory limit reached for '${name}' (${(uint8.length / (1024 * 1024)).toFixed(1)} MB). Model renders directly via WebGPU.`);
          return false;
        }

        const nameBytes = new TextEncoder().encode(name + '\0');
        const namePtr = BglState.wasmExports.malloc(nameBytes.length);
        if (!namePtr) {
          BglState.wasmExports.free(bufPtr);
          return false;
        }

        try {
          // Acquire TypedArray view after all malloc calls so wasmMemory.grow cannot detach it.
          const heap = new Uint8Array(BglState.wasmMemory.buffer);
          heap.set(uint8, bufPtr);
          heap.set(nameBytes, namePtr);
          BglState.wasmExports.bgl_load_file_buffer(namePtr, bufPtr, uint8.length);
        } finally {
          BglState.wasmExports.free(namePtr);
          BglState.wasmExports.free(bufPtr);
        }
        BglLogger.log('Wasm', `Successfully ingested model '${name}' (${uint8.length.toLocaleString()} bytes) into engine memory`);
        return true;
      } catch (err) {
        BglLogger.warn('Wasm', `Notice feeding buffer '${name}' to Wasm:`, err.message || err);
        return false;
      }
    }
  };


  const exportsMap = {
    BglLogger,
    BglNotifier,
    BglState,
    BglUI,
    BglModelCache,
    BglLoadingScreen,
    BglNetwork,
    BglWasmBridge,
    BglFrameScheduler
  };
  Object.assign(globalThis, exportsMap);
  if (typeof window !== 'undefined' && window !== globalThis) {
    Object.assign(window, exportsMap);
  }
})();
