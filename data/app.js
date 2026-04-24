 async function fetchJson(url) {
  const response = await fetch(url, { cache: "no-store" });
  if (!response.ok) {
    throw new Error(`HTTP ${response.status}`);
  }
  return response.json();
}

function setText(id, value) {
  const element = document.getElementById(id);
  if (element) {
    element.textContent = value;
  }
}

function setTextMany(ids, value) {
  ids.forEach((id) => setText(id, value));
}

function setStatus(id, ok, text) {
  const el = document.getElementById(id);
  if (!el) {
    return;
  }

  el.textContent = text;
  el.className = ok ? "status-pill ok" : "status-pill bad";
}

function setNeutralStatus(id, text) {
  const el = document.getElementById(id);
  if (!el) {
    return;
  }

  el.textContent = text;
  el.className = "status-pill neutral";
}

function setMapLink(latitude, longitude) {
  const mapLink = document.getElementById("gpsMap");
  if (!mapLink) {
    return;
  }

  mapLink.href = `https://maps.google.com/?q=${latitude},${longitude}`;
}

function setPortalSignInStatus(text, isSignedIn = false) {
  const el = document.getElementById("portalSignInStatus");
  if (!el) {
    return;
  }
  el.textContent = text;
  el.className = isSignedIn ? "portal-signin-status ok" : "portal-signin-status";
}

function setActiveView(viewName) {
  const panels = document.querySelectorAll(".view-panel[data-view]");
  panels.forEach((panel) => {
    const isActive = panel.dataset.view === viewName;
    panel.classList.toggle("is-active", isActive);
  });

  const navButtons = document.querySelectorAll(".view-nav-item[data-view]");
  navButtons.forEach((button) => {
    const isActive = button.dataset.view === viewName;
    button.classList.toggle("active", isActive);
    button.setAttribute("aria-pressed", isActive ? "true" : "false");
  });

  if (location.hash !== `#${viewName}`) {
    history.replaceState(null, "", `#${viewName}`);
  }

  if (viewName === "diagnostics") {
    startModemHealthRefresh();
  } else {
    stopModemHealthRefresh();
  }
}

async function refreshPortalStatus() {
  try {
    const status = await fetchJson("/portal/status");
    if (status.online) {
      const ip = status.ipAddress && status.ipAddress !== "0.0.0.0" ? ` · ${status.ipAddress}` : "";
      setPortalSignInStatus(`Internet online${ip}`, true);
    } else if (status.signedIn) {
      setPortalSignInStatus(status.reason || "Connected to portal", false);
    } else {
      setPortalSignInStatus(status.reason || "Not signed in", false);
    }
  } catch {
    setPortalSignInStatus("Status unavailable", false);
  }
}

async function signInPortal() {
  const btn = document.getElementById("portalSignInBtn");
  const voucherEl = document.getElementById("portalVoucher");
  if (!btn) {
    return;
  }

  const code = voucherEl ? voucherEl.value.trim() : "";
  if (!code) {
    setPortalSignInStatus("Enter voucher code", false);
    return;
  }

  btn.disabled = true;
  setPortalSignInStatus("Signing in...");

  try {
    const response = await fetch("/portal/signin", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ code }),
    });
    const payload = await response.json();
    if (!response.ok || !payload.ok) {
      setPortalSignInStatus("Sign in failed");
    } else if (payload.online) {
      setPortalSignInStatus("Internet online", true);
    } else {
      setPortalSignInStatus(payload.reason || "Signed in", false);
    }
  } catch {
    setPortalSignInStatus("Sign in failed");
  } finally {
    btn.disabled = false;
  }
}

function setSmsStatus(text, isError = false) {
  const el = document.getElementById("smsStatus");
  if (!el) {
    return;
  }
  el.textContent = text;
  el.className = isError ? "sms-status error" : "sms-status ok";
}

function extractCmgsRef(modemResponse) {
  const text = String(modemResponse || "");
  const match = text.match(/\+CMGS:\s*(\d+)/);
  return match ? match[1] : "";
}

