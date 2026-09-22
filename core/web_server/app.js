const loginForm = document.querySelector('#login-form');
const loginContainer = document.querySelector('#login-container');
const dashboard = document.querySelector('#dashboard');
const viewVideo = document.querySelector('#view-video');
const viewQuery = document.querySelector('#view-query');
const loginMessage = document.querySelector('#login-message');
const cameraToggle = document.querySelector('#camera-toggle');
const cameraStatus = document.querySelector('#camera-status');
const cameraFpsIndicator = document.querySelector('#camera-fps-indicator');
const stream = document.querySelector('#stream');
const detectionOverlay = document.querySelector('#detection-overlay');
const liveDatetime = document.querySelector('#live-datetime');
const noCameraOverlay = document.querySelector('#camera-no-feed');
const logTerminal = document.querySelector('#log-terminal');
const pluginLogTerminal = document.querySelector('#plugin-log-terminal');
const recordToggleBtn = document.querySelector('#record-toggle-btn');
const restartRecordBtn = document.querySelector('#restart-record-btn');
const captureBtn = document.querySelector('#capture-btn');
const recordStatusIndicator = document.querySelector('#record-status-indicator');
const recordStatusText = document.querySelector('#record-status-text');
const querySearchInput = document.querySelector('#query-search');
const queryDateFilter = document.querySelector('#query-date-filter');
const queryTypeFilter = document.querySelector('#query-type-filter');
const queryMediaList = document.querySelector('#query-session-list');
const pluginList = document.querySelector('#plugin-list');
const pluginSettings = document.querySelector('#plugin-settings');
const scanPluginsBtn = document.querySelector('#scan-plugins-btn');
const disablePluginBtn = document.querySelector('#disable-plugin-btn');
const navTabs = Array.from(document.querySelectorAll('.nav-tab'));
let queryMedia = [];
let expandedGroupIds = new Set();
let selectedPluginName = '';
let toggleRequestActive = false;
let streamActive = false;
let streamReconnectTimerId = null;
let recordingRequestActive = false;
let recordingTimerId = null;
let recordingStartedAt = null;

function formatDuration(totalSeconds) {
  const hours = String(Math.floor(totalSeconds / 3600)).padStart(2, '0');
  const minutes = String(Math.floor((totalSeconds % 3600) / 60)).padStart(2, '0');
  const seconds = String(totalSeconds % 60).padStart(2, '0');
  return `${hours}:${minutes}:${seconds}`;
}

function stopRecordingTimer() {
  if (recordingTimerId) {
    clearInterval(recordingTimerId);
    recordingTimerId = null;
  }
  recordingStartedAt = null;
  const recordDuration = document.querySelector('#record-duration');
  if (recordDuration) {
    recordDuration.hidden = true;
    recordDuration.textContent = '00:00:00';
  }
}

function startRecordingTimer(initialElapsedSeconds = 0) {
  const recordDuration = document.querySelector('#record-duration');
  if (!recordDuration) return;

  recordingStartedAt = Date.now() - (initialElapsedSeconds * 1000);
  recordDuration.hidden = false;

  if (recordingTimerId) {
    clearInterval(recordingTimerId);
  }

  const updateTimer = () => {
    if (!recordingStartedAt) return;
    const elapsedSeconds = Math.floor((Date.now() - recordingStartedAt) / 1000);
    recordDuration.textContent = formatDuration(elapsedSeconds);
  };

  updateTimer();
  recordingTimerId = setInterval(updateTimer, 1000);
}

// Real-Time Green Clock Function
function startLiveClock() {
  function updateClock() {
    const now = new Date();
    const day = String(now.getDate()).padStart(2, '0');
    const month = String(now.getMonth() + 1).padStart(2, '0');
    const year = now.getFullYear();
    const hours = String(now.getHours()).padStart(2, '0');
    const minutes = String(now.getMinutes()).padStart(2, '0');
    const seconds = String(now.getSeconds()).padStart(2, '0');

    liveDatetime.textContent = `${day}/${month}/${year} ${hours}:${minutes}:${seconds}`;
  }
  updateClock();
  setInterval(updateClock, 1000);
}

// HTTP Helper Function
async function request(path, options = {}) {
  const response = await fetch(path, {
    credentials: 'same-origin',
    ...options
  });

  const contentType = response.headers.get('content-type') || '';
  const payload = contentType.includes('application/json') ? await response.json() : await response.text();

  if (!response.ok) {
    throw new Error(typeof payload === 'string' ? payload : JSON.stringify(payload));
  }

  return payload;
}

let systemLogLines = [];
let pluginLogLines = [];

function logCategory(line) {
  const match = String(line).match(/\]\s*\[(?:INFO|WARN|ERROR|DEBUG)\]\s*\[([^\]]+)\]/i);
  return match ? match[1] : '';
}

function isPluginLog(line) {
  const category = logCategory(line);
  if (!category || category === 'PLUGIN_MGR') {
    return false;
  }
  return /PLUGIN/i.test(category);
}

