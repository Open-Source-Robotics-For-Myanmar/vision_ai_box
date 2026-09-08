const loginForm = document.querySelector('#login-form');
const loginContainer = document.querySelector('#login-container');
const dashboard = document.querySelector('#dashboard');
const loginMessage = document.querySelector('#login-message');
const cameraToggle = document.querySelector('#camera-toggle');
const cameraStatus = document.querySelector('#camera-status');
const stream = document.querySelector('#stream');
const liveDatetime = document.querySelector('#live-datetime');
const logTerminal = document.querySelector('#log-terminal');

let toggleRequestActive = false;
let streamActive = false;

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
}

// Initialize Dashboard & Timers
startLiveClock();

if (window.location.pathname === '/dashboard') {
  loginContainer.hidden = true;
  dashboard.hidden = false;
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