let latestGpsForSms = {
  hasFix: false,
  latitude: null,
  longitude: null,
};

function buildSmsPreview(senderName, senderContact, messageBody, gps) {
  let composed = "ELQDrone\n";
  composed += "From:";
  composed += senderName.length > 0 ? senderName : "Unknown";
  if (senderContact.length > 0) {
    composed += ` ${senderContact}`;
  }
  composed += "\n";

  if (gps.hasFix && Number.isFinite(gps.latitude) && Number.isFinite(gps.longitude)) {
    const lat = gps.latitude.toFixed(6);
    const lon = gps.longitude.toFixed(6);
    composed += `Loc:${lat},${lon}\n`;
  }

  composed += "Msg:";
  composed += messageBody;
  return composed;
}

function updateSmsPreview() {
  const senderNameEl = document.getElementById("smsSenderName");
  const senderContactEl = document.getElementById("smsSenderContact");
  const bodyEl = document.getElementById("smsMessageBody");
  const previewEl = document.getElementById("smsMessage");

  if (!senderNameEl || !senderContactEl || !bodyEl || !previewEl) {
    return;
  }

  const senderName = senderNameEl.value.trim();
  const senderContact = senderContactEl.value.trim();
  const messageBody = bodyEl.value.trim();
  previewEl.value = buildSmsPreview(senderName, senderContact, messageBody, latestGpsForSms);
}

function escapeHtml(value) {
  return String(value || "")
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/\"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

function renderSmsInboxCards(messages) {
  const inboxEl = document.getElementById("smsInboxCards");
  if (!inboxEl) {
    return;
  }

  if (!Array.isArray(messages) || messages.length === 0) {
    inboxEl.innerHTML = '<div class="inbox-empty">No messages found.</div>';
    return;
  }

  inboxEl.innerHTML = messages
    .map((message) => {
      const index = escapeHtml(message.index || "--");
      const status = escapeHtml(message.status || "UNKNOWN");
      const number = escapeHtml(message.number || "Unknown sender");
      const timestamp = escapeHtml((message.timestamp || "").replace(",", " "));
      const body = escapeHtml(message.body || "(empty)").replace(/\n/g, "<br>");

      return `
        <article class="inbox-card">
          <div class="inbox-card-head">
            <span class="inbox-chip">#${index}</span>
            <span class="inbox-chip inbox-chip-status">${status}</span>
          </div>
          <div class="inbox-from">${number}</div>
          <div class="inbox-time">${timestamp || "No timestamp"}</div>
          <div class="inbox-body">${body}</div>
        </article>
      `;
    })
    .join("");
}

async function refreshSmsInbox() {
  const inboxEl = document.getElementById("smsInboxCards");
  if (!inboxEl) {
    return;
  }

  inboxEl.innerHTML = '<div class="inbox-empty">Loading inbox...</div>';

  try {
    const payload = await fetchJson("/sms/inbox");
    if (!payload.ok) {
      inboxEl.innerHTML = `<div class="inbox-empty">${escapeHtml(payload.error || "Failed to read inbox.")}</div>`;
      return;
    }

    renderSmsInboxCards(payload.messages || []);
  } catch (error) {
    inboxEl.innerHTML = `<div class="inbox-empty">${escapeHtml(`Inbox error: ${error.message}`)}</div>`;
  }
}

async function sendSmsFromUi() {
  const senderNameEl = document.getElementById("smsSenderName");
  const senderContactEl = document.getElementById("smsSenderContact");
  const bodyEl = document.getElementById("smsMessageBody");
  const toEl = document.getElementById("smsTo");
  const previewEl = document.getElementById("smsMessage");
  const btnEl = document.getElementById("smsSendBtn");

  if (!senderNameEl || !senderContactEl || !bodyEl || !toEl || !previewEl || !btnEl) {
    return;
  }

  const senderName = senderNameEl.value.trim();
  const senderContact = senderContactEl.value.trim();
  const to = toEl.value.trim();
  const message = bodyEl.value.trim();

  if (!senderName || !senderContact || !to || !message) {
    setSmsStatus("Sender name, contact, recipient, and message are required", true);
    return;
  }

  if (!to || !message) {
    setSmsStatus("Phone number and message are required", true);
    return;
  }

  if (message.length > 220) {
    setSmsStatus("Message is too long (max 220 chars)", true);
    return;
  }

  updateSmsPreview();

  btnEl.disabled = true;
  setSmsStatus("Composing template and sending...");

  try {
    const response = await fetch("/sms/send", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ to, message, senderName, senderContact, includeLocation: true }),
    });

    const payload = await response.json();
    if (!response.ok || !payload.ok) {
      setSmsStatus(payload.error || "Failed to send SMS", true);
    } else {
      const suffix = payload.truncated ? " (shortened to 160 chars)" : "";
      const gpsFallback = payload.usedLocationFallback ? " (GPS map link auto-removed for reliable send)" : "";
      const cmgsRef = extractCmgsRef(payload.response);
      const refText = cmgsRef ? ` [CMGS:${cmgsRef}]` : "";
      setSmsStatus(`Submitted to network for ${payload.to}${suffix}${gpsFallback}${refText}`);
      bodyEl.value = "";
      updateSmsPreview();
    }
  } catch (error) {
    setSmsStatus(`Send error: ${error.message}`, true);
  } finally {
    btnEl.disabled = false;
  }
}

