const $ = (selector, root = document) => root.querySelector(selector);
const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];
let state = null;
let activeView = location.pathname === "/setup" ? "network" : "dashboard";
let selectedNetworkSecure = true;
let selectedSsid = "";
let formDirty = false;
let pollTimer;
let selectedFirmware = null;
let otaUploadActive = false;
let refreshPending = false;
let apiQueue = Promise.resolve();

function titleCase(value = "unknown") {
  return value.replace(/(^|_)([a-z])/g, (_, p, c) => `${p ? " " : ""}${c.toUpperCase()}`);
}

function api(path, options = {}) {
  const request = apiQueue.then(async () => {
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 5000);
    try {
      const response = await fetch(path, {
        ...options,
        signal: controller.signal,
        headers: { "Content-Type": "application/json", ...(options.headers || {}) },
      });
      const payload = await response.json();
      if (!response.ok) throw new Error(payload.error || `Request failed (${response.status})`);
      return payload;
    } finally {
      clearTimeout(timeout);
    }
  });
  apiQueue = request.catch(() => {});
  return request;
}

function showToast(message) {
  const toast = $("#toast");
  toast.textContent = message;
  toast.classList.add("show");
  setTimeout(() => toast.classList.remove("show"), 2800);
}

function setView(view) {
  activeView = view;
  document.body.dataset.view = view;
  $$('[data-view]').forEach((node) => { node.hidden = node.dataset.view !== view; });
  $$('[data-view-button]').forEach((button) => button.classList.toggle("active", button.dataset.viewButton === view));
  window.scrollTo({ top: 0, behavior: "smooth" });
}

function setLimit(which, engaged) {
  $(`[data-limit-dot="${which}"]`).classList.toggle("engaged", engaged);
  $(`[data-limit-text="${which}"]`).textContent = engaged ? "Engaged" : "Not engaged";
}

function renderNetworks(network) {
  const list = $("#network-list");
  if (network.scanning && !network.scan.length) {
    list.innerHTML = '<div class="network-placeholder">Scanning for nearby networks…</div>';
    return;
  }
  if (!network.scan.length) {
    list.innerHTML = '<div class="network-placeholder">No networks found yet. Select “Scan again”.</div>';
    return;
  }
  list.innerHTML = network.scan.slice(0, 5).map((entry) => `
    <button type="button" class="network-option${entry.ssid === selectedSsid ? " selected" : ""}" data-network="${entry.ssid.replaceAll('"', '&quot;')}" data-secure="${entry.secure}">
      <i></i><span>${entry.ssid}</span><b>${entry.secure ? "⌁" : "○"}</b>
    </button>`).join("");
  $$('[data-network]', list).forEach((button) => button.addEventListener("click", () => {
    $$('[data-network]', list).forEach((item) => item.classList.remove("selected"));
    button.classList.add("selected");
    selectedSsid = button.dataset.network;
    formDirty = true;
    $("#wifi-ssid").value = button.dataset.network;
    selectedNetworkSecure = button.dataset.secure === "true";
    $("#wifi-password").required = selectedNetworkSecure;
  }));
}

