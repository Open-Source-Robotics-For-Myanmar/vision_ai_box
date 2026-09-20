const loginForm = document.querySelector('#login-form');
const loginContainer = document.querySelector('#login-container');
const dashboard = document.querySelector('#dashboard');
const viewVideo = document.querySelector('#view-video');
const viewQuery = document.querySelector('#view-query');
const viewOptions = document.querySelector('#view-options');
const loginMessage = document.querySelector('#login-message');
const cameraToggle = document.querySelector('#camera-toggle');
const cameraStatus = document.querySelector('#camera-status');
const stream = document.querySelector('#stream');
const liveDatetime = document.querySelector('#live-datetime');
const noCameraOverlay = document.querySelector('#camera-no-feed');
const logTerminal = document.querySelector('#log-terminal');
const recordToggleBtn = document.querySelector('#record-toggle-btn');
const restartRecordBtn = document.querySelector('#restart-record-btn');
const captureBtn = document.querySelector('#capture-btn');
const recordStatusIndicator = document.querySelector('#record-status-indicator');
const recordStatusText = document.querySelector('#record-status-text');
const cameraSettingsForm = document.querySelector('#camera-settings-form');
const cancelSettingsBtn = document.querySelector('#cancel-settings-btn');
const usbDeviceField = document.querySelector('#usb-device-field');
const viewSystem = document.querySelector('#view-system');
const viewPlugin = document.querySelector('#view-plugin');
const querySearchInput = document.querySelector('#query-search');
const queryDateFilter = document.querySelector('#query-date-filter');
const queryTypeFilter = document.querySelector('#query-type-filter');
const queryMediaList = document.querySelector('#query-session-list');
const pluginList = document.querySelector('#plugin-list');
const pluginSettings = document.querySelector('#plugin-settings');
const scanPluginsBtn = document.querySelector('#scan-plugins-btn');
const navTabs = Array.from(document.querySelectorAll('.nav-tab'));

let queryMedia = [];
let expandedGroupIds = new Set();
let selectedPluginName = '';

let toggleRequestActive = false;
let streamActive = false;
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

// Fetch Session Logs and Auto-Scroll Terminal
async function fetchSessionLogs() {
  try {
    const logs = await request('/api/logs');
    if (Array.isArray(logs) && logs.length > 0) {
      const shouldScrollToBottom = logTerminal.scrollHeight - logTerminal.clientHeight <= logTerminal.scrollTop + 24;
      logTerminal.textContent = logs.join('\n');
      if (shouldScrollToBottom || logTerminal.scrollTop === 0) {
        logTerminal.scrollTop = logTerminal.scrollHeight;
      }
    } else {
      logTerminal.textContent = 'No logs recorded for this session.';
      logTerminal.scrollTop = 0;
    }
  } catch (err) {
    logTerminal.textContent = 'Failed to fetch session logs...';
    logTerminal.scrollTop = 0;
  }
}