let modemHealthRefreshTimer = null;

async function refreshModemHealth() {
  try {
    const payload = await fetchJson("/modem/health");
    console.log("[refreshModemHealth] Received payload:", payload);
    if (!payload.ok) {
      console.warn("[refreshModemHealth] Payload ok=false, skipping update");
      return;
    }

    const now = new Date();
    setText("diagLastUpdate", `Updated ${now.toLocaleTimeString()}`);

    setText("diagSignalQuality", payload.signalQuality || "--");
    setText("diagRssi", `RSSI: ${payload.rssi} dBm`);

    const cregMap = { 0: "Not registered", 1: "Home", 2: "Searching", 5: "Roaming", "-1": "Unknown" };
    const cregStatus = cregMap[payload.creg] || `State ${payload.creg}`;
    setText("diagCreg", cregStatus);
    setText("diagCregNote", `CREG: ${payload.creg}`);

    const ceregMap = { 0: "Not registered", 1: "Home", 2: "Searching", 5: "Roaming", "-1": "Unknown" };
    const ceregStatus = ceregMap[payload.cereg] || `State ${payload.cereg}`;
    setText("diagCereg", ceregStatus);
    setText("diagCeregNote", `CEREG: ${payload.cereg} (Packet Domain)`);

    const cgattStatus = payload.cgatt ? "Attached" : "Detached";
    setText("diagCgatt", cgattStatus);
    setText("diagCgattNote", `CGATT: ${payload.cgatt}`);

    const simStatus = payload.simReady ? "Ready" : "Not ready";
    setText("diagSimReady", simStatus);
    setText("diagSimNote", payload.simReady ? "SIM detected" : "No SIM");

    const dataStatus = payload.dataConnected ? "Connected" : "Not connected";
    setText("diagDataConnected", dataStatus);
    const ipDisplay = payload.ipAddress && payload.ipAddress !== "0.0.0.0" ? payload.ipAddress : "--";
    setText("diagIpAddress", `IP: ${ipDisplay}`);
  } catch (error) {
    //silently ignore refresh errors
  }
}

function startModemHealthRefresh() {
  if (modemHealthRefreshTimer) {
    clearInterval(modemHealthRefreshTimer);
  }
  refreshModemHealth();
  modemHealthRefreshTimer = setInterval(refreshModemHealth, 2500);
}