function render(next) {
  state = next;
  const { door, led, network, mqtt, ota } = state;
  $$('[data-device-name]').forEach((node) => { node.textContent = state.deviceName; });
  $("#door-state").textContent = titleCase(door.state);
  setLimit("upper", door.upperLimit);
  setLimit("lower", door.lowerLimit);
  const moving = ["opening", "closing", "homing"].includes(door.state);
  $("#door-panel").classList.toggle("open", door.state === "open" || door.state === "opening");
  $("#door-panel").classList.toggle("moving", moving);
  $("#activity-text").textContent = door.state === "homing" ? `Homing ${state.homeTarget}` : moving ? titleCase(door.state) : door.state === "fault" ? "Needs attention" : "Idle";
  $(".activity").classList.toggle("busy", moving);
  $("#state-seal").textContent = door.state === "fault" ? "!" : door.state === "open" ? "↑" : "▣";
  $("#limit-summary").textContent = door.upperLimit ? "Upper limit engaged" : door.lowerLimit ? "Lower limit engaged" : "Between limits";
  $("#fault-text").hidden = !door.fault;
  $("#fault-text").textContent = door.fault;
  $("#safety-banner").hidden = door.actuatorArmed;
  $$('[data-door-command]').forEach((button) => {
    const command = button.dataset.doorCommand;
    button.disabled = !door.actuatorArmed && command !== "stop" || moving && command !== "stop" || command === "open" && door.upperLimit || command === "close" && door.lowerLimit;
  });
  $$('[data-home-target]').forEach((button) => {
    button.classList.toggle("selected", button.dataset.homeTarget === state.homeTarget);
    button.disabled = moving;
  });
  $("#home-help").textContent = `Homing moves the door to the ${state.homeTarget} limit and establishes its reference position.`;
  $("#brightness").value = led.brightness;
  $("#brightness-value").textContent = `${led.brightness}%`;
  const color = `#${[led.red, led.green, led.blue].map((value) => value.toString(16).padStart(2, "0")).join("")}`;
  $("#led-color").value = color;
  $$('[data-led-mode]').forEach((button) => button.classList.toggle("selected", button.dataset.ledMode === led.mode));
  $("#wifi-status").textContent = network.connected ? `${network.ssid} · ${network.ip}` : network.connecting ? "Connecting…" : "Setup mode";
  $("#mqtt-status").textContent = mqtt.connected ? "Online" : mqtt.enabled ? "Connecting…" : "Not configured";
  $("#setup-status").textContent = network.connected ? `Connected to ${network.ssid}` : network.apActive ? "Setup access point active" : "Waiting for Wi‑Fi";
  $("#settings-armed").textContent = door.actuatorArmed ? "Armed" : "Disarmed";
  $("#settings-motor").textContent = door.motorReady ? "Ready" : "Unavailable";
  $("#settings-ip").textContent = network.ip || "—";
  $("#settings-uptime").textContent = `${Math.floor(state.uptimeSeconds / 60)} minutes`;
  $("#settings-version").textContent = ota.runningVersion;
  $("#ota-health").textContent = ota.rebootPending ? "Update staged · rebooting" : ota.imageConfirmed ? "Rollback protection ready" : "Testing this firmware before confirmation";
  $("#ota-health").classList.toggle("ready", ota.imageConfirmed && !ota.rebootPending);
  $("#ota-upload").disabled = otaUploadActive || !selectedFirmware || !ota.imageConfirmed || ota.rebootPending;
  if (!formDirty) {
    selectedSsid = network.ssid || selectedSsid;
    $("#wifi-ssid").value = selectedSsid;
    $("#device-name").value = state.deviceName;
    $("#mqtt-enabled").checked = mqtt.enabled;
    $("#mqtt-host").value = mqtt.host || "";
    $("#mqtt-port").value = mqtt.port || 1883;
    $("#mqtt-username").value = mqtt.username || "";
    $("#mqtt-fields").hidden = !mqtt.enabled;
  }
  $("#last-update").textContent = "just now";
  renderNetworks(network);
}

async function refresh() {
  if (refreshPending || otaUploadActive) return;
  refreshPending = true;
  try {
    const next = await api("/api/state");
    if (otaUploadActive && !next.ota.rebootPending) {
      otaUploadActive = false;
      $("#ota-status").textContent = next.ota.imageConfirmed ? "Firmware update is online and confirmed." : "New firmware is online and completing its 30-second rollback safety check.";
    }
    render(next);
  } catch (error) {
    $("#last-update").textContent = "device offline";
  } finally {
    refreshPending = false;
  }
}

async function commandDoor(command) {
  try {
    render(await api("/api/door", { method: "POST", body: JSON.stringify({ command }) }));
    showToast(`${titleCase(command)} command accepted`);
  } catch (error) { showToast(error.message); }
}

async function setHomeTarget(target) {
  try {
    render(await api("/api/config", { method: "POST", body: JSON.stringify({ homeTarget: target }) }));
    showToast(`Door will home toward the ${target} limit`);
  } catch (error) { showToast(error.message); }
}

async function setLed(mode) {
  const color = $("#led-color").value;
  const brightness = Number($("#brightness").value);
  try {
    render(await api("/api/led", { method: "POST", body: JSON.stringify({ mode, color, brightness }) }));
  } catch (error) { showToast(error.message); }
}

async function scanNetworks() {
  if (!state?.network.scanning) {
    try { render(await api("/api/wifi/scan")); } catch (error) { showToast(error.message); }
  }
}

async function saveSetup(event) {
  event.preventDefault();
  const message = $("#form-message");
  message.classList.remove("error");
  message.textContent = "Saving settings…";
  try {
    const config = {
      deviceName: $("#device-name").value.trim(), mqttEnabled: $("#mqtt-enabled").checked,
      mqttHost: $("#mqtt-host").value.trim(), mqttPort: Number($("#mqtt-port").value || 1883),
      mqttUsername: $("#mqtt-username").value.trim(), mqttPassword: $("#mqtt-password").value,
    };
    await api("/api/config", { method: "POST", body: JSON.stringify(config) });
    render(await api("/api/wifi", { method: "POST", body: JSON.stringify({
      ssid: $("#wifi-ssid").value.trim(), password: $("#wifi-password").value, secure: selectedNetworkSecure,
    }) }));
    message.textContent = "Saved. The door is connecting to your network…";
    formDirty = false;
    showToast("Network settings saved");
  } catch (error) {
    message.classList.add("error");
    message.textContent = error.message;
  }
}

