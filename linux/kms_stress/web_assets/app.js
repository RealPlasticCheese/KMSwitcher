"use strict";

const $ = id => document.getElementById(id);
let config = null;
let revision = -1;
let state = null;
let reports = [];
let dirty = false;
let pending = false;
let reportSignature = "";
let configRefreshPending = false;

function hasTextSelection() {
  const selection = document.getSelection?.();
  return selection && !selection.isCollapsed;
}

function setText(element, text) {
  if (element.textContent !== text) element.textContent = text;
}

function showError(error) {
  $("error").textContent = error.message || String(error);
  $("error").hidden = false;
}

async function request(path, data) {
  const options = data === undefined ? {} : {
    method: "POST", headers: {"Content-Type": "application/json", "X-KMS-Request": "1"}, body: JSON.stringify(data)
  };
  const response = await fetch(path, {...options, cache: "no-store"});
  const result = await response.json();
  if (!response.ok) throw new Error(result.error || `Request failed (${response.status})`);
  return result;
}

let previewTimer;
let previewRevision = 0;
function markDirty() {
  dirty = true;
  clearTimeout(previewTimer);
  const current = ++previewRevision;
  previewTimer = setTimeout(async () => {
    try {
      const preview = await request("/api/preview", readConfig(false));
      if (current === previewRevision) $("workload").textContent = preview.text;
    } catch (error) {
      if (current === previewRevision) $("workload").textContent = "Enter valid settings to preview the workload.";
    }
  }, 150);
}

function deviceInput(device, key, type, label) {
  const input = document.createElement("input");
  input.type = type;
  input.setAttribute("aria-label", label);
  if (type === "checkbox") input.checked = device[key];
  else input.value = device[key];
  if (key === "slot") { input.min = "1"; input.max = "7"; }
  input.addEventListener("input", () => {
    device[key] = type === "checkbox" ? input.checked : type === "number" ? Number(input.value) : input.value;
    markDirty();
  });
  return input;
}

function renderDevices() {
  $("devices").replaceChildren();
  const rows = [...config.masters.map(d => ["Master", d, config.masters]), ...config.generators.map(d => ["Generator", d, config.generators]),
    ...config.receivers.map(d => ["Receiver", d, config.receivers])];
  for (const [role, device, collection] of rows) {
    const row = document.createElement("tr");
    function cell(content) {
      const td = document.createElement("td");
      if (typeof content === "string") td.textContent = content;
      else td.append(content);
      row.append(td);
    }
    cell(deviceInput(device, "enabled", "checkbox", `Enable ${device.name || role}`));
    cell(role);
    cell(deviceInput(device, "name", "text", `${role} name`));
    cell(deviceInput(device, "identity", "text", `${role} chip identity`));
    cell(deviceInput(device, role === "Receiver" ? "slot" : "uart", role === "Receiver" ? "number" : "text", `${role} ${role === "Receiver" ? "slot" : "UART path"}`));
    const remove = document.createElement("button");
    remove.type = "button";
    remove.textContent = "Remove";
    remove.addEventListener("click", () => { collection.splice(collection.indexOf(device), 1); markDirty(); renderDevices(); });
    const restart = document.createElement("button");
    restart.type = "button";
    restart.textContent = "Restart";
    restart.setAttribute("data-idle", "");
    restart.setAttribute("aria-label", `Restart ${device.name || role}`);
    restart.addEventListener("click", () => restartDevices({role: role.toLowerCase(), identity: device.identity}));
    const actions = document.createElement("div");
    actions.className = "actions";
    actions.append(restart, remove);
    cell(actions);
    $("devices").append(row);
  }
}

function renderConfig(value) {
  config = {campaign_repeats: 3, campaign_idle_seconds: 30, recovery_attempts: 3, campaign_recipe: "", fixture_notes: "", restart_before_run: false, ...structuredClone(value)};
  config.masters ??= config.master.identity || config.master.uart ? [config.master] : [];
  renderDevices();
  for (const key of ["preset", "rates", "seconds", "soak_seconds", "seed", "max_log_mb", "output", "campaign_repeats", "campaign_idle_seconds", "recovery_attempts", "fixture_notes"])
    $(key).value = key === "rates" ? config.rates.join(", ") : config[key];
  $("stop_on_failure").checked = config.stop_on_failure;
  $("restart_before_run").checked = config.restart_before_run;
  $("loop-note").hidden = config.preset !== "all-tests";
  dirty = false;
}

function readConfig(validate = true) {
  if (validate && !$("configuration").reportValidity()) throw new Error("Review the highlighted configuration fields.");
  const value = structuredClone(config);
  for (const key of ["preset", "output", "fixture_notes"]) value[key] = $(key).value.trim();
  for (const key of ["seconds", "soak_seconds", "seed", "max_log_mb", "campaign_repeats", "campaign_idle_seconds", "recovery_attempts"]) value[key] = Number($(key).value);
  value.rates = $("rates").value.split(",").map(s => s.trim()).filter(Boolean).map(Number);
  value.stop_on_failure = $("stop_on_failure").checked;
  value.restart_before_run = $("restart_before_run").checked;
  const selected = value.masters.filter(device => device.enabled);
  value.master = selected.length === 1 ? selected[0] : {identity: "", name: "Master", uart: "", slot: 0, enabled: false};
  return value;
}