function stopModemHealthRefresh() {
  if (modemHealthRefreshTimer) {
    clearInterval(modemHealthRefreshTimer);
    modemHealthRefreshTimer = null;
  }
}

async function testGpsInterference(action) {
  const resultEl = document.getElementById("gpsTestResult");
  const offBtn = document.getElementById("gpsTestOffBtn");
  const onBtn = document.getElementById("gpsTestOnBtn");

  if (!resultEl || !offBtn || !onBtn) {
    return;
  }

  offBtn.disabled = true;
  onBtn.disabled = true;
  resultEl.textContent = `${action === "off" ? "Disabling" : "Enabling"} GPS...`;
  resultEl.className = "sms-status ok";

  try {
    const response = await fetch(`/modem/gps-test?action=${action}`, { method: "POST" });
    const payload = await response.json();

    if (!response.ok || !payload.ok) {
      resultEl.textContent = `Failed: ${payload.response || "Unknown error"}`;
      resultEl.className = "sms-status error";
      return;
    }

    const state = payload.modemState;
    const summary = `GPS ${action === "off" ? "disabled" : "enabled"}. RSSI: ${state.rssi} dBm, CREG: ${state.creg}, CEREG: ${state.cereg}, CGATT: ${state.cgatt}`;
    resultEl.textContent = summary;
    resultEl.className = "sms-status ok";

    refreshModemHealth();
  } catch (error) {
    resultEl.textContent = `Test error: ${error.message}`;
    resultEl.className = "sms-status error";
  } finally {
    offBtn.disabled = false;
    onBtn.disabled = false;
  }
}

function updateTimestamp() {
  const now = new Date();
  setText("lastUpdated", now.toLocaleTimeString());
}

let lastGpsPingMs = 0;

function updateGpsPingAge() {
  if (!lastGpsPingMs) {
    setText("lastGpsPingAge", "--");
    return;
  }

  const elapsedSeconds = Math.max(0, Math.floor((Date.now() - lastGpsPingMs) / 1000));
  setText("lastGpsPingAge", `${elapsedSeconds}s ago`);
}

function setGpsFallback(reasonText) {
  setText("gpsDistance", "Satellite distance: not available without a GPS fix");
  setText("gpsHint", reasonText);
}

async function refreshGps() {
  const gps = await fetchJson("/gps");
  lastGpsPingMs = Date.now();
  updateGpsPingAge();

  latestGpsForSms = {
    hasFix: !!gps.hasFix,
    latitude: gps.latitude,
    longitude: gps.longitude,
  };

  if (gps.hasFix) {
    const statusText = `Ping OK · Fix ${gps.fixType} · ${gps.satellites} sats`;
    setStatus("gpsStatus", true, statusText);
    setStatus("gpsStatusLocation", true, statusText);
    setText("gpsAboutStatus", statusText);
    const coords = `${gps.latitude.toFixed(6)}, ${gps.longitude.toFixed(6)}`;
    setTextMany(["gpsCoords", "gpsCoordsLocation", "gpsCoordsLarge"], coords);
    setText("gpsDistance", `Altitude: ${gps.altitudeMeters.toFixed(2)} m · Satellites: ${gps.satellites}`);
    setText("gpsHint", `${gps.altitudeMeters.toFixed(2)} m altitude · ${gps.speedKph.toFixed(2)} kph`);
    setText("locationAltitude", `${gps.altitudeMeters.toFixed(2)} m`);
    setText("gpsRaw", gps.raw || "--");
    setMapLink(gps.latitude, gps.longitude);
  } else {
    const waitingStatus = "Ping OK · GNSS waiting";
    setNeutralStatus("gpsStatus", waitingStatus);
    setNeutralStatus("gpsStatusLocation", waitingStatus);
    setText("gpsAboutStatus", waitingStatus);
    const waitingText = gps.raw || "Waiting for satellites...";
    setTextMany(["gpsCoords", "gpsCoordsLocation", "gpsCoordsLarge"], waitingText);
    setText("locationAltitude", "--");
    setText("gpsRaw", gps.raw || "--");
    setGpsFallback(
      "The map is not loading because the SIM7600G does not have a valid GPS fix yet. Move near a window or open sky, and keep the GPS antenna clear of metal, then wait for enough satellites."
    );
    const mapLink = document.getElementById("gpsMap");
    if (mapLink) {
      mapLink.href = "#";
    }
  }

  updateSmsPreview();
}