function renderLogTerminal(terminal, lines, emptyText) {
  if (!terminal) return;
  if (!lines.length) {
    terminal.textContent = emptyText;
    terminal.scrollTop = 0;
    return;
  }

  const shouldScrollToBottom = terminal.scrollHeight - terminal.clientHeight <= terminal.scrollTop + 24;
  terminal.textContent = lines.join('\n');
  if (shouldScrollToBottom || terminal.scrollTop === 0) {
    terminal.scrollTop = terminal.scrollHeight;
  }
}

function applySessionLogs(payload) {
  const incoming = Array.isArray(payload) ? payload : (payload && payload.lines) || [];
  const reset = !payload || payload.reset === true || Array.isArray(payload);
  const systemIncoming = incoming.filter((line) => !isPluginLog(line));
  const pluginIncoming = incoming.filter((line) => isPluginLog(line));

  if (reset) {
    systemLogLines = systemIncoming;
    pluginLogLines = pluginIncoming;
  } else {
    systemLogLines.push(...systemIncoming);
    pluginLogLines.push(...pluginIncoming);
  }

  renderLogTerminal(logTerminal, systemLogLines, 'No system logs recorded for this session.');
  renderLogTerminal(pluginLogTerminal, pluginLogLines, 'No plugin logs yet.');
}

// AI Overlay
//
// Detections arrive on their own SSE channel instead of being drawn into the
// JPEG server-side. That keeps inference rate and stream rate independent,
// costs the device no extra encoding, and leaves the boxes crisp even when the
// adaptive ladder has degraded the video quality.
let dashboardSource = null;
let detections = [];
let detectionsReceivedAt = 0;
let overlayFrameId = null;
let mediaSource = null;
let sourceBuffer = null;
let streamReader = null;
let pendingChunks = [];
let streamObjectUrl = '';

const DETECTION_STALE_MS = 2000;
const H264_MIME = 'video/mp4; codecs="avc1.42C028"';

function applyDetections(payload) {
  detections = payload && Array.isArray(payload.detections) ? payload.detections : [];
  detectionsReceivedAt = performance.now();
}

function startOverlayLoop() {
  if (overlayFrameId === null) {
    overlayFrameId = requestAnimationFrame(renderOverlay);
  }
}

function stopOverlayLoop() {
  detections = [];
  if (overlayFrameId !== null) {
    cancelAnimationFrame(overlayFrameId);
    overlayFrameId = null;
  }
  if (detectionOverlay) detectionOverlay.hidden = true;
}

function appendPendingChunks() {
  if (!sourceBuffer || sourceBuffer.updating || pendingChunks.length === 0) return;
  const chunk = pendingChunks.shift();
  try {
    sourceBuffer.appendBuffer(chunk);
  } catch (error) {
    reconnectStream();
  }
}

function stopH264Stream() {
  if (streamReader) {
    streamReader.cancel().catch(() => {});
    streamReader = null;
  }
  pendingChunks = [];
  sourceBuffer = null;
  if (mediaSource) {
    if (mediaSource.readyState === 'open') {
      try { mediaSource.endOfStream(); } catch (error) {}
    }
    mediaSource = null;
  }
  if (streamObjectUrl) {
    URL.revokeObjectURL(streamObjectUrl);
    streamObjectUrl = '';
  }
  if (stream) {
    stream.removeAttribute('src');
    stream.load();
  }
}

async function pumpH264Stream(response) {
  if (!response.ok || !response.body) {
    reconnectStream();
    return;
  }

  streamReader = response.body.getReader();
  while (streamActive && streamReader) {
    const { value, done } = await streamReader.read();
    if (done) break;
    if (value && value.byteLength > 0) {
      pendingChunks.push(value.buffer.slice(value.byteOffset, value.byteOffset + value.byteLength));
      appendPendingChunks();
    }
  }

  if (streamActive) {
    reconnectStream();
  }
}

function startH264Stream() {
  if (!stream || !window.MediaSource || !MediaSource.isTypeSupported(H264_MIME)) {
    console.error('This browser cannot play the live H.264 stream');
    return;
  }

  stopH264Stream();
  mediaSource = new MediaSource();
  streamObjectUrl = URL.createObjectURL(mediaSource);
  stream.src = streamObjectUrl;
  stream.hidden = false;

  mediaSource.addEventListener('sourceopen', async () => {
    try {
      sourceBuffer = mediaSource.addSourceBuffer(H264_MIME);
      sourceBuffer.mode = 'sequence';
      sourceBuffer.addEventListener('updateend', appendPendingChunks);
      const response = await fetch('/api/camera/stream?generation=' + Date.now(), {
        credentials: 'same-origin'
      });
      await pumpH264Stream(response);
    } catch (error) {
      if (streamActive) reconnectStream();
    }
  }, { once: true });
}

// The <img> is object-fit: contain, so the picture is letterboxed inside the
// element. Detections are normalised against the source frame, which maps them
// onto that inner rectangle -- using the element box instead would push boxes
// into the black bars.
function videoContentRect() {
  const elementWidth = stream.clientWidth;
  const elementHeight = stream.clientHeight;
  const sourceWidth = stream.videoWidth || stream.naturalWidth;
  const sourceHeight = stream.videoHeight || stream.naturalHeight;
  if (!elementWidth || !elementHeight || !sourceWidth || !sourceHeight) return null;

  const scale = Math.min(elementWidth / sourceWidth, elementHeight / sourceHeight);
  const width = sourceWidth * scale;
  const height = sourceHeight * scale;
  return { left: (elementWidth - width) / 2, top: (elementHeight - height) / 2, width, height };
}

