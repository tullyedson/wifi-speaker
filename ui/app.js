"use strict";
const $ = id => document.getElementById(id);
const fields = ["listen_url", "public_url", "outbound_url", "outbound_token", "api_token", "backend_mode", "model", "router_instance_id", "router_header_name", "system_prompt", "whisper_model", "voice_id"];
const numbers = ["whisper_threads", "speech_threshold", "silence_ms", "max_utterance_seconds", "response_timeout_seconds", "model_max_tokens"];
let settings = null;
let status = null;
let dirty = false;
let editVersion = 0;
let saving = false;
let setupSpeaker = null;
const rows = new Map();
const invoke = (command, args) => window.__TAURI__.core.invoke(command, args);
function notice(message, error = false) { $("notice").textContent = String(message); $("notice").className = error ? "error" : ""; $("notice").hidden = false; }
function changed() { dirty = true; editVersion++; $("save-state").textContent = "Unsaved changes. Saving reconnects speakers and clears active requests."; }
function words(value) { return [...new Set(value.split(",").map(x => x.trim()).filter(Boolean))]; }
function callbackUrl() { return settings.public_url.replace(/\/+$/, "") + "/v1/responses"; }
function derived() { $("openai-fields").hidden = settings.backend_mode !== "open_ai"; $("callback-url").value = callbackUrl(); }
function el(tag, className, text) { const node = document.createElement(tag); if (className) node.className = className; if (text !== undefined) node.textContent = text; return node; }
async function copy(value) { try { await navigator.clipboard.writeText(value); notice("Copied to clipboard."); } catch { notice("Clipboard is unavailable. Select the field and press Ctrl+C.", true); } }
async function action(button, operation) { button.disabled = true; try { await operation(); } catch (error) { notice(error, true); } finally { button.disabled = false; } }
function populate() {
  for (const id of [...fields, ...numbers]) $(id).value = settings[id];
  $("wake_names").value = settings.wake_names.join(", ");
  derived(); renderSpeakers(); examples();
}
function labeledInput(title, value, update) {
  const label = el("label", "", title); const input = el("input"); input.value = value;
  input.addEventListener("input", () => { update(input.value); changed(); }); label.append(input); return label;
}
function audioControl(title, value, min, max, step, suffix, update) {
  const label = el("label", "audio-control"), heading = el("span", "audio-heading"), output = el("output", "", value + suffix);
  heading.append(document.createTextNode(title), output);
  const input = el("input"); input.type = "range"; input.min = min; input.max = max; input.step = step; input.value = value; input.setAttribute("aria-label", title);
  input.addEventListener("input", () => { const next = Number(input.value); update(next); output.textContent = next + suffix; changed(); });
  label.append(heading, input); return label;
}
function renderSpeakers() {
  $("speaker-list").replaceChildren(); rows.clear(); $("empty-speakers").hidden = settings.speakers.length > 0;
  for (const speaker of settings.speakers) {
    const card = el("article", "speaker-card"); card.dataset.speakerId = speaker.id;
    const top = el("div", "speaker-top"); top.append(el("span", "speaker-symbol", "≋"));
    const identity = el("div", "identity"), name = el("strong", "", speaker.name); identity.append(name, el("small", "", speaker.id));
    const badge = el("span", "badge neutral", "Offline"); top.append(identity, badge); card.append(top);
    const fields = el("div", "fields two");
    fields.append(labeledInput("Speaker name", speaker.name, value => { speaker.name = value; name.textContent = value || "Unnamed speaker"; }), labeledInput("Tags, separated by commas", speaker.tags.join(", "), value => { speaker.tags = words(value); })); card.append(fields);
    const audioFields = el("div", "fields two audio-fields");
    audioFields.append(audioControl("Microphone gain", speaker.microphone_gain ?? 1, 0.25, 8, 0.25, "×", value => { speaker.microphone_gain = value; }), audioControl("Playback volume", speaker.volume ?? 65, 0, 100, 1, "%", value => { speaker.volume = value; }));
    const volumeState = el("small", "audio-help", "Save settings to apply this speaker’s gain and volume.");
    card.append(audioFields, volumeState);
    const meterLabel = el("div", "meter-label"), frames = el("span", "", "Waiting for connection"); meterLabel.append(el("span", "", "Microphone"), frames);
    const meter = el("div", "meter"), bar = el("i"); meter.append(bar); card.append(meterLabel, meter);
    const error = el("div", "speaker-error"); error.hidden = true; card.append(error);
    const bottom = el("div", "speaker-bottom"), enabled = el("label", "speaker-enabled"), checkbox = el("input"); checkbox.type = "checkbox"; checkbox.checked = speaker.enabled;
    checkbox.addEventListener("change", () => { speaker.enabled = checkbox.checked; changed(); }); enabled.append(checkbox, document.createTextNode("Enabled"));
    const actions = el("div", "speaker-actions"), setup = el("button", "secondary", "Setup details"), test = el("button", "secondary", "Test voice"), remove = el("button", "danger", "Remove");
    setup.addEventListener("click", () => openSetup(speaker));
    test.addEventListener("click", () => action(test, async () => { if (dirty) throw new Error("Save settings before testing a speaker."); await invoke("test_speaker", { speakerId: speaker.id }); notice("Test speech sent to " + speaker.name + "."); }));
    remove.addEventListener("click", () => { settings.speakers = settings.speakers.filter(s => s.id !== speaker.id); changed(); renderSpeakers(); examples(); });
    actions.append(setup, test, remove); bottom.append(enabled, actions); card.append(bottom); $("speaker-list").append(card);
    rows.set(speaker.id, { badge, bar, frames, error, test, volumeState });
  }
  updateStatus();
}
function updateStatus() {
  if (!status || !settings) return;
  const online = status.speakers.filter(s => s.connected).length;
  $("connected-count").textContent = `${online} / ${settings.speakers.length}`;
  $("model-state").textContent = status.model_ready ? "Ready" : "Model needed";
  $("wake-display").textContent = settings.wake_names[0] || "Choose a wake name";
  $("hub-badge").textContent = !status.listening_address ? "Listener unavailable" : status.paused ? "Listening paused" : "Hub running";
  $("hub-badge").className = "badge " + (status.listening_address && !status.paused ? "ok" : "neutral");
  $("pause").textContent = status.paused ? "Resume listening" : "Pause listening";
  for (const [id, row] of rows) {
    const speaker = status.speakers.find(s => s.speaker.id === id);
    const phase = speaker?.phase || "offline";
    row.badge.textContent = phase.charAt(0).toUpperCase() + phase.slice(1);
    row.badge.className = "badge " + (phase === "listening" ? "ok" : ["offline", "paused", "muted"].includes(phase) ? "neutral" : "busy");
    const level = speaker?.level || 0;
    const db = level > 0 ? 20 * Math.log10(level) : -60;
    row.bar.style.width = Math.max(0, Math.min(100, (db + 60) / 60 * 100)) + "%";
    row.bar.classList.toggle("hot", db > -6);
    row.frames.textContent = speaker?.connected ? `${Math.max(-60, Math.round(db))} dB · ${speaker.requests} requests` : "Waiting for connection";
    row.volumeState.textContent = !speaker?.connected ? "Gain and volume are saved for this speaker." : speaker.device_volume == null ? "Device volume control needs compatible firmware. Gain runs on this PC." : `Device volume: ${speaker.device_volume}%. Save settings to apply adjustments.`;
    row.error.textContent = speaker?.last_error || ""; row.error.hidden = !speaker?.last_error;
    row.test.disabled = !speaker?.connected || phase !== "listening" || dirty;
  }
}
function openSetup(speaker) { setupSpeaker = speaker; $("setup-title").textContent = speaker.name; $("setup-hub").value = settings.public_url; $("setup-id").value = speaker.id; $("setup-token").value = speaker.token; $("setup-dialog").showModal(); }
function examples() {
  const id = settings.speakers[0]?.id || "speaker-id-from-prompt";
  $("response-example").textContent = JSON.stringify({ request_id: "copy-request_id-from-the-prompt", speaker_id: id, text: "Your speaker is connected." }, null, 2);
  $("announcement-example").textContent = JSON.stringify({ request_id: "a-new-UUID-for-this-announcement", speaker_ids: [], tags: ["downstairs"], text: "Dinner is ready." }, null, 2);
}
const pages = { speakers: ["A voice for every room.", "Connect your speakers and give each one a place."], connection: ["Keep the conversation flowing.", "Choose where prompts go and how replies return."], speech: ["Choose your voice and wake name.", "Choose a voice and tune listening for your rooms."], guide: ["A small API. Many possibilities.", "Send a reply, speak to one room, or address a group."] };
for (const button of document.querySelectorAll(".nav")) button.addEventListener("click", () => {
  for (const tab of document.querySelectorAll(".nav")) tab.classList.toggle("active", tab === button);
  for (const page of document.querySelectorAll(".page")) page.hidden = page.id !== "page-" + button.dataset.page;
  [$("page-title").textContent, $("page-subtitle").textContent] = pages[button.dataset.page]; window.scrollTo(0, 0);
});
for (const id of fields) $(id).addEventListener("input", () => { settings[id] = $(id).value; changed(); derived(); });
for (const id of numbers) $(id).addEventListener("input", () => { settings[id] = Number($(id).value); changed(); });
$("wake_names").addEventListener("input", () => { settings.wake_names = words($("wake_names").value); changed(); });
$("save").addEventListener("click", () => action($("save"), async () => {
  saving = true;
  const savingVersion = editVersion;
  try { await invoke("save_settings", { settings: structuredClone(settings) }); dirty = editVersion !== savingVersion; $("save-state").textContent = dirty ? "Newer edits still need saving" : "All changes saved"; notice(dirty ? "Settings saved. Your newer edits still need saving." : "Settings saved. Speakers will reconnect automatically."); await poll(); }
  finally { saving = false; }
}));
$("add-speaker").addEventListener("click", () => action($("add-speaker"), async () => { if (!settings) return; if (settings.speakers.length >= 32) throw new Error("Up to 32 speakers can be registered."); settings.speakers.push(await invoke("new_speaker")); changed(); renderSpeakers(); examples(); }));
$("pause").addEventListener("click", () => action($("pause"), async () => { await invoke("set_paused", { paused: !status.paused }); await poll(); }));
$("startup").addEventListener("change", () => action($("startup"), async () => { try { await invoke("set_startup", { enabled: $("startup").checked }); } catch (error) { $("startup").checked = !$("startup").checked; throw error; } }));
$("download-model").addEventListener("click", () => action($("download-model"), async () => { notice("Downloading the English model (148 MB). This can take a few minutes."); const path = await invoke("download_model"); settings.whisper_model = path; $("whisper_model").value = path; changed(); notice("Model downloaded and verified. Save settings to use it."); }));
$("copy-callback").addEventListener("click", () => copy(callbackUrl()));
$("copy-api-token").addEventListener("click", () => copy(settings.api_token));
$("copy-speaker-token").addEventListener("click", () => copy(setupSpeaker.token));
$("copy-provision").addEventListener("click", () => action($("copy-provision"), async () => { if (!$("wifi-ssid").value) throw new Error("Enter the Wi-Fi network name first."); await copy(JSON.stringify({ type: "provision", wifi_ssid: $("wifi-ssid").value, wifi_password: $("wifi-password").value, hub_url: settings.public_url.replace(/\/+$/, ""), speaker_id: setupSpeaker.id, speaker_token: setupSpeaker.token, mic_channel: 0, mic_gain: 1 })); }));
$("setup-dialog").addEventListener("close", () => { $("wifi-ssid").value = ""; $("wifi-password").value = ""; $("setup-token").value = ""; setupSpeaker = null; });
async function poll() { status = await invoke("get_status"); updateStatus(); }
async function refreshLoop() { try { if (!saving) await poll(); } catch { $("hub-badge").textContent = "Hub unavailable"; $("hub-badge").className = "badge neutral"; } finally { setTimeout(refreshLoop, 1200); } }
async function initialize() {
  if (!window.__TAURI__) throw new Error("Open Smart Speaker.exe to use these settings.");
  settings = await invoke("get_settings");
  try { for (const voice of await invoke("get_voices")) { const option = el("option", "", `${voice.name} (${voice.language})`); option.value = voice.id; $("voice_id").append(option); } } catch (error) { notice("Windows voices could not be loaded: " + error, true); }
  populate(); await poll();
  try { $("startup").checked = await invoke("startup_enabled"); } catch { $("startup").disabled = true; }
  refreshLoop();
}
initialize().catch(error => { notice(error, true); for (const button of document.querySelectorAll("button:not(.nav)")) button.disabled = true; });