async function refreshNet() {
  const net = await fetchJson("/netinfo");
  const lteText = net.dataConnected ? "Internet up" : "No data";
  setStatus("lteStatus", net.dataConnected, lteText);
  setText("lteAboutStatus", lteText);
  setText("rssi", `RSSI ${net.rssi}`);
  setText("simReady", net.simReady ? "SIM ready" : "SIM missing");
  setText("registered", `CREG ${net.creg} · CEREG ${net.cereg}`);
  setText("attached", net.cgatt ? "Attached" : "Detached");
  setText("speed", `${net.downloadMbps} / ${net.uploadMbps} Mbps`);
  setText("internetState", net.dataConnected ? `APN ${net.apn} · IP ${net.ipAddress || "--"}` : `APN ${net.apn || "--"} · waiting for session`);
}

async function refreshLogs() {
  const logs = await fetchJson("/logs");
  const gpsLog = logs.gpsLog || "(no /gps_log.csv yet)";
  const sessionLog = logs.sessionLog || "(no /sessions.log yet)";
  setText("logs", `GPS LOG\n${gpsLog}\n\nSESSION LOG\n${sessionLog}`);
}

async function tick() {
  try {
    const wifiText = "Connected to ELQWifi";
    setNeutralStatus("wifiStatus", wifiText);
    setText("wifiStatusTop", "Online");
    setText("wifiStatusAbout", "Online");
    await Promise.all([refreshGps(), refreshNet(), refreshLogs(), refreshPortalStatus()]);
    updateTimestamp();
  } catch (error) {
    setNeutralStatus("wifiStatus", "Telemetry error");
    setText("wifiStatusTop", "Error");
    setText("wifiStatusAbout", "Error");
    setText("logs", `Error fetching data: ${error.message}`);
  }
}

tick();
setInterval(updateGpsPingAge, 1000);
setInterval(tick, 30000);

const navButtons = document.querySelectorAll(".view-nav-item[data-view]");
navButtons.forEach((button) => {
  button.addEventListener("click", () => setActiveView(button.dataset.view || "home"));
});

setActiveView((location.hash || "#home").replace("#", "") || "home");
window.addEventListener("hashchange", () => {
  setActiveView((location.hash || "#home").replace("#", "") || "home");
});

const portalSignInBtn = document.getElementById("portalSignInBtn");
if (portalSignInBtn) {
  portalSignInBtn.addEventListener("click", signInPortal);
}

const smsSendBtn = document.getElementById("smsSendBtn");
if (smsSendBtn) {
  smsSendBtn.addEventListener("click", sendSmsFromUi);
}

["smsSenderName", "smsSenderContact", "smsMessageBody"].forEach((id) => {
  const el = document.getElementById(id);
  if (el) {
    el.addEventListener("input", updateSmsPreview);
  }
});

const smsInboxRefreshBtn = document.getElementById("smsInboxRefreshBtn");
if (smsInboxRefreshBtn) {
  smsInboxRefreshBtn.addEventListener("click", refreshSmsInbox);
}

const gpsTestOffBtn = document.getElementById("gpsTestOffBtn");
if (gpsTestOffBtn) {
  gpsTestOffBtn.addEventListener("click", () => testGpsInterference("off"));
}

const gpsTestOnBtn = document.getElementById("gpsTestOnBtn");
if (gpsTestOnBtn) {
  gpsTestOnBtn.addEventListener("click", () => testGpsInterference("on"));
}

refreshSmsInbox();
updateSmsPreview();