function renderOverlay() {
  overlayFrameId = requestAnimationFrame(renderOverlay);

  if (!detectionOverlay || !stream || stream.hidden) {
    if (detectionOverlay) detectionOverlay.hidden = true;
    return;
  }

  const rect = videoContentRect();
  if (!rect) {
    detectionOverlay.hidden = true;
    return;
  }
  detectionOverlay.hidden = false;

  const ratio = window.devicePixelRatio || 1;
  const pixelWidth = Math.round(stream.clientWidth * ratio);
  const pixelHeight = Math.round(stream.clientHeight * ratio);
  if (detectionOverlay.width !== pixelWidth || detectionOverlay.height !== pixelHeight) {
    detectionOverlay.width = pixelWidth;
    detectionOverlay.height = pixelHeight;
  }

  const context = detectionOverlay.getContext('2d');
  if (!context) return;

  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  context.clearRect(0, 0, stream.clientWidth, stream.clientHeight);

  // Boxes always trail their frame by the inference time. If results stop
  // arriving altogether, clear them rather than leaving stale boxes frozen
  // over a scene that has moved on.
  if (performance.now() - detectionsReceivedAt > DETECTION_STALE_MS) return;

  context.lineWidth = 2;
  context.font = '13px monospace';
  context.textBaseline = 'top';

  for (const detection of detections) {
    const x = rect.left + Number(detection.x || 0) * rect.width;
    const y = rect.top + Number(detection.y || 0) * rect.height;
    const width = Number(detection.width || 0) * rect.width;
    const height = Number(detection.height || 0) * rect.height;
    if (!(width > 0) || !(height > 0)) continue;

    context.strokeStyle = '#00ff00';
    context.strokeRect(x, y, width, height);

    const confidence = Number(detection.confidence || 0);
    const label = `${detection.label || 'object'} ${(confidence * 100).toFixed(0)}%`;
    const labelTop = Math.max(rect.top, y - 17);
    context.fillStyle = 'rgba(0, 0, 0, 0.65)';
    context.fillRect(x, labelTop, context.measureText(label).width + 8, 17);
    context.fillStyle = '#00ff00';
    context.fillText(label, x + 4, labelTop + 1);
  }
}

function applyRecording(recording) {
  if (!recordStatusIndicator || !recordStatusText || !recordToggleBtn) return;
  const active = Boolean(recording && recording.recording);
  const elapsedSeconds = Number((recording && recording.elapsed_seconds) || 0);

  const dot = recordStatusIndicator.querySelector('span');
  if (dot) {
    dot.style.background = active ? '#ff3b30' : '#666';
  }
  recordStatusText.textContent = active ? 'Recording' : 'Idle';
  recordToggleBtn.textContent = active ? 'Stop Record' : 'Start Record';
  recordToggleBtn.style.background = active ? '#d32f2f' : '#1e88e5';

  if (active) {
    const timerElapsedSeconds = Math.max(0, elapsedSeconds);
    const timerDriftSeconds = recordingStartedAt ? Math.abs(Math.floor((Date.now() - recordingStartedAt) / 1000) - timerElapsedSeconds) : Number.MAX_SAFE_INTEGER;
    if (!recordingTimerId || !recordingStartedAt || timerDriftSeconds > 1) {
      startRecordingTimer(timerElapsedSeconds);
    }
  } else {
    stopRecordingTimer();
  }

  const canRecord = canRecordWithCamera();
  recordToggleBtn.disabled = !canRecord;
  if (restartRecordBtn) restartRecordBtn.disabled = !canRecord;
  if (captureBtn) captureBtn.disabled = !canRecord;
}

