const connection = document.getElementById("bms_connection");
const bmsStatus = document.getElementById("bms_status");
const boardGrid = document.getElementById("board_grid");
const faultLog = document.getElementById("fault_log");

const faultLabels = {
  cell_voltage_open: "Cell voltage sense OPEN",
  temperature_open: "Temperature sensor OPEN",
  temperature_short: "Temperature sensor SHORT",
  balance_stuck_open: "Balance channel stuck OPEN",
  balance_stuck_closed: "Balance channel stuck CLOSED",
  low_negative_voltage_open: "Low-side negative voltage sense OPEN",
  low_positive_voltage_fault: "Low-side positive voltage sense FAULT",
  high_negative_voltage_open: "High-side negative voltage sense OPEN",
  high_positive_voltage_fault: "High-side positive voltage sense FAULT",
  ltc6804_low_communication: "Low-side LTC6804 communication FAULT",
  ltc6804_high_communication: "High-side LTC6804 communication FAULT"
};

function finite(value) {
  return value !== null && value !== undefined && value !== "" &&
    Number.isFinite(Number(value));
}
function value(value, digits) { return finite(value) ? Number(value).toFixed(digits) : "--"; }
function set(id, text) { const element = document.getElementById(id); if (element) element.textContent = text; }

function setConnection(status) {
  const live = status === "LIVE";
  connection.textContent = status;
  connection.className = `pill ${live ? "online" : "offline"}`;
  bmsStatus.textContent = status;
  bmsStatus.className = `pill ${live ? "online" : "offline"}`;
}

function measurement(name, raw, valid, faultText, digits, unit) {
  const fault = Boolean(faultText);
  const usable = valid && raw !== null && raw !== undefined && finite(raw);
  const classes = `measurement ${usable ? "" : "unavailable"} ${fault ? "fault" : ""}`;
  const state = faultText || (!usable ? "UNAVAILABLE" : "");
  return `<div class="${classes}"><span class="measurement-name">${name}</span><span class="measurement-value">${usable ? value(raw, digits) + unit : "--"}${state ? ` · ${state}` : ""}</span></div>`;
}

function renderBoard(board) {
  const faults = board.faults || {};
  const cells = board.cells_v || [];
  const cellValid = board.cell_valid || [];
  const temps = board.temps_c || [];
  const tempValid = board.temp_valid || [];
  const activeFaults = Number(board.fault_count || 0);
  const status = board.status || (board.valid ? "LIVE" : "OFFLINE");
  const cellsHtml = cells.map((raw, index) => measurement(
    `C${String(index + 1).padStart(2, "0")}`, raw, Boolean(cellValid[index]),
    faults.voltage_open && faults.voltage_open[index] ? "OPEN" : "", 3, "V")).join("");
  const tempsHtml = temps.map((raw, index) => {
    let fault = "";
    if (faults.temp_open && faults.temp_open[index]) fault = "OPEN";
    if (faults.temp_short && faults.temp_short[index]) fault = "SHORT";
    return measurement(`T${String(index + 1).padStart(2, "0")}`, raw,
      Boolean(tempValid[index]), fault, 0, "°C");
  }).join("");
  const faultSummary = activeFaults === 0 ? "No active faults" : `${activeFaults} active fault${activeFaults === 1 ? "" : "s"}`;
  return `<article class="card board-card"><div class="board-heading"><div><p class="eyebrow">KPL07</p><h2>Board ${board.board_number ?? board.id + 1}</h2></div><div class="board-health">${status}<br>${value(board.age_ms, 0)} ms old</div></div><div class="measurement-title">CELL VOLTAGE · 24 CHANNELS</div><div class="measurement-grid">${cellsHtml}</div><div class="measurement-title">TEMPERATURE · T01–T08</div><div class="measurement-grid temperature-grid">${tempsHtml}</div><div class="fault-summary ${activeFaults === 0 ? "ok" : ""}">${faultSummary}${board.faults_valid ? "" : " · FAULT DATA STALE"}</div></article>`;
}

function formatTime(timestampUs) {
  const total = Math.floor(Number(timestampUs || 0) / 1000000);
  const h = String(Math.floor(total / 3600) % 100).padStart(2, "0");
  const m = String(Math.floor(total / 60) % 60).padStart(2, "0");
  const s = String(total % 60).padStart(2, "0");
  return `${h}:${m}:${s}`;
}

function renderEvents(events) {
  const list = Array.isArray(events) ? events : [];
  set("event_count", `${list.length} event${list.length === 1 ? "" : "s"}`);
  if (!list.length) {
    faultLog.innerHTML = `<div class="empty-log">No fault events</div>`;
    return;
  }
  faultLog.innerHTML = list.map(event => {
    const cell = event.global_cell ? ` · Global C${event.global_cell}` : "";
    const channel = event.channel ? ` · Ch${event.channel}` : "";
    const label = faultLabels[event.type] || event.type || "Unknown fault";
    const state = event.state || "FAULT";
    return `<div class="fault-event ${state === "FAULT" ? "fault" : "recovered"}"><span class="fault-event-time">${formatTime(event.timestamp_us)}</span><span>B${event.board_number ?? Number(event.board || 0) + 1}${channel}${cell} · ${label}</span><strong>${state}</strong></div>`;
  }).join("");
}

function render(data) {
  const status = data.status || (data.valid ? "LIVE" : "WAITING");
  setConnection(status);
  const meter = data.meter || {};
  set("bms_hv1", value(meter.hv1_v, 2));
  set("bms_hv2", value(meter.hv2_v, 2));
  set("bms_hv3", value(meter.hv3_v, 2));
  set("bms_hv4", value(meter.hv4_v, 2));
  set("bms_hv5", value(meter.hv5_v, 2));
  set("bms_current", value(meter.current_a, 2));
  set("meter_detail", `Meter UI: ${meter.ui_valid ? "VALID" : "STALE"} · Meter AP: ${meter.ap_valid ? "VALID" : "STALE"}`);
  boardGrid.innerHTML = (data.boards || []).map(renderBoard).join("");
  renderEvents(data.fault_events);
}

async function refresh() {
  try {
    const response = await fetch("/api/bms", { cache: "no-store" });
    const data = await response.json();
    render(data);
  } catch (_) {
    setConnection("OFFLINE");
  }
}

setConnection("OFFLINE");
refresh();
setInterval(refresh, 1000);
