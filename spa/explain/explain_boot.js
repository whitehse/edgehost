/**
 * Auth gate for /explain/, then load libanim player (linked under ./player/).
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

function setVisible(el, visible) {
  if (!el) return;
  el.hidden = !visible;
  el.classList.toggle("hidden", !visible);
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

async function loadTemplates() {
  const sel = $("tmplSelect");
  if (!sel) return;
  const r = await fetchJson("/api/v1/explain/templates");
  sel.innerHTML = "";
  const list = r.json?.templates || [];
  if (!list.length) {
    const o = document.createElement("option");
    o.value = "optical_path";
    o.textContent = "optical_path (local fallback)";
    sel.appendChild(o);
    return;
  }
  for (const name of list) {
    const o = document.createElement("option");
    o.value = name;
    o.textContent = name;
    sel.appendChild(o);
  }
}

async function assetsReady() {
  const r = await fetch("./player/demo.js", {
    method: "GET",
    credentials: "same-origin",
  });
  return r.ok;
}

async function enterExplain(user) {
  setVisible($("auth-gate"), false);
  setVisible($("wrap"), true);
  setVisible($("topbar"), true);
  const b = $("userBadge");
  if (b && user) {
    b.textContent = user.sub || "employee";
    b.className = "badge ok";
  }

  const ready = await assetsReady().catch(() => false);
  if (!ready) {
    $("narration").textContent =
      "Player assets missing. Run ./scripts/run-explain.sh from edgehost.";
    return;
  }

  await loadTemplates().catch((e) => {
    $("apiOut").textContent = String(e);
  });

  /* Dynamic import of player module adapted for explain page */
  const mod = await import("./explain_player.js");
  await mod.startExplainPlayer();
}

async function main() {
  const user = await checkSession();
  if (user) {
    await enterExplain(user);
    return;
  }
  $("btnLogin")?.addEventListener("click", async () => {
    const pw = $("labPw")?.value || "lab";
    const r = await labLogin(pw);
    $("gateOut").textContent = "HTTP " + r.status + "\n" + r.text;
    if (r.ok) {
      const u = await checkSession();
      if (u) await enterExplain(u);
    }
  });
  $("labPw")?.addEventListener("keydown", (e) => {
    if (e.key === "Enter") $("btnLogin")?.click();
  });
}

main().catch((e) => {
  console.error(e);
  $("gateOut").textContent = String(e);
});
