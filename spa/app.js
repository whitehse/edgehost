/* Lab console: health, lab login, state, WS, packages (phase-1 E2E). */
(function () {
  var ws = null;

  function $(id) { return document.getElementById(id); }

  function setText(id, text) {
    var el = $(id);
    if (el) el.textContent = text;
  }

  function appendLog(id, line) {
    var el = $(id);
    if (!el) return;
    var t = new Date().toISOString().slice(11, 19);
    el.textContent += "[" + t + "] " + line + "\n";
    el.scrollTop = el.scrollHeight;
  }

  function setBadge(ok, label) {
    var b = $("authBadge");
    if (!b) return;
    b.textContent = label;
    b.className = "badge " + (ok ? "ok" : "muted");
  }

  async function fetchText(url, opts) {
    var r = await fetch(url, Object.assign({ credentials: "same-origin" }, opts || {}));
    var body = await r.text();
    return { status: r.status, body: body, headers: r.headers, ok: r.ok };
  }

  async function refreshHealth() {
    try {
      var r = await fetchText("/health");
      setText("healthOut", "HTTP " + r.status + "\n" + r.body);
    } catch (e) {
      setText("healthOut", "error: " + e);
    }
  }

  async function login() {
    var pw = $("labPw").value;
    var r = await fetchText("/auth/lab-login", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ password: pw })
    });
    setText("authOut", "HTTP " + r.status + "\n" + r.body);
    if (r.ok) {
      setBadge(true, "session cookie set");
      await me();
    } else {
      setBadge(false, "login failed");
    }
  }

  async function me() {
    var r = await fetchText("/auth/me");
    setText("authOut", "HTTP " + r.status + "\n" + r.body);
    if (r.ok) setBadge(true, "authenticated");
    else setBadge(false, "not logged in");
  }

  function stateUrl() {
    var ns = $("stateNs").value;
    var key = $("stateKey").value.replace(/^\/+/, "");
    return "/api/v1/state/" + ns + "/" + key;
  }

  async function putState() {
    var r = await fetchText(stateUrl(), {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: $("stateBody").value
    });
    setText("stateOut", "PUT " + r.status + "\n" + r.body);
  }

  async function getState() {
    var r = await fetchText(stateUrl());
    setText("stateOut", "GET " + r.status + "\n" + r.body);
  }

  async function listState() {
    var ns = $("stateNs").value;
    var r = await fetchText("/api/v1/state/" + ns);
    setText("stateOut", "LIST " + r.status + "\n" + r.body);
  }

  function wsConnect() {
    if (ws) {
      try { ws.close(); } catch (e) { /* ignore */ }
      ws = null;
    }
    $("wsOut").textContent = "";
    var proto = location.protocol === "https:" ? "wss:" : "ws:";
    var url = proto + "//" + location.host + "/api/v1/stream?topics=state";
    appendLog("wsOut", "connecting " + url);
    ws = new WebSocket(url);
    ws.onopen = function () { appendLog("wsOut", "open"); };
    ws.onclose = function (ev) {
      appendLog("wsOut", "close code=" + ev.code);
      ws = null;
    };
    ws.onerror = function () { appendLog("wsOut", "error"); };
    ws.onmessage = function (ev) { appendLog("wsOut", "msg " + ev.data); };
  }

  function wsClose() {
    if (ws) ws.close();
  }

  async function loadPackages() {
    var r = await fetchText("/packages/index.json");
    setText("pkgOut", "HTTP " + r.status + "\n" + r.body);
  }

  $("btnHealth").addEventListener("click", refreshHealth);
  $("btnLogin").addEventListener("click", login);
  $("btnMe").addEventListener("click", me);
  $("btnPut").addEventListener("click", putState);
  $("btnGet").addEventListener("click", getState);
  $("btnList").addEventListener("click", listState);
  $("btnWsConnect").addEventListener("click", wsConnect);
  $("btnWsClose").addEventListener("click", wsClose);
  $("btnPkgIndex").addEventListener("click", loadPackages);

  refreshHealth();
  me().catch(function () { /* ignore */ });
})();