function applyStatus(status) {
  const enabled = Boolean(status.enabled);
  const connected = Boolean(status.connected);
  const running = Boolean(status.running);
  const error = Boolean(status.error);
  const actualFps = Number(status.fps || 0);
  const streamFps = Number(status.stream_fps || 0);
  const streamClients = Number(status.stream_clients || 0);
  const streamWidth = Number(status.stream_width || 0);
  const streamHeight = Number(status.stream_height || 0);
  const streamQuality = Number(status.stream_quality || 0);

  cameraToggle.checked = enabled;

  if (cameraFpsIndicator) {
    if (running && Number.isFinite(actualFps) && actualFps > 0) {
      // Capture rate and delivered rate diverge whenever the stream adapts, so
      // showing only the former hides what the browser is really receiving.
      const parts = [`CAM ${actualFps.toFixed(1)}`];
      if (streamClients > 0 && Number.isFinite(streamFps)) {
        const detail = streamWidth > 0 && streamHeight > 0
          ? ` @ ${streamWidth}x${streamHeight} q${streamQuality}`
          : '';
        parts.push(`STREAM ${streamFps.toFixed(1)}${detail}`);
      }
      cameraFpsIndicator.textContent = `FPS ${parts.join(' | ')}`;
      cameraFpsIndicator.hidden = false;
    } else {
      cameraFpsIndicator.textContent = 'FPS 0.0';
      cameraFpsIndicator.hidden = !enabled;
    }
  }

  if (running) {
    cameraStatus.textContent = 'Running';
    cameraStatus.classList.add('active');
  } else if (!enabled) {
    cameraStatus.textContent = 'Stopped';
    cameraStatus.classList.remove('active');
  } else if (!connected || error) {
    cameraStatus.textContent = 'Unplugged';
    cameraStatus.classList.remove('active');
  } else {
    cameraStatus.textContent = 'Waiting';
    cameraStatus.classList.remove('active');
  }

  if (noCameraOverlay) {
    const noCameraMessage = noCameraOverlay.querySelector('.camera-no-feed-text');
    if (!enabled) {
      noCameraOverlay.hidden = false;
      if (noCameraMessage) noCameraMessage.textContent = 'Open Your Camera';
    } else if (enabled && (!connected || error)) {
      noCameraOverlay.hidden = false;
      if (noCameraMessage) noCameraMessage.textContent = 'Camera Unplugged';
    } else {
      noCameraOverlay.hidden = true;
      if (noCameraMessage) noCameraMessage.textContent = '';
    }
  }

  stream.hidden = !(enabled && connected && running);
  if (enabled && connected && running && !streamActive) {
    streamActive = true;
    startH264Stream();
    startOverlayLoop();
  } else if (!enabled || !connected || !running) {
    streamActive = false;
    stopH264Stream();
    stopOverlayLoop();
  }

  const canRecord = canRecordWithCamera();
  if (recordToggleBtn) recordToggleBtn.disabled = !canRecord && !(recordStatusText && recordStatusText.textContent === 'Recording');
  if (restartRecordBtn) restartRecordBtn.disabled = !canRecord;
  if (captureBtn) captureBtn.disabled = !canRecord;
}

async function refreshStatus() {
  const status = await request('/api/camera/status');
  applyStatus(status);
  try {
    applyRecording(await request('/api/record/status'));
  } catch (error) {
    if (recordStatusText) recordStatusText.textContent = 'Status unavailable';
    stopRecordingTimer();
  }
}

function canRecordWithCamera() {
  if (!cameraToggle || !cameraStatus) return false;
  if (!cameraToggle.checked) return false;

  const state = cameraStatus.textContent.trim();
  const recordingState = recordStatusText ? recordStatusText.textContent.trim() : '';
  return state === 'Running' || recordingState === 'Recording';
}

function reconnectStream() {
  if (!streamActive || !stream || streamReconnectTimerId) return;

  streamReconnectTimerId = setTimeout(() => {
    streamReconnectTimerId = null;
    if (!streamActive) return;
    startH264Stream();
  }, 500);
}

function startDashboardEvents() {
  if (dashboardSource) return;

  dashboardSource = new EventSource('/api/events');
  dashboardSource.addEventListener('status', (event) => {
    try { applyStatus(JSON.parse(event.data)); } catch (error) {}
  });
  dashboardSource.addEventListener('recording', (event) => {
    try { applyRecording(JSON.parse(event.data)); } catch (error) {}
  });
  dashboardSource.addEventListener('logs', (event) => {
    try { applySessionLogs(JSON.parse(event.data)); } catch (error) {}
  });
  dashboardSource.addEventListener('plugins', (event) => {
    try { renderPluginList(JSON.parse(event.data)); } catch (error) {}
  });
  dashboardSource.addEventListener('detections', (event) => {
    try { applyDetections(JSON.parse(event.data)); } catch (error) { detections = []; }
  });
  dashboardSource.onerror = () => {
    detections = [];
  };
  startOverlayLoop();
}

function showView(viewName) {
  const isVideo = viewName === 'video';
  const isQuery = viewName === 'query';

  viewVideo.hidden = !isVideo;
  if (viewQuery) viewQuery.hidden = !isQuery;

  navTabs.forEach(tab => {
    const label = tab.textContent.trim();
    const active = label === (isVideo ? 'Main' : isQuery ? 'Query data' : 'Main');
    tab.classList.toggle('active', active);
  });
  if (isQuery) {
    fetchQueryMedia().catch(error => {
      console.error('Unable to load query data', error);
    });
  }

}

function resolveOptionValue(option) {
  if (typeof option === 'string' || typeof option === 'number') {
    return String(option);
  }

  if (option && typeof option === 'object') {
    if (typeof option.label === 'string' && option.label.trim()) {
      return option.label;
    }
    if (Number.isFinite(option.width) && Number.isFinite(option.height)) {
      return `${option.width}x${option.height}`;
    }
  }

  return '';
}

