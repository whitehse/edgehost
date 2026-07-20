/* Company home: lab login → status map / lab console. */
(function () {
  function $(id) { return document.getElementById(id); }

  function setText(id, text) {
    var el = $(id);
    if (el) el.textContent = text;
  }

  function setBadge(ok, label) {
    var b = $("authBadge");
    if (!b) return;
    b.textContent = label;
    b.className = "badge " + (ok ? "ok" : "muted");
  }

  function showApp(authed) {
    var login = $("loginPanel");
    var app = $("appPanel");
    if (login) login.classList.toggle("hidden", !!authed);
    if (app) app.classList.toggle("hidden", !authed);
  }

  async function fetchText(url, opts) {
    var r = await fetch(url, Object.assign({ credentials: "same-origin" }, opts || {}));
    var body = await r.text();
    return { status: r.status, body: body, ok: r.ok };
  }

  async function me() {
    var r = await fetchText("/auth/me");
    if (r.ok) {
      setBadge(true, "authenticated");
      showApp(true);
      setText("authOut", "HTTP " + r.status + "\n" + r.body);
      return true;
    }
    setBadge(false, "not logged in");
    showApp(false);
    return false;
  }

  async function login() {
    var pwEl = $("labPw");
    var pw = pwEl ? pwEl.value : "lab";
    var r = await fetchText("/auth/lab-login", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ password: pw })
    });
    setText("authOut", "HTTP " + r.status + "\n" + r.body);
    if (r.ok) {
      setBadge(true, "session cookie set");
      showApp(true);
      /* Prefer map after successful login when ?next=map or default */
      var params = new URLSearchParams(location.search);
      var next = params.get("next") || "map";
      if (next === "map") {
        location.href = "/map/";
        return;
      }
      if (next === "lab") {
        location.href = "/lab/";
        return;
      }
      if (next === "e7") {
        location.href = "/e7/";
        return;
      }
      if (next === "junos") {
        location.href = "/junos/";
        return;
      }
      await me();
    } else {
      setBadge(false, "login failed");
      showApp(false);
    }
  }

  function logoutLocal() {
    /* Cookie is HttpOnly — clear client view only; TTL expires server-side. */
    setBadge(false, "not logged in");
    showApp(false);
    setText("authOut", "Local UI reset. Cookie remains until Max-Age unless you clear site data.");
  }

  if ($("btnLogin")) $("btnLogin").addEventListener("click", login);
  if ($("btnLogout")) $("btnLogout").addEventListener("click", logoutLocal);
  if ($("labPw")) {
    $("labPw").addEventListener("keydown", function (e) {
      if (e.key === "Enter") login();
    });
  }

  me().catch(function () {
    showApp(false);
  });
})();
