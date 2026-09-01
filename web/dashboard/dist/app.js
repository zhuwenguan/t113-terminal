const connection = document.getElementById("connection");
let socket;
let reconnectTimer;

function number(value, digits = 1) {
  return Number.isFinite(Number(value)) ? Number(value).toFixed(digits) : "--";
}

function set(id, value) {
  const element = document.getElementById(id);
  if (element) element.textContent = value;
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
  } catch (_) {
    set("state_age_ms", "--");
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
connect();
refreshSystem();
setInterval(refreshSystem, 1000);
