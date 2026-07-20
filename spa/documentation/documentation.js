/**
 * Employee documentation: auth gate + libanim 2-port tap lesson player.
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

/** Parse narr lines from ASCII plan for seek/chapter speech without re-fire. */
function parseNarrCues(planText) {
  const cues = [];
  if (!planText) return cues;
  for (const raw of planText.split(/\r?\n/)) {
    const line = raw.trim();
    if (!line.startsWith("narr ")) continue;
    const m = line.match(/^narr\s+(\d+)\s+(.+)$/);
    if (!m) continue;
    cues.push({ t_ms: Number(m[1]), text: m[2] });
  }
  cues.sort((a, b) => a.t_ms - b.t_ms);
  return cues;
}

function cueAtOrBefore(cues, ms) {
  let hit = null;
  for (const c of cues) {
    if (c.t_ms <= ms) hit = c;
    else break;
  }
  return hit;
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
    onStatus: (line) => {
      if (voiceStatus) voiceStatus.textContent = line;
    },
  });

  function refreshVoiceUi() {
    if (voiceStatus) voiceStatus.textContent = narrator.statusLine();
    const en = $("voiceEnabled");
    const hold = $("voiceHold");
    const rate = $("voiceRate");
    if (en) en.checked = narrator.enabled;
    if (hold) hold.checked = narrator.pauseWhileSpeaking;
    if (rate) rate.value = String(narrator.rate);
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
  $("btnTestVoice")?.addEventListener("click", () => {
    narrator.ensureVoices();
    const ok = narrator.testVoice();
    const d = narrator.diagnostics();
    console.info("[documentation/voice] test", d);
    if (!ok || d.lastError) {
      narration.textContent =
        "Voice test failed: " +
        (d.lastError || "unknown") +
        (d.voiceCount === 0
          ? ". No system TTS voices found. On Linux install: sudo apt install speech-dispatcher espeak-ng"
          : ". Try Chrome/Edge, check OS sound output, and click Test voice again.");
    } else {
      narration.textContent =
        "Voice test started — you should hear a short phrase. If silent, check OS volume and TTS voices.";
    }
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
  let planText = "";
  let narrCues = [];
  let lastSpokenKey = "";

  try {
    wasm = createAnimWasmHost({ log: (s) => console.info("[documentation]", s) });
    await wasm.init("/explain/player/anim.wasm");
  } catch (e) {
    narration.textContent =
      "Could not load anim.wasm. Run ./scripts/run-status-map-junos.sh to link player assets. " +
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

  function speakCue(text, force) {
    if (!text || !narrator.enabled) return;
    const key = text;
    if (!force && key === lastSpokenKey && narrator.isSpeaking()) return;
    lastSpokenKey = key;
    narration.textContent = text;
    narrator.speak(text, { force: !!force });
    refreshVoiceUi();
  }

  function drain(opts = {}) {
    const doSpeak = opts.speak !== false;
    for (;;) {
      const ev = wasm.pollEvent();
      if (!ev) break;
      if (ev.type === 2 && ev.text) {
        narration.textContent = ev.text;
        if (doSpeak) speakCue(ev.text, opts.force);
      }
      if (ev.type === 4) {
        playing = false;
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
      planText = await res.text();
      narrCues = parseNarrCues(planText);
      if (!wasm.loadPlan(planText)) throw new Error("anim_load_plan failed");
      drain({ speak: false });
      draw();
      narration.textContent =
        "Ready — click Test voice (confirm audio), then Play. Voice uses the browser’s speech engine.";
      loaded = true;
      break;
    } catch (e) {
      console.warn(p, e);
    }
  }
  if (!loaded) {
    narration.textContent =
      "Lesson file missing. Run ./scripts/run-status-map-junos.sh to link two_port_tap.anim.";
    return;
  }

  function speakAtPlayhead(force) {
    const cue = cueAtOrBefore(narrCues, wasm.timeMs());
    if (cue) speakCue(cue.text, force);
  }

  $("btnPlay")?.addEventListener("click", () => {
    /* User gesture: best moment to unlock / prime TTS */
    narrator.ensureVoices();
    if (wasm.timeMs() >= wasm.durationMs()) {
      wasm.seekMs(0);
      drain({ speak: false });
      lastSpokenKey = "";
    }
    wasm.play();
    playing = true;
    lastTs = performance.now();
    /* Speak current chapter line immediately under the click gesture */
    speakAtPlayhead(true);
    refreshVoiceUi();
  });
  $("btnPause")?.addEventListener("click", () => {
    playing = false;
    wasm.pause();
    narrator.cancel();
    refreshVoiceUi();
  });
  $("btnRestart")?.addEventListener("click", () => {
    narrator.cancel();
    lastSpokenKey = "";
    wasm.seekMs(0);
    drain({ speak: false });
    wasm.play();
    playing = true;
    lastTs = performance.now();
    speakAtPlayhead(true);
    draw();
  });
  seek?.addEventListener("input", () => {
    playing = false;
    wasm.pause();
    narrator.cancel();
    lastSpokenKey = "";
    wasm.seekMs(Number(seek.value));
    drain({ speak: false });
    draw();
    refreshVoiceUi();
  });
  seek?.addEventListener("change", () => {
    /* On release, speak the cue at this time (user gesture from slider) */
    speakAtPlayhead(true);
  });

  document.querySelectorAll("#chapters li").forEach((li) => {
    li.addEventListener("click", () => {
      const ms = Number(li.getAttribute("data-ms") || 0);
      playing = false;
      wasm.pause();
      narrator.cancel();
      lastSpokenKey = "";
      wasm.seekMs(ms);
      drain({ speak: false });
      draw();
      speakAtPlayhead(true);
      refreshVoiceUi();
    });
  });

  function tick(now) {
    if (playing) {
      if (narrator.shouldHoldTimeline()) {
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
