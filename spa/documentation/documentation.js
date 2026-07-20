/**
 * Employee documentation: auth gate + libanim 2-port tap lesson player.
 * Assets: /explain/player/* and ./lessons/two_port_tap.anim
 * Voice: Web Speech API via /explain/player/narrator.js
 */

import { createAnimWasmHost } from "/explain/player/wasm_host.js";
import {
  createAnimWebGpuRenderer,
  webgpuAvailable,
} from "/explain/player/webgpu_renderer.js";
import { createNarrator } from "/explain/player/narrator.js";

function $(id) {
  return document.getElementById(id);
}

function setVisible(el, visible) {
  if (!el) return;
  el.hidden = !visible;
  el.classList.toggle("hidden", !visible);
}

async function fetchJson(url, opts) {
  const r = await fetch(url, { credentials: "same-origin", ...opts });
  const text = await r.text();
  let json = null;
  try {
    json = text ? JSON.parse(text) : null;
  } catch {
    /* ignore */
  }
  return { ok: r.ok, status: r.status, text, json };
}

async function checkSession() {
  const r = await fetchJson("/auth/me");
  return r.ok ? r.json : null;
}

async function labLogin(password) {
  return fetchJson("/auth/lab-login", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ password }),
  });
}

function highlightChapter(ms) {
  const items = document.querySelectorAll("#chapters li");
  let active = null;
  items.forEach((li) => {
    const t = Number(li.getAttribute("data-ms") || 0);
    if (t <= ms) active = li;
    li.classList.remove("active");
  });
  if (active) active.classList.add("active");
}