// Refresh Camera Feed & Toggle Status
async function refreshStatus() {
  const status = await request('/api/camera/status');
  const enabled = Boolean(status.enabled);
  const connected = Boolean(status.connected);
  const running = Boolean(status.running);
  const error = Boolean(status.error);

  cameraToggle.checked = enabled;

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
    stream.src = '/api/camera/stream?generation=' + Date.now();
    streamActive = true;
  } else if (!enabled || !connected || !running) {
    stream.removeAttribute('src');
    streamActive = false;
  }

  try {
    const recording = await request('/api/record/status');
    const active = Boolean(recording.recording);
    const elapsedSeconds = Number(recording.elapsed_seconds || 0);

    const dot = recordStatusIndicator.querySelector('span');
    dot.style.background = active ? '#ff3b30' : '#666';
    recordStatusText.textContent = active ? 'Recording' : 'Idle';
    recordToggleBtn.textContent = active ? 'Stop Record' : 'Start Record';
    recordToggleBtn.style.background = active ? '#d32f2f' : '#1e88e5';

    const recordDuration = document.querySelector('#record-duration');
    if (active) {
      const timerElapsedSeconds = Math.max(0, Number(recording.elapsed_seconds || 0));
      const timerDriftSeconds = recordingStartedAt ? Math.abs(Math.floor((Date.now() - recordingStartedAt) / 1000) - timerElapsedSeconds) : Number.MAX_SAFE_INTEGER;

      if (!recordingTimerId || !recordingStartedAt || timerDriftSeconds > 1) {
        startRecordingTimer(timerElapsedSeconds);
      }
    } else {
      stopRecordingTimer();
    }

    const canRecord = canRecordWithCamera();
    recordToggleBtn.disabled = !canRecord;
    restartRecordBtn.disabled = !canRecord;
    captureBtn.disabled = !canRecord;
  } catch (error) {
    recordStatusText.textContent = 'Status unavailable';
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

function showView(viewName) {
  const isVideo = viewName === 'video';
  const isQuery = viewName === 'query';
  const isOptions = viewName === 'options';
  const isSystem = viewName === 'system';
  const isPlugin = viewName === 'plugin';

  viewVideo.hidden = !isVideo;
  if (viewQuery) viewQuery.hidden = !isQuery;
  viewOptions.hidden = !isOptions;
  if (viewSystem) viewSystem.hidden = !isSystem;
  if (viewPlugin) viewPlugin.hidden = !isPlugin;

  navTabs.forEach(tab => {
    const label = tab.textContent.trim();
    const active = label === (isVideo ? 'Video' : isQuery ? 'Query data' : isOptions ? 'Options' : isSystem ? 'System' : isPlugin ? 'Plugin' : 'Video');
    tab.classList.toggle('active', active);
  });

  if (isQuery) {
    fetchQueryMedia().catch(error => {
      console.error('Unable to load query data', error);
    });
  }

  if (isPlugin) {
    refreshPluginList().catch(error => {
      console.error('Unable to load plugin list', error);
    });
  }

  if (isOptions && !viewOptions.dataset.loaded) {
    loadCameraSettings().catch(() => {
      console.error('Unable to load camera settings');
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

function renderPluginList(plugins) {
  if (!pluginList) return;

  pluginList.innerHTML = '';
  const normalizedPlugins = Array.isArray(plugins) ? plugins : [];

  if (!normalizedPlugins.length) {
    pluginList.innerHTML = '<div class="query-empty-state">No plugins detected.</div>';
    return;
  }

  normalizedPlugins.forEach((plugin) => {
    const card = document.createElement('div');
    card.className = 'plugin-card';
    card.classList.toggle('selected', plugin.name === selectedPluginName || Boolean(plugin.enabled));
    card.tabIndex = 0;
    card.setAttribute('role', 'button');

    const radio = document.createElement('input');
    radio.type = 'radio';
    radio.name = 'pluginSelection';
    radio.value = plugin.name || '';
    radio.checked = Boolean(plugin.enabled);

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
    status.textContent = plugin.enabled ? 'Active' : plugin.loaded ? 'Disabled' : 'Unloaded';
    status.className = `plugin-status ${plugin.enabled ? 'active' : plugin.loaded ? 'disabled' : 'unloaded'}`;

    const description = document.createElement('div');
    description.className = 'plugin-card-description';
    description.textContent = plugin.name === 'face_recognition'
      ? 'Detects human faces'
      : plugin.name.includes('depth') ? 'Monocular depth estimation' : 'YOLOv8 bounding box detection';
    const version = document.createElement('div');
    version.className = 'plugin-card-version';
    version.textContent = 'v1.0.0';

    top.appendChild(radio);
    top.appendChild(text);
    top.appendChild(status);
    card.appendChild(top);
    card.appendChild(version);
    card.appendChild(description);

    const selectPlugin = async () => {
      try {
        await request('/api/plugins', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ name: plugin.name, enabled: true })
        });
        selectedPluginName = plugin.name;
        await refreshPluginList();
      } catch (error) {
        console.error('Unable to select plugin', error);
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

  const activePlugin = normalizedPlugins.find(plugin => plugin.name === selectedPluginName || plugin.enabled);
  if (activePlugin) {
    selectedPluginName = activePlugin.name;
    renderPluginSettings(activePlugin);
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
  subtitle.textContent = plugin.loaded ? 'Runtime configuration' : 'Select this plugin to load it at runtime';
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

async function loadCameraSettings() {
  const response = await request('/api/camera/settings');
  const settings = response.settings ?? response;
  const form = cameraSettingsForm;
  const cameraType = response.camera_type || 'usb';
  const available = response.available_settings || {};
  const availableResolutions = Array.isArray(available.color_resolutions) ? available.color_resolutions : [{ width: 640, height: 480, label: '640x480' }];
  const availableFps = Array.isArray(available.color_fps_options) ? available.color_fps_options : [30, 15];
  const colorWidth = Number.isFinite(Number(settings.color_width)) ? Number(settings.color_width) : 640;
  const colorHeight = Number.isFinite(Number(settings.color_height)) ? Number(settings.color_height) : 480;
  const depthWidth = Number.isFinite(Number(settings.depth_width)) ? Number(settings.depth_width) : 640;
  const depthHeight = Number.isFinite(Number(settings.depth_height)) ? Number(settings.depth_height) : 480;

  populateSelect(form.color_resolution, availableResolutions, `${colorWidth}x${colorHeight}`);
  populateSelect(form.color_fps, availableFps, String(Number.isFinite(Number(settings.color_fps)) ? Number(settings.color_fps) : 30));

  if (cameraType === 'realsense') {
    const depthContainer = document.querySelector('#depth-settings');
    if (depthContainer) depthContainer.hidden = false;
    populateSelect(form.depth_resolution, Array.isArray(available.depth_resolutions) ? available.depth_resolutions : [{ width: 640, height: 480, label: '640x480' }], `${depthWidth}x${depthHeight}`);
    populateSelect(form.depth_fps, Array.isArray(available.depth_fps_options) ? available.depth_fps_options : [30, 15], String(Number.isFinite(Number(settings.depth_fps)) ? Number(settings.depth_fps) : 30));
  } else {
    const depthContainer = document.querySelector('#depth-settings');
    if (depthContainer) depthContainer.hidden = true;
  }

  form.auto_exposure.value = String(Boolean(settings.auto_exposure));
  form.auto_white_balance.value = String(Boolean(settings.auto_white_balance));
  if (form.usb_device_index) {
    form.usb_device_index.value = settings.usb_device_index ?? -1;
  }

  const showUsbField = settings.usb_device_index !== undefined;
  usbDeviceField.style.display = showUsbField ? 'grid' : 'none';
  viewOptions.dataset.loaded = 'true';
}

async function submitCameraSettings(event) {
  event.preventDefault();

  const applyButton = event.submitter || cameraSettingsForm.querySelector('button[type="submit"]');
  if (applyButton) {
    applyButton.disabled = true;
    applyButton.textContent = 'Restarting camera...';
  }

  try {
    const selectedResolution = cameraSettingsForm.color_resolution.value || '640x480';
    const [width, height] = selectedResolution.split('x').map(Number);

    const payload = {
      color_width: Number(width),
      color_height: Number(height),
      color_fps: Number(cameraSettingsForm.color_fps.value),
      auto_exposure: cameraSettingsForm.auto_exposure.value === 'true',
      auto_white_balance: cameraSettingsForm.auto_white_balance.value === 'true'
    };

    if (cameraSettingsForm.depth_resolution && !cameraSettingsForm.depth_resolution.closest('#depth-settings').hidden) {
      const selectedDepthResolution = cameraSettingsForm.depth_resolution.value || '640x480';
      const [depthWidth, depthHeight] = selectedDepthResolution.split('x').map(Number);
      payload.depth_enabled = true;
      payload.depth_width = Number(depthWidth);
      payload.depth_height = Number(depthHeight);
      payload.depth_fps = Number(cameraSettingsForm.depth_fps.value);
    }

    if (cameraSettingsForm.usb_device_index) {
      payload.usb_device_index = Number(cameraSettingsForm.usb_device_index.value);
    }

    await request('/api/camera/settings', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload)
    });

    await new Promise(resolve => setTimeout(resolve, 1500));
    showView('video');
    alert('Camera settings updated.');
  } finally {
    if (applyButton) {
      applyButton.disabled = false;
      applyButton.textContent = 'Apply Settings';
    }
  }
}

// Initialize Dashboard & Timers
startLiveClock();

navTabs.forEach(tab => {
  tab.addEventListener('click', () => {
    const label = tab.textContent.trim();
    if (label === 'Video') showView('video');
    if (label === 'Query data') showView('query');
    if (label === 'Options') showView('options');
    if (label === 'System') showView('system');
    if (label === 'Plugin') showView('plugin');
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

cameraSettingsForm.addEventListener('submit', submitCameraSettings);
cancelSettingsBtn.addEventListener('click', () => showView('video'));

if (window.location.pathname === '/dashboard') {
  loginContainer.hidden = true;
  dashboard.hidden = false;
  showView('video');
  refreshStatus().catch(error => { cameraStatus.textContent = error.message; });

  fetchSessionLogs();
  refreshPluginList().catch(() => {});
  setInterval(fetchSessionLogs, 1000);
  setInterval(refreshStatus, 2000);
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