function uploadFirmware() {
  if (!selectedFirmware || otaUploadActive) return;
  if (!selectedFirmware.name.toLowerCase().endsWith(".bin")) {
    showToast("Choose the zephyr.signed.bin firmware file");
    return;
  }

  otaUploadActive = true;
  clearInterval(pollTimer);
  const progress = $("#ota-progress");
  const status = $("#ota-status");
  const button = $("#ota-upload");
  progress.hidden = false;
  progress.value = 0;
  status.classList.remove("error");
  status.textContent = "Uploading signed firmware… Keep the door powered.";
  button.disabled = true;

  const request = new XMLHttpRequest();
  request.open("POST", "/api/ota");
  request.setRequestHeader("Content-Type", "application/octet-stream");
  request.upload.addEventListener("progress", (event) => {
    if (event.lengthComputable) {
      progress.value = Math.round(event.loaded * 100 / event.total);
      status.textContent = `Uploading signed firmware… ${progress.value}%`;
    }
  });
  request.addEventListener("load", () => {
    let payload = {};
    try { payload = JSON.parse(request.responseText); } catch (_) { /* reboot may win the race */ }
    if (request.status >= 200 && request.status < 300) {
      progress.value = 100;
      status.textContent = `Firmware ${payload.version || "update"} staged. Rebooting into a rollback-protected test boot…`;
      showToast("Firmware uploaded; the door is rebooting");
      setTimeout(() => { pollTimer = setInterval(refresh, 2000); refresh(); }, 7000);
    } else {
      otaUploadActive = false;
      status.classList.add("error");
      status.textContent = payload.error || `Upload failed (${request.status})`;
      button.disabled = !selectedFirmware;
      pollTimer = setInterval(refresh, 1000);
    }
  });
  request.addEventListener("error", () => {
    otaUploadActive = false;
    status.classList.add("error");
    status.textContent = "Upload connection failed. The existing firmware is still active.";
    button.disabled = !selectedFirmware;
    pollTimer = setInterval(refresh, 1000);
  });
  request.send(selectedFirmware);
}

$$('[data-view-button]').forEach((button) => button.addEventListener("click", () => {
  $("#settings-dialog").close(); setView(button.dataset.viewButton);
}));
$$('[data-door-command]').forEach((button) => button.addEventListener("click", () => commandDoor(button.dataset.doorCommand)));
$$('[data-home-target]').forEach((button) => button.addEventListener("click", () => setHomeTarget(button.dataset.homeTarget)));
$$('[data-led-mode]').forEach((button) => button.addEventListener("click", () => setLed(button.dataset.ledMode)));
$("#brightness").addEventListener("input", (event) => { $("#brightness-value").textContent = `${event.target.value}%`; });
$("#brightness").addEventListener("change", () => setLed(state?.led.mode || "status"));
$("#led-color").addEventListener("change", () => setLed("solid"));
$("#scan-button").addEventListener("click", scanNetworks);
$("#setup-form").addEventListener("submit", saveSetup);
$("#setup-form").addEventListener("input", () => { formDirty = true; });
$("#mqtt-enabled").addEventListener("change", (event) => { $("#mqtt-fields").hidden = !event.target.checked; });
$$('[data-toggle-password]').forEach((button) => button.addEventListener("click", () => {
  const input = document.getElementById(button.dataset.togglePassword);
  input.type = input.type === "password" ? "text" : "password";
  button.textContent = input.type === "password" ? "Show" : "Hide";
}));
$$('[data-open-settings]').forEach((button) => button.addEventListener("click", () => $("#settings-dialog").showModal()));
$("#ota-file").addEventListener("change", (event) => {
  selectedFirmware = event.target.files[0] || null;
  $("#ota-file-name").textContent = selectedFirmware ? `${selectedFirmware.name} · ${(selectedFirmware.size / 1024).toFixed(0)} KB` : "Choose signed firmware";
  $("#ota-upload").disabled = !selectedFirmware || otaUploadActive || !state?.ota.imageConfirmed;
});
$("#ota-upload").addEventListener("click", uploadFirmware);

setView(activeView);
setTimeout(() => {
  refresh();
  pollTimer = setInterval(refresh, 1000);
}, 750);