async function startLesson() {
  const canvas = $("stage");
  const overlay = $("overlay");
  const narration = $("narration");
  const modeLabel = $("modeLabel");
  const voiceStatus = $("voiceStatus");
  const timeLabel = $("timeLabel");
  const seek = $("seek");

  const narrator = createNarrator({
    enabled: true,
    pauseWhileSpeaking: true,
    rate: 1.0,
    log: (s) => console.info("[documentation/voice]", s),
  });

  function refreshVoiceUi() {
    if (voiceStatus) voiceStatus.textContent = narrator.statusLine();
    const en = $("voiceEnabled");
    const hold = $("voiceHold");
    const rate = $("voiceRate");
    if (en) en.checked = narrator.enabled;
    if (hold) hold.checked = narrator.pauseWhileSpeaking;
    if (rate) rate.value = String(narrator.rate);
    if (!narrator.available() && voiceStatus) {
      voiceStatus.textContent =
        "Voice: unavailable in this browser (Web Speech API missing)";
    }
  }

  $("voiceEnabled")?.addEventListener("change", (e) => {
    narrator.setEnabled(e.target.checked);
    refreshVoiceUi();
  });
  $("voiceHold")?.addEventListener("change", (e) => {
    narrator.setPauseWhileSpeaking(e.target.checked);
    refreshVoiceUi();
  });
  $("voiceRate")?.addEventListener("input", (e) => {
    narrator.setRate(e.target.value);
    refreshVoiceUi();
  });
  $("btnStopVoice")?.addEventListener("click", () => {
    narrator.cancel();
    refreshVoiceUi();
  });
  refreshVoiceUi();

  let wasm = null;
  let gpu = null;
  let drawMode = "canvas2d";
  let ctx2d = null;
  let octx = overlay ? overlay.getContext("2d") : null;
  let playing = false;
  let lastTs = 0;

  try {
    wasm = createAnimWasmHost({ log: (s) => console.info("[documentation]", s) });
    await wasm.init("/explain/player/anim.wasm");
  } catch (e) {
    narration.textContent =
      "Could not load anim.wasm. Run ./scripts/run-status-map-junos.sh (or run-documentation.sh) to link player assets. " +
      e;
    modeLabel.textContent = "Engine: unavailable";
    return;
  }

  if (webgpuAvailable()) {
    try {
      gpu = await createAnimWebGpuRenderer(canvas, {
        log: (s) => console.info("[documentation]", s),
      });
      drawMode = "webgpu";
    } catch (e) {
      console.warn(e);
      ctx2d = canvas.getContext("2d");
    }
  } else {
    ctx2d = canvas.getContext("2d");
  }

  modeLabel.textContent =
    "Engine: WASM · Draw: " + (drawMode === "webgpu" ? "WebGPU" : "Canvas2D");

  function onNarration(text) {
    if (!text) return;
    narration.textContent = text;
    narrator.speak(text);
    refreshVoiceUi();
  }

  function drain(opts = {}) {
    const speak = opts.speak !== false;
    for (;;) {
      const ev = wasm.pollEvent();
      if (!ev) break;
      if (ev.type === 2 && ev.text) {
        narration.textContent = ev.text;
        if (speak) narrator.speak(ev.text);
      }
      if (ev.type === 4) {
        playing = false;
        /* let last line finish */
      }
    }
    refreshVoiceUi();
  }

  function draw() {
    const cam = wasm.camera();
    const verts = wasm.fillVertices();
    if (drawMode === "webgpu" && gpu) {
      gpu.draw(verts, cam);
      if (octx) octx.clearRect(0, 0, canvas.width, canvas.height);
    } else if (ctx2d) {
      ctx2d.fillStyle = "#070b16";
      ctx2d.fillRect(0, 0, canvas.width, canvas.height);
    }
    const t = wasm.timeMs();
    const d = wasm.durationMs();
    timeLabel.textContent = t + " / " + d + " ms";
    seek.max = String(d);
    seek.value = String(t);
    highlightChapter(t);
  }

  const paths = [
    "./lessons/two_port_tap.anim",
    "/documentation/lessons/two_port_tap.anim",
  ];
  let loaded = false;
  for (const p of paths) {
    try {
      const res = await fetch(p, { credentials: "same-origin" });
      if (!res.ok) continue;
      const planText = await res.text();
      if (!wasm.loadPlan(planText)) throw new Error("anim_load_plan failed");
      drain({ speak: false });
      draw();
      narration.textContent =
        "Ready — press Play for voice + animation (or click a chapter). First click enables browser speech.";
      loaded = true;
      break;
    } catch (e) {
      console.warn(p, e);
    }
  }
  if (!loaded) {
    narration.textContent =
      "Lesson file missing. Link fixtures/two_port_tap.anim via run-status-map-junos.sh or run-documentation.sh.";
    return;
  }

  $("btnPlay")?.addEventListener("click", () => {
    /* User gesture unlocks speech in most browsers */
    if (wasm.timeMs() >= wasm.durationMs()) {
      wasm.seekMs(0);
      drain({ speak: false });
    }
    wasm.play();
    playing = true;
    lastTs = performance.now();
    /* If already on a cue with text showing, re-speak current box */
    if (narration.textContent && !narration.textContent.startsWith("Ready")) {
      narrator.speak(narration.textContent, { force: true });
    }
  });
  $("btnPause")?.addEventListener("click", () => {
    playing = false;
    wasm.pause();
    narrator.cancel();
    refreshVoiceUi();
  });
  $("btnRestart")?.addEventListener("click", () => {
    narrator.cancel();
    wasm.seekMs(0);
    drain({ speak: false });
    wasm.play();
    playing = true;
    lastTs = performance.now();
    draw();
  });
  seek?.addEventListener("input", () => {
    playing = false;
    wasm.pause();
    narrator.cancel();
    wasm.seekMs(Number(seek.value));
    drain({ speak: false });
    draw();
    /* Speak the caption that is now current (last drained or on-screen) */
    if (narrator.enabled && narration.textContent) {
      /* after seek, re-fire nearest past narration by scanning plan is heavy;
         speak whatever text we last set if user clicks Play */
    }
    refreshVoiceUi();
  });

  document.querySelectorAll("#chapters li").forEach((li) => {
    li.addEventListener("click", () => {
      const ms = Number(li.getAttribute("data-ms") || 0);
      playing = false;
      wasm.pause();
      narrator.cancel();
      wasm.seekMs(ms);
      /* Chapter click is a user gesture — speak cues at this playhead */
      drain({ speak: true });
      draw();
      refreshVoiceUi();
    });
  });

  function tick(now) {
    if (playing) {
      if (narrator.shouldHoldTimeline()) {
        /* Hold playhead; keep drawing and voice status */
        lastTs = now;
        draw();
        refreshVoiceUi();
      } else {
        const dt = Math.max(0, now - lastTs);
        lastTs = now;
        wasm.tick(Math.round(dt));
        drain({ speak: true });
        if (!wasm.isPlaying() && !narrator.isSpeaking()) playing = false;
        draw();
      }
    }
    requestAnimationFrame(tick);
  }
  requestAnimationFrame(tick);
}

async function enter(user) {
  setVisible($("auth-gate"), false);
  setVisible($("wrap"), true);
  const b = $("userBadge");
  if (b) b.textContent = (user && user.sub) || "employee";
  await startLesson();
}

async function main() {
  const user = await checkSession();
  if (user) {
    await enter(user);
    return;
  }
  $("btnLogin")?.addEventListener("click", async () => {
    const r = await labLogin($("labPw")?.value || "lab");
    $("gateOut").textContent = "HTTP " + r.status + "\n" + r.text;
    if (r.ok) {
      const u = await checkSession();
      if (u) await enter(u);
    }
  });
  $("labPw")?.addEventListener("keydown", (e) => {
    if (e.key === "Enter") $("btnLogin")?.click();
  });
}

main().catch((e) => {
  console.error(e);
  const g = $("gateOut");
  if (g) g.textContent = String(e);
});