function populateSelect(selectElement, options, value) {
  if (!selectElement) return;

  selectElement.innerHTML = '';
  const normalizedOptions = Array.isArray(options) ? options : [];

  normalizedOptions.forEach(option => {
    const optionValue = resolveOptionValue(option);
    if (!optionValue) return;

    const optionElement = document.createElement('option');
    optionElement.value = optionValue;
    optionElement.textContent = optionValue;

    if (value !== undefined && optionValue === String(value)) {
      optionElement.selected = true;
    }

    selectElement.appendChild(optionElement);
  });

  const selectedValue = value === undefined ? '' : String(value);
  if (selectedValue && !Array.from(selectElement.options).some(option => option.value === selectedValue)) {
    const fallback = document.createElement('option');
    fallback.value = selectedValue;
    fallback.textContent = selectedValue;
    fallback.selected = true;
    selectElement.appendChild(fallback);
  }
}

function getFileExtensionName(fileName) {
  if (!fileName) return 'file';
  const lower = fileName.toLowerCase();
  if (lower.endsWith('.mp4') || lower.endsWith('.avi') || lower.endsWith('.mov') || lower.endsWith('.mkv')) return '🎬';
  if (lower.endsWith('.jpg') || lower.endsWith('.jpeg') || lower.endsWith('.png') || lower.endsWith('.webp') || lower.endsWith('.bmp')) return '🖼️';
  return '📄';
}

const pluginStatusStyles = {
  active: { label: 'Active', color: '#00ff00', className: 'active' },
  loading: { label: 'Loading', color: '#4ea1ff', className: 'loading' },
  unloading: { label: 'Unloading', color: '#ff8a00', className: 'unloading' },
  loaded: { label: 'Disabled', color: '#ffb300', className: 'disabled' },
  unloaded: { label: 'Unloaded', color: '#888', className: 'unloaded' }
};

function pluginStatusStyle(state) {
  return pluginStatusStyles[state] || pluginStatusStyles.unloaded;
}

function markPluginState(pluginName, state) {
  if (!pluginList) return;
  const style = pluginStatusStyle(state);
  pluginList.querySelectorAll('.plugin-card').forEach((card) => {
    const radio = card.querySelector('input[type="radio"]');
    if (!radio || radio.value !== pluginName) return;
    const status = card.querySelector('.plugin-status');
    if (!status) return;
    status.className = `plugin-status ${style.className}`;
    status.innerHTML = `<span style="display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px;vertical-align:middle;background:${style.color}"></span>${style.label}`;
  });
}

function renderPluginList(plugins) {
  if (!pluginList) return;

  pluginList.innerHTML = '';
  const normalizedPlugins = Array.isArray(plugins) ? plugins : [];

  if (!normalizedPlugins.length) {
    pluginList.innerHTML = '<div class="query-empty-state">No plugins detected.</div>';
    return;
  }

  const activePluginName = normalizedPlugins.find(plugin => Boolean(plugin.enabled))?.name || selectedPluginName || '';
  if (activePluginName) {
    selectedPluginName = activePluginName;
  }

  normalizedPlugins.forEach((plugin) => {
    const card = document.createElement('div');
    card.className = 'plugin-card';
    const isSelected = Boolean(plugin.enabled) || (plugin.name === selectedPluginName && !normalizedPlugins.some(item => Boolean(item.enabled)));
    card.classList.toggle('selected', isSelected);
    card.tabIndex = 0;
    card.setAttribute('role', 'button');

    const radio = document.createElement('input');
    radio.type = 'radio';
    radio.name = 'pluginSelection';
    radio.value = plugin.name || '';
    radio.checked = isSelected;

    const text = document.createElement('span');
    text.textContent = plugin.name || 'Unknown plugin';
    text.className = 'plugin-card-name';

    const icon = document.createElement('span');
    icon.textContent = plugin.name === 'face_recognition' ? '👤' : plugin.name.includes('depth') ? '📐' : '📦';
    text.prepend(icon);

    const top = document.createElement('div');
    top.className = 'plugin-card-top';

    const settings = plugin.settings || {};
    const status = document.createElement('span');
    const pluginState = String(plugin.state || (plugin.enabled ? 'active' : plugin.loaded ? 'loaded' : 'unloaded'));
    const style = pluginStatusStyle(pluginState);
    status.innerHTML = `<span style="display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px;vertical-align:middle;background:${style.color}"></span>${style.label}`;
    status.className = `plugin-status ${style.className}`;

    top.appendChild(radio);
    top.appendChild(text);
    top.appendChild(status);
    card.appendChild(top);

    const selectPlugin = async () => {
      if (plugin.enabled && plugin.name === selectedPluginName) {
        return;
      }
      const outgoingName = selectedPluginName;
      try {
        selectedPluginName = plugin.name;
        if (outgoingName && outgoingName !== plugin.name) {
          markPluginState(outgoingName, 'unloading');
        }
        markPluginState(plugin.name, 'loading');
        await request('/api/plugins', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ name: plugin.name, enabled: true })
        });
        await refreshPluginList();
      } catch (error) {
        console.error('Unable to select plugin', error);
        await refreshPluginList();
      }
    };
    radio.addEventListener('change', () => {
      if (radio.checked) selectPlugin();
    });
    card.addEventListener('click', (event) => {
      if (event.target !== radio) selectPlugin();
    });
    card.addEventListener('keydown', (event) => {
      if (event.key === 'Enter' || event.key === ' ') {
        event.preventDefault();
        selectPlugin();
      }
    });

    pluginList.appendChild(card);
  });

  const activePlugin = normalizedPlugins.find(plugin => Boolean(plugin.enabled)) ||
    normalizedPlugins.find(plugin => plugin.name === selectedPluginName) ||
    null;
  if (disablePluginBtn) {
    disablePluginBtn.disabled = !activePlugin || !activePlugin.enabled;
  }
  if (activePlugin) {
    selectedPluginName = activePlugin.name;
    renderPluginSettings(activePlugin);
  } else {
    selectedPluginName = '';
    if (pluginSettings) {
      pluginSettings.innerHTML = '<div class="plugin-empty">Select a plugin to view its settings.</div>';
    }
  }
}