function reportLink(name, download) {
  const link = document.createElement("a");
  link.className = "link-button";
  link.href = `/reports/${encodeURIComponent(name)}/${download ? "download.zip" : "report.html"}`;
  link.textContent = download ? "Download ZIP" : "View report";
  if (download) link.download = `${name}.zip`;
  else { link.target = "_blank"; link.rel = "noopener"; }
  return link;
}

function updateControls() {
  const busy = pending || !state || state.busy;
  for (const element of document.querySelectorAll("[data-idle]")) element.disabled = busy;
  $("editor").disabled = busy;
  $("stop").disabled = pending || !state || !state.busy;
}

function applyState(value, force = false) {
  state = value;
  configRefreshPending ||= force;
  // Polling continues while copying, but replacing any selected text node
  // would collapse the browser's selection. Render the newest state afterward.
  if (hasTextSelection()) { updateControls(); return; }
  if (!config || configRefreshPending || (value.revision !== revision && !dirty)) renderConfig(value.config);
  configRefreshPending = false;
  revision = value.revision;
  setText($("version"), `v${value.version}`);
  setText($("footer-version"), `v${value.version}`);
  setText($("profile"), `Server profile: ${value.profile}`);
  if (!dirty) setText($("workload"), value.workload_text);
  setText($("connection"), value.busy ? `${value.operation} in progress` : "Controller ready");
  $("connection").classList.toggle("busy", value.busy);
  const progress = value.progress;
  $("suite-progress").hidden = !progress;
  $("run-summary").hidden = !progress;
  if (progress) {
    const counts = Object.entries(progress.counts || {}).map(([key, count]) => `${key}: ${count}`).join(" · ");
    const seconds = Math.max(0, Math.ceil(progress.remaining_seconds));
    const remaining = `${Math.floor(seconds / 3600)}h ${Math.floor(seconds % 3600 / 60)}m ${seconds % 60}s`;
    const phase = progress.phase;
    const timing = phase === "finished" ? progress.status || "Finished"
      : phase === "cleanup" ? "Finishing capture and cleanup"
      : `${progress.estimated ? "Estimated " : ""}${remaining} remaining`;
    const activity = phase === "preparing" ? "Preparing hardware"
      : phase === "collecting" || seconds === 0 && phase !== "finished" && phase !== "cleanup"
        ? "Collecting evidence" : "";
    const summary = [timing, counts, activity].filter(Boolean).join(" · ");
    setText($("run-summary"), summary);
    const test = [progress.cycle ? `Cycle ${progress.cycle}` : "", progress.test,
      progress.slot === "direct" ? "direct USB" : progress.slot ? `receiver ${progress.slot}` : ""];
    setText($("suite-progress"), [...test, summary].filter(Boolean).join(" · "));
  }
  setText($("status"), value.messages.at(-1)?.split("\n").filter(Boolean).at(-1) || "Ready");
  const log = $("log");
  const atEnd = log.scrollHeight - log.scrollTop - log.clientHeight < 40;
  const text = value.messages.join("\n");
  if (log.textContent !== text) { log.textContent = text; if (atEnd) log.scrollTop = log.scrollHeight; }
  $("latest").replaceChildren();
  $("latest").hidden = !value.report;
  if (value.report) $("latest").append(reportLink(value.report, false), reportLink(value.report, true));
  updateControls();
}

async function action(path) {
  if (pending) return;
  $("error").hidden = true;
  try {
    const data = path === "/api/stop" ? {} : readConfig();
    if (path === "/api/run" && data.preset !== "baseline" && data.masters.filter(device => device.enabled).length !== 1)
      throw new Error("Select exactly one master in the Use column before running.");
    pending = true; updateControls();
    applyState(await request(path, data), path !== "/api/stop");
    await refreshReports();
  } catch (error) { showError(error); }
  finally { pending = false; updateControls(); }
}

async function restartDevices(target = null) {
  if (pending || !state || state.busy) return;
  $("error").hidden = true;
  try {
    const data = {config: readConfig(false), target};
    pending = true; updateControls();
    applyState(await request("/api/restart", data), true);
  } catch (error) { showError(error); }
  finally { pending = false; updateControls(); }
}

