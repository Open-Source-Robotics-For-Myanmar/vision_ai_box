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
const logTerminal = document.querySelector('#log-terminal');
const recordToggleBtn = document.querySelector('#record-toggle-btn');
const restartRecordBtn = document.querySelector('#restart-record-btn');
const captureBtn = document.querySelector('#capture-btn');
const recordStatusIndicator = document.querySelector('#record-status-indicator');
const recordStatusText = document.querySelector('#record-status-text');
const cameraSettingsForm = document.querySelector('#camera-settings-form');
const cancelSettingsBtn = document.querySelector('#cancel-settings-btn');
const usbDeviceField = document.querySelector('#usb-device-field');
const navTabs = Array.from(document.querySelectorAll('.nav-tab'));

let toggleRequestActive = false;
let streamActive = false;
let recordingRequestActive = false;

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
      logTerminal.textContent = logs.join('\n');
      logTerminal.scrollTop = logTerminal.scrollHeight;
    } else {
      logTerminal.textContent = 'No logs recorded for this session.';
    }
  } catch (err) {
    logTerminal.textContent = 'Failed to fetch session logs...';
  }
}

// Refresh Camera Feed & Toggle Status
async function refreshStatus() {
  const status = await request('/api/camera/status');
  cameraToggle.checked = status.running;

  if (status.running) {
    cameraStatus.textContent = 'Running';
    cameraStatus.classList.add('active');
  } else {
    cameraStatus.textContent = 'Stopped';
    cameraStatus.classList.remove('active');
  }

  stream.hidden = !status.running;
  if (status.running && !streamActive) {
    stream.src = '/api/camera/stream?generation=' + Date.now();
    streamActive = true;
  } else if (!status.running && streamActive) {
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
  } catch (error) {
    recordStatusText.textContent = 'Status unavailable';
  }
}

function showView(viewName) {
  const isVideo = viewName === 'video';
  viewVideo.hidden = !isVideo;
  viewOptions.hidden = isVideo;

  navTabs.forEach(tab => {
    const active = tab.textContent.trim() === (isVideo ? 'Video' : 'Options');
    tab.classList.toggle('active', active);
  });

  if (!isVideo && !viewOptions.dataset.loaded) {
    loadCameraSettings().catch(() => {
      console.error('Unable to load camera settings');
    });
  }
}

async function loadCameraSettings() {
  const settings = await request('/api/camera/settings');
  const form = cameraSettingsForm;
  form.color_width.value = settings.color_width ?? 640;
  form.color_height.value = settings.color_height ?? 480;
  form.color_fps.value = settings.color_fps ?? 30;
  form.auto_exposure.value = String(Boolean(settings.auto_exposure));
  form.auto_white_balance.value = String(Boolean(settings.auto_white_balance));
  form.usb_device_index.value = settings.usb_device_index ?? -1;

  const showUsbField = settings.usb_device_index !== undefined;
  usbDeviceField.style.display = showUsbField ? 'grid' : 'none';
  viewOptions.dataset.loaded = 'true';
}

async function submitCameraSettings(event) {
  event.preventDefault();

  const payload = {
    color_width: Number(cameraSettingsForm.color_width.value),
    color_height: Number(cameraSettingsForm.color_height.value),
    color_fps: Number(cameraSettingsForm.color_fps.value),
    auto_exposure: cameraSettingsForm.auto_exposure.value === 'true',
    auto_white_balance: cameraSettingsForm.auto_white_balance.value === 'true'
  };

  if (cameraSettingsForm.usb_device_index) {
    payload.usb_device_index = Number(cameraSettingsForm.usb_device_index.value);
  }

  await request('/api/camera/settings', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload)
  });

  alert('Camera settings updated.');
  showView('video');
}

// Initialize Dashboard & Timers
startLiveClock();

navTabs.forEach(tab => {
  tab.addEventListener('click', () => {
    const label = tab.textContent.trim();
    if (label === 'Video') showView('video');
    if (label === 'Options') showView('options');
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