function renderPluginSettings(plugin) {
  if (!pluginSettings) return;
  const settings = plugin.settings || {};
  pluginSettings.innerHTML = '';

  const title = document.createElement('div');
  title.className = 'plugin-settings-title';
  title.textContent = `Configuration: ${plugin.name}`;
  const subtitle = document.createElement('div');
  subtitle.className = 'plugin-settings-subtitle';
  subtitle.textContent = plugin.state === 'loading' ? 'Loading plugin library...'
    : plugin.state === 'unloading' ? 'Unloading plugin library...'
    : plugin.loaded ? 'Runtime configuration'
    : 'Select this plugin to load it at runtime';
  pluginSettings.appendChild(title);
  pluginSettings.appendChild(subtitle);

  const threshold = document.createElement('div');
  threshold.className = 'plugin-setting';
  const thresholdLabel = document.createElement('label');
  thresholdLabel.textContent = 'Confidence Threshold';
  const rangeRow = document.createElement('div');
  rangeRow.className = 'plugin-range-row';
  const range = document.createElement('input');
  range.type = 'range';
  range.min = '0';
  range.max = '1';
  range.step = '0.01';
  range.value = settings.confidence_threshold ?? '0.75';
  const rangeValue = document.createElement('span');
  rangeValue.className = 'plugin-range-value';
  rangeValue.textContent = Number(range.value).toFixed(2);
  range.addEventListener('input', () => { rangeValue.textContent = Number(range.value).toFixed(2); });
  rangeRow.appendChild(range);
  rangeRow.appendChild(rangeValue);
  threshold.appendChild(thresholdLabel);
  threshold.appendChild(rangeRow);
  pluginSettings.appendChild(threshold);

  const labels = document.createElement('div');
  labels.className = 'plugin-setting plugin-toggle';
  const labelsText = document.createElement('label');
  labelsText.textContent = 'Show Labels on Stream';
  const labelsToggle = document.createElement('input');
  labelsToggle.type = 'checkbox';
  labelsToggle.checked = settings.show_labels ?? true;
  labels.appendChild(labelsText);
  labels.appendChild(labelsToggle);
  pluginSettings.appendChild(labels);

  const save = document.createElement('button');
  save.type = 'button';
  save.className = 'btn';
  save.textContent = 'Save Settings';
  save.disabled = !plugin.loaded;
  save.addEventListener('click', async () => {
    try {
      await request('/api/plugins/settings', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name: plugin.name, settings: {
          ...settings,
          confidence_threshold: Number(range.value),
          show_labels: labelsToggle.checked
        }})
      });
      await refreshPluginList();
    } catch (error) {
      console.error('Unable to save plugin settings', error);
    }
  });
  pluginSettings.appendChild(save);
}

async function refreshPluginList() {
  try {
    const plugins = await request('/api/plugins');
    renderPluginList(plugins);
  } catch (error) {
    if (pluginList) {
      pluginList.innerHTML = '<div class="query-empty-state">Unable to load plugin list.</div>';
    }
  }
}

if (scanPluginsBtn) {
  scanPluginsBtn.addEventListener('click', async () => {
    scanPluginsBtn.disabled = true;
    try {
      await request('/api/plugins/scan', { method: 'POST' });
      await refreshPluginList();
    } finally {
      scanPluginsBtn.disabled = false;
    }
  });
}

if (disablePluginBtn) {
  disablePluginBtn.addEventListener('click', async () => {
    if (!selectedPluginName) return;
    const pluginName = selectedPluginName;
    if (!window.confirm(`Disable and unload ${pluginName}?`)) return;
    disablePluginBtn.disabled = true;
    try {
      const response = await request('/api/plugins/unload', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name: pluginName })
      });
      if (response && response.success) {
        selectedPluginName = '';
        await refreshPluginList();
      }
    } catch (error) {
      console.error('Unable to disable plugin', error);
      disablePluginBtn.disabled = false;
    }
  });
}

