const loginForm = document.querySelector('#login-form');
const loginContainer = document.querySelector('#login-container');
const dashboard = document.querySelector('#dashboard');
const viewVideo = document.querySelector('#view-video');
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
const navTabs = Array.from(document.querySelectorAll('.nav-tab'));

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

function startRecordingTimer() {
  const recordDuration = document.querySelector('#record-duration');
  if (!recordDuration) return;

  recordingStartedAt = Date.now();
  recordDuration.hidden = false;
  recordDuration.textContent = '00:00:00';

  if (recordingTimerId) {
    clearInterval(recordingTimerId);
  }

  recordingTimerId = setInterval(() => {
    if (!recordingStartedAt) return;
    const elapsedSeconds = Math.floor((Date.now() - recordingStartedAt) / 1000);
    recordDuration.textContent = formatDuration(elapsedSeconds);
  }, 1000);
}

// Real-Time Green Clock Function
function startLiveClock() {
  function updateClock() {
    const now = new Date();
    const month = String(now.getMonth() + 1).padStart(2, '0');
    const day = String(now.getDate()).padStart(2, '0');
    const year = now.getFullYear();
    const hours = String(now.getHours()).padStart(2, '0');
    const minutes = String(now.getMinutes()).padStart(2, '0');
    const seconds = String(now.getSeconds()).padStart(2, '0');
    
    liveDatetime.textContent = `${month}/${day}/${year} ${hours}:${minutes}:${seconds}`;
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
    const dot = recordStatusIndicator.querySelector('span');
    dot.style.background = active ? '#ff3b30' : '#666';
    recordStatusText.textContent = active ? 'Recording' : 'Idle';
    recordToggleBtn.textContent = active ? 'Stop Record' : 'Start Record';
    recordToggleBtn.style.background = active ? '#d32f2f' : '#1e88e5';

    const recordDuration = document.querySelector('#record-duration');
    if (active) {
      if (!recordDuration || recordDuration.hidden) {
        startRecordingTimer();
      } else if (recordingStartedAt === null) {
        startRecordingTimer();
      }
    } else {
      stopRecordingTimer();
    }
  } catch (error) {
    recordStatusText.textContent = 'Status unavailable';
    stopRecordingTimer();
  }
}

function showView(viewName) {
  const isVideo = viewName === 'video';
  const isOptions = viewName === 'options';
  const isSystem = viewName === 'system';

  viewVideo.hidden = !isVideo;
  viewOptions.hidden = !isOptions;
  if (viewSystem) viewSystem.hidden = !isSystem;

  navTabs.forEach(tab => {
    const label = tab.textContent.trim();
    const active = label === (isVideo ? 'Video' : isOptions ? 'Options' : isSystem ? 'System' : 'Video');
    tab.classList.toggle('active', active);
  });

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
    if (label === 'Options') showView('options');
    if (label === 'System') showView('system');
  });
});

cameraSettingsForm.addEventListener('submit', submitCameraSettings);
cancelSettingsBtn.addEventListener('click', () => showView('video'));

if (window.location.pathname === '/dashboard') {
  loginContainer.hidden = true;
  dashboard.hidden = false;
  showView('video');
  refreshStatus().catch(error => { cameraStatus.textContent = error.message; });
  
  fetchSessionLogs();
  setInterval(fetchSessionLogs, 1000);
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