/**
 * Auth gate for /map/, then load the libwebmap demo host (main.js).
 *
 * Expects run-status-map.sh to have linked:
 *   spa/map/main.js, display/, basemap/, fiber_data/, webmap.wasm, …
 */

async function fetchJson(url, opts) {
  const r = await fetch(url, { credentials: "same-origin", ...opts });
  const text = await r.text();
  let json = null;
  try {
    json = text ? JSON.parse(text) : null;
  } catch {
    /* non-json */
  }
  return { ok: r.ok, status: r.status, text, json };
}

function $(id) {
  return document.getElementById(id);
}

function setGateMsg(msg) {
  const el = $("gateOut");
  if (el) el.textContent = msg;
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

function setVisible(el, visible) {
  if (!el) return;
  /* Prefer class + attribute: author CSS display rules override bare [hidden]. */
  el.hidden = !visible;
  el.classList.toggle("hidden", !visible);
}

function revealMap(user) {
  setVisible($("auth-gate"), false);
  setVisible($("mapTopbar"), true);
  setVisible($("wrap"), true);
  const mu = $("mapUser");
  if (mu && user) {
    const roles = Array.isArray(user.roles) ? user.roles.join(", ") : "";
    mu.textContent =
      " · " + (user.sub || "user") + (roles ? " (" + roles + ")" : "");
  }
}

function assetsReady() {
  /* edgehost static routes accept GET only (HEAD → 405). */
  return fetch("./main.js", { method: "GET", credentials: "same-origin" }).then(
    (r) => r.ok
  );
}

async function startMap() {
  const status = $("status");
  if (status) status.textContent = "Checking map assets…";

  const ready = await assetsReady().catch(() => false);
  if (!ready) {
    if (status) {
      status.innerHTML =
        '<span style="color:#f07178">Map assets missing.</span> ' +
        "From the edgehost repo run <code>./scripts/run-status-map.sh</code> " +
        "(links libwebmap demo basemap / fiber_data / display / webmap.wasm).";
    }
    const log = $("log");
    if (log) {
      log.textContent +=
        "missing spa/map/main.js — run scripts/run-status-map.sh\n";
    }
    return;
  }

  if (status) status.textContent = "Loading WebGPU map host…";
  await import("./main.js");
}

async function enterMap(user) {
  revealMap(user);
  try {
    await startMap();
  } catch (e) {
    setGateMsg("map load error: " + e);
    console.error(e);
  }
}

async function boot() {
  let user = await checkSession();
  if (!user) {
    const btn = $("gateLogin");
    const pw = $("gatePw");
    if (btn) {
      btn.addEventListener("click", async () => {
        try {
          const r = await labLogin(pw ? pw.value : "lab");
          setGateMsg("HTTP " + r.status + "\n" + r.text);
          if (!r.ok) return;
          user = (await checkSession()) || r.json || { sub: "lab" };
          await enterMap(user);
        } catch (e) {
          setGateMsg("login error: " + e);
          console.error(e);
        }
      });
    }
    if (pw) {
      pw.addEventListener("keydown", (e) => {
        if (e.key === "Enter" && btn) btn.click();
      });
    }
    return;
  }

  await enterMap(user);
}

boot().catch((e) => {
  setGateMsg("boot error: " + e);
  console.error(e);
});