function renderQueryTree(node, depth = 0) {
  const wrapper = document.createElement('div');
  wrapper.style.marginLeft = depth ? `${depth * 12}px` : '0';
  wrapper.className = 'query-node ' + (node.type === 'folder' ? 'folder' : 'file');

  const main = document.createElement('div');
  main.className = 'query-node-main';
  const icon = document.createElement('span');
  icon.textContent = node.type === 'folder' ? '📁' : getFileExtensionName(node.name);
  main.appendChild(icon);

  if (node.type === 'folder') {
    const text = document.createElement('span');
    text.textContent = node.name;
    main.appendChild(text);
  } else {
    const link = document.createElement('a');
    link.className = 'query-file-link';
    link.href = '/api/query/media?path=' + encodeURIComponent(node.path);
    link.target = '_blank';
    link.rel = 'noopener noreferrer';
    link.textContent = node.name;
    main.appendChild(link);
  }

  wrapper.appendChild(main);

  if (node.type === 'folder' && Array.isArray(node.children) && node.children.length) {
    const children = document.createElement('div');
    children.style.display = 'grid';
    children.style.gap = '2px';
    children.style.marginTop = '4px';
    node.children.forEach(child => {
      children.appendChild(renderQueryTree(child, depth + 1));
    });
    wrapper.appendChild(children);
  }

  return wrapper;
}

function normalizeDateValue(value) {
  if (!value) return '';

  const text = String(value).trim();
  if (!text) return '';

  if (/^\d{4}-\d{2}-\d{2}$/.test(text)) {
    return text;
  }

  const match = text.match(/(\d{1,2})[-/](\d{1,2})[-/](\d{4})/);
  if (match) {
    const [, day, month, year] = match;
    return `${year}-${month.padStart(2, '0')}-${day.padStart(2, '0')}`;
  }

  return '';
}

function renderMediaCard(mediaItem) {
  const card = document.createElement('div');
  card.className = 'query-session-card';
  if (expandedGroupIds.has(mediaItem.id)) {
    card.classList.add('expanded');
  }

  const row = document.createElement('button');
  row.type = 'button';
  row.className = 'query-session-row';
  row.dataset.groupId = mediaItem.id;

  const meta = document.createElement('div');
  meta.className = 'query-session-meta';

  const date = document.createElement('span');
  date.className = 'query-session-date';
  date.textContent = mediaItem.date || mediaItem.name || mediaItem.id;
  meta.appendChild(date);

  const itemName = document.createElement('span');
  itemName.className = 'query-session-id';
  itemName.textContent = mediaItem.name || mediaItem.id;
  meta.appendChild(itemName);

  const icons = document.createElement('div');
  icons.className = 'query-session-icons';
  const counts = mediaItem.counts || {};
  if (Number(counts.video) > 0) {
    const videoIcon = document.createElement('span');
    videoIcon.className = 'query-media-icon';
    videoIcon.textContent = '🎬';
    videoIcon.title = `${counts.video} video`;
    icons.appendChild(videoIcon);
  }
  if (Number(counts.capture) > 0) {
    const captureIcon = document.createElement('span');
    captureIcon.className = 'query-media-icon';
    captureIcon.textContent = '🖼️';
    captureIcon.title = `${counts.capture} captured photos`;
    icons.appendChild(captureIcon);
  }
  meta.appendChild(icons);

  row.appendChild(meta);

  const toggle = document.createElement('span');
  toggle.className = 'query-session-toggle';
  toggle.textContent = expandedGroupIds.has(mediaItem.id) ? '▾' : '▸';
  row.appendChild(toggle);

  const body = document.createElement('div');
  body.className = 'query-session-body';

  const tree = document.createElement('div');
  tree.className = 'query-tree';
  const root = mediaItem.tree || { name: mediaItem.id, type: 'folder', children: [] };
  tree.appendChild(renderQueryTree(root));
  body.appendChild(tree);

  card.appendChild(row);
  card.appendChild(body);
  return card;
}

function applyQueryFilters() {
  if (!queryMediaList) return;

  const searchValue = (querySearchInput ? querySearchInput.value : '').trim().toLowerCase();
  const dateFilter = queryDateFilter ? queryDateFilter.value : '';
  const typeFilter = queryTypeFilter ? queryTypeFilter.value : 'all';

  const visibleMedia = queryMedia.filter(item => {
    const matchesSearch = !searchValue || [item.id, item.name, item.date, ...(item.files || []).map(file => file.name || '')].join(' ').toLowerCase().includes(searchValue);
    const normalizedFilter = normalizeDateValue(dateFilter);
    const itemDate = normalizeDateValue(item.date || '');
    const matchesDate = !normalizedFilter || itemDate === normalizedFilter;

    const itemType = String(item.type || '').toLowerCase();
    const fileKinds = (item.files || []).map(file => String(file.kind || '').toLowerCase());
    const hasVideo = itemType === 'recording' || itemType === 'video' || fileKinds.includes('video');
    const hasCapture = itemType === 'capture' || itemType === 'captured_photo' || fileKinds.includes('captured_photo');
    const matchesType = typeFilter === 'all' ||
      (typeFilter === 'video' && hasVideo) ||
      (typeFilter === 'capture' && hasCapture);

    return matchesSearch && matchesDate && matchesType;
  });

  queryMediaList.innerHTML = '';
  if (!visibleMedia.length) {
    const empty = document.createElement('div');
    empty.className = 'query-empty-state';
    empty.textContent = 'No matching media found.';
    queryMediaList.appendChild(empty);
    return;
  }

  visibleMedia.forEach(item => {
    queryMediaList.appendChild(renderMediaCard(item));
  });
}

