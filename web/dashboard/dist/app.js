const connection = document.getElementById("connection");
const videoCard = document.getElementById("video_card");
const liveVideo = document.getElementById("live_video");
const videoPlaceholder = document.getElementById("video_placeholder");
const videoStatus = document.getElementById("video_status");
let socket;
let reconnectTimer;
let videoCapability = false;
let videoStarted = false;

function number(value, digits = 1) {
  return Number.isFinite(Number(value)) ? Number(value).toFixed(digits) : "--";
}

function set(id, value) {
  const element = document.getElementById(id);
  if (element) element.textContent = value;
}

function renderVideo(video) {
  if (!videoCapability || !videoCard) return;
  videoCard.hidden = false;
  const live = video && video.available === true && video.status === "live";
  const stale = video && video.status === "stale";
  const label = live ? "LIVE" : stale ? "VIDEO STALE" : "CAMERA OFFLINE";
  videoStatus.textContent = label;
  videoStatus.className = `pill ${live ? "online" : "offline"}`;
  set("camera_status", label);
  set("video_fps", live ? `${number(video.web_publish_fps, 1)} FPS` : "-- FPS");
  set("video_resolution", video && video.width ?
    `${video.width} × ${video.height}` : "-- × --");
  set("video_jpeg_size", live ?
    `${number(Number(video.jpeg_size_bytes) / 1024, 1)} KB JPEG` : "-- KB JPEG");
  if (live && !videoStarted) {
    liveVideo.src = `/video/live.mjpg?stream=${Date.now()}`;
    liveVideo.style.display = "block";
    videoPlaceholder.style.display = "none";
    videoStarted = true;
  } else if (!live && videoStarted) {
    liveVideo.removeAttribute("src");
    liveVideo.style.display = "none";
    videoPlaceholder.style.display = "grid";
    videoStarted = false;
  }
}

function renderState(state) {
  const valid = state && state.valid !== false;
  set("speed_kph", valid ? number(state.speed_kph, 1) : "--");
  set("motor_rpm", valid ? number(state.motor_rpm, 0) : "--");
  set("torque_nm", valid ? number(state.torque_nm, 1) : "--");
  set("hv_power_kw", valid ? number(state.hv_power_kw, 1) : "--");
  set("hv_voltage_v", valid ? number(state.hv_voltage_v, 1) : "--");
  set("hv_current_a", valid ? number(state.hv_current_a, 1) : "--");
  set("hv_soc_percent", valid ? number(state.hv_soc_percent, 1) : "--");
  set("battery_temperature_c", valid ? number(state.battery_temperature_c, 1) : "--");
  set("lap_number", valid ? state.lap_number : "--");
  set("lap_time_s", valid ? number(state.lap_time_s, 3) : "--");
  set("best_lap_time_s", valid ? number(state.best_lap_time_s, 3) : "--");
  set("delta_s", valid ? number(state.delta_s, 3) : "--");
  set("can_status", valid ? (state.can_valid ? "OK" : "STALE") : "--");
  set("gps_status", valid ? (state.gps_valid ? "OK" : "NO FIX") : "--");
  set("drive_state", valid ? state.drive_state : "--");
}

function setConnection(online) {
  connection.textContent = online ? "ONLINE" : "OFFLINE";
  connection.className = `pill ${online ? "online" : "offline"}`;
}

async function refreshSystem() {
  try {
    const response = await fetch("/api/system", { cache: "no-store" });
    const system = await response.json();
    set("state_age_ms", system.vehicle_state_age_ms ?? "--");
    renderVideo(system.video);
  } catch (_) {
    set("state_age_ms", "--");
  }
}

async function loadCapabilities() {
  try {
    const response = await fetch("/api/capabilities", { cache: "no-store" });
    const capabilities = await response.json();
    videoCapability = capabilities.video === true;
    if (videoCard) videoCard.hidden = !videoCapability;
    if (!videoCapability) set("camera_status", "NOT AVAILABLE");
  } catch (_) {
    videoCapability = false;
  }
}

function connect() {
  const protocol = location.protocol === "https:" ? "wss:" : "ws:";
  socket = new WebSocket(`${protocol}//${location.host}/ws/telemetry`);
  socket.onopen = () => {
    setConnection(true);
    fetch("/api/state", { cache: "no-store" })
      .then(response => response.ok ? response.json() : null)
      .then(state => state && renderState(state))
      .catch(() => {});
  };
  socket.onmessage = event => {
    try { renderState(JSON.parse(event.data)); } catch (_) {}
  };
  socket.onclose = () => {
    setConnection(false);
    clearTimeout(reconnectTimer);
    reconnectTimer = setTimeout(connect, 1500);
  };
  socket.onerror = () => socket.close();
}

setConnection(false);
loadCapabilities();
connect();
refreshSystem();
setInterval(refreshSystem, 1000);
