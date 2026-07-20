/**
 * Wire libanim demo player modules for /explain/ and template API render.
 */

import { createAnimWasmHost } from "./player/wasm_host.js";
import { createAnimWebGpuRenderer, webgpuAvailable } from "./player/webgpu_renderer.js";

function $(id) {
  return document.getElementById(id);
}

export async function startExplainPlayer() {
  const canvas = $("stage");
  const overlay = $("overlay");
  const narration = $("narration");
  const modeLabel = $("modeLabel");
  const timeLabel = $("timeLabel");
  const seek = $("seek");

  let wasm = null;
  let gpu = null;
  let drawMode = "canvas2d";
  let ctx2d = null;
  let octx = overlay ? overlay.getContext("2d") : null;
  let playing = false;
  let lastTs = 0;
  let planText = "";

  try {
    wasm = createAnimWasmHost({ log: (s) => console.info("[explain]", s) });
    await wasm.init("./player/anim.wasm");
  } catch (e) {
    narration.textContent = "anim.wasm load failed: " + e;
    modeLabel.textContent = "Engine: failed";
    return;
  }

  if (webgpuAvailable()) {
    try {
      gpu = await createAnimWebGpuRenderer(canvas, {
        log: (s) => console.info("[explain]", s),
      });
      drawMode = "webgpu";
    } catch (e) {
      console.warn(e);
      ctx2d = canvas.getContext("2d");
      drawMode = "canvas2d";
    }
  } else {
    ctx2d = canvas.getContext("2d");
  }

  modeLabel.textContent =
    "Engine: WASM (" +
    (wasm.buildId || "anim") +
    ") · Draw: " +
    (drawMode === "webgpu" ? "WebGPU" : "Canvas2D");

  function drainNarration() {
    for (;;) {
      const ev = wasm.pollEvent();
      if (!ev) break;
      if (ev.type === 2 && ev.text) narration.textContent = ev.text;
      if (ev.type === 4) playing = false;
    }
  }

  function draw() {
    const cam = wasm.camera();
    const verts = wasm.fillVertices();
    if (drawMode === "webgpu" && gpu) {
      gpu.draw(verts, cam);
      if (octx) {
        octx.clearRect(0, 0, canvas.width, canvas.height);
      }
    } else if (ctx2d) {
      ctx2d.clearRect(0, 0, canvas.width, canvas.height);
      /* simple fill: rely on GPU path primarily */
      ctx2d.fillStyle = "#070b16";
      ctx2d.fillRect(0, 0, canvas.width, canvas.height);
    }
    timeLabel.textContent = wasm.timeMs() + " / " + wasm.durationMs() + " ms";
    seek.max = String(wasm.durationMs());
    seek.value = String(wasm.timeMs());
  }

  function loadPlanText(text) {
    planText = text;
    if (!wasm.loadPlan(text)) throw new Error("anim_load_plan failed");
    drainNarration();
    playing = false;
    draw();
    narration.textContent = "Ready — press Play.";
  }

  async function renderTemplate() {
    const tmpl = $("tmplSelect")?.value || "optical_path";
    let params = {};
    try {
      params = JSON.parse($("paramsJson")?.value || "{}");
    } catch (e) {
      $("apiOut").textContent = "params JSON error: " + e;
      return;
    }
    const r = await fetch("/api/v1/explain/render", {
      method: "POST",
      credentials: "same-origin",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ template: tmpl, params }),
    });
    const text = await r.text();
    $("apiOut").textContent = "HTTP " + r.status + "\n" + text.slice(0, 2000);
    let json;
    try {
      json = JSON.parse(text);
    } catch {
      return;
    }
    if (!json.ok || !json.plan) {
      narration.textContent = json.error || "render failed";
      return;
    }
    loadPlanText(json.plan);
  }

  $("btnRender")?.addEventListener("click", () => {
    renderTemplate().catch((e) => {
      $("apiOut").textContent = String(e);
    });
  });
  $("btnPlay")?.addEventListener("click", () => {
    if (wasm.timeMs() >= wasm.durationMs()) wasm.seekMs(0);
    wasm.play();
    playing = true;
    lastTs = performance.now();
  });
  $("btnPause")?.addEventListener("click", () => {
    playing = false;
    wasm.pause();
  });
  $("btnRestart")?.addEventListener("click", () => {
    wasm.seekMs(0);
    wasm.play();
    playing = true;
    lastTs = performance.now();
  });
  seek?.addEventListener("input", () => {
    playing = false;
    wasm.pause();
    wasm.seekMs(Number(seek.value));
    drainNarration();
    draw();
  });

  function tick(now) {
    if (playing) {
      const dt = Math.max(0, now - lastTs);
      lastTs = now;
      wasm.tick(Math.round(dt));
      drainNarration();
      if (!wasm.isPlaying()) playing = false;
      draw();
    }
    requestAnimationFrame(tick);
  }
  requestAnimationFrame(tick);

  /* default: try render optical_path if API works; else load fixture via fetch */
  try {
    await renderTemplate();
  } catch {
    try {
      const res = await fetch("./player/../fixtures/optical_path.anim");
      /* fixtures linked under explain/fixtures */
    } catch {
      /* ignore */
    }
    try {
      const res = await fetch("./fixtures/optical_path.anim");
      if (res.ok) loadPlanText(await res.text());
    } catch {
      narration.textContent =
        "Click Fill & play after templates are linked (run-explain.sh).";
    }
  }
}