async function fetchQueryMedia() {
  try {
    const media = await request('/api/query/media');
    queryMedia = Array.isArray(media) ? media : [];
    applyQueryFilters();
  } catch (error) {
    if (queryMediaList) {
      queryMediaList.innerHTML = '<div class="query-empty-state">Unable to load media library.</div>';
    }
  }
}

// Initialize Dashboard & Timers
startLiveClock();

navTabs.forEach(tab => {
  tab.addEventListener('click', () => {
    const label = tab.textContent.trim();
    if (label === 'Main') showView('video');
    if (label === 'Query data') showView('query');
  });
});

if (querySearchInput) querySearchInput.addEventListener('input', applyQueryFilters);
if (queryDateFilter) {
  queryDateFilter.addEventListener('input', applyQueryFilters);
  queryDateFilter.addEventListener('change', applyQueryFilters);
  queryDateFilter.addEventListener('click', () => {
    if (typeof queryDateFilter.showPicker === 'function') {
      queryDateFilter.showPicker();
    }
  });
  queryDateFilter.addEventListener('focus', () => {
    if (typeof queryDateFilter.showPicker === 'function') {
      queryDateFilter.showPicker();
    }
  });
}
if (queryTypeFilter) queryTypeFilter.addEventListener('change', applyQueryFilters);

stream.addEventListener('error', reconnectStream);

if (queryMediaList) {
  queryMediaList.addEventListener('click', event => {
    const fileLink = event.target.closest('.query-file-link');
    if (fileLink) {
      return;
    }

    const row = event.target.closest('.query-session-row');
    if (!row) {
      return;
    }

    const groupId = row.dataset.groupId;
    if (!groupId) {
      return;
    }

    if (expandedGroupIds.has(groupId)) {
      expandedGroupIds.delete(groupId);
    } else {
      expandedGroupIds.add(groupId);
    }

    applyQueryFilters();
  });
}

if (window.location.pathname === '/dashboard') {
  loginContainer.hidden = true;
  dashboard.hidden = false;
  showView('video');
  startDashboardEvents();
  refreshStatus().catch(error => { cameraStatus.textContent = error.message; });
  refreshPluginList().catch(() => {});
}

// Login Event Listener
loginForm.addEventListener('submit', async (event) => {
  event.preventDefault();
  loginMessage.textContent = '';

  const formData = Object.fromEntries(new FormData(loginForm));

  try {
    await request('/api/login', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(formData),
      credentials: 'same-origin'
    });
    window.location.replace('/dashboard');
  } catch (error) {
    loginMessage.textContent = 'Login failed';
  }
});

// Camera Toggle Event Listener
cameraToggle.addEventListener('change', async () => {
  if (toggleRequestActive) return;
  toggleRequestActive = true;
  cameraToggle.disabled = true;
  const endpoint = cameraToggle.checked ? '/api/camera/start' : '/api/camera/stop';
  try {
    await request(endpoint, { method: 'POST' });
    await refreshStatus();
  } catch (error) {
    cameraStatus.textContent = error.message;
    await refreshStatus();
  } finally {
    toggleRequestActive = false;
    cameraToggle.disabled = false;
  }
});

recordToggleBtn.addEventListener('click', async () => {
  if (!canRecordWithCamera()) {
    alert('camera must be power on (or) open your camera by check in checkbox');
    return;
  }

  if (recordingRequestActive) return;
  recordingRequestActive = true;
  recordToggleBtn.disabled = true;

  try {
    const currentRecording = recordStatusText.textContent === 'Recording';
    const endpoint = currentRecording ? '/api/record/stop' : '/api/record/start';
    await request(endpoint, { method: 'POST' });
    await refreshStatus();
  } catch (error) {
    recordStatusText.textContent = error.message;
  } finally {
    recordingRequestActive = false;
    recordToggleBtn.disabled = false;
  }
});

restartRecordBtn.addEventListener('click', async () => {
  if (!canRecordWithCamera()) {
    alert('camera must be power on (or) open your camera by check in checkbox');
    return;
  }

  if (recordingRequestActive) return;
  recordingRequestActive = true;
  restartRecordBtn.disabled = true;

  try {
    await request('/api/record/restart', { method: 'POST' });
    await refreshStatus();
  } catch (error) {
    recordStatusText.textContent = error.message;
  } finally {
    recordingRequestActive = false;
    restartRecordBtn.disabled = false;
  }
});

captureBtn.addEventListener('click', async () => {
  if (!canRecordWithCamera()) {
    alert('camera must be power on (or) open your camera by check in checkbox');
    return;
  }

  if (recordingRequestActive) return;
  recordingRequestActive = true;
  captureBtn.disabled = true;

  try {
    const result = await request('/api/record/capture', { method: 'POST' });
    if (result.captured) {
      const priorText = recordStatusText.textContent;
      recordStatusText.textContent = 'Captured';
      setTimeout(() => {
        recordStatusText.textContent = priorText;
      }, 1200);
    }
  } catch (error) {
    recordStatusText.textContent = error.message;
  } finally {
    recordingRequestActive = false;
    captureBtn.disabled = false;
  }
});