function renderReports() {
  const query = $("report-search").value.toLowerCase().trim();
  const visible = reports.filter(report => `${report.name} ${report.status} ${report.message}`.toLowerCase().includes(query));
  $("reports").replaceChildren();
  for (const report of visible) {
    const row = document.createElement("tr");
    for (const text of [report.name, report.status, new Date(report.modified * 1000).toLocaleString()]) {
      const cell = document.createElement("td");
      cell.textContent = text;
      row.append(cell);
    }
    row.children[0].title = report.message;
    row.children[1].className = `result ${report.status.endsWith("PASS") ? "pass" : report.status.includes("FAIL") ? "fail" : ""}`;
    const files = document.createElement("td");
    const links = document.createElement("div"); links.className = "actions";
    links.append(reportLink(report.name, false), reportLink(report.name, true));
    files.append(links); row.append(files); $("reports").append(row);
  }
  $("empty-reports").hidden = visible.length > 0;
  $("empty-reports").textContent = reports.length ? "No reports match your search." : "No completed reports in this directory yet. Run a test or try a demo.";
}

async function refreshReports() {
  const result = await request("/api/reports");
  if (hasTextSelection()) return;
  setText($("report-root"), `Server results directory: ${result.root}`);
  const signature = JSON.stringify(result);
  if (signature !== reportSignature) { reports = result.reports; reportSignature = signature; renderReports(); }
}

function downloadText(name, text, type) {
  const url = URL.createObjectURL(new Blob([text], {type}));
  const link = document.createElement("a"); link.href = url; link.download = name;
  document.body.append(link); link.click(); link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

for (const name of ["discover", "demo", "stop"]) $(name).addEventListener("click", () => action(`/api/${name}`));
$("start").addEventListener("click", () => action("/api/run"));
$("configuration").addEventListener("submit", event => { event.preventDefault(); action("/api/profile"); });
$("configuration").addEventListener("input", markDirty);
$("preset").addEventListener("change", () => {
  const timed = $("preset").value === "all-tests";
  $("loop-note").hidden = !timed;
  if (timed) $("stop_on_failure").checked = false;
  markDirty();
});
for (const role of ["generator", "receiver"]) $(`add-${role}`).addEventListener("click", () => {
  config[`${role}s`].push({identity: "", name: `New ${role}`, uart: "", slot: role === "receiver" ? 1 : 0, enabled: false});
  markDirty(); renderDevices();
});
for (const enabled of [true, false]) $(enabled ? "enable-all" : "disable-all").addEventListener("click", () => {
  for (const device of [...config.generators, ...config.receivers]) device.enabled = enabled;
  markDirty(); renderDevices();
});
$("export-profile").addEventListener("click", () => {
  try { downloadText("profile.json", JSON.stringify({version: 1, ...readConfig()}, null, 2) + "\n", "application/json"); }
  catch (error) { showError(error); }
});
$("import-profile").addEventListener("change", async event => {
  try {
    const file = event.target.files[0];
    if (!file) return;
    if (file.size > 128 * 1024) throw new Error("Profile must be at most 128 KiB.");
    const value = JSON.parse(await file.text());
    if (value.version !== 1 || !value.master || !Array.isArray(value.generators) || !Array.isArray(value.receivers) || !Array.isArray(value.rates))
      throw new Error("Select a KMSwitcher version 1 JSON profile.");
    delete value.version;
    renderConfig(value); markDirty();
    $("status").textContent = "Profile imported for review. Save profile to keep these settings on the server.";
  } catch (error) { showError(error); }
  finally { event.target.value = ""; }
});
$("save-log").addEventListener("click", () => downloadText("kmswitcher-diagnostics.txt", `KMSwitcher ${state?.version || ""}\nProfile: ${state?.profile || ""}\n\n${state?.messages.join("\n") ?? $("log").textContent}\n`, "text/plain"));
$("campaign_recipe_file").addEventListener("change", async event => {
  try {
    const file = event.target.files[0];
    if (!file) return;
    if (file.size > 16384) throw new Error("Campaign recipe must be at most 16 KiB.");
    const text = await file.text(); const recipe = JSON.parse(text);
    if (recipe.version !== 1 || !recipe.case || !recipe.fixture || !recipe.case_id) throw new Error("Select a USB campaign recipe JSON file.");
    const current = readConfig(false);
    current.campaign_recipe = text; current.preset = "usb-stall";
    renderConfig(current); markDirty();
  } catch (error) { showError(error); }
  finally { event.target.value = ""; }
});
$("clear_campaign_recipe").addEventListener("click", () => {
  config.campaign_recipe = ""; markDirty();
});
$("restart-all").addEventListener("click", () => restartDevices());
$("refresh-reports").addEventListener("click", () => refreshReports().catch(showError));
$("report-search").addEventListener("input", renderReports);

async function poll() {
  try {
    if (!pending) {
      const value = await request("/api/state");
      if (!config && !hasTextSelection()) for (const preset of value.presets) {
        const option = document.createElement("option"); option.value = preset; option.textContent = preset === "all-tests" ? "All tests — timed loop" : preset[0].toUpperCase() + preset.slice(1);
        $("preset").append(option);
      }
      applyState(value);
      await refreshReports();
    }
  } catch (error) {
    if (!hasTextSelection()) {
      setText($("connection"), "Disconnected · retrying");
      setText($("status"), error.message);
    }
    state = null; updateControls();
  } finally { setTimeout(poll, 1500); }
}
poll();
